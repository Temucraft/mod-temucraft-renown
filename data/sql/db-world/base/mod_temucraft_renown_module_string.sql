-- mod-temucraft-renown: player-facing texts. Translations go in module_string_locale (same module and id, plus the locale).
DELETE FROM `module_string` WHERE `module` = 'mod-temucraft-renown';
INSERT INTO `module_string` (`module`, `id`, `string`) VALUES
('mod-temucraft-renown', 1, '|cff66bbffRenown:|r {} - {} {} reached.'),
('mod-temucraft-renown', 2, '|cff66bbffReward:|r {}'),
('mod-temucraft-renown', 3, 'Your bags were full: your Renown reward is in your mailbox.'),
('mod-temucraft-renown', 4, 'Renown reward'),
('mod-temucraft-renown', 5, 'Your reward for {}.'),
('mod-temucraft-renown', 6, 'Renown: {} tracks, {} levels loaded.'),
('mod-temucraft-renown', 7, 'You need {} {} with {} to buy this.');
