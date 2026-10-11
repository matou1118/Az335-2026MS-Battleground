/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "gtest/gtest.h"

#include "TestRun/DcTestComp.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <string_view>
#include <vector>

using DcTestComp::BuildComp;
using DcTestComp::kPartySize;
using DcTestComp::RolePool;
using DcTestComp::Roster;
using DcTestComp::Slot;

namespace
{
    constexpr std::uint8_t kDk = 6;

    // Class 10 does not exist on 3.3.5a; everything else 1..11 is pooled
    // somewhere, death knight (6) included.
    bool IsKnownClass(std::uint8_t c)
    {
        return c == 1 || c == 2 || c == 3 || c == 4 || c == 5 || c == kDk ||
               c == 7 || c == 8 || c == 9 || c == 11;
    }

    bool HasDeathKnight(std::vector<Slot> const& comp)
    {
        return std::any_of(comp.begin(), comp.end(),
                           [](Slot const& s) { return s.classId == kDk; });
    }

    std::vector<Slot> Five(std::uint32_t seed, Roster roster)
    {
        auto const c = BuildComp(seed, roster);
        return { c.begin(), c.end() };
    }
}

// Same seed -> byte-identical comp (classes and specs). This is what lets a
// bug-tripping run be replayed via `.dc test start <d> seed=N`.
TEST(DcTestComp, DeterministicForSameSeed)
{
    for (std::uint32_t seed : {1u, 7u, 42u, 1000u, 0xABCDEF12u})
    {
        auto a = BuildComp(seed, Roster::NoDeathKnights);
        auto b = BuildComp(seed, Roster::NoDeathKnights);
        for (std::size_t i = 0; i < kPartySize; ++i)
        {
            EXPECT_EQ(a[i].classId, b[i].classId) << "seed=" << seed << " slot=" << i;
            EXPECT_STREQ(a[i].specName, b[i].specName) << "seed=" << seed << " slot=" << i;
        }
    }
}

// Slot 0 is always the tank (leader), slot 1 the healer, slots 2-4 DPS.
TEST(DcTestComp, RoleLayoutIsStable)
{
    for (std::uint32_t seed = 0; seed < 500; ++seed)
    {
        auto c = BuildComp(seed, Roster::NoDeathKnights);
        EXPECT_STREQ(c[0].role, "tank") << "seed=" << seed;
        EXPECT_STREQ(c[1].role, "heal") << "seed=" << seed;
        for (std::size_t i = 2; i < kPartySize; ++i)
            EXPECT_STREQ(c[i].role, "dps") << "seed=" << seed << " slot=" << i;
    }
}

// All five bots are on distinct, known classes with a named spec.
TEST(DcTestComp, DistinctKnownClassesWithSpecs)
{
    for (std::uint32_t seed = 0; seed < 500; ++seed)
    {
        auto c = BuildComp(seed, Roster::NoDeathKnights);
        std::set<std::uint8_t> classes;
        for (std::size_t i = 0; i < kPartySize; ++i)
        {
            EXPECT_TRUE(IsKnownClass(c[i].classId))
                << "seed=" << seed << " classId=" << int(c[i].classId);
            ASSERT_NE(c[i].specName, nullptr);
            EXPECT_NE(std::string_view(c[i].specName), std::string_view())
                << "empty spec, seed=" << seed << " slot=" << i;
            classes.insert(c[i].classId);
        }
        EXPECT_EQ(classes.size(), kPartySize) << "duplicate class, seed=" << seed;
    }
}

// The point of the feature: comps actually vary. Over many seeds we expect
// more than one tank class, more than one healer class, and a healthy spread
// of DPS classes — otherwise the "randomisation" is stuck on one shape.
TEST(DcTestComp, ActuallyVaries)
{
    std::set<std::uint8_t> tanks, heals, dps;
    for (std::uint32_t seed = 0; seed < 500; ++seed)
    {
        auto c = BuildComp(seed, Roster::NoDeathKnights);
        tanks.insert(c[0].classId);
        heals.insert(c[1].classId);
        for (std::size_t i = 2; i < kPartySize; ++i)
            dps.insert(c[i].classId);
    }
    EXPECT_GT(tanks.size(), 1u);
    EXPECT_GT(heals.size(), 1u);
    EXPECT_GE(dps.size(), 5u);
}

TEST(DcTestComp, RolePoolLookup)
{
    EXPECT_FALSE(RolePool("tank", Roster::NoDeathKnights).empty());
    EXPECT_FALSE(RolePool("heal", Roster::NoDeathKnights).empty());
    EXPECT_FALSE(RolePool("dps", Roster::NoDeathKnights).empty());
    EXPECT_TRUE(RolePool("bogus", Roster::NoDeathKnights).empty());

    // Every pool entry carries the role token it was fetched under.
    for (Slot const& s : RolePool("tank", Roster::WithDeathKnights))
        EXPECT_STREQ(s.role, "tank");
    for (Slot const& s : RolePool("heal", Roster::WithDeathKnights))
        EXPECT_STREQ(s.role, "heal");
    for (Slot const& s : RolePool("dps", Roster::WithDeathKnights))
        EXPECT_STREQ(s.role, "dps");
}

