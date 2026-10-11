/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

// SEQUENCING tests for the stuck-recovery ladder.
//
// The arithmetic of this ladder has been well covered for a long time —
// DecideRejoinRefusal, IsOffLineWithHysteresis and DcProgressWatchdog are all
// pure and all pinned. It kept shipping the same bug anyway, four times:
//
//   S1089  a Resnap that "succeeded" reset the ladder          (rung 1 pinned)
//   S1487  any tick that DISPLACED 0.5yd reset the ladder      (87/87 at rung 1)
//   S2227  a rejoin that refused still reset stuckCount        (3924 refusals, clear diag)
//   S2238  an off-path REBUILD reset the rejoin's refusals     (68 refusals, strike never fired)
//
// None of those is an arithmetic error, which is exactly why the pure-function
// tests could not see them. Every one is an ORDERING error: rung X, running
// earlier in the tick, destroys the state rung Y needs to escalate. That is a
// property of a SEQUENCE of ticks, so it needs a test that runs a sequence.
//
// These cases therefore drive DcApproachState across scripted tick sequences
// modelled on the live circuits, and assert the property that actually matters:
// a bot that is not getting anywhere must always reach its give-up rung in
// bounded time. Each also pins the OLD policy as a failing sequence, so the
// regression is described rather than merely absent.

#include "gtest/gtest.h"

#include <cfloat>

#include "Ai/Dungeon/DungeonClear/DcApproachState.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonPathFollower.h"

namespace
{
    constexpr float MIN_CLOSE = 0.5f;   // DC_STUCK_DISPLACEMENT
    constexpr uint32 REFUSAL_LIMIT = 8; // DC_REJOIN_REFUSAL_LIMIT (DcAdvanceAction.cpp)
    constexpr uint32 STUCK_LIMIT   = 8; // DC_STUCK_LIMIT          (DcAdvanceAction.cpp)

    // The distance a frozen bot reports to its objective, every tick, forever.
    constexpr float FROZEN_DIST = 84.8f;  // tr-20260902-134056-1's actual reading

    // One advance tick's worth of the live circuit in tr-20260902-134056-1.
    //
    //   FillStuckObs        -> NoteRecoveryProgress (the one reset authority)
    //   DoOffLineRejoin     -> DcMoveTo refused, so ++rejoinRefusals
    //   FillPathObs (tier B) -> every OFF_PATH_TICK_LIMIT ticks the Resnap fails,
    //                           DoOffPathRebuild reseeds the follower, deviation
    //                           drops to ~0 and the off-line latch RELEASES.
    //
    // `legacyEpisodeReset` models the pre-fix line that made the latch release
    // zero the refusal counter along with the drift baseline.
    void RunFrozenRejoinCircuit(DcApproachState& appr, uint32 ticks, uint32 rebuildEvery,
                                bool legacyEpisodeReset)
    {
        for (uint32 t = 1; t <= ticks; ++t)
        {
            // The bot never moves, so the distance never improves.
            appr.NoteRecoveryProgress(FROZEN_DIST, MIN_CLOSE, t);

            if (rebuildEvery != 0 && t % rebuildEvery == 0)
            {
                // Episode boundary: the rebuild reseeded the cursor, the bot is
                // nominally "back on the corridor". It has still moved nowhere.
                appr.rejoinBestDev = FLT_MAX;
                if (legacyEpisodeReset)
                    appr.rejoinRefusals = 0;
                continue;
            }
            ++appr.rejoinRefusals;
        }
    }

    // A single-segment path along +X with `spacing` yards between points.
    ChunkedPathfinder::Result StraightPath(size_t points, float spacing)
    {
        ChunkedPathfinder::Result path;
        PathSegment seg;
        for (size_t i = 1; i <= points; ++i)
            seg.polyline.push_back(G3D::Vector3(spacing * float(i), 0.0f, 0.0f));
        path.segments.push_back(seg);
        return path;
    }
}

// ---- The reset authority -------------------------------------------------

// Net progress is the one fact allowed to clear the ladder, and it clears ALL
// of it — the counters are a single decision, not five independent ones.
TEST(DcRecoveryEscalation, ProgressClearsEveryRecoveryCounter)
{
    DcApproachState appr;
    appr.NoteRecoveryProgress(100.0f, MIN_CLOSE, 1);  // arm the tracker

    appr.stuckCount      = 5;
    appr.rebuildAttempts = 2;
    appr.resnapAttempts  = 2;
    appr.nudgeAttempts   = 1;
    appr.rejoinRefusals  = 7;

    EXPECT_TRUE(appr.NoteRecoveryProgress(50.0f, MIN_CLOSE, 2));
    EXPECT_EQ(appr.stuckCount, 0u);
    EXPECT_EQ(appr.rebuildAttempts, 0u);
    EXPECT_EQ(appr.resnapAttempts, 0u);
    EXPECT_EQ(appr.nudgeAttempts, 0u);
    EXPECT_EQ(appr.rejoinRefusals, 0u);
}

