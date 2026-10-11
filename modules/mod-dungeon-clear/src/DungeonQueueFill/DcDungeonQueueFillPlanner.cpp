/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DungeonQueueFill/DcDungeonQueueFillPlanner.h"

#include <algorithm>
#include <cstring>
#include <set>

namespace DcDungeonQueueFillPlanner
{
    // splitmix32, the same tiny deterministic PRNG DcTestComp::BuildComp uses
    // and for the same reason: the draw must be pure (same seed, same classes)
    // so a fill is reproducible and the tests are not flaky. The project's
    // urand()/rand32() draw from shared world state.
    std::uint32_t NextRand(std::uint32_t& state)
    {
        state += 0x9e3779b9u;
        std::uint32_t z = state;
        z = (z ^ (z >> 16)) * 0x21f0aaadu;
        z = (z ^ (z >> 15)) * 0x735a2d97u;
        return z ^ (z >> 15);
    }

    namespace
    {
        // Every way of putting each human on exactly one role they ticked
        // without breaking the 1 tank / 1 healer / 3 dps quota. Enumerated
        // rather than picked greedily because the choice is now random: a
        // greedy random pass can paint itself into a corner (two "tank or dps"
        // players both rolling tank) and refuse a party the roles could plainly
        // have made. The quota prunes as it descends, so the recursion is
        // bounded by the five seats however many humans are passed in.
        void EnumerateAssignments(std::vector<Human> const& humans, std::size_t i,
                                  std::size_t tanks, std::size_t healers, std::size_t dps,
                                  std::vector<std::uint8_t>& current,
                                  std::vector<std::vector<std::uint8_t>>& out)
        {
            if (i == humans.size())
            {
                out.push_back(current);
                return;
            }

            std::uint8_t const roles[3] = {kRoleTank, kRoleHealer, kRoleDamage};
            for (std::uint8_t const role : roles)
            {
                if (!(humans[i].roleMask & role))
                    continue;

                std::size_t const t = tanks + (role == kRoleTank ? 1 : 0);
                std::size_t const h = healers + (role == kRoleHealer ? 1 : 0);
                std::size_t const d = dps + (role == kRoleDamage ? 1 : 0);
                if (t > kTanksNeeded || h > kHealersNeeded || d > kDpsNeeded)
                    continue;

                current[i] = role;
                EnumerateAssignments(humans, i + 1, t, h, d, current, out);
            }
            current[i] = kRoleNone;
        }

        bool Contains(std::vector<std::uint8_t> const& v, std::uint8_t c)
        {
            return std::find(v.begin(), v.end(), c) != v.end();
        }

        // Draw one class for `role` that is not already in the party and,
        // where the pool allows it, not one the player's last fill fielded.
        //
        // Distinct classes are preferred for the same reason a test comp
        // prefers them — it maximises what the party exercises and keeps the
        // addclass-pool draw from needing several characters of one class —
        // but unlike a test comp this one may NOT fail. Hence three tiers,
        // each strictly weaker than the last: fresh classes first, then any
        // unused class, then the whole pool. The tank pool is three classes
        // deep before Wrath, so a party that already holds two of them has to
        // be allowed to repeat last run's third one rather than field no tank.
        Slot Draw(char const* role, std::set<std::uint8_t>& used,
                  std::vector<std::uint8_t> const& avoid, DcTestComp::Roster roster,
                  std::uint32_t& state)
        {
            std::vector<DcTestComp::Slot> const pool = DcTestComp::RolePool(role, roster);

            std::vector<DcTestComp::Slot> unused;
            std::vector<DcTestComp::Slot> candidates;
            unused.reserve(pool.size());
            candidates.reserve(pool.size());
            for (DcTestComp::Slot const& s : pool)
            {
                if (used.find(s.classId) != used.end())
                    continue;
                unused.push_back(s);
                if (!Contains(avoid, s.classId))
                    candidates.push_back(s);
            }
            if (candidates.empty())
                candidates = unused;
            if (candidates.empty())
                candidates = pool;

            Slot out;
            if (candidates.empty())  // only if the role token is unknown
                return out;

            DcTestComp::Slot const& pick = candidates[NextRand(state) % candidates.size()];
            out.role = role;
            out.roleMask = MaskForRole(role);
            out.classId = pick.classId;
            out.specName = pick.specName;
            out.fallbackSpec = pick.fallbackSpec;
            used.insert(pick.classId);
            return out;
        }
    }

