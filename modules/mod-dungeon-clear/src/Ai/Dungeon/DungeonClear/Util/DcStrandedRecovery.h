/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCSTRANDEDRECOVERY_H
#define _PLAYERBOT_DCSTRANDEDRECOVERY_H

class Player;

// Engine glue for the stranded-member recovery failsafe (see DcStrandedDecision.h
// for the kernel). This is the only place the feature touches game objects: it
// resolves the leader tank, maintains the run's no-progress clock on the leader's
// DcRunState, reads the settings via DcSettings, runs the pure kernel over a
// same-map group snapshot, and — when the verdict says so — teleports the stuck
// bot member(s) to the tank and re-arms the clock.
//
// SINGLE-OWNER, and dead-tolerant like the rez recovery. Evaluate is called on
// every bot by the recovery trigger but no-ops on all but one, so there is still
// exactly one clock-writer site (the same single-threaded cross-bot access the
// other run-state clocks rely on). Which bot that is depends on whether the tank
// is standing up:
//
//   * tank alive — the elected leader, gathering the strays on itself. Unchanged.
//   * tank dead  — FindLeaderTank elects only among ALIVE tank bots, so in a
//                  5-man it returns nullptr the moment the tank dies. The
//                  deterministic FindTerminalDriver takes the tick and the strays
//                  are gathered on the run owner's CORPSE.
//
// The second case used to be excluded on the theory that "a dead leader is a
// wipe/rez situation, not a stranded one". It is both, and the rez cannot happen
// until the stranding is fixed: a rez is cast at the body, so survivors parked
// 122yd away are exactly what stops the rez recovery from ever running. Gundrak
// tp-20260830-231921-1 is the measurement — five runs, tank dead on Slad'ran,
// survivors stranded for ten minutes apiece, all five killed by the 600s
// no-progress watchdog with this failsafe never once evaluated.
//
// PROGRESS SIGNAL mirrors the test-harness livelock net (DcTestRunJob): the clock
// re-stamps whenever a boss/objective completes or the tank closes on the next
// anchor, and combat re-arms it wholesale (a fight is progress) so neither a long
// boss fight nor a slow rest ever trips the failsafe. Only a genuine freeze — the
// tank parked at a spread gate it can never satisfy because a member fell under
// the world — lets the clock run out.
namespace DcStrandedRecovery
{
    // DungeonClear.StrandedRecovery, per-run override -> conf -> default.
    bool Enabled(Player* bot);

    // Tick the run's no-progress clock and evaluate the failsafe for `bot`'s run.
    // Callable by any bot every tick; no-ops (returns false) unless `bot` is the
    // elected clock owner of an enabled, unpaused run — the leader tank, or the
    // terminal driver once that tank is a corpse. Maintains the progress clock on
    // the run OWNER's DcRunState as a side effect. Returns true only when a
    // teleport should fire THIS tick — out of combat, the no-progress window
    // elapsed, and at least one bot member stranded out of range.
    bool Evaluate(Player* bot);

    // Teleport every stranded bot member to the anchor (fanned out so they don't
    // stack), drop their stale follow splines, re-arm the clock, and announce.
    // Called by the action after Evaluate returned true. Safe to call on any bot;
    // no-ops unless `bot` is the same clock owner Evaluate elected. Note the
    // driver is NOT exempt from the teleport when the anchor is a corpse — the
    // sole survivor is routinely both the driver and the stray.
    void Recover(Player* bot);
}

#endif  // _PLAYERBOT_DCSTRANDEDRECOVERY_H
