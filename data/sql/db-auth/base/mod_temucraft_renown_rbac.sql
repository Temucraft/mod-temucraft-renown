-- mod-temucraft-renown: its command (under .temucraft) as RBAC permission 104301 (range 104301-104349). Admin only by
-- default: linked to the administrator roles 192 and 1014. Grant it per account with .rbac account grant <account> 104301.
-- Only adds rows (no DELETE): run again, it changes nothing.
INSERT IGNORE INTO `rbac_permissions` (`id`, `name`) VALUES
(104301, 'Command: temucraft renown reload');
INSERT IGNORE INTO `rbac_linked_permissions` (`id`, `linkedId`) VALUES
(192, 104301),
(1014, 104301);