    std::uint8_t MaskForRole(char const* role)
    {
        if (!role)
            return kRoleNone;
        if (std::strcmp(role, "tank") == 0)
            return kRoleTank;
        if (std::strcmp(role, "heal") == 0)
            return kRoleHealer;
        if (std::strcmp(role, "dps") == 0)
            return kRoleDamage;
        return kRoleNone;
    }

    std::vector<std::uint8_t> AssignHumanRoles(std::vector<Human> const& humans,
                                               std::uint32_t seed)
    {
        // One role each, drawn at random. A player who ticks "tank or dps" is
        // telling the queue either is fine, not that they will cover both — the
        // stock matchmaker picks one of them for that player and so do we,
        // which is what keeps the deficit at four bots for a solo queue however
        // many boxes were ticked.
        std::vector<std::uint8_t> current(humans.size(), kRoleNone);
        std::vector<std::vector<std::uint8_t>> legal;
        EnumerateAssignments(humans, 0, 0, 0, 0, current, legal);

        // No legal assignment: somebody's ticked roles are all spoken for by
        // players scarcer than them. Say nothing rather than half-assign —
        // Plan reads the all-none result as Unresolvable.
        if (legal.empty())
            return std::vector<std::uint8_t>(humans.size(), kRoleNone);

        std::uint32_t state = seed;
        return legal[NextRand(state) % legal.size()];
    }

    Result Plan(std::vector<Human> const& humans, std::uint32_t level, std::uint32_t expansion,
                std::uint32_t seed, std::vector<std::uint8_t> const& avoidClasses)
    {
        Result r;

        if (humans.empty())
        {
            r.kind = Kind::NoHumans;
            r.detail = "no players in the queue to fill around";
            return r;
        }
        if (humans.size() >= kPartySize)
        {
            r.kind = Kind::PartyFull;
            r.detail = "the party is already " + std::to_string(kPartySize) + " strong";
            return r;
        }

        r.humanRoles = AssignHumanRoles(humans, seed);

        std::size_t haveTank = 0, haveHeal = 0, haveDps = 0;
        for (std::uint8_t const mask : r.humanRoles)
        {
            if (mask == kRoleTank)
                ++haveTank;
            else if (mask == kRoleHealer)
                ++haveHeal;
            else if (mask == kRoleDamage)
                ++haveDps;
        }

        std::size_t const needTank = kTanksNeeded - haveTank;
        std::size_t const needHeal = kHealersNeeded - haveHeal;
        std::size_t const needDps = kDpsNeeded - haveDps;
        std::size_t const need = needTank + needHeal + needDps;

        // The two counts must agree: every human occupies a party slot whether
        // or not their ticked roles could be honoured. They disagree exactly
        // when a human was left unassigned — two tank-only players, say — which
        // is a party the stock matchmaker could never complete either. Refuse
        // it by name rather than queueing bots into a match that cannot form.
        if (need != kPartySize - humans.size())
        {
            r.kind = Kind::Unresolvable;
            r.detail = "the queued roles cannot make a 1 tank / 1 healer / 3 dps party";
            return r;
        }

        DcTestComp::Roster const roster = RosterFor(level, expansion);
        // A different stream from the one the role draw ran on, so the class a
        // fill fields is not a tell for the role its player was given.
        std::uint32_t state = seed ^ 0x5bf03635u;
        std::set<std::uint8_t> used;

        r.slots.reserve(need);
        for (std::size_t i = 0; i < needTank; ++i)
            r.slots.push_back(Draw("tank", used, avoidClasses, roster, state));
        for (std::size_t i = 0; i < needHeal; ++i)
            r.slots.push_back(Draw("heal", used, avoidClasses, roster, state));
        for (std::size_t i = 0; i < needDps; ++i)
            r.slots.push_back(Draw("dps", used, avoidClasses, roster, state));

        r.kind = Kind::Ok;
        return r;
    }
}
