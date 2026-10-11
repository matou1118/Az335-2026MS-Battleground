/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCTESTPLAN_H
#define _PLAYERBOT_DCTESTPLAN_H

#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "TestRun/DcTestGearTiers.h"

// Pure kernel for `.dc test plan` campaigns: a plan keeps up to `concurrent`
// test runs in flight until `total` have completed, then the outcomes are
// aggregated into one summary (DcTestPlanSummary). A plan runs one dungeon, or
// draws each launch from a POOL of dungeons (`pool=`), optionally forever
// (`total=0`, the Test Deck's Continuous mode). Engine-free —
// the launch decision and the argument parsing are unit-testable in isolation
// (mirroring DcTestRunVerdict / DcTestRunSelect); DcTestPlanManager owns the
// live state and feeds this kernel.

namespace DcTestPlan
{
    // One dungeon a pool plan draws from. `size` and `resetSensitive` are
    // filled by the manager from the catalogue (the kernel knows no maps): a
    // raid entry launches at its catalogue default size, and heroic / raid
    // entries are the ones the global instance reset repops mid-run.
    struct PoolEntry
    {
        std::string token;
        bool heroic = false;
        std::uint32_t size = 0;       // 0 = classic 5-man
        bool resetSensitive = false;

        // "rfc", "uk:heroic" — the stable key per-entry stats are filed under.
        std::string Key() const { return heroic ? token + ":heroic" : token; }
    };

    // Bag (default): every entry runs once per cycle in shuffled order, then
    // the bag refills — random, but a 12-dungeon pool cannot go 40 runs without
    // touching one of them. Random: independent uniform draws.
    enum class PickMode
    {
        Bag,
        Random,
    };

    struct Spec
    {
        std::string planId;        // "tp-…", assigned by the manager
        std::string dungeonToken;  // DcTestDungeonRegistry token ("" for pool= plans)
        // Entries every launch is drawn from. A single-dungeon plan becomes a
        // one-entry pool in the manager, so there is one launch path; `isPool`
        // remembers which form the operator typed.
        std::vector<PoolEntry> pool;
        bool isPool = false;
        bool endless = false;          // total=0 with pool=: run until stopped
        // `paused`: register the plan without launching; `.dc test plan
        // resume` starts it. Lets a caller whose console line is too short
        // for the whole pool start with part of it and `edit add=` the rest.
        bool startPaused = false;
        PickMode pick = PickMode::Bag;
        std::uint32_t total = 0;       // runs to complete (failures count)
        std::uint32_t concurrent = 0;  // plan-local in-flight cap
        std::uint32_t level = 0;       // 0 = registry default for the difficulty
        bool heroic = false;           // children run at DUNGEON_DIFFICULTY_HEROIC
        std::uint32_t seedBase = 0;    // 0 = random comp per run;
                                       // N = child i replays seed N+i
        std::uint32_t size = 0;        // 0 = classic 5-man; 2-40 = raid comp size
        // Gear ceiling every child run is geared to. Plan-wide rather than
        // per-child on purpose: a campaign exists to compare runs against each
        // other, which only means anything at one ceiling.
        DcTestGearTiers::Spec gear;
    };

    struct Counters
    {
        std::uint32_t launched = 0;   // Start() accepted (active + finished)
        std::uint32_t succeeded = 0;
        std::uint32_t failed = 0;     // any non-success incl. setup_failed
        std::uint32_t activeNow = 0;  // live child runs
    };

    // One finished child run, distilled from its DcTestRunRecord::Record by the
    // run manager's erase pass. bossKills keeps only named mask-kills (in kill
    // order) — anchor "objective" completions carry no name worth aggregating.
    // One pull carried up from a child run for the plan-level pull stats. The
    // full DcTestRunRecord::PullEntry stays in the run's own JSONL; the plan only
    // needs the two numbers whose relationship is the diagnosis (what the
    // governor predicted vs. what showed up), plus the two flags that split the
    // population (which maneuver it chose, and whether the party died on it).
    struct PullSample
    {
        std::uint32_t predicted = 0;
        std::uint32_t observed = 0;
        bool advanced = false;
        bool wipedHere = false;
    };

    struct RunOutcome
    {
        std::string runId;
        std::string result;      // verdict token ("success", "setup_failed", …)
        std::string failReason;
        std::uint32_t durationS = 0;
        std::uint32_t bossesKilled = 0;
        std::uint32_t bossesTotal = 0;
        std::vector<std::string> bossKills;
        // Every boss on the run's roster, in progression order. Gives the plan
        // summary a denominator that includes bosses no run ever reached.
        std::vector<std::string> bossRoster;
        // What the party was down to when the run ended, boss or trash; empty
        // when nobody died or nothing was on them (DcTestRunRecord::wipeOpponent).
        std::string wipeOpponent;
        bool wipeOnBoss = false;
        std::vector<PullSample> pulls;
    };

    // How many new runs to launch this tick: bounded by the plan's remaining
    // budget (total - launched; none for an endless plan), the plan's own
    // concurrency headroom, and the run manager's global cap headroom; zero
    // while a backoff is pending.
    std::uint32_t LaunchesWanted(Spec const& s, Counters const& c,
                                 std::uint32_t globalHeadroom, std::uint32_t backoffMs);

