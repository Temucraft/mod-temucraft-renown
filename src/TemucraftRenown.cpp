/*
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

/*
 * mod-temucraft-renown: Renown tracks for reputations, and account-wide ("Warband") reputation, for the Temucraft
 * client's reputation window and Journeys page (TemucraftReputation.lua / TemucraftRenown.lua in
 * mod-temucraft-client-version).
 *
 *   - Renown: mod_temucraft_renown_track (world DB) gives a faction levels, each at a whole standing (the faction's
 *     reputation counted from 0 = Neutral: Friendly is 3000, Honored 9000, Revered 21000, Exalted 42000) with a
 *     reward: an item (mailed when the bags are full), a spell (learned), a title, a vendor unlock (the item can only
 *     be bought from that level on, from any vendor that sells it) or none. Level 0, when it is there, is the track
 *     itself: its title is the word the bar shows ("Renown" by default, "Level" for friendship-style tracks), its
 *     reputation where the bar of level 1 starts, its description the Journeys page's overview. Every level reached
 *     is granted once (mod_temucraft_renown_granted, characters DB) when the reputation changes and at login (so new
 *     levels and reloaded tracks catch up), and the client is told.
 *   - Warband reputation (off by default): for the factions in Temucraft.Renown.Warband.Factions the highest
 *     standing any character of the account reached is kept (mod_temucraft_renown_warband, characters DB, written at
 *     saves and logout) and given to the account's other characters when they log in. It is never lowered, and a
 *     faction the character's side can never have (base standing Hated, like the other side's capitals) is skipped.
 *
 * Addon messages (prefix TRN, see TemucraftAddonMessage.h). Client asks:
 *   HELLO -> S~renown~warband
 *            T~faction~label~levels~name, TD~faction~base~overview, V~faction~level~reputation~type~reward~count~
 *            highlight~title~description ... (each track), TE~tracks
 *            W~faction,faction,... (the account-wide factions)
 *            F~id=name^id=name... (every faction's name in the player's locale: the client knows factions by name), FE
 *            P~faction~level,level,... (the levels this character got), PE (when the character's rows are read)
 * The server sends G~faction~level when a level is granted and R after .temucraft renown reload (the client asks
 * HELLO again). A record longer than one message goes as "+piece" messages and a last "=piece".
 *
 * GM command: .temucraft renown reload (RBAC 104301) reads the tracks again.
 *
 * Reads at login are asynchronous queries answered through the session's query processor, writes asynchronous
 * Executes: nothing waits on the database on the world thread.
 */
#include "Chat.h"
#include "CommandScript.h"
#include "Config.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "GameTime.h"
#include "Item.h"
#include "Mail.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerScript.h"
#include "ReputationMgr.h"
#include "ScriptMgr.h"
#include "SpellMgr.h"
#include "StringConvert.h"
#include "TemucraftAddonMessage.h"
#include "TemucraftModuleText.h"
#include "Tokenize.h"
#include "WorldScript.h"
#include "WorldSession.h"
#include "WorldSessionMgr.h"
#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

using namespace Acore::ChatCommands;

namespace
{
    constexpr char const* MODULE = "mod-temucraft-renown";
    constexpr char const* PREFIX = "TRN";
    constexpr size_t PIECE = 200;

    struct Settings
    {
        bool enable = true;
        bool announce = true;
        bool warband = false;
        std::set<uint32> warbandFactions;
    } cfg;

    enum class Reward : uint8 { None, Item, Spell, Title, Vendor };
    char const* RewardName(Reward r)
    {
        switch (r)
        {
            case Reward::Item: return "item";
            case Reward::Spell: return "spell";
            case Reward::Title: return "title";
            case Reward::Vendor: return "vendor";
            default: return "none";
        }
    }

    struct Level
    {
        uint32 level = 0;
        int32 reputation = 0;
        Reward type = Reward::None;
        uint32 reward = 0;
        uint32 count = 1;
        bool highlight = false;
        std::string title, description;
    };

