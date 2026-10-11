/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "TestRun/DcTestPlan.h"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <sstream>

namespace DcTestPlan
{
    std::uint32_t LaunchesWanted(Spec const& s, Counters const& c,
                                 std::uint32_t globalHeadroom, std::uint32_t backoffMs)
    {
        if (backoffMs > 0)
            return 0;
        std::uint32_t const remaining =
            s.endless ? std::numeric_limits<std::uint32_t>::max()
                      : (s.total > c.launched ? s.total - c.launched : 0);
        std::uint32_t const planHeadroom =
            s.concurrent > c.activeNow ? s.concurrent - c.activeNow : 0;
        return std::min({remaining, planHeadroom, globalHeadroom});
    }

    ParseResult ParseStartArgs(std::string const& args)
    {
        ParseResult out;
        auto usage = [&](std::string const& why) -> ParseResult&
        {
            out.ok = false;
            out.err = why +
                " — usage: .dc test plan start <dungeon> [heroic] total=N [concurrent=N] [level=N]"
                " [seed=N] [size=N] [ilvl=N|none] [quality=rare|epic|…]"
                "  |  .dc test plan start pool=<dungeon[:heroic]>,… [total=0|N] [pick=bag|random]"
                " [concurrent=N] [level=N] [seed=N] [ilvl=…] [quality=…] [paused]";
            return out;
        };
        bool sawTotal = false;
        bool sawSize = false;

        std::istringstream in{args};
        std::string word;
        while (in >> word)
        {
            std::size_t const eq = word.find('=');
            if (eq == std::string::npos)
            {
                if (word == "heroic")
                {
                    out.spec.heroic = true;
                    continue;
                }
                if (word == "paused")
                {
                    out.spec.startPaused = true;
                    continue;
                }
                if (!out.spec.dungeonToken.empty())
                    return usage("unexpected '" + word + "'");
                out.spec.dungeonToken = word;
                continue;
            }

            std::string const key = word.substr(0, eq);
            std::string const val = word.substr(eq + 1);

            if (key == "pool")
            {
                if (out.spec.isPool)
                    return usage("pool= given twice");
                PoolParse const pool = ParsePool(val);
                if (!pool.ok)
                    return usage(pool.err);
                out.spec.pool = pool.entries;
                out.spec.isPool = true;
                continue;
            }
            if (key == "pick")
            {
                if (val == "bag")
                    out.spec.pick = PickMode::Bag;
                else if (val == "random")
                    out.spec.pick = PickMode::Random;
                else
                    return usage("bad pick in '" + word + "' (bag|random)");
                continue;
            }

            // The two gear options take words as well as numbers ("ilvl=none",
            // "quality=epic"), so they are read before the numeric parse below
            // rejects them.
            if (key == "ilvl")
            {
                bool ok = false;
                std::int32_t const ilvl = DcTestGearTiers::ParseIlvl(val, &ok);
                if (!ok)
                    return usage("bad item level in '" + word + "' (1-400, or none)");
                out.spec.gear.ilvl = ilvl;
                continue;
            }
            if (key == "quality")
            {
                std::uint32_t const q = DcTestGearTiers::ParseQuality(val);
                if (q == 0)
                    return usage("bad quality in '" + word +
                                 "' (normal|uncommon|rare|epic|legendary, or 1-5)");
                out.spec.gear.quality = q;
                continue;
            }

            char* end = nullptr;
            unsigned long const n = std::strtoul(val.c_str(), &end, 10);
            if (val.empty() || !end || *end != '\0')
                return usage("bad value in '" + word + "'");

            if (key == "total")
            {
                out.spec.total = static_cast<std::uint32_t>(n);
                sawTotal = true;
            }
            else if (key == "concurrent")
                out.spec.concurrent = static_cast<std::uint32_t>(n);
            else if (key == "level")
                out.spec.level = static_cast<std::uint32_t>(n);
            else if (key == "seed")
                out.spec.seedBase = static_cast<std::uint32_t>(n);
            else if (key == "size")
            {
                if (n < 2 || n > 40)
                    return usage("size must be 2-40 in '" + word + "'");
                out.spec.size = static_cast<std::uint32_t>(n);
                sawSize = true;
            }
            else
                return usage("unknown option '" + key + "'");
        }

        if (out.spec.isPool)
        {
            // The pool carries its own dungeons and difficulties; a stray
            // positional token or bare `heroic` would be silently ambiguous.
            if (!out.spec.dungeonToken.empty())
                return usage("pool= replaces the dungeon token ('" + out.spec.dungeonToken + "')");
            if (out.spec.heroic)
                return usage("with pool= mark heroic per entry (uk:heroic)");
            // Each raid entry launches at its catalogue default size.
            if (sawSize)
                return usage("size= does not apply to pool= (raid entries use their default size)");
            // total=0 (or omitted) with a pool = endless: the Continuous mode.
            out.spec.endless = out.spec.total == 0;
        }
        else
        {
            if (out.spec.dungeonToken.empty())
                return usage("missing dungeon");
            // An endless plan is only legal with pool= — a typo'd
            // single-dungeon plan must not run forever.
            if (sawTotal && out.spec.total == 0)
                return usage("total=0 (run until stopped) needs pool=");
            if (out.spec.total == 0)
                return usage("missing total=N");
        }

        out.ok = true;
        return out;
    }

