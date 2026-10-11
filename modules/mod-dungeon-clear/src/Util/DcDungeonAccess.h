/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCDUNGEONACCESS_H
#define _PLAYERBOT_DCDUNGEONACCESS_H

#include <cstdint>
#include <string>

#include "DBCEnums.h"

class Player;

// Making a throwaway pool bot ELIGIBLE to be teleported into a dungeon it was
// never played through.
//
// Player::TeleportTo refuses a far teleport in two places, and BOTH of them
// refuse SILENTLY — the call returns false, no packet the headless harness can
// see is produced, and the run simply times out on "tank did not arrive at the
// dungeon entrance" with the party still standing where it was provisioned:
//
//   * dungeon_access_requirements. MapMgr::PlayerCannotEnter ends in
//     Player::Satisfy(sObjectMgr->GetAccessRequirement(...)), which walks the
//     quest/item/achievement rows for the destination map. The three Frozen
//     Halls 5-mans chain off each other, so Pit of Saron (658) demands quest
//     24499/24511 "Echoes of Tortured Souls" and Halls of Reflection (668)
//     demands 24710/24712 "Deliverance from the Pit". A pool bot has done
//     neither, so those two dungeons could never be tested at all — the Forge
//     of Souls (632) has no quest row, which is exactly why it always worked.
//     TBC heroics gate the same way on their key achievements.
//
//   * The death-knight starting-zone lock. A death knight standing on
//     MAP_EBON_HOLD (609) cannot far-teleport anywhere until it knows Death
//     Gate (50977), the spell the Ebon Hold intro chain grants. A recycled
//     addclass death knight has neither the chain nor the spell, so it is
//     pinned to Acherus and every run that draws it as a member dies in setup.
//     This gate is checked BEFORE the access requirement and is not covered by
//     TELE_TO_GM_MODE, so it needs its own answer.
//
// The answer here is to SATISFY each gate rather than to bypass it: mark the
// required quests rewarded, hand over the required items and achievements,
// teach the death knight its own class spell. Every other core entry check —
// level, the instance-per-hour budget, a disabled map, an instance being reset
// — stays in force, which is what a teleport with TELE_TO_GM_MODE would have
// thrown away wholesale.
namespace DcDungeonAccess
{
    // Teach a death knight parked on Acherus its Death Gate so it can leave the
    // starting zone at all. True when the spell was actually granted.
    bool UnlockTravel(Player* bot);

    // UnlockTravel, then satisfy every unmet dungeon_access_requirements row
    // for `mapId` at `difficulty` that applies to this bot's faction. Returns a
    // comma-separated summary of what had to be granted, empty when the bot
    // already qualified.
    std::string GrantEntry(Player* bot, std::uint32_t mapId, Difficulty difficulty);
}

#endif  // _PLAYERBOT_DCDUNGEONACCESS_H