// ...and nothing short of progress clears any of it. A tick that merely happened
// leaves every counter standing.
TEST(DcRecoveryEscalation, StandingStillPreservesEveryRecoveryCounter)
{
    DcApproachState appr;
    appr.NoteRecoveryProgress(FROZEN_DIST, MIN_CLOSE, 1);  // arm

    appr.stuckCount     = 3;
    appr.rejoinRefusals = 4;

    for (uint32 t = 2; t <= 20; ++t)
        EXPECT_FALSE(appr.NoteRecoveryProgress(FROZEN_DIST, MIN_CLOSE, t));

    EXPECT_EQ(appr.stuckCount, 3u);
    EXPECT_EQ(appr.rejoinRefusals, 4u);
}

// A SHUTTLING bot displaces plenty and arrives nowhere. Its near end only ties
// the best distance and its far end is worse, so it can never buy a reset —
// the property S1487 established, restated here for the whole counter set.
TEST(DcRecoveryEscalation, ShuttlingNeverClearsTheLadder)
{
    DcApproachState appr;
    appr.NoteRecoveryProgress(40.0f, MIN_CLOSE, 1);  // arm at the near end
    appr.stuckCount = 6;

    for (uint32 t = 2; t <= 40; ++t)
        appr.NoteRecoveryProgress((t % 2 == 0) ? 69.0f : 40.0f, MIN_CLOSE, t);

    EXPECT_EQ(appr.stuckCount, 6u);
}

// ---- The S2238 circuit: a rebuild must not disarm the rejoin -------------

// THE REGRESSION. tr-20260902-134056-1 ran this exact circuit for seven minutes:
// the rejoin refused on every tick it owned, and an off-path rebuild ended the
// off-line episode every third tick. With the episode boundary zeroing the
// refusal count, 68 consecutive refusals never once reached the 8 needed to
// escalate, and the run died on the 600s no-progress watchdog instead of
// stalling for `dc skip`.
TEST(DcRecoveryEscalation, LegacyEpisodeResetStarvesTheRejoinEscalation)
{
    DcApproachState appr;
    RunFrozenRejoinCircuit(appr, /*ticks*/ 200, /*rebuildEvery*/ 3, /*legacy*/ true);

    // Two refusals per cycle, wiped by the third tick, forever.
    EXPECT_LT(appr.rejoinRefusals, REFUSAL_LIMIT)
        << "pins the bug: the rebuild kept the counter below its own limit";
}

// With the reset moved onto net progress, the same circuit escalates. The bot
// is no better off — it still has not moved — but the rung now gets to say so.
TEST(DcRecoveryEscalation, RejoinEscalationSurvivesTheOffPathRebuild)
{
    DcApproachState appr;
    RunFrozenRejoinCircuit(appr, /*ticks*/ 200, /*rebuildEvery*/ 3, /*legacy*/ false);

    EXPECT_GE(appr.rejoinRefusals, REFUSAL_LIMIT);
}

// Bounded time, not merely eventually: the limit is reached inside the first
// handful of cycles, so the stall reaches the player in seconds rather than
// after the ten-minute watchdog.
TEST(DcRecoveryEscalation, RejoinEscalationIsReachedPromptly)
{
    DcApproachState appr;
    uint32 ticksToLimit = 0;

    for (uint32 t = 1; t <= 200 && appr.rejoinRefusals < REFUSAL_LIMIT; ++t)
    {
        RunFrozenRejoinCircuit(appr, /*ticks*/ 1, /*rebuildEvery*/ 0, /*legacy*/ false);
        if (t % 3 == 0)
            appr.rejoinBestDev = FLT_MAX;
        ticksToLimit = t;
    }

    EXPECT_EQ(appr.rejoinRefusals, REFUSAL_LIMIT);
    EXPECT_LE(ticksToLimit, 12u) << "8 refusals should cost ~8 ticks, not a watchdog";
}

// A rejoin that ACTUALLY ISSUES still breaks the streak — the counter measures
// consecutive ticks that moved nothing, so this is its own definition rather
// than a claim about progress. Without this the rung would escalate on a
// healthy re-entry that is simply taking several ticks to walk.
TEST(DcRecoveryEscalation, AnIssuedRejoinBreaksTheRefusalStreak)
{
    DcApproachState appr;
    RunFrozenRejoinCircuit(appr, /*ticks*/ 5, /*rebuildEvery*/ 0, /*legacy*/ false);
    ASSERT_EQ(appr.rejoinRefusals, 5u);

    appr.rejoinRefusals = 0;  // DoOffLineRejoin's `if (rejoining)` branch

    RunFrozenRejoinCircuit(appr, /*ticks*/ 3, /*rebuildEvery*/ 0, /*legacy*/ false);
    EXPECT_EQ(appr.rejoinRefusals, 3u);
    EXPECT_LT(appr.rejoinRefusals, REFUSAL_LIMIT);
}

