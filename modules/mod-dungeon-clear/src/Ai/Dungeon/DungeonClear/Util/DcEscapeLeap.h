/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DC_ESCAPE_LEAP_H
#define _PLAYERBOT_DC_ESCAPE_LEAP_H

#include <string_view>

// Stock escape LEAPS: movement spells that throw the caster a fixed distance
// directly away from its current target, with no look at what is behind it.
//
//   * mage `blink back` (CastBlinkBackAction, action name "blink") turns the mage
//     180 degrees off its target and blinks 20yd;
//   * hunter `disengage` faces the target and leaps backwards.
//
// Both answer "enemy too close", which is permanently true for ranged at a camp
// fight: the tank drags the pack onto the party. Out in the world that is a kite;
// in a dungeon or raid it launches the bot into the next pack, and the healers'
// reposition chases it there (tr-20260926-151320-21, Karazhan banquet hall: six
// elites pulled, thirty-six on the party). No player uses them that way in group
// content, so an active DC run bans them outright.
namespace DcEscapeLeap
{
    inline bool IsBanned(std::string_view actionName)
    {
        return actionName == "blink" || actionName == "blink back" ||
               actionName == "disengage";
    }
}

#endif
