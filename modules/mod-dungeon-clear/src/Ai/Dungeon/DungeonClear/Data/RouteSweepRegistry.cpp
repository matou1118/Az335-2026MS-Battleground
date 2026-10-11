/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "RouteSweepRegistry.h"

namespace
{
    // The maps, each earned by its own run data. Measured off acore_world.creature.
    //
    //   34 — Stormwind Stockade (issue #17). The reference geometry, and about as
    //   pure a case as the failure has. The first boss by DBC order (Targorr) sits
    //   105yd dead ahead of the entrance up the central axis, so the opening leg of
    //   the run is a straight 105yd corridor. Its cells flank that axis at 10-21yd
    //   against ~29yd of real aggro reach, on BOTH sides — so every fight spot in
    //   the hall is inside two neighbours at once, and there is no line to detour
    //   to. Every trash mob is rank-1 elite at lvl 23-25, which is why the Dynamic
    //   ceiling (thirds-of-an-elite against PullDynamicMaxLeeroyMobs * 3) reads
    //   comfortably safe while the room is not: live run tr-20260809-201248-2
    //   classified 35 LEEROY to 4 ADVANCED, and its one sized pull predicted 3 mobs
    //   and fought 7.
    //
    //   532 — Karazhan (tr-20260927-003140-1 and both siblings of its plan). The
    //   Grand Ballroom is a field of Phantom Guest formations 30-40yd apart, each
    //   member with ~22yd of aggro reach and a 10yd assistance chain, and the
    //   Moroes route runs straight through the middle of it. The corridor band
    //   (half-width 18yd) walked the tank past a cluster 20yd beside route vertex
    //   23 without ever picking it; the cluster picked the tank at 18yd instead,
    //   with no verdict and no camp, and the walk-in engage fought it in place —
    //   which on that floor means the room. The Banquet Hall next door is the
    //   same geometry with Moroes' dinner guests in it, and both sibling runs
    //   lost him to a trash fight no pull was in flight for. 10-man normal only:
    //   EnRouteSweepApplies excludes heroic, and Karazhan has none.
    uint32 const kSweepMaps[] =
    {
        34,     // The Stockade
        532,    // Karazhan
    };
}

bool RouteSweepRegistry::SweepsRoute(uint32 mapId)
{
    for (uint32 const id : kSweepMaps)
        if (id == mapId)
            return true;
    return false;
}
