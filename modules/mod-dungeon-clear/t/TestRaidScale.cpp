/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "gtest/gtest.h"

#include "Ai/Dungeon/DungeonClear/Util/DcFormation.h"
#include "Ai/Dungeon/DungeonClear/Util/DcRezDecision.h"

// Raid-scale kernels (raid-support plan, Plan B): the parallel rez pairing,
// the wipe-fraction verdict + entrance-regroup outcome, and the formation ring
// geometry.

namespace
{
    DcRezDecision::Member Mk(bool dead, bool rezClass, bool healer, bool tank,
                             bool isBot = true)
    {
        DcRezDecision::Member m;
        m.isDead = dead;
        m.canRezClass = rezClass;
        m.isHealerRole = healer;
        m.isTankRole = tank;
        m.isBot = isBot;
        return m;
    }
}

// --- PickPairs -------------------------------------------------------------

TEST(DcRaidRezTest, PairsAreDeterministicAndDistinct)
{
    // idx0 living healer-rezzer, idx1 living non-healer rezzer, idx2 dead tank,
    // idx3 dead healer, idx4 dead dps.
    std::vector<DcRezDecision::Member> members{
        Mk(false, true, true, false),
        Mk(false, true, false, false),
        Mk(true, false, false, true),
        Mk(true, true, true, false),
        Mk(true, false, false, false),
    };
    auto const pairs = DcRezDecision::PickPairs(members);
    // Two rezzers -> two pairs; targets healer-first then tank; rezzers
    // healer-first.
    ASSERT_EQ(pairs.size(), 2u);
    EXPECT_EQ(pairs[0].first, 0);
    EXPECT_EQ(pairs[0].second, 3);  // dead healer first
    EXPECT_EQ(pairs[1].first, 1);
    EXPECT_EQ(pairs[1].second, 2);  // dead tank second
}

TEST(DcRaidRezTest, PairsMatchSingleElection)
{
    std::vector<DcRezDecision::Member> members{
        Mk(false, true, false, false),   // non-healer rezzer
        Mk(true, false, false, false),   // dead dps
        Mk(false, true, true, false),    // healer rezzer — must lead
    };
    DcRezDecision::Inputs in;
    in.nowMs = 1000;
    auto const r = DcRezDecision::Decide(in, members);
    ASSERT_EQ(r.outcome, DcRezDecision::Outcome::Hold);
    ASSERT_EQ(r.reason, DcRezDecision::Reason::Recovering);
    ASSERT_FALSE(r.pairs.empty());
    EXPECT_EQ(r.pairs[0].first, r.rezzerIdx);
    EXPECT_EQ(r.pairs[0].second, r.targetIdx);
    EXPECT_EQ(r.rezzerIdx, 2);
}

TEST(DcRaidRezTest, LeftoverCorpsesStayUnpaired)
{
    std::vector<DcRezDecision::Member> members{
        Mk(false, true, false, false),
        Mk(true, false, false, false),
        Mk(true, false, false, false),
        Mk(true, false, false, false),
    };
    auto const pairs = DcRezDecision::PickPairs(members);
    ASSERT_EQ(pairs.size(), 1u);  // one rezzer, one wave at a time
}

TEST(DcRaidRezTest, HumansAreNeverPairedAsRezzers)
{
    std::vector<DcRezDecision::Member> members{
        Mk(false, true, true, false, /*isBot*/ false),  // human healer
        Mk(true, false, false, false),
    };
    EXPECT_TRUE(DcRezDecision::PickPairs(members).empty());
}

// --- Wipe fraction + Regroup ----------------------------------------------

TEST(DcRaidRezTest, FullWipeStillDisablesWithoutRegroupFlag)
{
    std::vector<DcRezDecision::Member> members{
        Mk(true, true, true, false), Mk(true, false, false, true)};
    DcRezDecision::Inputs in;
    in.nowMs = 1000;
    auto const r = DcRezDecision::Decide(in, members);
    EXPECT_EQ(r.outcome, DcRezDecision::Outcome::Disable);
    EXPECT_EQ(r.reason, DcRezDecision::Reason::Wipe);
}

TEST(DcRaidRezTest, FullWipeRegroupsWhenAsked)
{
    std::vector<DcRezDecision::Member> members{
        Mk(true, true, true, false), Mk(true, false, false, true)};
    DcRezDecision::Inputs in;
    in.nowMs = 1000;
    in.regroupOnWipe = true;
    auto const r = DcRezDecision::Decide(in, members);
    EXPECT_EQ(r.outcome, DcRezDecision::Outcome::Regroup);
    EXPECT_EQ(r.reason, DcRezDecision::Reason::Wipe);
}

