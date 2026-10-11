/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCCOMBATPURGEREGISTRY_H
#define _PLAYERBOT_DCCOMBATPURGEREGISTRY_H

#include "Define.h"

// Static registry of creatures whose combat with the party may be DROPPED once
// the run has stopped progressing.
//
// The fourth member of the "do not fight that" family, and the only one that
// acts on a fight already under way:
//
//   * FightInPlaceRegistry / BossPullbackRegistry — WHERE do we fight it.
//   * DcNeverTargetRegistry   — is killing it PROGRESS? (a permanent fact; it
//                               filters the clear's own scans and nothing else)
//   * DcTargetExclusionRegistry — will killing it RIGHT NOW lose the run?
//   * this one                — it has us in combat and that fight can never
//                               END, so the run cannot continue until the
//                               reference is broken by hand.
//
// The bar is a MECHANISM, not an annoyance: a listed creature must be able to
// reach a state in which it is simultaneously alive, hostile, holding a
// CombatReference on a party member, and permanently unable to be reached or to
// evade. Nothing in the core ends that fight — instanced creatures never leash,
// and a creature that has re-homed where it stands has no home to walk back to —
// so the party stays flagged for the rest of the run: the pull FSM cannot retire
// its phase, the rest gates cannot eat or drink, and the clear stops where it is.
//
// This table is deliberately NOT a reachability test. Two live post-mortems
// (dc-phantom-combat-recovery, dc-scancombatholders-reachable-is-chunked) end the
// same way: every navmesh-reachability guard the stuck-combat hatch was given
// answered "reachable" for a holder hundreds of yards away through solid rock,
// and the hatch went inert in precisely the case it was written for. So the
// judgement is made HERE, once, by hand, against the world data — and the
// runtime half asks only "has the run stopped moving".
//
// Keep it small and keep the evidence in the table. A row is a claim about a
// specific creature's spawn/eject mechanics on a specific map, and it should
// read like one.
// WHEN a listed creature may be dropped. The table above says a row is a claim
// about a mechanism; this says how much of that creature's life the claim covers.
//
//   Always — the creature is a stranding bug BY CONSTRUCTION. There is no state
//            in which fighting it is legitimate, so the no-progress clock alone
//            is enough. The Drakkari Raider is ejected off a cliff into water
//            before the party ever sees it.
//
//   Evading — the creature is a LEGITIMATE opponent that the party is supposed
//            to fight and kill, and only becomes unendable after it has given
//            up. Slad'ran's snakes are the case: they are a real part of a real
//            encounter, and dropping them while that encounter is live would
//            disarm the fight the party is winning. The purge clock cannot tell
//            those apart on its own — it is combat-blind by design
//            (DcRunProgress), so a boss fight longer than
//            UnreachableCombatPurgeSecs reads exactly like a freeze. This gate
//            is what keeps the table from breaking the fights it is meant to
//            unstick: an evading creature at full health is never part of one.
enum class DcCombatPurgeWhen : uint8
{
    Always,
    Evading
};

struct DcCombatPurgeRow
{
    uint32 mapId{0};
    uint32 entry{0};   // the NORMAL creature entry; heroic spawns keep it
                       // (Creature::InitEntry swaps m_creatureInfo, not the entry)
    DcCombatPurgeWhen when{DcCombatPurgeWhen::Always};
};

class DcCombatPurgeRegistry
{
public:
    // May combat with `entry` on `mapId` be dropped when the run has stalled?
    // Pure (no game state) so it is unit-testable on its own. Linear scan; the
    // table is tiny.
    //
    // `isEvading` is the caller's read of the live creature
    // (CombatManager::IsInEvadeMode — the same predicate DcCombatFlag and the
    // teardown diag's EVADING column use, so a triage never sees the blame table
    // and the gate disagree). It is ignored by Always rows and is the whole test
    // for Evading ones.
    static bool IsPurgeable(uint32 mapId, uint32 entry, bool isEvading);

    // Does this map have any rows at all? The cheap gate the runtime sweep takes
    // before it walks anybody's combat references.
    static bool HasRowsFor(uint32 mapId);
};

#endif  // _PLAYERBOT_DCCOMBATPURGEREGISTRY_H
