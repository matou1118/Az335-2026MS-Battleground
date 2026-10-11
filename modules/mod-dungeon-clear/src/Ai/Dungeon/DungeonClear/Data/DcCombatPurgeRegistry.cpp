/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DcCombatPurgeRegistry.h"

namespace
{
    // ---- the table ------------------------------------------------------
    //
    // Gundrak (604) — Drakkari Raider (29982).
    //
    // It has no `creature` row anywhere in the world DB. Every raider that
    // exists in the instance is a VEHICLE PASSENGER: vehicle_template_accessory
    // seats three of them on the Drakkari Rhino (29931, seats 0/1/2), and map 604
    // has exactly one such rhino, spawned at (1865.1, 742.8, 136.4) in the
    // entrance hall.
    //
    // The rhino's SmartAI then drives them off a drop, literally:
    //
    //   29931  e:38  (data set 0 2)             -> a:232 start waypoint path 1272070
    //   29931  e:108 (path 1272070 point 3)     -> a:101 set home position
    //                                           -> a:223 Do Action 150 / 151 / 152
    //   29982  e:72  (on action 150/151/152)    -> a:203 EXIT VEHICLE
    //                                           -> a:101 SET HOME POSITION
    //
    // and waypoint path 1272070 is three points:
    //
    //   1  (1844.9, 743.2, 136.4)
    //   2  (1817.4, 743.2, 120.4)   <- a SIXTEEN-YARD drop
    //   3  (1777.3, 743.7, 119.9)   <- the raiders are ejected HERE
    //
    // So the three raiders are dismounted at the bottom of the drop, in the
    // water, already aggroed on whoever the rhino charged on the way down — and
    // their own next SmartAI line SETS THEIR HOME POSITION where they land. That
    // last action is what makes this permanent rather than merely awkward: evade
    // has nowhere to walk them back to.
    //
    // Every property that normally ends a fight is missing at once. They are
    // alive, they are hostile, they hold a CombatReference on the party, they are
    // off the navmesh the party can reach, instanced creatures never leash, and
    // their home is the hole they are standing in. The party is left flagged in
    // combat with nothing killable: the pull FSM never retires its phase, the
    // rest gates can never eat or drink, and the clear stops. Reported by the
    // operator as the common Gundrak deadlock, and the reason this table exists.
    //
    // NOT LISTED — 30934, "Drakkari Raider (1)". Same display name, but no
    // vehicle_template_accessory row, no `creature` row, and nothing on map 604
    // references it: a duplicate template, not a second stranding mob. Rows here
    // are claims about a MECHANISM, never about a name.
    // Gundrak (604) — Slad'ran Viper (29680) and Slad'ran Constrictor (29713).
    //
    // Neither has a `creature` row on any map: every one that exists is summoned
    // by boss_slad_ran, two at 90% health and three more at 50/75%, with
    //
    //     me->SummonCreature(..., TEMPSUMMON_CORPSE_TIMED_DESPAWN, 20s)
    //
    // — a despawn type whose timer starts at the CORPSE. A snake that is never
    // killed is never scheduled to despawn at all.
    //
    // The other half is in BossAI. `summons.DespawnAll()` lives in _Reset() and
    // _JustDied(); `BossAI::_EnterEvadeMode` has neither — it sets the boss state
    // back to NOT_STARTED and nothing else. So a party that WIPES on Slad'ran
    // leaves the snakes behind: the boss evades, and every snake it summoned stays
    // alive at full health, holds its CombatReference on whoever survived, and
    // evades in place. Measured on tp-20260830-231921-1: 486 references on a
    // single survivor, ten minutes after the wipe, every holder EVADING at 100%.
    //
    // Evading-only, and the distinction matters more here than anywhere else in
    // this table. These snakes are a real mechanic in a real fight — 44 of the 50
    // runs in that plan killed Slad'ran with them up — and the purge clock is
    // combat-blind, so any boss fight that runs past UnreachableCombatPurgeSecs
    // looks identical to a freeze from where it stands. An Always row here would
    // despawn the adds mid-encounter in the runs that were WINNING. Evading is the
    // state that only happens once the fight is already over.
    //
    // The Oculus (578) — Azure Ring Guardian (27638) and Greater Ley-Whelp (28276).
    //
    // Both FLY, and nothing on foot can reach them. The Guardians are 43 static
    // pickets hovering between the rings (wander 5, detection 20): one that
    // notices a party on an island's rim flags it into combat from twenty yards
    // off the edge and hovers there, holding it for ever — no rest, no muster,
    // and the driver refuses to lift off while anyone is engaged. Always: the
    // drakes that could shoot it down are exactly what that flag grounds.
    //
    // The whelps are Eregos's adds (DoZoneInCombat 300) and are fought from the
    // drakes during his encounter. Evading only, for the Slad'ran reason: an
    // Always row would despawn live adds out of a fight the riders are winning;
    // an evading whelp is one left over after it.
    DcCombatPurgeRow const kRows[] =
    {
        { 578, 27638, DcCombatPurgeWhen::Always },   // The Oculus — Azure Ring Guardian (flying picket, unreachable on foot)
        { 578, 28276, DcCombatPurgeWhen::Evading },  // The Oculus — Greater Ley-Whelp (Eregos add, outlives his evade)
        { 604, 29982, DcCombatPurgeWhen::Always },   // Gundrak — Drakkari Raider (rhino passenger, ejected into the water at path point 3)
        { 604, 29680, DcCombatPurgeWhen::Evading },  // Gundrak — Slad'ran Viper (boss summon, outlives the boss's evade)
        { 604, 29713, DcCombatPurgeWhen::Evading },  // Gundrak — Slad'ran Constrictor (boss summon, outlives the boss's evade)
    };
}

bool DcCombatPurgeRegistry::IsPurgeable(uint32 mapId, uint32 entry, bool isEvading)
{
    for (DcCombatPurgeRow const& r : kRows)
        if (r.mapId == mapId && r.entry == entry)
            return r.when == DcCombatPurgeWhen::Always || isEvading;
    return false;
}

bool DcCombatPurgeRegistry::HasRowsFor(uint32 mapId)
{
    for (DcCombatPurgeRow const& r : kRows)
        if (r.mapId == mapId)
            return true;
    return false;
}