    struct Track
    {
        uint32 faction = 0;
        std::string label = "Renown";
        int32 base = 0;
        std::string overview;
        std::vector<Level> levels;   // levels[0] is level 1

        // the highest level reached at this standing
        uint32 Reached(int32 standing) const
        {
            uint32 reached = 0;
            for (Level const& l : levels)
            {
                if (standing < l.reputation)
                    break;
                reached = l.level;
            }
            return reached;
        }
    };

    struct Data
    {
        std::map<uint32, Track> tracks;
        std::unordered_map<uint32, std::vector<std::pair<uint32, uint32>>> vendorGates;   // item -> (faction, level)
        uint32 levels = 0;
    };

    // the tracks: replaced as a whole by a reload, read by every map thread
    std::mutex dataLock;
    std::shared_ptr<Data const> data = std::make_shared<Data>();
    std::shared_ptr<Data const> GetData() { std::lock_guard<std::mutex> g(dataLock); return data; }

    // each online character's granted levels (only its own map thread touches its entry after it is made)
    struct PlayerState
    {
        bool loaded = false;
        bool helloPending = false;
        bool mirroring = false;
        std::map<uint32, std::set<uint32>> granted;
        std::map<uint32, int32> warbandDirty;   // faction -> standing to write to the account's row
    };
    std::mutex stateLock;
    std::unordered_map<ObjectGuid::LowType, PlayerState> states;

    PlayerState* State(Player* player, bool create = false)
    {
        std::lock_guard<std::mutex> g(stateLock);
        ObjectGuid::LowType const guid = player->GetGUID().GetCounter();
        auto it = states.find(guid);
        if (it == states.end())
        {
            if (!create)
                return nullptr;
            it = states.emplace(guid, PlayerState()).first;
        }
        return &it->second;
    }

    void DropState(Player* player)
    {
        std::lock_guard<std::mutex> g(stateLock);
        states.erase(player->GetGUID().GetCounter());
    }

    enum Text : uint32 { T_LEVEL = 1, T_REWARD, T_MAILED, T_MAIL_SUBJECT, T_MAIL_BODY, T_RELOADED, T_VENDOR_LOCKED };
    char const* Say(Player* player, uint32 id, char const* fallback) { return TemucraftModuleText(MODULE, player, id, fallback); }
    char const* Say(ChatHandler* handler, uint32 id, char const* fallback) { return TemucraftModuleText(MODULE, handler, id, fallback); }

    uint32 Now() { return uint32(GameTime::GetGameTime().count()); }

    std::string Fields(std::initializer_list<std::string> parts)
    {
        std::string out;
        for (std::string const& p : parts)
        {
            if (!out.empty())
                out += '~';
            out += p;
        }
        return out;
    }

    // one record, in pieces when it is too long for one addon message
    void Send(Player* player, std::string const& record)
    {
        if (record.size() <= PIECE + 30)
        {
            TemucraftAddonSend(player, PREFIX, record);
            return;
        }
        size_t at = 0;
        for (; at + PIECE < record.size(); at += PIECE)
            TemucraftAddonSend(player, PREFIX, "+" + record.substr(at, PIECE));
        TemucraftAddonSend(player, PREFIX, "=" + record.substr(at));
    }

    std::string FactionName(FactionEntry const* entry, WorldSession* session)
    {
        if (!entry)
            return "";
        LocaleConstant locale = session ? session->GetSessionDbcLocale() : LOCALE_enUS;
        char const* name = entry->name[locale];
        if (!name || !*name)
            name = entry->name[LOCALE_enUS];
        std::string out = name ? name : "";
        for (char& c : out) if (c == '~' || c == '\t' || c == '|' || c == '^' || c == '=') c = ' ';
        return out;
    }

