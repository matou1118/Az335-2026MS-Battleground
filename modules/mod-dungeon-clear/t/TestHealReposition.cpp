/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "gtest/gtest.h"

#include <cmath>

#include "DungeonClearMath.h"

using DungeonClearMath::HealCandidate;
using DungeonClearMath::HealTargetNone;
using DungeonClearMath::SelectHealTarget;
using DungeonClearMath::HealStandoffCandidates;

namespace
{
    constexpr float kFloor = 90.0f;
    constexpr float kBias = 15.0f;
}

// Nobody below the floor -> no target.
TEST(DungeonClearHealRepositionTest, AllHealthyNoTarget)
{
    std::vector<HealCandidate> m = {
        { 100.0f, false }, { 95.0f, true }, { 92.0f, false }
    };
    EXPECT_EQ(SelectHealTarget(m, kFloor, kBias), HealTargetNone);
}

// A single hurt member is picked.
TEST(DungeonClearHealRepositionTest, SingleHurtPicked)
{
    std::vector<HealCandidate> m = {
        { 100.0f, false }, { 40.0f, false }, { 95.0f, true }
    };
    EXPECT_EQ(SelectHealTarget(m, kFloor, kBias), 1u);
}

// Tank bias breaks the pick toward the tank when both need healing and are
// close in health (tank 85 vs dps 80: 85-15=70 < 80).
TEST(DungeonClearHealRepositionTest, TankBiasFavoursTank)
{
    std::vector<HealCandidate> m = {
        { 80.0f, false },  // dps, idx 0
        { 85.0f, true }    // tank, idx 1
    };
    EXPECT_EQ(SelectHealTarget(m, kFloor, kBias), 1u);
}

// A HEALTHY tank above the floor never steals the pick from a hurt dps — the
// "needs healing" gate is on raw health, applied before the bias.
TEST(DungeonClearHealRepositionTest, HealthyTankNeverStealsPick)
{
    std::vector<HealCandidate> m = {
        { 80.0f, false },  // dps, idx 0 (hurt)
        { 95.0f, true }    // tank, idx 1 (above floor -> excluded)
    };
    EXPECT_EQ(SelectHealTarget(m, kFloor, kBias), 0u);
}

// A clearly more-hurt dps still beats the tank even with the bias.
TEST(DungeonClearHealRepositionTest, MuchLowerDpsBeatsTank)
{
    std::vector<HealCandidate> m = {
        { 20.0f, false },  // dps, idx 0
        { 88.0f, true }    // tank, idx 1 (88-15=73 > 20)
    };
    EXPECT_EQ(SelectHealTarget(m, kFloor, kBias), 0u);
}

// Empty input is handled.
TEST(DungeonClearHealRepositionTest, EmptyNoTarget)
{
    std::vector<HealCandidate> m;
    EXPECT_EQ(SelectHealTarget(m, kFloor, kBias), HealTargetNone);
}

// Candidate count is ringPoints + 1.
TEST(DungeonClearHealRepositionTest, StandoffCount)
{
    Position target(100.0f, 100.0f, 50.0f, 0.0f);
    Position bot(80.0f, 100.0f, 50.0f, 0.0f);
    auto pts = HealStandoffCandidates(target, bot, 10.0f, 7);
    EXPECT_EQ(pts.size(), 8u);
}

// Every candidate lies on the standoff circle around the target.
TEST(DungeonClearHealRepositionTest, StandoffOnCircle)
{
    Position target(100.0f, 100.0f, 50.0f, 0.0f);
    Position bot(60.0f, 130.0f, 50.0f, 0.0f);
    float const r = 12.0f;
    auto pts = HealStandoffCandidates(target, bot, r, 7);
    for (Position const& p : pts)
    {
        float const dx = p.GetPositionX() - target.GetPositionX();
        float const dy = p.GetPositionY() - target.GetPositionY();
        EXPECT_NEAR(std::sqrt(dx * dx + dy * dy), r, 1e-2f);
    }
}

