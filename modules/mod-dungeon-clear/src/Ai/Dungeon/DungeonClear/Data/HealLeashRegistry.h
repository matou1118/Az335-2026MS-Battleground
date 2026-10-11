/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_HEALLEASHREGISTRY_H
#define _PLAYERBOT_HEALLEASHREGISTRY_H

#include "Define.h"

// Static registry of maps where a healer's LOS reposition (DungeonClearHealReposition)
// is LEASHED to the advanced-pull camp while it fights there.
//
// Everywhere else the reposition is deliberately unleashed: it chases the most-hurt
// member LOS-blind up to HealRepositionMaxRange, which is what makes it recover a
// tank dragged around a corner. That is the right call in a corridor dungeon and the
// wrong one in a room of parked casters. Karazhan's Banquet Hall is the case
// (tr-20260926-192642-1): Phantom Guests park in ranged mode ~25yd out on the
// ballroom floor, melee DPS follow them up, and both healers chased those DPS ~35-45yd
// off the camp onto the floor — where they proximity-aggroed 13 idle mobs sitting on
// their spawn points (two Phantom Attendants, the Retainer, a Valet, six Guests),
// each with a healer as the nearest member.
//
// On a leashed map the healer only accepts a standoff point within `radius` of the
// camp. When none exists it stays put and heals what it can see from the camp — the
// DPS that ran out has to come back to it, not the other way round.
struct HealLeashRow
{
    uint32 mapId{0};
    float  radius{0.0f};  // yd, 2D, measured from the pull camp
};

class HealLeashRegistry
{
public:
    // Leash radius for this map, or 0 when the map is not leashed. Pure; linear scan
    // over a tiny table.
    static float Radius(uint32 mapId);

    // Is (x, y) inside a leash of `radius` around (campX, campY)? 2D — the camp and
    // the healer can sit on different storeys of the same ramp. A radius <= 0 means
    // unleashed and always passes.
    static bool WithinLeash(float campX, float campY, float radius, float x, float y);
};

#endif  // _PLAYERBOT_HEALLEASHREGISTRY_H