    void LoadSettings()
    {
        cfg.enable = sConfigMgr->GetOption<bool>("Temucraft.Renown.Enable", true);
        cfg.announce = sConfigMgr->GetOption<bool>("Temucraft.Renown.Announce", true);
        cfg.warband = sConfigMgr->GetOption<bool>("Temucraft.Renown.Warband.Enable", false);
        cfg.warbandFactions.clear();
        std::string const list = sConfigMgr->GetOption<std::string>("Temucraft.Renown.Warband.Factions", "");
        for (std::string_view part : Acore::Tokenize(list, ',', false))
        {
            std::string s(part);
            s.erase(std::remove(s.begin(), s.end(), ' '), s.end());
            if (auto id = Acore::StringTo<uint32>(s))
            {
                if (!sFactionStore.GetNumRows())   // the first config load comes before the DBC stores: checked at startup
                {
                    cfg.warbandFactions.insert(*id);
                    continue;
                }
                FactionEntry const* entry = sFactionStore.LookupEntry(*id);
                if (entry && entry->CanHaveReputation())
                    cfg.warbandFactions.insert(*id);
                else
                    LOG_ERROR("server.loading", "{}: Temucraft.Renown.Warband.Factions: {} is not a faction with a reputation.", MODULE, *id);
            }
        }
    }

    Reward ParseReward(std::string const& s)
    {
        if (s == "item") return Reward::Item;
        if (s == "spell") return Reward::Spell;
        if (s == "title") return Reward::Title;
        if (s == "vendor") return Reward::Vendor;
        return Reward::None;
    }

    // the tracks from the world DB (at startup and on .temucraft renown reload)
    void LoadTracks()
    {
        auto fresh = std::make_shared<Data>();
        if (QueryResult r = WorldDatabase.Query("SELECT `faction`, `level`, `reputation`, `reward_type`, `reward_id`, `reward_count`, `highlight`, "
            "`title`, `description` FROM `mod_temucraft_renown_track` ORDER BY `faction`, `level`"))
            do
            {
                Field* f = r->Fetch();
                uint32 const faction = f[0].Get<uint32>();
                FactionEntry const* entry = sFactionStore.LookupEntry(faction);
                if (!entry || !entry->CanHaveReputation())
                {
                    LOG_ERROR("sql.sql", "{}: mod_temucraft_renown_track: faction {} doesn't exist or has no reputation, skipped.", MODULE, faction);
                    continue;
                }
                Track& t = fresh->tracks[faction];
                t.faction = faction;
                Level l;
                l.level = f[1].Get<uint8>();
                l.reputation = f[2].Get<int32>();
                l.type = ParseReward(f[3].Get<std::string>());
                l.reward = f[4].Get<uint32>();
                l.count = std::max<uint32>(1, f[5].Get<uint16>());
                l.highlight = f[6].Get<uint8>() != 0;
                l.title = TemucraftAddonClean(f[7].Get<std::string>());
                l.description = TemucraftAddonClean(f[8].Get<std::string>());
                if (l.level == 0)
                {
                    if (!l.title.empty())
                        t.label = l.title;
                    t.base = l.reputation;
                    t.overview = l.description;
                    continue;
                }
                bool ok = true;
                switch (l.type)
                {
                    case Reward::Item:
                    case Reward::Vendor: ok = sObjectMgr->GetItemTemplate(l.reward) != nullptr; break;
                    case Reward::Spell: ok = sSpellMgr->GetSpellInfo(l.reward) != nullptr; break;
                    case Reward::Title: ok = sCharTitlesStore.LookupEntry(l.reward) != nullptr; break;
                    default: break;
                }
                if (!ok)
                {
                    LOG_ERROR("sql.sql", "{}: mod_temucraft_renown_track: faction {} level {}: {} {} doesn't exist, the level gives nothing.",
                        MODULE, faction, l.level, RewardName(l.type), l.reward);
                    l.type = Reward::None;
                    l.reward = 0;
                }
                if (l.level != t.levels.size() + 1)
                {
                    LOG_ERROR("sql.sql", "{}: mod_temucraft_renown_track: faction {} has no level {} (levels must be 1, 2, 3, ...): level {} and above skipped.",
                        MODULE, faction, t.levels.size() + 1, l.level);
                    continue;
                }
                if (!t.levels.empty() && l.reputation < t.levels.back().reputation)
                {
                    LOG_ERROR("sql.sql", "{}: mod_temucraft_renown_track: faction {} level {} needs less reputation than level {}: raised to {}.",
                        MODULE, faction, l.level, l.level - 1, t.levels.back().reputation);
                    l.reputation = t.levels.back().reputation;
                }
                if (l.type == Reward::Vendor)
                    fresh->vendorGates[l.reward].emplace_back(faction, l.level);
                t.levels.push_back(l);
                ++fresh->levels;
            } while (r->NextRow());
        for (auto it = fresh->tracks.begin(); it != fresh->tracks.end();)
            it = it->second.levels.empty() ? fresh->tracks.erase(it) : std::next(it);
        std::lock_guard<std::mutex> g(dataLock);
        data = fresh;
    }