// First candidate is on the bot's side of the target (shortest reposition):
// it is the closest of all candidates to the bot.
TEST(DungeonClearHealRepositionTest, StandoffFirstIsBotSide)
{
    Position target(100.0f, 100.0f, 50.0f, 0.0f);
    Position bot(70.0f, 100.0f, 50.0f, 0.0f);  // due -X of target
    auto pts = HealStandoffCandidates(target, bot, 10.0f, 7);
    ASSERT_FALSE(pts.empty());
    // First candidate should sit between target and bot (x ~ 90).
    EXPECT_NEAR(pts[0].GetPositionX(), 90.0f, 1e-2f);
    EXPECT_NEAR(pts[0].GetPositionY(), 100.0f, 1e-2f);

    float const fdx = pts[0].GetPositionX() - bot.GetPositionX();
    float const fdy = pts[0].GetPositionY() - bot.GetPositionY();
    float const firstDist = std::sqrt(fdx * fdx + fdy * fdy);
    for (std::size_t i = 1; i < pts.size(); ++i)
    {
        float const dx = pts[i].GetPositionX() - bot.GetPositionX();
        float const dy = pts[i].GetPositionY() - bot.GetPositionY();
        EXPECT_LE(firstDist, std::sqrt(dx * dx + dy * dy) + 1e-3f);
    }
}

// Degenerate bot-on-target input falls back to +X without NaNs.
TEST(DungeonClearHealRepositionTest, StandoffDegenerate)
{
    Position target(100.0f, 100.0f, 50.0f, 0.0f);
    Position bot(100.0f, 100.0f, 50.0f, 0.0f);
    auto pts = HealStandoffCandidates(target, bot, 10.0f, 7);
    ASSERT_EQ(pts.size(), 8u);
    EXPECT_NEAR(pts[0].GetPositionX(), 110.0f, 1e-2f);
    EXPECT_NEAR(pts[0].GetPositionY(), 100.0f, 1e-2f);
}

// --- the close-on-target fallback walks the ROUTE, not the chord -------------
//
// The fallback used to be a point on the straight bot->target line. On Karazhan's
// Maiden hairpin the two legs of the corridor sit a few yards apart through a wall
// with no navmesh in it, so that point was inside the wall and healers/DPS walked
// through it (tr-20260927-184653-9, -190943-12). The point is now taken along the
// route (DcEngageGeometry::PathedCloseOn -> PointShortOfPathEnd).

// The hairpin: up one leg, through the doorway, back down the other. The target
// at the end is 10yd from the start through the wall and 70yd along the route.
// Stopping 5yd short lands on the far leg, never between the legs.
TEST(DungeonClearHealRepositionTest, FallbackWalksBackAlongTheRouteNotTheChord)
{
    std::vector<G3D::Vector3> const hairpin = {
        { 0.0f, 0.0f, 92.0f },   // bot, on the west leg
        { 0.0f, 30.0f, 92.0f },  // up the west leg to the doorway
        { 10.0f, 30.0f, 92.0f }, // through it
        { 10.0f, 0.0f, 92.0f },  // down the east leg to the target
    };
    G3D::Vector3 const p = DungeonClearMath::PointShortOfPathEnd(hairpin, 5.0f);
    EXPECT_NEAR(p.x, 10.0f, 1e-3f);  // on the east leg, not x 0..10 through the wall
    EXPECT_NEAR(p.y, 5.0f, 1e-3f);
    EXPECT_NEAR(p.z, 92.0f, 1e-3f);
}

// The walk-back crosses vertices: 12yd short of the end is 2yd before the
// doorway corner on the leg before it.
TEST(DungeonClearHealRepositionTest, FallbackCrossesRouteVertices)
{
    std::vector<G3D::Vector3> const path = {
        { 0.0f, 0.0f, 0.0f }, { 0.0f, 20.0f, 0.0f }, { 10.0f, 20.0f, 0.0f } };
    G3D::Vector3 const p = DungeonClearMath::PointShortOfPathEnd(path, 12.0f);
    EXPECT_NEAR(p.x, 0.0f, 1e-3f);
    EXPECT_NEAR(p.y, 18.0f, 1e-3f);
}

