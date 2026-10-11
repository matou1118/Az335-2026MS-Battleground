/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCTESTPLANSUMMARY_H
#define _PLAYERBOT_DCTESTPLANSUMMARY_H

#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "TestRun/DcTestPlan.h"

// Aggregates a finished plan's child-run outcomes into the one JSON line per
// plan appended to dc_testplans.jsonl (env override DC_TESTPLANS_FILE) — the
// dashboard's plan-history panel tails this file. Pure builder + the same
// append pattern as DcTestRunRecord.

namespace DcTestPlanSummary
{
    struct KeyCount
    {
        std::string key;
        std::uint32_t count = 0;
    };

    // Per-boss outcome counts across the plan, in progression order — the
    // "where do runs die" funnel, and the answer to "what percentage of runs
    // kill this boss, and what percentage lose to him".
    //
    // Ordering is the runs' own boss roster when they reported one (schema 6+);
    // bosses seen only in a kill timeline or a wipe (a summoned bonus boss the
    // roster never listed) are appended after it by ascending mean kill
    // position. Older outcomes carry no roster, so the whole list falls back to
    // mean kill position as before.
    //
    // `killed` counts runs that killed the boss (deduped within a run; only
    // named mask-kills feed it). `wiped` counts runs that ENDED with the party
    // down to him — a wipe or a death bailout the rez never recovered from,
    // both of which report the same opponent. The two are independent: a run
    // that kills three bosses and then wipes on the fourth is a kill for each of
    // the first three and a wipe for the last. Percentages are the caller's
    // business — divide by Stats::launched.
    struct FunnelEntry
    {
        std::string name;
        std::uint32_t killed = 0;
        std::uint32_t wiped = 0;
    };

    // Plan-wide pull population: how big the fights actually were, and how well
    // the Dynamic governor called them in advance.
    //
    // The two questions this exists to separate, because they need opposite
    // fixes and look identical from a wipe count alone:
    //
    //   errorP50/P90 near zero, observedP90 high
    //       -> the estimate is right and the CEILING is too generous. Lower
    //          PullDynamicMaxLeeroyMobs.
    //   errorP90 well above zero (underestimated is a large share)
    //       -> the ESTIMATE is blind to part of the room. Lowering the ceiling
    //          only papers over it; fix the reach/eligibility model.
    //
    // Percentiles are nearest-rank on the sorted sample (index (p*n)/100 clamped
    // to n-1), so they need no interpolation and are exact on tiny samples —
    // the same shape a plan of 5 runs and one of 500 can both be read from.
    // Error is signed: observed minus predicted, so negative means the governor
    // over-estimated (a pull that was smaller than feared).
    struct PullStats
    {
        std::uint32_t pulls = 0;          // samples across every run in the plan
        std::uint32_t advanced = 0;       // ...that committed the Advanced maneuver
        std::uint32_t underestimated = 0; // ...where observed > predicted
        std::uint32_t observedP50 = 0;
        std::uint32_t observedP90 = 0;
        std::uint32_t observedMax = 0;
        std::int32_t  errorP50 = 0;
        std::int32_t  errorP90 = 0;
        // The pulls a run ended on. `wipeObservedMax` is the biggest fight any
        // of them turned into — the number that says whether the party is dying
        // to over-pulls or to packs it pulled correctly and still lost.
        std::uint32_t wipePulls = 0;
        std::uint32_t wipeObservedMax = 0;
    };

    struct Stats
    {
        std::uint32_t launched = 0;   // outcomes seen (== runs started)
        std::uint32_t succeeded = 0;
        std::uint32_t failed = 0;
        std::vector<KeyCount> verdicts;     // by count desc, then name
        std::vector<KeyCount> failReasons;  // failures only, by count desc
        // Duration stats over SUCCESSFUL runs only (failure durations are
        // timeout artifacts); all zero when no run succeeded.
        std::uint32_t minS = 0, avgS = 0, medianS = 0, maxS = 0;
        std::vector<FunnelEntry> funnel;
        // The other half of the wipe ledger: runs lost to a specific trash mob,
        // by count desc. Without it the boss funnel's wipe column silently drops
        // the runs a pull ate on the way in, and the columns look unaccountably
        // short of the plan's wipe total.
        std::vector<KeyCount> trashWipes;
        // "wipe"-verdict runs the harness could not pin on anything: the party
        // was out of combat when the last member fell (a fall, a hazard, a
        // hostile nobody was engaged with). Counted only for that verdict, the
        // one where a death is certain — a "disabled" run with no opponent may
        // simply be a run nobody died in.
        std::uint32_t unattributedWipes = 0;
        PullStats pulls;
        std::vector<std::string> runIds;
        // True when runIds holds only the most recent runs (an endless plan
        // caps the list so a days-long soak doesn't grow without bound).
        bool runIdsRecent = false;
    };

