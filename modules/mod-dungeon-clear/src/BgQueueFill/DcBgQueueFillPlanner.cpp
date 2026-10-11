/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "BgQueueFill/DcBgQueueFillPlanner.h"

#include <algorithm>
#include <map>

#include "DungeonQueueFill/DcDungeonQueueFillPlanner.h"

namespace DcBgQueueFillPlanner
{
    namespace
    {
        using DcDungeonQueueFillPlanner::NextRand;

        bool Contains(std::vector<std::uint8_t> const& v, std::uint8_t c)
        {
            return std::find(v.begin(), v.end(), c) != v.end();
        }

        // Draw one class for `role` on a side whose classes so far are
        // counted in `used`.
        //
        // A battleground side is far bigger than the class list, so "distinct
        // classes" cannot hold the way it does for a five-man; the nearest
        // thing is "least-used first". The tiers, each weaker than the last:
        // the least-used classes of the pool not on the avoid list, then the
        // least-used classes whatever the list says. The pool never runs dry,
        // so neither does the draw.
        Slot Draw(char const* role, DcTestComp::Roster roster, std::map<std::uint8_t, std::uint32_t>& used,
                  std::vector<std::uint8_t> const& avoid, std::uint32_t& state)
        {
            std::vector<DcTestComp::Slot> const pool = DcTestComp::RolePool(role, roster);

            Slot out;
            out.role = role;
            if (pool.empty())  // only if the role token is unknown
                return out;

            std::uint32_t fewest = 0xFFFFFFFFu;
            for (DcTestComp::Slot const& s : pool)
                fewest = std::min(fewest, used[s.classId]);

            std::vector<DcTestComp::Slot> tier;
            std::vector<DcTestComp::Slot> fresh;
            for (DcTestComp::Slot const& s : pool)
            {
                if (used[s.classId] != fewest)
                    continue;
                tier.push_back(s);
                if (!Contains(avoid, s.classId))
                    fresh.push_back(s);
            }
            std::vector<DcTestComp::Slot> const& candidates = fresh.empty() ? tier : fresh;

            DcTestComp::Slot const& pick = candidates[NextRand(state) % candidates.size()];
            out.classId = pick.classId;
            out.specName = pick.specName;
            out.fallbackSpec = pick.fallbackSpec;
            ++used[pick.classId];
            return out;
        }

        // Is seat `i` of `n` one of the `healers` healer seats? Spreads the
        // healers evenly through the side instead of stacking them at the
        // front or the back, so the start-critical prefix carries its share —
        // a battleground that pops at 20v20 should not pop with every healer
        // still logging in.
        bool IsHealerSeat(std::uint32_t i, std::uint32_t n, std::uint32_t healers)
        {
            if (!n || !healers)
                return false;
            return ((i + 1) * healers) / n > (i * healers) / n;
        }
    }

    char const* TeamName(std::uint8_t team)
    {
        return team == kTeamAlliance ? "Alliance" : team == kTeamHorde ? "Horde" : "?";
    }

    std::uint32_t HealerQuota(std::uint32_t maxPerTeam, std::uint32_t configured)
    {
        if (configured)
            return configured;
        return (maxPerTeam + kSeatsPerHealer - 1) / kSeatsPerHealer;
    }

    std::pair<std::uint32_t, std::uint32_t> LevelWindow(Request const& req)
    {
        std::uint32_t const bmin = req.bracketMinLevel;
        std::uint32_t const bmax = std::max(req.bracketMinLevel, req.bracketMaxLevel);
        std::uint32_t const pl = std::clamp(req.playerLevel, bmin, bmax);

        std::uint32_t const lo = std::max(bmin, pl > req.levelSpread ? pl - req.levelSpread : 0u);
        std::uint32_t const hi = std::min(bmax, pl + req.levelSpread);
        return {lo, hi};
    }