    // A plan started from the console has no issuing GM until the headless
    // test driver finishes logging in, so the scheduler has to sit on its
    // hands for the first few ticks. Wait while the login is in flight, but
    // bound it — a driver that can't come up at all (or one whose login never
    // lands) must fail the plan rather than leave it parked forever.
    enum class DriverWait
    {
        Wait,
        Abort,
    };
    inline DriverWait DriverWaitVerdict(bool loginPending, std::uint32_t waitedMs,
                                        std::uint32_t capMs)
    {
        if (!loginPending)
            return DriverWait::Abort;  // misconfigured — retrying can't fix it
        return waitedMs >= capMs ? DriverWait::Abort : DriverWait::Wait;
    }

    // A plan is finished (ready to summarize) once nothing is in flight and
    // either the total has completed or the plan was told to stop launching.
    // An endless plan only ever finishes by being stopped.
    inline bool IsFinished(Spec const& s, Counters const& c, bool stopping)
    {
        if (c.activeNow != 0)
            return false;
        if (s.endless)
            return stopping;
        return stopping || c.succeeded + c.failed >= s.total;
    }

    // ---- Pool picking --------------------------------------------------------

    // Per-plan picker state. `bag` holds the pool indices not yet run this
    // cycle, in draw order.
    struct PickState
    {
        std::vector<std::uint32_t> bag;
        std::mt19937 rng;
    };

    // Seed the picker: seedBase != 0 makes a plan's pick order reproducible;
    // 0 takes `entropy` (the manager passes a random_device draw).
    void SeedPicker(PickState& st, std::uint32_t seedBase, std::uint32_t entropy);

    // Next pool index to launch, or -1 when no entry is eligible.
    // `eligible[i]` false skips entry i for now without consuming its turn:
    // a bag keeps it for later, so a reset-guarded heroic runs as soon as the
    // guard lifts. An empty `eligible` means every entry is eligible.
    std::int32_t PickNext(PickState& st, Spec const& s, std::vector<bool> const& eligible);

    // What PickNext would return, without drawing (bag mode only — a random
    // draw has no "next" until it is made): -1 when unknown or none eligible.
    std::int32_t PeekNext(PickState const& st, Spec const& s, std::vector<bool> const& eligible);

    // Drop the bag, e.g. after the pool was edited; the next pick refills it.
    inline void ResetBag(PickState& st) { st.bag.clear(); }

    // ---- Instance-reset guard ------------------------------------------------
    //
    // The core's global instance reset (Instance.ResetTimeHour, applied in raw
    // epoch arithmetic = UTC day boundaries) repops every heroic and raid
    // party in the world. Launching one of those shortly before it just books
    // a bogus failure, so a pool plan skips them from `guardMin` minutes
    // before the reset until `afterS` seconds after it.
    inline constexpr std::uint32_t kResetGuardAfterS = 120;

    // Unix second of the next reset strictly after nowS.
    std::uint64_t NextResetS(std::uint64_t nowS, std::uint32_t resetHourUtc);

    // 0 when the guard is not active at nowS; otherwise the unix second it
    // lifts (the reset it is guarding + afterS). guardMin 0 disables it.
    std::uint64_t ResetGuardUntil(std::uint64_t nowS, std::uint32_t resetHourUtc,
                                  std::uint32_t guardMin,
                                  std::uint32_t afterS = kResetGuardAfterS);

    // Which entries may launch while the guard is (or is not) active.
    std::vector<bool> EligibleEntries(Spec const& s, bool guardActive);

    // `.dc test plan start <token> [heroic] total=N [concurrent=N] [level=N]
    // [seed=N] [size=N] [ilvl=N|none] [quality=N|epic|…]`, or
    // `.dc test plan start pool=<token[:heroic|:hc]>,… [total=0|N] [pick=bag|random] …`.
    // Fills the whole Spec except planId (and the pool entries' size /
    // resetSensitive, which need the catalogue). ok is false with a
    // usage-shaped err on a missing token/total, a duplicate bare word or pool
    // entry, total=0 without pool=, or a malformed key=value. The positional
    // form leaves `pool` empty — the manager builds its one entry.
    struct ParseResult
    {
        bool ok = false;
        std::string err;
        Spec spec;
    };
    ParseResult ParseStartArgs(std::string const& args);

    // `token[:heroic|:hc],…` → entries (size/resetSensitive left for the
    // manager). ok false on an empty list, an empty entry, an unknown suffix or
    // a duplicate entry.
    struct PoolParse
    {
        bool ok = false;
        std::string err;
        std::vector<PoolEntry> entries;
    };
    PoolParse ParsePool(std::string const& list);

    // `.dc test plan edit <planId> [pool=…|add=…] [concurrent=N]` — at least
    // one change is required. pool= replaces the pool, add= appends to it
    // (refusing an entry already there); the two cannot be combined.
    struct EditSpec
    {
        std::string planId;
        bool hasPool = false;
        std::vector<PoolEntry> pool;
        bool hasAdd = false;
        std::vector<PoolEntry> add;
        bool hasConcurrent = false;
        std::uint32_t concurrent = 0;
    };
    struct EditParse
    {
        bool ok = false;
        std::string err;
        EditSpec edit;
    };
    EditParse ParseEditArgs(std::string const& args);
}

#endif  // _PLAYERBOT_DCTESTPLAN_H
