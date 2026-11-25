-- mod-temucraft-renown: Renown tracks. One row per level of a faction's track; level 0 (optional) is the track itself.
--   faction       Faction.dbc id (it must have a reputation)
--   level         1, 2, 3, ... (no gaps); 0 = the track: `title` is the word the bar shows ("Renown" when empty,
--                 "Level" for friendship-style tracks), `reputation` where the bar towards level 1 starts, `description`
--                 the Journeys page's overview
--   reputation    the whole standing needed, counted from 0 = Neutral (Friendly 3000, Honored 9000, Revered 21000,
--                 Exalted 42000, the cap 42999; Unfriendly -3000, Hostile -6000); never less than the level before
--   reward_type   none, item (reward_count of reward_id, mailed when the bags are full), spell (learned), title
--                 (CharTitles.dbc id), vendor (item reward_id can only be bought, from any vendor, from this level on)
--   highlight     1 = a "Journey Highlights" card on the Journeys page (when no level of the track has it, every
--                 level with a title is a card)
--   title         the reward's / the card's name; description: the card's text
-- Rewards are granted once per character (mod_temucraft_renown_granted, characters DB). `.temucraft renown reload`
-- reads this table again.
CREATE TABLE IF NOT EXISTS `mod_temucraft_renown_track` (
    `faction`      INT UNSIGNED NOT NULL,
    `level`        TINYINT UNSIGNED NOT NULL,
    `reputation`   INT NOT NULL DEFAULT 0,
    `reward_type`  ENUM('none', 'item', 'spell', 'title', 'vendor') NOT NULL DEFAULT 'none',
    `reward_id`    INT UNSIGNED NOT NULL DEFAULT 0,
    `reward_count` SMALLINT UNSIGNED NOT NULL DEFAULT 1,
    `highlight`    TINYINT UNSIGNED NOT NULL DEFAULT 0,
    `title`        VARCHAR(100) NOT NULL DEFAULT '',
    `description`  VARCHAR(255) NOT NULL DEFAULT '',
    PRIMARY KEY (`faction`, `level`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- EXAMPLE TRACKS (for Ryan to replace): three stock WotLK factions, stock rewards only (items 43154, 43155, 44723
-- and spell 61357 are in the core's item_template; title 131 is the stock "the Argent Champion" of achievement 945).
DELETE FROM `mod_temucraft_renown_track` WHERE `faction` IN (1106, 1098, 1073);
INSERT INTO `mod_temucraft_renown_track` (`faction`, `level`, `reputation`, `reward_type`, `reward_id`, `reward_count`, `highlight`, `title`, `description`) VALUES
-- Argent Crusade
(1106, 0, 0,     'none',  0,     1, 0, 'Renown', 'EXAMPLE track. The Argent Crusade gathers every hand willing to stand against the Scourge.'),
(1106, 1, 3000,  'none',  0,     1, 1, 'Friend of the Crusade', 'The Crusade knows your name.'),
(1106, 2, 9000,  'item',  43154, 1, 1, 'Tabard of the Argent Crusade', 'Wear the colours of the Crusade.'),
(1106, 3, 21000, 'none',  0,     1, 0, 'Trusted Crusader', 'Tirion''s officers trust you with their plans.'),
(1106, 4, 42000, 'title', 131,   1, 1, 'the Argent Champion', 'The title of the Crusade''s champions.'),
-- Knights of the Ebon Blade
(1098, 0, 0,     'none',  0,     1, 0, 'Renown', 'EXAMPLE track. The Knights of the Ebon Blade hunt the Lich King from Acherus.'),
(1098, 1, 3000,  'none',  0,     1, 0, 'Ally of Acherus', 'The death knights let you into the Shadow Vault.'),
(1098, 2, 9000,  'item',  43155, 1, 1, 'Tabard of the Ebon Blade', 'The colours of the Ebon Blade.'),
(1098, 3, 21000, 'none',  0,     1, 0, 'Shadow Vault Regular', 'The quartermaster keeps her best for you.'),
(1098, 4, 42000, 'none',  0,     1, 1, 'Hero of the Ebon Blade', 'The Ebon Blade counts you among its own.'),
-- The Kalu'ak (a friendship-style track: the bar says "Level N")
(1073, 0, 0,     'none',   0,     1, 0, 'Level', 'EXAMPLE track. The tuskarr of the Kalu''ak welcome friends at their fishing villages.'),
(1073, 1, 3000,  'none',   0,     1, 0, 'Fishing Friend', 'The tuskarr share their catch with you.'),
(1073, 2, 9000,  'none',   0,     1, 0, 'Honoured Guest', 'You are welcome at every Kalu''ak fire.'),
(1073, 3, 21000, 'vendor', 44723, 1, 1, 'Nurtured Penguin Egg', 'Tanaika and Sairuk sell you the egg from Level 3 on.'),
(1073, 4, 42000, 'spell',  61357, 1, 1, 'Pengu', 'Pengu waddles after you.');