    // an asynchronous read answered on the session's thread, while the player is still logged in
    void Read(WorldSession* session, std::string const& sql, std::function<void(Player*, QueryResult)> answer)
    {
        session->GetQueryProcessor().AddCallback(CharacterDatabase.AsyncQuery(sql).WithCallback([session, answer](QueryResult r)
        {
            if (Player* player = session->GetPlayer())
                answer(player, r);
        }));
    }

    void GiveItem(Player* player, uint32 itemId, uint32 count, std::string const& what)
    {
        ItemPosCountVec dest;
        if (player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, itemId, count) == EQUIP_ERR_OK)
        {
            if (Item* item = player->StoreNewItem(dest, itemId, true, Item::GenerateItemRandomPropertyId(itemId)))
            {
                player->SendNewItem(item, count, true, false);
                return;
            }
        }
        // bags full (or the item is unique and the bags can't take it now): by mail
        MailDraft draft(Say(player, T_MAIL_SUBJECT, "Renown reward"), Acore::StringFormat(Say(player, T_MAIL_BODY, "Your reward for {}."), what));
        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        if (Item* item = Item::CreateItem(itemId, count, player))
        {
            item->SaveToDB(trans);
            draft.AddItem(item);
        }
        draft.SendMailTo(trans, MailReceiver(player), MailSender(MAIL_NORMAL, 0, MAIL_STATIONERY_DEFAULT));
        CharacterDatabase.CommitTransaction(trans);
        ChatHandler(player->GetSession()).SendSysMessage(Say(player, T_MAILED, "Your bags were full: your Renown reward is in your mailbox."));
    }

    void Grant(Player* player, PlayerState& st, Track const& track, Level const& l)
    {
        st.granted[track.faction].insert(l.level);
        CharacterDatabase.Execute("INSERT IGNORE INTO `mod_temucraft_renown_granted` (`guid`, `faction`, `level`, `time`) VALUES ({}, {}, {}, {})",
            player->GetGUID().GetCounter(), track.faction, l.level, Now());
        std::string const name = FactionName(sFactionStore.LookupEntry(track.faction), player->GetSession());
        std::string const what = Acore::StringFormat("{} {} {}", name, track.label, l.level);
        switch (l.type)
        {
            case Reward::Item:
                GiveItem(player, l.reward, l.count, what);
                break;
            case Reward::Spell:
                if (!player->HasSpell(l.reward))
                    player->learnSpell(l.reward);
                break;
            case Reward::Title:
                if (CharTitlesEntry const* title = sCharTitlesStore.LookupEntry(l.reward))
                    if (!player->HasTitle(title))
                        player->SetTitle(title);
                break;
            default:   // a vendor unlock is checked when buying; none gives nothing
                break;
        }
        if (cfg.announce)
        {
            ChatHandler chat(player->GetSession());
            chat.PSendSysMessage(Say(player, T_LEVEL, "|cff66bbffRenown:|r {} - {} {} reached."), name, track.label, l.level);
            if (!l.title.empty() && l.type != Reward::None)
                chat.PSendSysMessage(Say(player, T_REWARD, "|cff66bbffReward:|r {}"), l.title);
        }
        TemucraftAddonSend(player, PREFIX, Fields({ "G", std::to_string(track.faction), std::to_string(l.level) }));
    }

    // every level reached at `standing` that this character hasn't got yet
    void CatchUp(Player* player, PlayerState& st, Track const& track, int32 standing)
    {
        uint32 const reached = track.Reached(standing);
        if (!reached)
            return;
        std::set<uint32>& got = st.granted[track.faction];
        for (Level const& l : track.levels)
        {
            if (l.level > reached)
                break;
            if (!got.count(l.level))
                Grant(player, st, track, l);
        }
    }

    void CatchUpAll(Player* player, PlayerState& st)
    {
        if (!cfg.enable)
            return;
        auto d = GetData();
        for (auto const& [faction, track] : d->tracks)
            if (FactionEntry const* entry = sFactionStore.LookupEntry(faction))
                if (player->GetReputationMgr().GetState(entry))
                    CatchUp(player, st, track, player->GetReputationMgr().GetReputation(entry));
    }

    void SendGranted(Player* player, PlayerState const& st)
    {
        for (auto const& [faction, levels] : st.granted)
        {
            std::string list;
            for (uint32 l : levels)
            {
                if (!list.empty())
                    list += ',';
                list += std::to_string(l);
            }
            Send(player, Fields({ "P", std::to_string(faction), list }));
        }
        TemucraftAddonSend(player, PREFIX, "PE");
    }

    void SendHello(Player* player)
    {
        TemucraftAddonSend(player, PREFIX, Fields({ "S", cfg.enable ? "1" : "0", cfg.warband ? "1" : "0" }));
        WorldSession* session = player->GetSession();
        auto d = GetData();
        uint32 count = 0;
        if (cfg.enable)
            for (auto const& [faction, t] : d->tracks)
            {
                std::string const f = std::to_string(faction);
                Send(player, Fields({ "T", f, t.label, std::to_string(t.levels.size()), FactionName(sFactionStore.LookupEntry(faction), session) }));
                if (t.base || !t.overview.empty())
                    Send(player, Fields({ "TD", f, std::to_string(t.base), t.overview }));
                for (Level const& l : t.levels)
                    Send(player, Fields({ "V", f, std::to_string(l.level), std::to_string(l.reputation), RewardName(l.type), std::to_string(l.reward),
                        std::to_string(l.count), l.highlight ? "1" : "0", l.title, l.description }));
                ++count;
            }
        TemucraftAddonSend(player, PREFIX, "TE~" + std::to_string(count));
        std::string list;
        if (cfg.warband)
            for (uint32 f : cfg.warbandFactions)
            {
                if (!list.empty())
                    list += ',';
                list += std::to_string(f);
            }
        Send(player, "W~" + list);
        // the names the client's list shows, in the client's locale
        std::string names;
        for (auto const& [listId, state] : player->GetReputationMgr().GetStateList())
        {
            std::string const name = FactionName(sFactionStore.LookupEntry(state.ID), session);
            if (name.empty())
                continue;
            std::string const entry = std::to_string(state.ID) + "=" + name;
            if (!names.empty() && names.size() + entry.size() + 1 > PIECE)
            {
                TemucraftAddonSend(player, PREFIX, "F~" + names);
                names.clear();
            }
            if (!names.empty())
                names += '^';
            names += entry;
        }
        if (!names.empty())
            TemucraftAddonSend(player, PREFIX, "F~" + names);
        TemucraftAddonSend(player, PREFIX, "FE");
        if (PlayerState* st = State(player))
        {
            if (st->loaded)
                SendGranted(player, *st);
            else
                st->helloPending = true;
        }
    }

    // the account's best standings: given to this character where they are higher; this character's higher ones go
    // to the account's rows
    void Mirror(Player* player, PlayerState& st, QueryResult r)
    {
        std::map<uint32, int32> stored;
        if (r)
            do
            {
                Field* f = r->Fetch();
                stored[f[0].Get<uint32>()] = f[1].Get<int32>();
            } while (r->NextRow());
        ReputationMgr& rm = player->GetReputationMgr();
        st.mirroring = true;
        for (uint32 faction : cfg.warbandFactions)
        {
            FactionEntry const* entry = sFactionStore.LookupEntry(faction);
            if (!entry || !rm.GetState(entry))
                continue;
            if (rm.GetBaseReputation(entry) <= ReputationMgr::Reputation_Bottom)
                continue;   // the other side's faction: never for this character
            int32 const current = rm.GetReputation(entry);
            auto it = stored.find(faction);
            if (it != stored.end() && it->second > current)
            {
                rm.SetOneFactionReputation(entry, float(it->second), false);
                rm.SendState(rm.GetState(entry));
            }
            else if (it == stored.end() || current > it->second)
                st.warbandDirty[faction] = current;
        }
        st.mirroring = false;
    }

    void FlushWarband(Player* player)
    {
        if (!cfg.warband)
            return;
        PlayerState* st = State(player);
        if (!st || st->warbandDirty.empty())
            return;
        uint32 const account = player->GetSession() ? player->GetSession()->GetAccountId() : 0;
        if (!account)
            return;
        for (auto const& [faction, standing] : st->warbandDirty)
            CharacterDatabase.Execute("INSERT INTO `mod_temucraft_renown_warband` (`account`, `faction`, `standing`, `time`) VALUES ({}, {}, {}, {}) "
                "ON DUPLICATE KEY UPDATE `standing` = GREATEST(`standing`, VALUES(`standing`)), `time` = VALUES(`time`)", account, faction, standing, Now());
        st->warbandDirty.clear();
    }

    // after login: the granted levels are read, then the account's standings, then the levels are caught up
    void Loaded(Player* player)
    {
        PlayerState* st = State(player);
        if (!st)
            return;
        st->loaded = true;
        CatchUpAll(player, *st);
        if (st->helloPending)
        {
            st->helloPending = false;
            SendGranted(player, *st);
        }
    }

    void OnLogin(Player* player)
    {
        State(player, true);
        WorldSession* session = player->GetSession();
        if (!session)
            return;
        uint32 const account = session->GetAccountId();
        Read(session, Acore::StringFormat("SELECT `faction`, `level` FROM `mod_temucraft_renown_granted` WHERE `guid` = {}", player->GetGUID().GetCounter()),
            [account](Player* p, QueryResult r)
        {
            PlayerState* st = State(p);
            if (!st)
                return;
            if (r)
                do
                {
                    Field* f = r->Fetch();
                    st->granted[f[0].Get<uint32>()].insert(f[1].Get<uint8>());
                } while (r->NextRow());
            if (!cfg.warband || cfg.warbandFactions.empty())
            {
                Loaded(p);
                return;
            }
            Read(p->GetSession(), Acore::StringFormat("SELECT `faction`, `standing` FROM `mod_temucraft_renown_warband` WHERE `account` = {}", account),
                [](Player* p2, QueryResult r2)
            {
                if (PlayerState* st2 = State(p2))
                    Mirror(p2, *st2, r2);
                Loaded(p2);
            });
        });
    }
}

