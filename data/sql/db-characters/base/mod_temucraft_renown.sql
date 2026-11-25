-- mod-temucraft-renown: the Renown levels each character got (granted once), and the account-wide ("Warband")
-- standings: the best standing any character of the account reached in a faction of Temucraft.Renown.Warband.Factions.
CREATE TABLE IF NOT EXISTS `mod_temucraft_renown_granted` (
    `guid`    INT UNSIGNED NOT NULL,
    `faction` INT UNSIGNED NOT NULL,
    `level`   TINYINT UNSIGNED NOT NULL,
    `time`    INT UNSIGNED NOT NULL DEFAULT 0,
    PRIMARY KEY (`guid`, `faction`, `level`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS `mod_temucraft_renown_warband` (
    `account`  INT UNSIGNED NOT NULL,
    `faction`  INT UNSIGNED NOT NULL,
    `standing` INT NOT NULL DEFAULT 0,
    `time`     INT UNSIGNED NOT NULL DEFAULT 0,
    PRIMARY KEY (`account`, `faction`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