// --- Sized (raid) comps — raid-support Plan D ------------------------------

TEST(DcTestComp, RoleQuotaTable)
{
    using DcTestComp::RoleQuota;
    auto q = RoleQuota(5);
    EXPECT_EQ(q.tanks, 1u);
    EXPECT_EQ(q.healers, 1u);
    EXPECT_EQ(q.dps, 3u);

    q = RoleQuota(10);
    EXPECT_EQ(q.tanks, 2u);
    EXPECT_EQ(q.healers, 2u);
    EXPECT_EQ(q.dps, 6u);

    q = RoleQuota(25);
    EXPECT_EQ(q.tanks, 3u);
    EXPECT_EQ(q.healers, 6u);
    EXPECT_EQ(q.dps, 16u);

    q = RoleQuota(40);
    EXPECT_EQ(q.tanks, 4u);
    EXPECT_EQ(q.healers, 10u);
    EXPECT_EQ(q.dps, 26u);

    // Tiny sizes never quota away the whole party.
    q = RoleQuota(2);
    EXPECT_EQ(q.tanks + q.healers + q.dps, 2u);
    EXPECT_GE(q.tanks, 1u);
}

TEST(DcTestComp, SizedCompIsDeterministicAndQuotaed)
{
    auto const a = DcTestComp::BuildComp(1234u, 25, Roster::NoDeathKnights);
    auto const b = DcTestComp::BuildComp(1234u, 25, Roster::NoDeathKnights);
    ASSERT_EQ(a.size(), 25u);
    for (std::size_t i = 0; i < a.size(); ++i)
    {
        EXPECT_EQ(a[i].classId, b[i].classId) << i;
        EXPECT_STREQ(a[i].specName, b[i].specName) << i;
    }
    std::size_t tanks = 0, heals = 0, dps = 0;
    for (auto const& s : a)
    {
        if (std::string_view(s.role) == "tank") ++tanks;
        else if (std::string_view(s.role) == "heal") ++heals;
        else ++dps;
    }
    EXPECT_EQ(tanks, 3u);
    EXPECT_EQ(heals, 6u);
    EXPECT_EQ(dps, 16u);
}

TEST(DcTestComp, SizedCompSpreadsDuplicatesEvenly)
{
    // 6 healers over a 4-class heal pool (priest twice via disc/holy): the
    // least-used draw means no class is used 3+ times while another is unused.
    auto const comp = DcTestComp::BuildComp(777u, 25, Roster::NoDeathKnights);
    std::map<std::uint8_t, std::size_t> healUse;
    for (auto const& s : comp)
        if (std::string_view(s.role) == "heal")
            ++healUse[s.classId];
    std::size_t maxUse = 0, minUse = SIZE_MAX;
    for (auto const& kv : healUse)
    {
        maxUse = std::max(maxUse, kv.second);
        minUse = std::min(minUse, kv.second);
    }
    EXPECT_LE(maxUse - minUse, 1u);  // even spread across the classes drawn
}

TEST(DcTestComp, SizeIsClamped)
{
    EXPECT_EQ(DcTestComp::BuildComp(1u, 1, Roster::NoDeathKnights).size(), DcTestComp::kMinPartySize);
    EXPECT_EQ(DcTestComp::BuildComp(1u, 99, Roster::NoDeathKnights).size(), DcTestComp::kMaxPartySize);
}

// --- Death knights (WotLK only) --------------------------------------------

// The gate: a classic or TBC run must never field a death knight, whatever
// the seed and whatever the size. Below level 55 the class has no abilities at
// all, and the factory levels every test bot to the run's own level.
TEST(DcTestComp, PreWrathNeverDrawsDeathKnights)
{
    for (std::uint32_t seed = 0; seed < 500; ++seed)
    {
        EXPECT_FALSE(HasDeathKnight(Five(seed, Roster::NoDeathKnights)))
            << "death knight leaked into a pre-Wrath 5-man, seed=" << seed;
        EXPECT_FALSE(HasDeathKnight(BuildComp(seed, 25, Roster::NoDeathKnights)))
            << "death knight leaked into a pre-Wrath raid, seed=" << seed;
    }
}

