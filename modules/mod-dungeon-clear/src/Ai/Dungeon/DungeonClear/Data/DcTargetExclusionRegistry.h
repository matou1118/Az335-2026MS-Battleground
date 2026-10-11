/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCTARGETEXCLUSIONREGISTRY_H
#define _PLAYERBOT_DCTARGETEXCLUSIONREGISTRY_H

#include "Define.h"

class Player;

// Static registry of creatures the DPS must not SHOOT, for a window the world
// itself defines.
//
// This is the third and last member of the "do not attack that" family, and the
// three are genuinely different questions:
//
//   * FightInPlaceRegistry / BossPullbackRegistry answer "where do we fight it".
//   * DcNeverTargetRegistry answers "is killing this progress" — a permanent
//     (mapId, entry) fact, and it only filters the CLEAR's own scans. The stock
//     combat engine is untouched, which is exactly right for a mob that merely
//     wastes the clear's time.
//   * This one answers "will killing it right now LOSE THE RUN", and it has to
//     reach the stock combat engine's own target selection, because that is what
//     actually points the raid's damage. It is also WINDOWED — the same creature
//     is the correct target ten seconds later.
//
// Razorgore is the case it was written for and shows why the other two cannot
// serve. In phase 1 he is mind-controlled, walked around his chamber breaking
// thirty eggs; if he DIES before the last one he casts 20038 and instakills the
// whole raid. In phase 2 he is the boss and killing him is the entire objective.
// A permanent row would block the kill; a clear-side filter would not reach the
// bots doing the shooting.
//
// HOW IT REACHES THE ENGINE, and why this lives in mod-dungeon-clear at all:
// mod-playerbots' Strategy base exposes two virtuals — HasTargetExclusions() as
// a cheap gate and AppendTargetExclusions() to name guids — and
// GatherStrategyTargetExclusions walks EVERY strategy attached to the bot's
// combat engine, not a hardcoded list of raid strategies. DungeonClearCombat
// Strategy is one of those, so overriding the pair there gives DC the same
// authority over target selection that `moltencore` and `karazhan` have, with no
// change to mod-playerbots at all. Raid content DC drives can therefore keep its
// combat guards next to the encounter logic that needs them.
//
// Keep it small and keep the justification in the table. The bar is "killing
// this, in this window, loses the run" — not "wastes damage", which is
// DcNeverTargetRegistry's weaker bar.
struct DcTargetExclusionRow
{
    uint32 mapId{0};
    uint32 entry{0};

    // Is the exclusion in force RIGHT NOW? Evaluated per target pick, so it must
    // be cheap and must read live state rather than latch. nullptr = always.
    bool (*inForce)(Player* bot){nullptr};

    // Does the bar reach the TANK's pick as well?
    //
    // Default false, which is Razorgore's case and the reason the tank carve-out
    // exists at all: somebody must still HOLD the creature the rest of the raid
    // is barred from killing. It is exactly wrong for an OUT-OF-ORDER boss — a
    // tank that answers a boss two rooms ahead walks the raid there, which is the
    // whole harm the row is meant to prevent — so those rows set it.
    bool alsoTank{false};
};

class DcTargetExclusionRegistry
{
public:
    // Does this map have any rows at all? The fast gate behind
    // Strategy::HasTargetExclusions, which mod-playerbots caches per engine and
    // recomputes on every strategy add/remove — and ApplyInstanceStrategies
    // adds/removes on every map change, so a map-keyed answer is refreshed
    // exactly when the map changes. Pure; unit-testable on its own.
    static bool HasRowsFor(uint32 mapId);

    // Is `entry` excluded for `bot` right now? Runs the row's live gate.
    //
    // `forTank` asks the question for a TANK pick, which only rows carrying
    // `alsoTank` answer yes to. Everything else — DPS, attacker pools, and the
    // clear's own hold-fire rung — asks the default.
    static bool IsExcluded(Player* bot, uint32 mapId, uint32 entry, bool forTank = false);
};

#endif  // _PLAYERBOT_DCTARGETEXCLUSIONREGISTRY_H