    PoolParse ParsePool(std::string const& list)
    {
        PoolParse out;
        auto fail = [&](std::string const& why) -> PoolParse&
        {
            out.ok = false;
            out.err = why;
            out.entries.clear();
            return out;
        };

        if (list.empty())
            return fail("empty pool=");

        std::size_t start = 0;
        while (start <= list.size())
        {
            std::size_t const comma = list.find(',', start);
            std::string const item =
                list.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
            if (item.empty())
                return fail("empty entry in pool='" + list + "'");

            PoolEntry e;
            std::size_t const colon = item.find(':');
            e.token = item.substr(0, colon);
            if (colon != std::string::npos)
            {
                std::string const mode = item.substr(colon + 1);
                if (mode == "heroic" || mode == "hc")
                    e.heroic = true;
                else if (mode != "normal")
                    return fail("bad difficulty in pool entry '" + item + "' (:heroic or :hc)");
            }
            if (e.token.empty())
                return fail("empty dungeon in pool entry '" + item + "'");

            for (PoolEntry const& prev : out.entries)
                if (prev.token == e.token && prev.heroic == e.heroic)
                    return fail("duplicate pool entry '" + e.Key() + "'");
            out.entries.push_back(std::move(e));

            if (comma == std::string::npos)
                break;
            start = comma + 1;
        }

        out.ok = true;
        return out;
    }

    EditParse ParseEditArgs(std::string const& args)
    {
        EditParse out;
        auto usage = [&](std::string const& why) -> EditParse&
        {
            out.ok = false;
            out.err = why + " — usage: .dc test plan edit <planId> [pool=<dungeon[:heroic]>,…"
                            " | add=<dungeon[:heroic]>,…] [concurrent=N]";
            return out;
        };

        std::istringstream in{args};
        std::string word;
        while (in >> word)
        {
            std::size_t const eq = word.find('=');
            if (eq == std::string::npos)
            {
                if (!out.edit.planId.empty())
                    return usage("unexpected '" + word + "'");
                out.edit.planId = word;
                continue;
            }
            std::string const key = word.substr(0, eq);
            std::string const val = word.substr(eq + 1);
            if (key == "pool")
            {
                PoolParse const pool = ParsePool(val);
                if (!pool.ok)
                    return usage(pool.err);
                out.edit.pool = pool.entries;
                out.edit.hasPool = true;
            }
            else if (key == "add")
            {
                PoolParse const add = ParsePool(val);
                if (!add.ok)
                    return usage(add.err);
                out.edit.add = add.entries;
                out.edit.hasAdd = true;
            }
            else if (key == "concurrent")
            {
                char* end = nullptr;
                unsigned long const n = std::strtoul(val.c_str(), &end, 10);
                if (val.empty() || !end || *end != '\0' || n == 0)
                    return usage("bad value in '" + word + "' (1 or more)");
                out.edit.concurrent = static_cast<std::uint32_t>(n);
                out.edit.hasConcurrent = true;
            }
            else
                return usage("unknown option '" + key + "'");
        }

        if (out.edit.planId.empty())
            return usage("missing planId");
        if (out.edit.hasPool && out.edit.hasAdd)
            return usage("pool= and add= cannot be combined");
        if (!out.edit.hasPool && !out.edit.hasAdd && !out.edit.hasConcurrent)
            return usage("nothing to change");
        out.ok = true;
        return out;
    }

