/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "HealLeashRegistry.h"

#include <cmath>

namespace
{
    // Karazhan (532). The radius is sized off the Banquet Hall pull in
    // tr-20260926-192642-1: the camp sat at (-11012.0,-1964.2) on the z~68 landing,
    // and the healers that aggroed the floor were ~35-45yd out from it. The nearest
    // idle floor pack spawns ~40yd from that camp (Phantom Guest at -10972.7,-1969.2),
    // so 15yd keeps a healer on the landing with 25yd of margin while still letting
    // it step round the landing's own corners for LOS to the tank.
    HealLeashRow const kRows[] =
    {
        { 532, 15.0f },  // Karazhan — Banquet Hall parked casters
    };
}

float HealLeashRegistry::Radius(uint32 mapId)
{
    for (HealLeashRow const& r : kRows)
        if (r.mapId == mapId)
            return r.radius;
    return 0.0f;
}

bool HealLeashRegistry::WithinLeash(float campX, float campY, float radius, float x, float y)
{
    if (!(radius > 0.0f))
        return true;
    float const dx = x - campX;
    float const dy = y - campY;
    return std::sqrt(dx * dx + dy * dy) <= radius;
}
