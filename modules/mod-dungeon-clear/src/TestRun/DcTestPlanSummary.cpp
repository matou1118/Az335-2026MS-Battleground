/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "TestRun/DcTestPlanSummary.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <sstream>

#include "TestRun/DcTestRunRecord.h"

namespace DcTestPlanSummary
{
    namespace
    {
        // Ordered accumulation (std::map keyed by string) then a stable sort by
        // count desc — ties stay alphabetical, so output order is deterministic.
        std::vector<KeyCount> CountSorted(std::map<std::string, std::uint32_t> const& counts)
        {
            std::vector<KeyCount> out;
            out.reserve(counts.size());
            for (auto const& [key, count] : counts)
                out.push_back({key, count});
            std::stable_sort(out.begin(), out.end(),
                             [](KeyCount const& a, KeyCount const& b) { return a.count > b.count; });
            return out;
        }

        // The idx-th element (0-based) of the sorted sample a histogram
        // describes. idx must be < the histogram's total count.
        template <typename K>
        K NthFromHist(std::map<K, std::uint32_t> const& hist, std::uint64_t idx)
        {
            for (auto const& [value, count] : hist)
            {
                if (idx < count)
                    return value;
                idx -= count;
            }
            return hist.empty() ? K{} : hist.rbegin()->first;
        }

        // Nearest-rank percentile over a histogram of the sample: no
        // interpolation, exact on a sample of one, and every returned value is
        // a value that really occurred. Empty sample -> 0, which is also the
        // natural "no pulls were observed" reading.
        template <typename K>
        K PercentileFromHist(std::map<K, std::uint32_t> const& hist, std::uint64_t n,
                             std::uint32_t pct)
        {
            if (n == 0)
                return K{};
            std::uint64_t idx = (static_cast<std::uint64_t>(pct) * n) / 100;
            if (idx >= n)
                idx = n - 1;
            return NthFromHist(hist, idx);
        }

        void CountKey(std::map<std::string, std::uint32_t>& counts, std::string const& key)
        {
            auto it = counts.find(key);
            if (it != counts.end())
                ++it->second;
            else if (counts.size() < Accumulator::kMaxDistinctKeys)
                counts.emplace(key, 1);
            else
                ++counts["(other)"];
        }
    }

    Stats Build(std::vector<DcTestPlan::RunOutcome> const& outcomes)
    {
        Accumulator acc;
        for (DcTestPlan::RunOutcome const& o : outcomes)
            acc.Add(o);
        return acc.Build();
    }

    void Accumulator::Add(DcTestPlan::RunOutcome const& o)
    {
        ++_launched;

        // Progression order, taken from the longest roster any run reported.
        // Longest rather than first because a run that failed during setup never
        // got one, and a multi-wing map filters the roster to the wing the party
        // spawned in — the fullest list is the one that names the most bosses.
        if (o.bossRoster.size() > _roster.size())
            _roster = o.bossRoster;

        _runIds.push_back(o.runId);
        if (_runIdCap && _runIds.size() > _runIdCap)
        {
            _runIds.pop_front();
            _runIdsDropped = true;
        }

        ++_verdicts[o.result];
        if (o.result == "success")
        {
            ++_succeeded;
            ++_successDurations[o.durationS];
            _successDurationSum += o.durationS;
        }
        else
        {
            ++_failed;
            if (!o.failReason.empty())
                CountKey(_reasons, o.failReason);
        }

        // Funnel: per boss name, kill count (deduped within a run), wipe count,
        // and the sum of timeline positions, so entries with no roster to order
        // them still sort into progression order by mean position even when runs
        // kill in slightly different orders.
        std::vector<std::string> seen;
        for (std::size_t pos = 0; pos < o.bossKills.size(); ++pos)
        {
            std::string const& name = o.bossKills[pos];
            if (std::find(seen.begin(), seen.end(), name) != seen.end())
                continue;
            seen.push_back(name);
            FunnelAcc& f = _funnel[name];
            ++f.killed;
            f.posSum += pos;
        }

        if (!o.wipeOpponent.empty())
        {
            if (o.wipeOnBoss)
                ++_funnel[o.wipeOpponent].wiped;
            else
                CountKey(_trashWipes, o.wipeOpponent);
        }
        else if (o.result == "wipe")
        {
            // A wipe the harness could not pin on anything — the party was
            // out of combat when the last member fell.
            ++_unattributedWipes;
        }

        // Pull population, pooled across every run in the plan. Pooled rather
        // than averaged per run on purpose: one 90-pull run and one that wiped
        // on its second pull contribute the pulls they actually made, so a plan
        // that keeps dying early cannot flatter its own numbers by weighting a
        // two-sample run as heavily as a full clear.
        for (DcTestPlan::PullSample const& p : o.pulls)
        {
            ++_pulls.pulls;
            if (p.advanced)
                ++_pulls.advanced;
            if (p.observed > p.predicted)
                ++_pulls.underestimated;
            ++_observed[p.observed];
            ++_errors[static_cast<std::int32_t>(p.observed) -
                      static_cast<std::int32_t>(p.predicted)];
            if (p.wipedHere)
            {
                ++_pulls.wipePulls;
                _pulls.wipeObservedMax = std::max(_pulls.wipeObservedMax, p.observed);
            }
        }
    }

