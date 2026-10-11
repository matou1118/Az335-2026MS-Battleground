/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonWingRegistry.h"

#include <unordered_map>

// Blackrock Spire (map 229) is two dungeons on one map: Lower (LFG 32, ends at
// Overlord Wyrmthalak) and Upper (LFG 44, Pyroguard Emberseer -> General
// Drakkisath). This file is its definition unit. Today that is only the wing
// split; the UBRS encounter gates (Dragonspine Door, hall rune packs, the
// Blackrock Altar, the Rend/Gyth stadium) land here as event rows and one map-229
// roster patch when UBRS support is built. The TU stays linked because
// DungeonWingRegistry's aggregator calls RegisterBlackrockSpireWings explicitly.

// --- wing layout ---------------------------------------------------------
void RegisterBlackrockSpireWings(std::unordered_map<uint32, DungeonWingLayout>& store)
{
    // --- Blackrock Spire (map 229) -------------------------------
    // Both wings are entered through the SAME portal and are stacked: from the
    // portal (78.5, -225.0, 49.8) the nearest bosses are UBRS's (Emberseer 87yd,
    // Drakkisath 96yd; Omokk, LBRS's first, is 128yd). Nearest-boss detection
    // would put an LBRS party on the UBRS list at the door, and the last brs test
    // runs show the other half of the problem: clear LBRS, then walk back up to
    // the Dragonspine Door for Emberseer. So the wing is chosen per RUN
    // (WingSelect::Explicit, see DcRunWing), defaulting to LBRS.
    //
    // Encounter bits from DungeonEncounter.dbc, verified against acore_world
    // (blackrock_spire.h's DATA_* indices diverge from bit 10 on — use these):
    //   LBRS bits 0-8: Omokk, Vosh'gajin, Voone, Smolderweb, Urok (GO summon),
    //                  Zigris, Gizrul (spawns on Halycon's death), Halycon,
    //                  Wyrmthalak (lastEncounterDungeon = 32)
    //   UBRS bits 9-13: Emberseer, Solakar (Father Flame summon), Rend, The
    //                  Beast, Drakkisath (lastEncounterDungeon = 44)
    // Urok, Gizrul and Solakar have no static spawn, so BossSpawnIndex never
    // lists them; they are here so credit and counting still resolve.
    store[229] = {true, {
        {"Blackrock Spire (Lower)", {
            9196,   // Highlord Omokk
            9236,   // Shadow Hunter Vosh'gajin
            9237,   // War Master Voone
            10596,  // Mother Smolderweb
            10584,  // Urok Doomhowl
            9736,   // Quartermaster Zigris
            10268,  // Gizrul the Slavener
            10220,  // Halycon
            9568,   // Overlord Wyrmthalak
        }, "lbrs", /*lfgDungeonId*/ 32, /*terminalBossEntry*/ 9568, /*encounterMask*/ 0x01FFu},
        {"Blackrock Spire (Upper)", {
            9816,   // Pyroguard Emberseer
            10264,  // Solakar Flamewreath
            10429,  // Warchief Rend Blackhand
            10430,  // The Beast
            10363,  // General Drakkisath
        }, "ubrs", /*lfgDungeonId*/ 44, /*terminalBossEntry*/ 10363, /*encounterMask*/ 0x3E00u},
    }, WingSelect::Explicit, /*defaultWing*/ "lbrs"};
}