// The feature: a WotLK run draws them, in BOTH roles they can fill. Blood
// tanks, frost and unholy are DPS; there is no death-knight healer, so the
// heal slot must never be one.
TEST(DcTestComp, WrathDrawsDeathKnightsAsTankAndDps)
{
    bool asTank = false, asDps = false;
    for (std::uint32_t seed = 0; seed < 500; ++seed)
    {
        auto const c = BuildComp(seed, Roster::WithDeathKnights);
        if (c[0].classId == kDk)
            asTank = true;
        EXPECT_NE(c[1].classId, kDk) << "death knight drawn as a healer, seed=" << seed;
        for (std::size_t i = 2; i < kPartySize; ++i)
            if (c[i].classId == kDk)
                asDps = true;
    }
    EXPECT_TRUE(asTank) << "no death-knight tank over 500 seeds";
    EXPECT_TRUE(asDps) << "no death-knight DPS over 500 seeds";
}

// Adding the class must not have moved any OTHER draw. These comps were
// computed with the pre-death-knight pools, so a recorded seed from before the
// change still replays the party it originally rolled — which is the whole
// point of recording it (`.dc test start <d> seed=N`).
TEST(DcTestComp, PreWrathSeedsStillReplayTheirOriginalComp)
{
    struct Golden
    {
        std::uint32_t seed;
        std::uint8_t classId[kPartySize];
        char const* spec[kPartySize];
    };
    static constexpr Golden kGolden[] = {
        { 0x00000001u, { 1, 5, 8, 4, 9 },
          { "prot pve", "holy pve", "arcane pve", "combat pve", "destro pve" } },
        { 0x00000007u, { 2, 5, 1, 7, 4 },
          { "prot pve", "disc pve", "arms pve", "enh pve", "combat pve" } },
        { 0x0000002Au, { 11, 7, 9, 4, 3 },
          { "bear pve", "resto pve", "destro pve", "subtlety pve", "mm pve" } },
        { 0x000003E8u, { 11, 2, 8, 4, 9 },
          { "bear pve", "holy pve", "arcane pve", "subtlety pve", "destro pve" } },
        { 0xABCDEF12u, { 1, 11, 4, 9, 5 },
          { "prot pve", "resto pve", "as pve", "demo pve", "shadow pve" } },
    };

    for (Golden const& g : kGolden)
    {
        auto const c = BuildComp(g.seed, Roster::NoDeathKnights);
        for (std::size_t i = 0; i < kPartySize; ++i)
        {
            EXPECT_EQ(c[i].classId, g.classId[i]) << "seed=" << g.seed << " slot=" << i;
            EXPECT_STREQ(c[i].specName, g.spec[i]) << "seed=" << g.seed << " slot=" << i;
        }
    }
}

// A substitution (drawn class has no free addclass character) must obey the
// same gate as the draw, or a classic run could still end up with one.
TEST(DcTestComp, RolePoolRespectsTheRoster)
{
    for (char const* role : { "tank", "heal", "dps" })
    {
        auto const without = RolePool(role, Roster::NoDeathKnights);
        auto const with = RolePool(role, Roster::WithDeathKnights);
        EXPECT_FALSE(HasDeathKnight(without)) << role;
        EXPECT_GE(with.size(), without.size()) << role;
        // Tank and DPS gain death knights; there is no death-knight healer.
        EXPECT_EQ(HasDeathKnight(with), std::string_view(role) != "heal") << role;
    }
}

// Wrath comps stay replayable too — same seed, same party.
TEST(DcTestComp, WrathCompIsDeterministic)
{
    for (std::uint32_t seed : { 3u, 11u, 99u, 0x1234u })
    {
        auto const a = BuildComp(seed, Roster::WithDeathKnights);
        auto const b = BuildComp(seed, Roster::WithDeathKnights);
        for (std::size_t i = 0; i < kPartySize; ++i)
        {
            EXPECT_EQ(a[i].classId, b[i].classId) << "seed=" << seed << " slot=" << i;
            EXPECT_STREQ(a[i].specName, b[i].specName) << "seed=" << seed << " slot=" << i;
        }
    }
}

// Distinct-class and role-layout invariants hold with death knights in play.
TEST(DcTestComp, WrathCompKeepsDistinctClassesAndRoleLayout)
{
    for (std::uint32_t seed = 0; seed < 500; ++seed)
    {
        auto const c = BuildComp(seed, Roster::WithDeathKnights);
        EXPECT_STREQ(c[0].role, "tank") << "seed=" << seed;
        EXPECT_STREQ(c[1].role, "heal") << "seed=" << seed;
        std::set<std::uint8_t> classes;
        for (std::size_t i = 0; i < kPartySize; ++i)
        {
            EXPECT_TRUE(IsKnownClass(c[i].classId))
                << "seed=" << seed << " classId=" << int(c[i].classId);
            classes.insert(c[i].classId);
        }
        EXPECT_EQ(classes.size(), kPartySize) << "duplicate class, seed=" << seed;
    }
}