// Along a ramp the point stays on the ramp: z follows the route, so it is never
// the target's floor over our own x/y (the Blackwing Lair ceiling clip,
// tp-20260828-171530-1).
TEST(DungeonClearHealRepositionTest, FallbackFollowsTheRampHeight)
{
    std::vector<G3D::Vector3> const ramp = {
        { 0.0f, 0.0f, 424.5f }, { 0.0f, 20.0f, 424.5f }, { 0.0f, 40.0f, 449.3f } };
    G3D::Vector3 const p = DungeonClearMath::PointShortOfPathEnd(ramp, 5.0f);
    EXPECT_GT(p.z, 424.5f);
    EXPECT_LT(p.z, 449.3f);
    EXPECT_GT(p.y, 20.0f);
}

// A route no longer than the gap is already close enough: its first point (the
// bot), so the caller issues nothing. Empty and zero-length inputs stay finite.
TEST(DungeonClearHealRepositionTest, FallbackShortOrDegenerateRoute)
{
    std::vector<G3D::Vector3> const shortPath = { { 1.0f, 2.0f, 3.0f }, { 1.0f, 5.0f, 3.0f } };
    G3D::Vector3 const a = DungeonClearMath::PointShortOfPathEnd(shortPath, 5.0f);
    EXPECT_NEAR(a.y, 2.0f, 1e-3f);

    std::vector<G3D::Vector3> const stacked = { { 4.0f, 4.0f, 4.0f }, { 4.0f, 4.0f, 4.0f } };
    G3D::Vector3 const b = DungeonClearMath::PointShortOfPathEnd(stacked, 0.0f);
    EXPECT_FALSE(std::isnan(b.x));
    EXPECT_NEAR(b.z, 4.0f, 1e-3f);

    G3D::Vector3 const c = DungeonClearMath::PointShortOfPathEnd({}, 5.0f);
    EXPECT_FALSE(std::isnan(c.x));
}

// ---------------------------------------------------------------------------
// HealLeashRegistry — the per-map camp leash (tr-20260926-192642-1).
// ---------------------------------------------------------------------------

#include "Ai/Dungeon/DungeonClear/Data/HealLeashRegistry.h"

TEST(DungeonClearHealRepositionTest, LeashOnlyOnKarazhan)
{
    EXPECT_GT(HealLeashRegistry::Radius(532), 0.0f);
    // Corridor dungeons and raids keep the unleashed reposition.
    EXPECT_EQ(HealLeashRegistry::Radius(585), 0.0f);  // Magisters' Terrace
    EXPECT_EQ(HealLeashRegistry::Radius(469), 0.0f);  // Blackwing Lair
    EXPECT_EQ(HealLeashRegistry::Radius(0), 0.0f);
}

TEST(DungeonClearHealRepositionTest, LeashKeepsHealerOffTheBanquetFloor)
{
    // Camp and healer positions from the run: the camp on the landing, and the
    // floor spots the two healers aggroed from.
    float const campX = -11012.0f, campY = -1964.2f;
    float const r = HealLeashRegistry::Radius(532);
    EXPECT_TRUE(HealLeashRegistry::WithinLeash(campX, campY, r, -11006.0f, -1960.0f));
    EXPECT_FALSE(HealLeashRegistry::WithinLeash(campX, campY, r, -10975.0f, -1970.0f));  // Zuntun
    EXPECT_FALSE(HealLeashRegistry::WithinLeash(campX, campY, r, -10983.0f, -1995.0f));  // Laihmora
    // The nearest idle floor pack must sit well outside the leash.
    EXPECT_FALSE(HealLeashRegistry::WithinLeash(campX, campY, r + 10.0f, -10972.7f, -1969.2f));
}

TEST(DungeonClearHealRepositionTest, ZeroRadiusIsUnleashed)
{
    EXPECT_TRUE(HealLeashRegistry::WithinLeash(0.0f, 0.0f, 0.0f, 500.0f, 500.0f));
}