    Stats Build(std::vector<DcTestPlan::RunOutcome> const& outcomes);

    // Incremental twin of Build: fold outcomes in one at a time, read Stats
    // out at any point. Keeps histograms rather than per-run samples, so its
    // memory is bounded by the number of DISTINCT durations / pull sizes /
    // reasons, not by the number of runs — an endless plan can run for days.
    // Build(outcomes) is exactly Accumulator-over-outcomes, so both produce
    // byte-identical summaries.
    class Accumulator
    {
    public:
        // runIdCap 0 keeps every run id (a finite plan's summary lists them
        // all); N keeps the last N (Stats::runIdsRecent then reads true once
        // any were dropped).
        explicit Accumulator(std::size_t runIdCap = 0) : _runIdCap(runIdCap) {}

        void Add(DcTestPlan::RunOutcome const& o);
        Stats Build() const;

        std::uint32_t Launched() const { return _launched; }
        std::uint32_t Succeeded() const { return _succeeded; }
        std::uint32_t Failed() const { return _failed; }

        // Distinct fail reasons / trash names kept before new ones fold into
        // "(other)" — fail reasons can embed names and numbers.
        static constexpr std::size_t kMaxDistinctKeys = 500;

    private:
        struct FunnelAcc
        {
            std::uint32_t killed = 0;
            std::uint32_t wiped = 0;
            std::uint64_t posSum = 0;
        };

        std::size_t _runIdCap = 0;
        std::uint32_t _launched = 0;
        std::uint32_t _succeeded = 0;
        std::uint32_t _failed = 0;
        std::map<std::string, std::uint32_t> _verdicts;
        std::map<std::string, std::uint32_t> _reasons;
        std::map<std::uint32_t, std::uint32_t> _successDurations;  // seconds -> runs
        std::uint64_t _successDurationSum = 0;
        std::map<std::string, FunnelAcc> _funnel;
        std::map<std::string, std::uint32_t> _trashWipes;
        std::uint32_t _unattributedWipes = 0;
        PullStats _pulls;
        std::map<std::uint32_t, std::uint32_t> _observed;  // pull size -> pulls
        std::map<std::int32_t, std::uint32_t> _errors;     // signed error -> pulls
        std::vector<std::string> _roster;
        std::deque<std::string> _runIds;
        bool _runIdsDropped = false;
    };

    // Plan identity + disposition, carried alongside the aggregated stats.
    struct Header
    {
        // 2: added heroic
        // 3: bossFunnel entries gained `wiped`; added trashWipes +
        //    unattributedWipes
        // 4: added the pull population (predicted vs observed)
        // 5: added the campaign's gear ceiling (gearIlvl/gearQuality)
        // 6: pool plans — `endless`, `checkpoint`, `pick`, per-entry `pool`
        //    stats, `runIdsRecent`
        std::uint32_t schema = 6;
        std::string planId;
        std::string dungeon;
        std::string dungeonName;
        std::uint32_t total = 0;
        std::uint32_t concurrent = 0;
        std::uint32_t level = 0;
        bool heroic = false;
        std::uint32_t seedBase = 0;
        std::uint32_t size = 0;      // 0 = classic 5-man comp
        // Gear ceiling asked for, as typed rather than resolved: 0 = "whatever
        // the server is set to", -1 = no limit. Resolved per run (and recorded
        // there); a campaign summary only needs to say what it requested.
        std::int32_t gearIlvl = 0;
        std::uint32_t gearQuality = 0;
        std::uint64_t startedAtMs = 0;
        std::uint64_t endedAtMs = 0;
        std::uint32_t durationS = 0;
        std::string result;       // "completed" | "stopped" | "aborted"
                                  // | "running" (checkpoint lines)
        std::string abortReason;  // "" unless aborted
        bool endless = false;
        // A periodic line an endless plan writes while it runs, so a crash
        // loses at most one checkpoint interval of aggregate. The plan's final
        // line has checkpoint false; readers take the last line per planId.
        bool checkpoint = false;
        std::string pick;         // "bag" | "random"; "" for a single-dungeon plan
    };

    // One pool entry's own slice of a pool plan's stats (per dungeon +
    // difficulty — a boss funnel only means something within one dungeon).
    struct PoolEntryStats
    {
        std::string token;
        bool heroic = false;
        std::uint32_t size = 0;
        bool inPool = true;       // false: edited out of the pool, still has runs
        Stats stats;
    };

    // `pool` empty = a single-dungeon plan: the line carries no "pool" key.
    std::string ToJsonl(Header const& h, Stats const& s,
                        std::vector<PoolEntryStats> const& pool = {});

    std::string CapturePath();
    void Append(Header const& h, Stats const& s, std::vector<PoolEntryStats> const& pool = {});
}

#endif  // _PLAYERBOT_DCTESTPLANSUMMARY_H