    Stats Accumulator::Build() const
    {
        Stats s;
        s.launched = _launched;
        s.succeeded = _succeeded;
        s.failed = _failed;
        s.verdicts = CountSorted(_verdicts);
        s.failReasons = CountSorted(_reasons);
        s.unattributedWipes = _unattributedWipes;
        s.runIds.assign(_runIds.begin(), _runIds.end());
        s.runIdsRecent = _runIdsDropped;

        if (_succeeded)
        {
            std::uint64_t const n = _succeeded;
            s.minS = _successDurations.begin()->first;
            s.maxS = _successDurations.rbegin()->first;
            s.avgS = static_cast<std::uint32_t>(_successDurationSum / n);
            s.medianS = n % 2 ? NthFromHist(_successDurations, n / 2)
                              : (NthFromHist(_successDurations, n / 2 - 1) +
                                 NthFromHist(_successDurations, n / 2)) / 2;
        }

        s.pulls = _pulls;
        s.pulls.observedP50 = PercentileFromHist(_observed, _pulls.pulls, 50);
        s.pulls.observedP90 = PercentileFromHist(_observed, _pulls.pulls, 90);
        s.pulls.observedMax = _observed.empty() ? 0 : _observed.rbegin()->first;
        s.pulls.errorP50 = PercentileFromHist(_errors, _pulls.pulls, 50);
        s.pulls.errorP90 = PercentileFromHist(_errors, _pulls.pulls, 90);

        s.trashWipes = CountSorted(_trashWipes);

        // Roster order first, so a boss no run ever reached still gets a row
        // (0 killed, 0 wiped) instead of vanishing from the funnel entirely —
        // "nobody got that far" is the single most useful thing a plan can say.
        std::vector<std::string> placed;
        for (std::string const& name : _roster)
        {
            if (std::find(placed.begin(), placed.end(), name) != placed.end())
                continue;
            placed.push_back(name);
            auto const it = _funnel.find(name);
            if (it == _funnel.end())
                s.funnel.push_back({name, 0, 0});
            else
                s.funnel.push_back({name, it->second.killed, it->second.wiped});
        }

        // Then anything the roster never listed — a summoned bonus boss, or a
        // creature the wipe latch resolved off its own boss flags. Ordered by
        // mean kill position; wipe-only entries have no position at all, so they
        // sort last (and alphabetically among themselves, via the map order).
        std::vector<std::pair<double, std::string>> ordered;
        for (auto const& [name, f] : _funnel)
        {
            if (std::find(placed.begin(), placed.end(), name) != placed.end())
                continue;
            double const pos = f.killed ? static_cast<double>(f.posSum) / f.killed
                                        : std::numeric_limits<double>::max();
            ordered.emplace_back(pos, name);
        }
        std::stable_sort(ordered.begin(), ordered.end(),
                         [](auto const& a, auto const& b) { return a.first < b.first; });
        for (auto const& [pos, name] : ordered)
        {
            FunnelAcc const& f = _funnel.at(name);
            s.funnel.push_back({name, f.killed, f.wiped});
        }

        return s;
    }

    namespace
    {
        std::string Str(std::string const& v)
        {
            return "\"" + DcTestRunRecord::EscapeJson(v) + "\"";
        }