class TemucraftRenown_Player : public PlayerScript
{
public:
    TemucraftRenown_Player() : PlayerScript("TemucraftRenown_Player", {
        PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_LOGOUT, PLAYERHOOK_ON_SAVE, PLAYERHOOK_ON_DELETE, PLAYERHOOK_ON_REPUTATION_CHANGE,
        PLAYERHOOK_ON_BEFORE_BUY_ITEM_FROM_VENDOR, PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT }) { }

    void OnPlayerLogin(Player* player) override { OnLogin(player); }

    void OnPlayerLogout(Player* player) override
    {
        FlushWarband(player);
        DropState(player);
    }

    void OnPlayerSave(Player* player) override { FlushWarband(player); }

    void OnPlayerDelete(ObjectGuid guid, uint32 /*accountId*/) override
    {
        CharacterDatabase.Execute("DELETE FROM `mod_temucraft_renown_granted` WHERE `guid` = {}", guid.GetCounter());
    }

    // standing = the faction's new whole standing (the change is applied right after this)
    bool OnPlayerReputationChange(Player* player, uint32 factionID, int32& standing, bool /*incremental*/) override
    {
        if (!player)
            return true;
        PlayerState* st = State(player);
        if (!st || st->mirroring)
            return true;
        if (cfg.warband && cfg.warbandFactions.count(factionID))
        {
            auto it = st->warbandDirty.find(factionID);
            if (it == st->warbandDirty.end() || standing > it->second)
                st->warbandDirty[factionID] = standing;
        }
        if (!cfg.enable || !st->loaded)
            return true;
        auto d = GetData();
        auto t = d->tracks.find(factionID);
        if (t != d->tracks.end())
            CatchUp(player, *st, t->second, standing);
        return true;
    }

