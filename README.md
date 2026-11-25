# mod-temucraft-renown

Renown and account-wide ("Warband") reputation for Temucraft. The client half is `TemucraftReputation.lua` (the
reputation window: "Renown N" / "Level N" bars, "Warband Reputation" tooltips, View Renown) and `TemucraftRenown.lua`
(the Journeys page, `/renown`) in [mod-temucraft-client-version](https://github.com/Temucraft/mod-temucraft-client-version).

- **Renown tracks** (world DB, `mod_temucraft_renown_track`): levels of a faction's reputation, each at a whole
  standing (counted from 0 = Neutral: Friendly 3000, Honored 9000, Revered 21000, Exalted 42000) with a reward:
  - `item`: `reward_count` of item `reward_id`, mailed when the bags are full;
  - `spell`: learned (mounts and companions work as spells);
  - `title`: a CharTitles.dbc id;
  - `vendor`: item `reward_id` can only be bought (from any vendor that sells it) from that level on; the vendor
    still lists it, buying it before says "You need Renown N with ...";
  - `none`.

  Level 0 (optional) describes the track: its `title` is the word the bar shows (`Renown` by default, `Level` for
  friendship-style tracks), its `reputation` where the bar towards level 1 starts, its `description` the Journeys
  page's overview. `highlight = 1` makes a level a "Journey Highlights" card. Every level reached is granted once per
  character (characters DB, `mod_temucraft_renown_granted`): when the reputation changes, and at login (so levels
  added later, or reached while the module was off, catch up). The client is told and refreshes its window.
- **Warband reputation** (off by default): for the factions in `Temucraft.Renown.Warband.Factions` the best
  standing any character of the account reached is kept (characters DB, `mod_temucraft_renown_warband`, written
  when the character is saved and at logout) and given to the account's other characters when they next log in.
  It is never lowered; a faction the character's side can never have (base standing Hated: the other side's
  capitals) is skipped; spillover to other factions doesn't happen. Neutral factions are the ones to list.

The SQL comes with **example tracks** for the Argent Crusade, the Knights of the Ebon Blade and the Kalu'ak
(stock rewards only: the two tabards, the Nurtured Penguin Egg as a vendor unlock, Pengu, the title "the Argent
Champion"), clearly marked: replace them with the real ones.

Reads at login are asynchronous queries answered on the player's session; writes are asynchronous.

## Command

`.temucraft renown reload` reads the tracks again and has every online client fetch them. RBAC permission 104301
(range 104301-104349), linked to the administrator roles 192 and 1014; give it to others with
`.rbac account grant <account> 104301`. Levels newly reached by a reload are granted at the player's next
reputation change or login.

## Install

1. Put this folder in `modules/` of your AzerothCore source, re-run CMake and build.
2. The SQL in `data/sql/` is applied by the worldserver on its next start: the track table with the examples and
   the texts (world), the two tables (characters), the RBAC permission (auth).
3. Copy `conf/mod-temucraft-renown.conf.dist` to `configs/modules/` as `mod-temucraft-renown.conf` to change the
   settings (`.reload config` applies them):

| Setting | Default | |
|---|---|---|
| `Temucraft.Renown.Enable` | 1 | Renown tracks |
| `Temucraft.Renown.Announce` | 1 | say in chat when a level is reached |
| `Temucraft.Renown.Warband.Enable` | 0 | account-wide reputation |
| `Temucraft.Renown.Warband.Factions` | "" | the shared factions, e.g. `1106,1098,1073` |

## The client's messages

Addon messages with the prefix `TRN`, whispered to oneself (`AddonChannel = 1`, the default). The client asks
`HELLO`; the answers (the settings, the tracks, the account-wide factions, every faction's name in the player's
locale, the levels the character got) and the server's own messages (`G` a level granted, `R` reloaded) are
described at the top of `src/TemucraftRenown.cpp`.

## Limits

- 3.3.5's interface has no faction ids, so the client matches factions by name; the server sends the names in the
  player's locale.
- Vendor unlocks don't hide the item in the vendor's list (the core has no hook for that); they stop the purchase.
- A reward lost later (an item deleted, a title removed) is not given again: each level is granted once.
- Warband standings are written at saves and logout: a crash loses what was gained since the last save.