        // `"runs":{…},"verdicts":{…},…,"unattributedWipes":N` — shared by the
        // plan line and each pool entry's own object.
        void WriteStatsBody(std::ostringstream& o, Stats const& s)
        {
            o << "\"runs\":{\"launched\":" << s.launched
              << ",\"succeeded\":" << s.succeeded
              << ",\"failed\":" << s.failed
              << "},\"verdicts\":{";
            for (std::size_t i = 0; i < s.verdicts.size(); ++i)
            {
                if (i)
                    o << ',';
                o << Str(s.verdicts[i].key) << ':' << s.verdicts[i].count;
            }
            o << "},\"failReasons\":[";
            for (std::size_t i = 0; i < s.failReasons.size(); ++i)
            {
                if (i)
                    o << ',';
                o << "{\"reason\":" << Str(s.failReasons[i].key)
                  << ",\"count\":" << s.failReasons[i].count << '}';
            }
            o << "],\"duration\":{\"minS\":" << s.minS
              << ",\"avgS\":" << s.avgS
              << ",\"medianS\":" << s.medianS
              << ",\"maxS\":" << s.maxS
              << "},\"bossFunnel\":[";
            for (std::size_t i = 0; i < s.funnel.size(); ++i)
            {
                if (i)
                    o << ',';
                o << "{\"name\":" << Str(s.funnel[i].name)
                  << ",\"killed\":" << s.funnel[i].killed
                  << ",\"wiped\":" << s.funnel[i].wiped << '}';
            }
            o << "],\"trashWipes\":[";
            for (std::size_t i = 0; i < s.trashWipes.size(); ++i)
            {
                if (i)
                    o << ',';
                o << "{\"name\":" << Str(s.trashWipes[i].key)
                  << ",\"count\":" << s.trashWipes[i].count << '}';
            }
            o << "],\"unattributedWipes\":" << s.unattributedWipes;
        }
    }

    std::string ToJsonl(Header const& h, Stats const& s, std::vector<PoolEntryStats> const& pool)
    {
        std::ostringstream o;
        o << "{\"schema\":" << h.schema
          << ",\"planId\":" << Str(h.planId)
          << ",\"dungeon\":" << Str(h.dungeon)
          << ",\"dungeonName\":" << Str(h.dungeonName)
          << ",\"requested\":{\"total\":" << h.total
          << ",\"concurrent\":" << h.concurrent
          << ",\"level\":" << h.level
          << ",\"heroic\":" << (h.heroic ? "true" : "false")
          << ",\"seedBase\":" << h.seedBase
          << ",\"size\":" << h.size
          << ",\"gearIlvl\":" << h.gearIlvl
          << ",\"gearQuality\":" << h.gearQuality
          << "},\"startedAtMs\":" << h.startedAtMs
          << ",\"endedAtMs\":" << h.endedAtMs
          << ",\"durationS\":" << h.durationS
          << ",\"result\":" << Str(h.result)
          << ",\"abortReason\":" << Str(h.abortReason)
          << ',';
        WriteStatsBody(o, s);
        o << ",\"pulls\":{\"count\":" << s.pulls.pulls
          << ",\"advanced\":" << s.pulls.advanced
          << ",\"underestimated\":" << s.pulls.underestimated
          << ",\"observedP50\":" << s.pulls.observedP50
          << ",\"observedP90\":" << s.pulls.observedP90
          << ",\"observedMax\":" << s.pulls.observedMax
          << ",\"errorP50\":" << s.pulls.errorP50
          << ",\"errorP90\":" << s.pulls.errorP90
          << ",\"wipePulls\":" << s.pulls.wipePulls
          << ",\"wipeObservedMax\":" << s.pulls.wipeObservedMax
          << "},\"runIds\":[";
        for (std::size_t i = 0; i < s.runIds.size(); ++i)
        {
            if (i)
                o << ',';
            o << Str(s.runIds[i]);
        }
        o << "],\"runIdsRecent\":" << (s.runIdsRecent ? "true" : "false")
          << ",\"endless\":" << (h.endless ? "true" : "false")
          << ",\"checkpoint\":" << (h.checkpoint ? "true" : "false");
        if (!pool.empty())
        {
            o << ",\"pick\":" << Str(h.pick) << ",\"pool\":[";
            for (std::size_t i = 0; i < pool.size(); ++i)
            {
                PoolEntryStats const& e = pool[i];
                if (i)
                    o << ',';
                o << "{\"token\":" << Str(e.token)
                  << ",\"heroic\":" << (e.heroic ? "true" : "false")
                  << ",\"size\":" << e.size
                  << ",\"inPool\":" << (e.inPool ? "true" : "false") << ',';
                WriteStatsBody(o, e.stats);
                o << '}';
            }
            o << ']';
        }
        o << '}';
        return o.str();
    }

    std::string CapturePath()
    {
        if (char const* env = std::getenv("DC_TESTPLANS_FILE"))
            if (env[0])
                return env;
        return "dc_testplans.jsonl";
    }

    void Append(Header const& h, Stats const& s, std::vector<PoolEntryStats> const& pool)
    {
        static std::mutex mtx;
        static std::ofstream file;
        static bool opened = false;

        std::lock_guard<std::mutex> lock(mtx);
        if (!opened)
        {
            file.open(CapturePath(), std::ios::out | std::ios::app);
            opened = true;
        }
        if (!file.is_open())
            return;

        file << ToJsonl(h, s, pool) << '\n';
        file.flush();
    }
}