    // vendor unlocks: the item is only sold from its level on
    void OnPlayerBeforeBuyItemFromVendor(Player* player, ObjectGuid vendorguid, uint32 /*vendorslot*/, uint32& item, uint8 /*count*/, uint8 /*bag*/, uint8 /*slot*/) override
    {
        if (!cfg.enable || !player || !item)
            return;
        auto d = GetData();
        auto gate = d->vendorGates.find(item);
        if (gate == d->vendorGates.end())
            return;
        for (auto const& [faction, level] : gate->second)
        {
            auto t = d->tracks.find(faction);
            FactionEntry const* entry = sFactionStore.LookupEntry(faction);
            if (t == d->tracks.end() || !entry)
                continue;
            if (t->second.Reached(player->GetReputationMgr().GetReputation(entry)) >= level)
                continue;
            player->SendBuyError(BUY_ERR_REPUTATION_REQUIRE, player->GetNPCIfCanInteractWith(vendorguid, UNIT_NPC_FLAG_VENDOR), item, 0);
            ChatHandler(player->GetSession()).PSendSysMessage(Say(player, T_VENDOR_LOCKED, "You need {} {} with {} to buy this."),
                t->second.label, level, FactionName(entry, player->GetSession()));
            item = 0;   // the core stops the purchase
            return;
        }
    }