// The same property one rung down: stuckCount is fed by refusals, and used to be
// zeroed by any rung that ISSUED a move (a chase, a spline launch, a ride). A bot
// that issues a move every tick and travels nowhere must still reach its limit.
TEST(DcRecoveryEscalation, IssuingMovesDoesNotDisarmTheStuckLadder)
{
    DcApproachState appr;
    appr.NoteRecoveryProgress(FROZEN_DIST, MIN_CLOSE, 1);  // arm

    for (uint32 t = 2; t <= 40 && appr.stuckCount < STUCK_LIMIT; ++t)
    {
        appr.NoteRecoveryProgress(FROZEN_DIST, MIN_CLOSE, t);  // frozen: no reset
        ++appr.stuckCount;                                     // a refused fallback move
    }

    EXPECT_EQ(appr.stuckCount, STUCK_LIMIT);
}

// A travelling bot must never accumulate toward a stall. Progress every tick
// keeps the ladder at zero no matter how many failures are interleaved.
TEST(DcRecoveryEscalation, TravellingKeepsTheLadderClear)
{
    DcApproachState appr;
    float dist = 200.0f;

    for (uint32 t = 1; t <= 40; ++t)
    {
        ++appr.stuckCount;
        ++appr.rejoinRefusals;
        dist -= 1.5f;  // a healthy glide tick
        appr.NoteRecoveryProgress(dist, MIN_CLOSE, t);
        EXPECT_EQ(appr.stuckCount, 0u);
        EXPECT_EQ(appr.rejoinRefusals, 0u);
    }
}

// ---- The S2238 fixed point: a cursor the bot has walked past --------------

// Resnap searches forward FROM the cursor and so considers the cursor's own
// point first. On a sparse anchor route a bot that has overshot by a few yards
// is nearer to the point it passed than to the next one, so Resnap re-picks it
// and reports success having repaired nothing. SkipPassedPoint is the escape:
// it always moves the cursor, so the rung cannot be a fixed point.
TEST(DcRecoveryEscalation, SkipPassedPointRetiresTheCursorPoint)
{
    ChunkedPathfinder::Result const path = StraightPath(6, 20.0f);
    DungeonFollowerState state;
    state.segmentIdx = 0;
    state.pointIdx   = 2;
    state.offPathTicks = 3;

    G3D::Vector3 passed;
    ASSERT_TRUE(DungeonPathFollower::SkipPassedPoint(path, state, passed));

    EXPECT_FLOAT_EQ(passed.x, 60.0f);      // the point that was retired
    EXPECT_EQ(state.pointIdx, 3u);         // cursor advanced
    EXPECT_EQ(state.offPathTicks, 0u);     // and the off-path streak reset
}

// Repeated application always makes ground — the property that makes it an
// escape rather than another loop.
TEST(DcRecoveryEscalation, SkipPassedPointIsNeverAFixedPoint)
{
    ChunkedPathfinder::Result const path = StraightPath(6, 20.0f);
    DungeonFollowerState state;

    uint32 lastPoint = state.pointIdx;
    G3D::Vector3 passed;
    while (DungeonPathFollower::SkipPassedPoint(path, state, passed))
    {
        EXPECT_GT(state.pointIdx, lastPoint);
        lastPoint = state.pointIdx;
    }
    // Walked off the end of the route; the hop-done ladder owns it from here.
    EXPECT_GE(state.segmentIdx, path.segments.size());
}

// It refuses at the route's end rather than inventing a cursor.
TEST(DcRecoveryEscalation, SkipPassedPointRefusesPastTheRoute)
{
    ChunkedPathfinder::Result const path = StraightPath(3, 20.0f);
    DungeonFollowerState state;
    state.segmentIdx = 1;  // past the only segment

    G3D::Vector3 passed;
    EXPECT_FALSE(DungeonPathFollower::SkipPassedPoint(path, state, passed));
}

// A jump segment's last point is a MoveJump the caller still has to drive, so it
// is never "walked past" — same carve-out SkipStrandedPoint makes.
TEST(DcRecoveryEscalation, SkipPassedPointNeverRetiresAJumpTail)
{
    ChunkedPathfinder::Result path = StraightPath(3, 20.0f);
    path.segments[0].jumpDown = true;

    DungeonFollowerState state;
    state.segmentIdx = 0;
    state.pointIdx   = 2;  // the segment's last point

    G3D::Vector3 passed;
    EXPECT_FALSE(DungeonPathFollower::SkipPassedPoint(path, state, passed));
    EXPECT_EQ(state.pointIdx, 2u);

    // A non-tail point of the same jump segment is still retirable.
    state.pointIdx = 0;
    EXPECT_TRUE(DungeonPathFollower::SkipPassedPoint(path, state, passed));
    EXPECT_EQ(state.pointIdx, 1u);
}
