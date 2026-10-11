-- Orb of Naxxramas does not exist in classic
UPDATE `gameobject` SET `spawnMask` = 3 WHERE `id` IN (202278, 202277);

-- Four Horseman chest
DELETE FROM `gameobject_template` WHERE `entry` = 361000;
INSERT INTO `gameobject_template` (`entry`, `type`, `displayId`, `name`, `IconName`, `castBarCaption`, `unk1`, `size`, `Data0`, `Data1`, `Data2`, `Data3`, `Data4`, `Data5`, `Data6`, `Data7`, `Data8`, `Data9`, `Data10`, `Data11`, `Data12`, `Data13`, `Data14`, `Data15`, `Data16`, `Data17`, `Data18`, `Data19`, `Data20`, `Data21`, `Data22`, `Data23`, `AIName`, `ScriptName`, `VerifiedBuild`) VALUES
(361000, 3, 1387, 'Four Horsemen Chest', '', '', '', 1, 1634, 361000, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, '', '', 1);

DELETE FROM `reference_loot_template` WHERE `entry` = 314800;
INSERT INTO `reference_loot_template` (`Entry`, `Item`, `Reference`, `Chance`, `QuestRequired`, `GroupId`, `MinCount`, `MaxCount`) VALUES
(314800, 22349, 0, 0.0, 0, 1, 1, 1),
(314800, 22350, 0, 0.0, 0, 1, 1, 1),
(314800, 22351, 0, 0.0, 0, 1, 1, 1);

DELETE FROM `gameobject_loot_template` WHERE `entry` = 361000;
INSERT INTO `gameobject_loot_template` (`Entry`, `Item`, `Reference`, `Chance`, `QuestRequired`, `GroupId`, `MinCount`, `MaxCount`) VALUES
(361000, 314800, 314800, 100, 0, 0, 2, 2),
(361000, 22691, 0, 0, 0, 2, 1, 1),
(361000, 22726, 0, 30, 0, 0, 1, 1),
(361000, 22809, 0, 0, 0, 2, 1, 1),
(361000, 22811, 0, 0, 0, 2, 1, 1),
(361000, 23025, 0, 0, 0, 2, 1, 1),
(361000, 23027, 0, 0, 0, 2, 1, 1),
(361000, 23071, 0, 0, 0, 2, 1, 1);

-- Add Entrance transporter object. Necromantic Runestone (id: 189314, displayID: 7786)
SET @TRANSPORTER_ENTRY    := 361001;
SET @TRANSPORTER_COOLDOWN := 5;
SET @TRANSPORTER_X        := 3123.26;
SET @TRANSPORTER_Y        := -3869.36;
SET @TRANSPORTER_Z        := 138.34;
SET @TRANSPORTER_O        := 0.2175;

SET @OGUID    := 361000;

DELETE FROM `gameobject_template` WHERE `entry` = @TRANSPORTER_ENTRY;
INSERT INTO `gameobject_template` (`entry`, `type`, `displayId`, `name`, `IconName`, `castBarCaption`, `unk1`, `size`, `Data4`, `Data11`, `AIName`, `ScriptName`, `VerifiedBuild`) VALUES
(@TRANSPORTER_ENTRY, 10, 7786, 'Teleport To Naxxramas', '', '', '', 1, @TRANSPORTER_COOLDOWN, 1, '', 'gobject_naxx40_tele', 12340);

DELETE FROM `gameobject` WHERE `map` = 0 AND `id` IN (@TRANSPORTER_ENTRY, 181056, 193166);
INSERT INTO `gameobject` (`guid`, `id`, `map`, `zoneId`, `areaId`, `spawnMask`, `phaseMask`, `position_x`, `position_y`, `position_z`, `orientation`, 
`rotation0`, `rotation1`, `rotation2`, `rotation3`, `spawntimesecs`, `animprogress`, `state`, `ScriptName`, `VerifiedBuild`) VALUES
--
(@OGUID+1, @TRANSPORTER_ENTRY, 0, 0, 0, 1, 1, @TRANSPORTER_X, @TRANSPORTER_Y, @TRANSPORTER_Z, @TRANSPORTER_O, 0, 0, -1, 0, 1, 0, 1, '', 0),
(@OGUID+2, 181056, 0, 0, 0, 1, 1, 3067.1255, -3533.4387, -331.89944, 0, 0, 0, -1, 0, 900, 100, 1, '', 0),
(@OGUID+3, 193166, 0, 0, 0, 1, 1, 3072.64, -3886.67, 130.449, 2.4011, 0.0, 0.0, -0.932239, -0.361844, 300, 0, 1, '', 0); -- Meeting Stone

DELETE FROM `gameobject_template_locale` WHERE `entry` = @TRANSPORTER_ENTRY;
INSERT INTO `gameobject_template_locale` (`entry`, `locale`, `name`, `castBarCaption`, `VerifiedBuild`) VALUES
(@TRANSPORTER_ENTRY, 'deDE', 'Teleporter nach Naxxramas', '', 0),
(@TRANSPORTER_ENTRY, 'esES', 'Teletransportador a Naxxramas', '', 0),
(@TRANSPORTER_ENTRY, 'esMX', 'Teletransportador a Naxxramas', '', 0),
(@TRANSPORTER_ENTRY, 'frFR', 'Téléporteur vers Naxxramas', '', 0);

DELETE FROM `transports` WHERE `guid` = 21 AND `entry` = 181056;
INSERT INTO `transports` (`guid`, `entry`, `name`, `ScriptName`) VALUES
(21, 181056, 'Naxxramas floating in Plaguewood - Eastern Plaguelands', '');

-- Set speed (Data1) and map (Data6)
UPDATE `gameobject_template` SET `Data1` = 1, `Data6` = 0 WHERE `entry` = 181056;

-- Update meeting stone requirements (previous: minLvl 15)
SET @MINLEVEL := 58;
DELETE FROM `gameobject_template` WHERE `entry` = 193166;
INSERT INTO `gameobject_template` (`entry`, `type`, `displayId`, `name`, `IconName`, `castBarCaption`, `unk1`, `size`, `Data0`, `Data1`, `Data2`, `AIName`, `ScriptName`, `VerifiedBuild`) VALUES
(193166, 23, 5492, 'Meeting Stone', '', '', '', 0.75, @MINLEVEL, 255, 3456, '', '', 12340);