    // the client's requests
    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 language, std::string& msg, Player* receiver) override
    {
        std::string body;
        if (!TemucraftAddonParse(player, type, language, msg, receiver, PREFIX, body))
            return true;
        std::vector<std::string> f = TemucraftAddonFields(body);
        if (!f.empty() && f[0] == "HELLO")
            SendHello(player);
        return false;
    }
};

class TemucraftRenown_World : public WorldScript
{
public:
    TemucraftRenown_World() : WorldScript("TemucraftRenown_World", { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_STARTUP }) { }

    void OnAfterConfigLoad(bool /*reload*/) override { LoadSettings(); }

    void OnStartup() override
    {
        LoadSettings();   // again: the faction list needs the DBC stores, loaded after the first config load
        LoadTracks();
        auto d = GetData();
        LOG_INFO("server.loading", "{}: {} Renown tracks ({} levels), warband reputation {} ({} factions).", MODULE, d->tracks.size(), d->levels,
            cfg.warband ? "on" : "off", cfg.warbandFactions.size());
    }
};

class TemucraftRenown_Commands : public CommandScript
{
public:
    TemucraftRenown_Commands() : CommandScript("TemucraftRenown_Commands") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable root =
        {
            { "temucraft renown reload", HandleReload, 104301, Console::Yes },
        };
        return root;
    }

    // .temucraft renown reload: the tracks again, and the clients asked to fetch them
    static bool HandleReload(ChatHandler* handler)
    {
        LoadTracks();
        auto d = GetData();
        handler->SendSysMessage(Acore::StringFormat(Say(handler, T_RELOADED, "Renown: {} tracks, {} levels loaded."), d->tracks.size(), d->levels));
        for (auto const& [id, session] : sWorldSessionMgr->GetAllSessions())
            if (session)
                if (Player* p = session->GetPlayer())
                    if (p->IsInWorld())
                        TemucraftAddonSend(p, PREFIX, "R");
        return true;
    }
};

void AddTemucraftRenownScripts()
{
    new TemucraftRenown_Player();
    new TemucraftRenown_World();
    new TemucraftRenown_Commands();
}
