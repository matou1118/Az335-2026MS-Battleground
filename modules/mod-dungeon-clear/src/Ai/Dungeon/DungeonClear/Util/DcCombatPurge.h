/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCCOMBATPURGE_H
#define _PLAYERBOT_DCCOMBATPURGE_H

#include "Define.h"

class Player;

// THE UNENDABLE FIGHT: break a run out of combat with a mob that can never be
// killed and can never let go.
//
// The shape, which the module has now hit on four maps: a creature ends up alive
// and hostile somewhere the party cannot reach, holding a CombatReference on it.
// Instanced creatures never leash (ac-instanced-creatures-dont-leash), and a mob
// that has re-homed where it stands has nowhere to evade back to, so nothing in
// the core ever ends that fight. The party is flagged in combat with nothing
// killable: the pull FSM cannot retire its phase, the rest gates cannot eat or
// drink (nobody eats while flagged), the between-pulls gates never open, and the
// clear stops where it is until the harness times the run out.
//
// Gundrak's Drakkari Raiders are the case this was written for — SmartAI ejects
// three of them off a sixteen-yard drop into water, mid-charge, and then has them
// set their home position where they land. See DcCombatPurgeRegistry for the
// world-data derivation.
//
// TWO HALVES, AND NEITHER IS A REACHABILITY TEST.
//
//   WHICH creature — DcCombatPurgeRegistry, a hand-vetted (mapId, entry) table.
//   WHEN          — this unit's combat-blind no-progress clock.
//
// The reachability test is conspicuously absent on purpose. The module's earlier
// stuck-combat hatch was guarded on navmesh reachability twice, and twice the
// guard is what broke it: `IsReachable` is the CHUNKED pathfinder and it called a
// 347yd holder through solid rock "reachable", so the hatch stood down forever in
// exactly the state it existed for (dc-phantom-combat-recovery,
// dc-scancombatholders-reachable-is-chunked). Asking the navmesh at runtime has
// been measured and it does not work. Asking the world data ONCE, by hand, in a
// table, does — and it costs a table row per case instead of a livelock per run.
//
// WHY THE CLOCK MUST BE COMBAT-BLIND. The stranded-member failsafe re-arms its
// clock on party engagement, because a fight is progress and it must never yank a
// bot out of one. Reused here that rule inverts: the subject IS a fight, held
// open forever, so an engagement-armed clock could never run out and the purge
// could never fire. DcRunProgress therefore keeps combat out of the shared
// detector and each failsafe adds back only what it needs — the stranded one
// adds engagement, this one adds nothing.
//
// WHY DROPPING COMBAT ALONE IS NOT ENOUGH. `CombatStop` leaves `current target`
// set (dc-relocation-must-drop-the-target) and the clear's own pickers will
// happily re-select the same mob the next tick, walk at water they cannot cross,
// and re-open the fight — a revolving door that purges every window forever. So a
// purge also drops the stale targets and arms a short TARGET BAR on the purged
// entry, which DcTargetExclusionRegistry reads (that registry is the one seam
// that reaches the stock combat engine's own target selection).
namespace DcCombatPurge
{
    // Global-tick sweep: for every bot running a clear, keep its run's
    // combat-blind no-progress clock and purge registered holders once that clock
    // goes stale. Cheap no-op when no run is active or the map has no rows.
    // Driven from DungeonClearReaperScript::OnUpdate rather than a
    // trigger, because the deadlock parks bots on the COMBAT engine, where the
    // clear's non-combat triggers never get a tick — the same reason the status
    // pusher and the ZF stray-summon despawner live on the global tick.
    void Tick(uint32 diff);

    // Drop every registered stranding holder off this party's combat, clear the
    // stale targets pointing at them, and arm the target bar. Returns how many
    // creatures were dropped. Safe to call on any bot; walks the caller's own
    // same-map group.
    uint32 Purge(Player* leader);

    // Is `entry` under a post-purge target bar for this bot's instance right now?
    // Read by DcTargetExclusionRegistry, which is what carries the answer into the
    // stock combat engine's target selection.
    bool IsBarred(Player* bot, uint32 entry);

    // Drop every bar for an instance. Called when a run ends so a fresh run on
    // the same instance id never inherits a stale bar.
    void ClearBars(Player* bot);
}

#endif  // _PLAYERBOT_DCCOMBATPURGE_H
