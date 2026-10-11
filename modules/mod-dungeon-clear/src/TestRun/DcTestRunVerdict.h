/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCTESTRUNVERDICT_H
#define _PLAYERBOT_DCTESTRUNVERDICT_H

#include <cstdint>

// Pure verdict kernel for the automated dungeon test harness (`.dc test`).
// Each monitoring tick the manager distills the live run into an Observation
// and asks this kernel whether the run is still going, has succeeded, or has
// failed — and why. Extracted engine-free so the precedence ladder is
// unit-testable in isolation, mirroring DcWaitAtBossDecision / DcSmartRest.
//
// Precedence (first match wins):
//   1. The run's own disable funnel fired (DisableDungeonClear) — that verdict
//      is authoritative: all-cleared reason = Success, anything else (party
//      death, dc off, left the dungeon) = FailDisabled.
//   2. Operator abort / leader gone / GM logged out — FailAborted.
//   3. The whole party dead past its grace — FailPartyWiped. This is NOT
//      redundant with (1): the death bailout that would disable the run lives
//      in the `dungeon clear` strategy, which is installed on the non-combat
//      and combat engines only, so a corpse (BOT_STATE_DEAD) never ticks it.
//      One death with a survivor still routes through (1) — the survivor fires
//      it — but a full wipe leaves nobody to, and without this rung the run
//      just sits there until the no-progress net calls it a livelock, which is
//      both the wrong diagnosis and ten minutes of a test slot wasted.
//   4. Pause outlasting its grace — a paused run is waiting for human input,
//      which a test run by definition never gets. The grace period exists so
//      the door-blocked auto-resume can win the race before we call it.
//   5. Stall outlasting its grace — the stall ladder gets time to recover.
//   6. No boss/objective progress for too long — the silent-livelock net.
//   7. Overall wall-clock cap.
//
// SCENARIO runs (a registry row with a success predicate — see
// DcTestDungeonRegistry::Row) add one rung and bend two:
//   0.5 Predicate held AND its grace ran out — Success ("grace": the event's
//      tail, e.g. the chest loot, was still pending). Sits just under (1), so a
//      real all-cleared on the same tick is still recorded as all-cleared.
//   (1) A disable with a non-all-cleared reason AFTER the predicate held is a
//      Success too: the objective was met, and the event's own tail (a death
//      during the loot, a dc off) cannot un-meet it.
//   (3)-(7) Likewise, once the predicate holds, a watchdog tripping during the
//      grace resolves to Success, not to the watchdog's failure.
//   (2) Abort is NOT bent: an operator stop, a vanished leader or a logged-out
//      GM ends the run as aborted whatever the predicate says.

namespace DcTestRun
{
    struct Limits
    {
        std::uint32_t pauseGraceMs = 60 * 1000;
        // Long enough that a battle rez or a soulstone gets to un-wipe the run
        // before it is called, short enough that a real wipe ends promptly.
        std::uint32_t wipeGraceMs = 15 * 1000;
        std::uint32_t stallGraceMs = 120 * 1000;
        std::uint32_t noProgressMs = 600 * 1000;
        std::uint32_t overallTimeoutMs = 7200 * 1000;
    };

    enum class Verdict : std::uint8_t
    {
        Continue,
        Success,
        FailDisabled,        // disable fired with a non-all-cleared reason
        FailPartyWiped,      // every member on the leader's map is dead
        FailPausedTimeout,
        FailStalledTimeout,
        FailNoProgress,
        FailOverallTimeout,
        FailAborted          // .dc test stop, GM logout, leader/bot missing
    };

    struct Observation
    {
        bool disableFired = false;
        bool disableAllCleared = false;
        bool abortRequested = false;
        bool leaderMissing = false;
        bool gmOnline = true;
        bool partyWiped = false;
        bool paused = false;
        bool stalled = false;
        std::uint32_t wipedForMs = 0;
        std::uint32_t pausedForMs = 0;
        std::uint32_t stalledForMs = 0;
        std::uint32_t sinceProgressMs = 0;
        std::uint32_t elapsedMs = 0;

        // Scenario runs only (both stay false everywhere else, which is exactly
        // today's behaviour). scenarioSuccess = the row's success predicate has
        // held at least once this run (latched — see ScenarioGrace);
        // graceExpired = its successGraceS has since run out.
        bool scenarioSuccess = false;
        bool graceExpired = false;
    };

    Verdict Classify(Observation const& o, Limits const& l);

    // HOW a Success was reached, for the record's `successBy` (schema 13):
    //   "allCleared" — DC's own all-cleared ended the run;
    //   "grace"      — the predicate held and its grace ran out first (the
    //                  event's tail was still pending);
    //   "predicate"  — the predicate held and the run ended some other way
    //                  inside the grace (see the bent rungs above).
    // "" for a non-success verdict.
    char const* SuccessBy(Observation const& o, Verdict v);

    // The no-progress clock, with the EVENT PROGRESS SEQUENCE folded in. The
    // harness decides per tick whether the run visibly progressed (a kill, an
    // anchor, the combat picture moving, the tank closing on its target) and
    // passes that as `progressed`; `eventSeq` is the leader's
    // DcRunState::eventProgressSeq, which a long scripted event bumps on its own
    // headway (chess: each move, piece death, phase change). A CHANGE in it
    // counts exactly like a kill. The very first sample only seeds the last-seen
    // value — a counter left non-zero by setup is not progress.
    struct ProgressClock
    {
        std::uint32_t sinceMs = 0;
        std::uint32_t lastEventSeq = 0;
        bool seeded = false;

        // Returns true when this tick counted as progress (the clock reset).
        bool Step(std::uint32_t dtMs, bool progressed, std::uint32_t eventSeq)
        {
            if (seeded && eventSeq != lastEventSeq)
                progressed = true;
            lastEventSeq = eventSeq;
            seeded = true;
            if (progressed)
                sinceMs = 0;
            else
                sinceMs += dtMs;
            return progressed;
        }
    };

    // The scenario success latch + its grace clock. The predicate LATCHES: an
    // objective that was met stays met even if the instance value later moves
    // (chess's DONE is permanent, but a future predicate might not be), and the
    // grace is measured from the first tick it held. graceMs 0 = expire on the
    // same tick the predicate first holds.
    struct ScenarioGrace
    {
        bool held = false;
        std::uint32_t heldForMs = 0;
        std::uint32_t heldAtMs = 0;   // caller's clock when it first held

        void Step(bool predicateHolds, std::uint32_t dtMs, std::uint32_t nowMs)
        {
            if (held)
            {
                heldForMs += dtMs;
                return;
            }
            if (predicateHolds)
            {
                held = true;
                heldForMs = 0;
                heldAtMs = nowMs;
            }
        }

        bool Expired(std::uint32_t graceMs) const { return held && heldForMs >= graceMs; }
    };

    // Stable machine token for the JSONL record ("success", "paused_timeout",
    // ...). "continue" is never written; it means keep monitoring.
    char const* VerdictName(Verdict v);

    inline bool IsTerminal(Verdict v) { return v != Verdict::Continue; }
    inline bool IsSuccess(Verdict v) { return v == Verdict::Success; }
}

#endif  // _PLAYERBOT_DCTESTRUNVERDICT_H
