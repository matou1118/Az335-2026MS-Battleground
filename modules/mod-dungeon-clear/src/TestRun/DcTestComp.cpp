/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "TestRun/DcTestComp.h"

#include <map>
#include <set>

namespace DcTestComp
{
    namespace
    {
        // splitmix32: a tiny, self-contained, deterministic PRNG. We can't lean
        // on the project's global urand()/rand32() here because BuildComp must
        // be PURE (same seed -> same comp, for replay and unit tests); those
        // draw from shared world state. This is comp selection, not gameplay
        // randomness, so distribution quality is irrelevant.
        std::uint32_t NextRand(std::uint32_t& state)
        {
            state += 0x9e3779b9u;
            std::uint32_t z = state;
            z = (z ^ (z >> 16)) * 0x21f0aaadu;
            z = (z ^ (z >> 15)) * 0x735a2d97u;
            return z ^ (z >> 15);
        }

        constexpr std::uint8_t kDeathKnightClassId = 6;

        // Whether `roster` admits this pool entry. Filtering the shared pools
        // beats holding a second copy of each: the death-knight rows are last,
        // so dropping them leaves the pre-Wrath candidate list byte-identical
        // to what it was before the class was added and every recorded seed
        // still replays its original comp.
        bool Eligible(Slot const& slot, Roster roster)
        {
            return roster == Roster::WithDeathKnights || slot.classId != kDeathKnightClassId;
        }

        // Draw one entry from `pool` whose class is eligible and not already
        // used, advancing the PRNG. Callers guarantee at least one such entry
        // exists.
        Slot Draw(Slot const* pool, std::size_t count, std::set<std::uint8_t> const& used,
                  Roster roster, std::uint32_t& state)
        {
            std::vector<Slot> candidates;
            candidates.reserve(count);
            for (std::size_t i = 0; i < count; ++i)
                if (Eligible(pool[i], roster) && used.find(pool[i].classId) == used.end())
                    candidates.push_back(pool[i]);

            // Defensive: should never happen given the pool sizes, but returning
            // a valid slot beats indexing an empty vector. It still has to
            // respect `roster` — falling back to pool[0] blindly would be fine
            // today only because no pool LEADS with a death knight.
            if (candidates.empty())
            {
                for (std::size_t i = 0; i < count; ++i)
                    if (Eligible(pool[i], roster))
                        return pool[i];
                return pool[0];
            }

            return candidates[NextRand(state) % candidates.size()];
        }

        // Raid-scale draw: duplicates allowed, but always from the LEAST-used
        // classes of the pool, so 25 slots over 9 classes spread as evenly as
        // the role pools permit instead of stacking one lucky class.
        Slot DrawSpread(Slot const* pool, std::size_t count,
                        std::map<std::uint8_t, std::size_t>& classUse,
                        Roster roster, std::uint32_t& state)
        {
            std::size_t best = SIZE_MAX;
            for (std::size_t i = 0; i < count; ++i)
            {
                if (!Eligible(pool[i], roster))
                    continue;
                auto const it = classUse.find(pool[i].classId);
                std::size_t const uses = it == classUse.end() ? 0 : it->second;
                if (uses < best)
                    best = uses;
            }
            std::vector<Slot> candidates;
            candidates.reserve(count);
            for (std::size_t i = 0; i < count; ++i)
            {
                if (!Eligible(pool[i], roster))
                    continue;
                auto const it = classUse.find(pool[i].classId);
                std::size_t const uses = it == classUse.end() ? 0 : it->second;
                if (uses == best)
                    candidates.push_back(pool[i]);
            }
            Slot const pick = candidates[NextRand(state) % candidates.size()];
            ++classUse[pick.classId];
            return pick;
        }
    }

    Quota RoleQuota(std::size_t size)
    {
        if (size < kMinPartySize)
            size = kMinPartySize;
        if (size > kMaxPartySize)
            size = kMaxPartySize;

        Quota q{};
        q.tanks = size <= 9 ? 1 : size <= 15 ? 2 : size <= 30 ? 3 : 4;
        q.healers = size / 4;
        if (q.healers == 0)
            q.healers = 1;
        // Never quota away the last DPS on tiny sizes (size 2 = tank + healer).
        while (q.tanks + q.healers > size)
            q.healers > 1 ? --q.healers : --q.tanks;
        q.dps = size - q.tanks - q.healers;
        return q;
    }

    std::array<Slot, kPartySize> BuildComp(std::uint32_t seed, Roster roster)
    {
        std::uint32_t state = seed;
        std::set<std::uint8_t> used;
        std::array<Slot, kPartySize> comp{};

        comp[0] = Draw(kTankPool, std::size(kTankPool), used, roster, state);
        used.insert(comp[0].classId);

        comp[1] = Draw(kHealPool, std::size(kHealPool), used, roster, state);
        used.insert(comp[1].classId);

        for (std::size_t i = 2; i < kPartySize; ++i)
        {
            comp[i] = Draw(kDpsPool, std::size(kDpsPool), used, roster, state);
            used.insert(comp[i].classId);
        }

        return comp;
    }

    std::vector<Slot> BuildComp(std::uint32_t seed, std::size_t size, Roster roster)
    {
        Quota const q = RoleQuota(size);
        std::uint32_t state = seed;
        std::map<std::uint8_t, std::size_t> classUse;
        std::vector<Slot> comp;
        comp.reserve(q.tanks + q.healers + q.dps);

        for (std::size_t i = 0; i < q.tanks; ++i)
            comp.push_back(DrawSpread(kTankPool, std::size(kTankPool), classUse, roster, state));
        for (std::size_t i = 0; i < q.healers; ++i)
            comp.push_back(DrawSpread(kHealPool, std::size(kHealPool), classUse, roster, state));
        for (std::size_t i = 0; i < q.dps; ++i)
            comp.push_back(DrawSpread(kDpsPool, std::size(kDpsPool), classUse, roster, state));
        return comp;
    }

    std::vector<Slot> RolePool(std::string_view role, Roster roster)
    {
        Slot const* pool = nullptr;
        std::size_t count = 0;
        if (role == "tank")
        {
            pool = kTankPool;
            count = std::size(kTankPool);
        }
        else if (role == "heal")
        {
            pool = kHealPool;
            count = std::size(kHealPool);
        }
        else if (role == "dps")
        {
            pool = kDpsPool;
            count = std::size(kDpsPool);
        }
        else
            return {};

        std::vector<Slot> out;
        out.reserve(count);
        for (std::size_t i = 0; i < count; ++i)
            if (Eligible(pool[i], roster))
                out.push_back(pool[i]);
        return out;
    }
}