    void SeedPicker(PickState& st, std::uint32_t seedBase, std::uint32_t entropy)
    {
        st.rng.seed(seedBase ? seedBase : entropy);
        st.bag.clear();
    }

    namespace
    {
        bool IsEligible(std::vector<bool> const& eligible, std::uint32_t idx)
        {
            return eligible.empty() || (idx < eligible.size() && eligible[idx]);
        }

        // Refill the bag with every pool index not already waiting in it, in a
        // fresh shuffled order, behind whatever is left. Never duplicates an
        // index, so entries skipped by the reset guard wait exactly once.
        void Refill(PickState& st, std::size_t poolSize)
        {
            std::vector<std::uint32_t> fresh;
            for (std::uint32_t i = 0; i < poolSize; ++i)
                if (std::find(st.bag.begin(), st.bag.end(), i) == st.bag.end())
                    fresh.push_back(i);
            std::shuffle(fresh.begin(), fresh.end(), st.rng);
            st.bag.insert(st.bag.end(), fresh.begin(), fresh.end());
        }

        bool AnyEligible(std::size_t poolSize, std::vector<bool> const& eligible)
        {
            for (std::uint32_t i = 0; i < poolSize; ++i)
                if (IsEligible(eligible, i))
                    return true;
            return false;
        }
    }

    std::int32_t PickNext(PickState& st, Spec const& s, std::vector<bool> const& eligible)
    {
        std::size_t const n = s.pool.size();
        if (n == 0 || !AnyEligible(n, eligible))
            return -1;

        // Drop indices a pool edit made out of range.
        st.bag.erase(std::remove_if(st.bag.begin(), st.bag.end(),
                                    [n](std::uint32_t i) { return i >= n; }),
                     st.bag.end());

        if (s.pick == PickMode::Random)
        {
            std::vector<std::uint32_t> candidates;
            for (std::uint32_t i = 0; i < n; ++i)
                if (IsEligible(eligible, i))
                    candidates.push_back(i);
            std::uniform_int_distribution<std::size_t> dist(0, candidates.size() - 1);
            return static_cast<std::int32_t>(candidates[dist(st.rng)]);
        }

        for (int pass = 0; pass < 2; ++pass)
        {
            for (auto it = st.bag.begin(); it != st.bag.end(); ++it)
            {
                if (!IsEligible(eligible, *it))
                    continue;
                std::uint32_t const idx = *it;
                st.bag.erase(it);
                return static_cast<std::int32_t>(idx);
            }
            // Nothing eligible left in this cycle: start the next one.
            Refill(st, n);
        }
        return -1;
    }

    std::int32_t PeekNext(PickState const& st, Spec const& s, std::vector<bool> const& eligible)
    {
        if (s.pick != PickMode::Bag)
            return -1;
        for (std::uint32_t idx : st.bag)
            if (idx < s.pool.size() && IsEligible(eligible, idx))
                return static_cast<std::int32_t>(idx);
        return -1;
    }

    std::uint64_t NextResetS(std::uint64_t nowS, std::uint32_t resetHourUtc)
    {
        constexpr std::uint64_t kDay = 24 * 3600;
        std::uint64_t const offset = static_cast<std::uint64_t>(resetHourUtc % 24) * 3600;
        std::uint64_t const today = nowS / kDay * kDay + offset;
        return today > nowS ? today : today + kDay;
    }

    std::uint64_t ResetGuardUntil(std::uint64_t nowS, std::uint32_t resetHourUtc,
                                  std::uint32_t guardMin, std::uint32_t afterS)
    {
        if (guardMin == 0)
            return 0;
        constexpr std::uint64_t kDay = 24 * 3600;
        std::uint64_t const next = NextResetS(nowS, resetHourUtc);
        if (next - nowS <= static_cast<std::uint64_t>(guardMin) * 60)
            return next + afterS;
        // Just after a reset: the one that fired within the last afterS.
        std::uint64_t const prev = next - kDay;
        if (nowS < prev + afterS)
            return prev + afterS;
        return 0;
    }

    std::vector<bool> EligibleEntries(Spec const& s, bool guardActive)
    {
        std::vector<bool> out(s.pool.size(), true);
        if (guardActive)
            for (std::size_t i = 0; i < s.pool.size(); ++i)
                out[i] = !s.pool[i].resetSensitive;
        return out;
    }
}