TEST(DcRaidRezTest, FractionWipeFiresWithSurvivorsQuiet)
{
    // 9 of 10 dead (90%), nobody engaged, fraction 90 -> wipe, even though a
    // rez-class survivor exists.
    std::vector<DcRezDecision::Member> members;
    for (int i = 0; i < 9; ++i)
        members.push_back(Mk(true, false, false, false));
    members.push_back(Mk(false, true, true, false));
    DcRezDecision::Inputs in;
    in.nowMs = 1000;
    in.wipeFractionPct = 90;
    in.regroupOnWipe = true;
    auto const r = DcRezDecision::Decide(in, members);
    EXPECT_EQ(r.outcome, DcRezDecision::Outcome::Regroup);
    EXPECT_EQ(r.reason, DcRezDecision::Reason::Wipe);
}

TEST(DcRaidRezTest, EngagementHoldsTheFractionVerdictOpen)
{
    std::vector<DcRezDecision::Member> members;
    for (int i = 0; i < 9; ++i)
        members.push_back(Mk(true, false, false, false));
    members.push_back(Mk(false, true, true, false));
    DcRezDecision::Inputs in;
    in.nowMs = 1000;
    in.wipeFractionPct = 90;
    in.regroupOnWipe = true;
    in.partyEngaged = true;  // last survivor still fighting
    auto const r = DcRezDecision::Decide(in, members);
    EXPECT_NE(r.reason, DcRezDecision::Reason::Wipe);
    EXPECT_EQ(r.outcome, DcRezDecision::Outcome::Hold);
}

TEST(DcRaidRezTest, DungeonDefaultKeepsLiteralWipeSemantics)
{
    // Default fraction 100: 4 of 5 dead with a survivor is a recovery, exactly
    // as before the raid axis existed.
    std::vector<DcRezDecision::Member> members;
    for (int i = 0; i < 4; ++i)
        members.push_back(Mk(true, false, false, false));
    members.push_back(Mk(false, true, true, false));
    DcRezDecision::Inputs in;
    in.nowMs = 1000;
    auto const r = DcRezDecision::Decide(in, members);
    EXPECT_EQ(r.outcome, DcRezDecision::Outcome::Hold);
    EXPECT_EQ(r.reason, DcRezDecision::Reason::Recovering);
}

// --- Formation geometry ----------------------------------------------------

TEST(DcFormationTest, RingRadiusFloorsAndGrows)
{
    // Small parties keep the tight base circle (3 members need ~1.2yd of ring;
    // the 1.5 floor wins)...
    EXPECT_FLOAT_EQ(DcFormation::RingRadius(3, 1.5f), 1.5f);
    // ...and the ring grows once the circumference demands it.
    float const r25 = DcFormation::RingRadius(25, 1.5f);
    EXPECT_GT(r25, 5.0f);
    EXPECT_GE(DcFormation::RingRadius(40, 1.5f), r25);
}

TEST(DcFormationTest, RoleRingsStayConcentric)
{
    auto const rings = DcFormation::ComputeRoleRings(10, 20, 8);
    EXPECT_GE(rings.ranged, rings.melee + DcFormation::kRingGapYd);
    EXPECT_GE(rings.healer, rings.ranged + DcFormation::kRingGapYd);
    // The crowded ranged ring must be big enough for its own population too.
    EXPECT_GE(rings.ranged, DcFormation::RingRadius(20, 0.0f));
}

TEST(DcFormationTest, SlotOffsetIsDeterministicAndBounded)
{
    auto const a1 = DcFormation::SlotOffset(1234, 5.0f);
    auto const a2 = DcFormation::SlotOffset(1234, 5.0f);
    EXPECT_FLOAT_EQ(a1.dx, a2.dx);
    EXPECT_FLOAT_EQ(a1.dy, a2.dy);
    float const r = std::sqrt(a1.dx * a1.dx + a1.dy * a1.dy);
    EXPECT_GE(r, 5.0f - 0.001f);
    EXPECT_LE(r, 6.0f + 0.001f);  // ring + up to 1yd variance
}

TEST(DcFormationTest, RaidFollowAngleSegmentsBySubgroup)
{
    constexpr float kTwoPi = 2.0f * 3.14159265f;
    for (uint8 sg = 0; sg < 8; ++sg)
    {
        float const a = DcFormation::RaidFollowAngle(sg, 42);
        EXPECT_GE(a, 0.0f);
        EXPECT_LT(a, kTwoPi);
    }
    // Distinct subgroups with the same seed land in distinct segments.
    EXPECT_NE(DcFormation::RaidFollowAngle(0, 7),
              DcFormation::RaidFollowAngle(4, 7));
}