    Result Plan(Request const& req)
    {
        Result r;

        if (req.maxPerTeam == 0 || req.minPerTeam > req.maxPerTeam)
        {
            r.detail = "battleground template has min " + std::to_string(req.minPerTeam) +
                       " / max " + std::to_string(req.maxPerTeam) + " per team";
            return r;
        }
        if (req.bracketMinLevel == 0 || req.bracketMinLevel > req.bracketMaxLevel)
        {
            r.detail = "bracket " + std::to_string(req.bracketMinLevel) + "-" +
                       std::to_string(req.bracketMaxLevel) + " is not a level range";
            return r;
        }
        if (req.playerTeam >= kTeams)
        {
            r.detail = "the queuing player has no faction";
            return r;
        }

        std::uint32_t minNeed[kTeams] = {0, 0};
        for (std::uint8_t t = 0; t < kTeams; ++t)
        {
            SideCounts const& side = req.sides[t];
            std::uint32_t const present = side.humansQueued + side.botsInFlight;
            r.need[t] = present < req.maxPerTeam ? req.maxPerTeam - present : 0;
            minNeed[t] = present < req.minPerTeam ? req.minPerTeam - present : 0;
            r.planned[t] = std::min(r.need[t], side.poolFree);
            r.shortfall[t] = r.need[t] - r.planned[t];

            if (r.planned[t] < minNeed[t])
            {
                r.kind = Kind::Unresolvable;
                r.detail = std::string("the ") + TeamName(t) + " pool has " +
                           std::to_string(side.poolFree) + " free character(s); the side needs " +
                           std::to_string(minNeed[t]) + " more to reach the battleground's minimum of " +
                           std::to_string(req.minPerTeam);
                return r;
            }
        }

        if (!r.planned[kTeamAlliance] && !r.planned[kTeamHorde])
        {
            r.kind = Kind::NothingToDo;
            r.detail = "both sides are already at " + std::to_string(req.maxPerTeam);
            return r;
        }

        auto const [lo, hi] = LevelWindow(req);

        // A different stream from the pool-character draw the job runs off
        // the same seed, so which class lands where is not a tell for which
        // character was picked.
        std::uint32_t state = req.seed ^ 0x6a09e667u;

        std::vector<Slot> perSide[kTeams];
        for (std::uint8_t t = 0; t < kTeams; ++t)
        {
            SideCounts const& side = req.sides[t];
            std::uint32_t const quota = HealerQuota(req.maxPerTeam, req.healersPerSide);
            std::uint32_t const healers =
                std::min(r.planned[t], quota > side.healerBots ? quota - side.healerBots : 0u);

            std::map<std::uint8_t, std::uint32_t> used;
            perSide[t].reserve(r.planned[t]);
            for (std::uint32_t i = 0; i < r.planned[t]; ++i)
            {
                std::uint32_t const level = lo + NextRand(state) % (hi - lo + 1);
                char const* role = IsHealerSeat(i, r.planned[t], healers) ? "heal" : "dps";
                Slot s = Draw(role, RosterFor(level), used, req.avoidClasses, state);
                s.team = t;
                s.level = level;
                s.startCritical = i < minNeed[t];
                perSide[t].push_back(s);
            }
        }

        // Start-critical first, alternating sides, so the match can pop at
        // min-v-min as early as the provisioning ration allows. The side
        // further from its minimum goes first; on a tie, the player's
        // opponents — the side with no human on it is the one the player is
        // waiting on.
        std::uint8_t first = req.playerTeam == kTeamAlliance ? kTeamHorde : kTeamAlliance;
        if (minNeed[kTeamAlliance] != minNeed[kTeamHorde])
            first = minNeed[kTeamAlliance] > minNeed[kTeamHorde] ? kTeamAlliance : kTeamHorde;
        std::uint8_t const second = first == kTeamAlliance ? kTeamHorde : kTeamAlliance;

        r.slots.reserve(perSide[0].size() + perSide[1].size());
        for (bool const critical : {true, false})
        {
            std::size_t idx[kTeams] = {0, 0};
            auto next = [&](std::uint8_t t) -> bool
            {
                while (idx[t] < perSide[t].size() && perSide[t][idx[t]].startCritical != critical)
                    ++idx[t];
                if (idx[t] >= perSide[t].size())
                    return false;
                r.slots.push_back(perSide[t][idx[t]++]);
                return true;
            };
            bool more = true;
            while (more)
            {
                bool const a = next(first);
                bool const b = next(second);
                more = a || b;
            }
        }

        r.kind = Kind::Ok;
        return r;
    }
}
