/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "gtest/gtest.h"

#include "Ai/Dungeon/DungeonClear/Util/DcSuppressionTransitDecision.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

// The pure kernel behind Blackwing Lair's Suppression Rooms transit: the
// hold/advance/complete decision, and the route cursor.
//
// What is being pinned here is not "does the raid walk" — that needs a map — but
// the four properties the leg's design rests on:
//
//   1. the gather gate OPENS, and it TIMES OUT rather than hanging;
//   2. each of the three holds fires on its own probe, in precedence order;
//   3. each hold's watchdog releases it, with its OWN budget;
//   4. the cursor only ever moves forward — except for the one recovery case it
//      is explicitly allowed to move back for;
//   5. the pack's hold point rides the POLYLINE, so a hold behind a bend stays on
//      the corridor instead of cutting the chord across it.

using namespace DcSuppressionTransit;

namespace
{
    // A snapshot mid-crossing: gathered, walking, nothing in reach.
    Inputs Walking()
    {
        Inputs in;
        in.cursorIndex = 5;
        in.anchorCount = 20;
        in.distToCursor = 14.0f;
        in.arriveRadius = 6.0f;
        in.gathered = true;
        in.quorumMet = true;
        in.topped = true;
        in.trailDist = 9.0f;
        in.packLeash = 25.0f;
        in.nearestEliteDist = -1.0f;
        in.eliteHoldRadius = 20.0f;
        in.nearestArmedDeviceDist = -1.0f;
        in.disarmHoldRadius = 15.0f;
        in.nowMs = 1000000;
        in.armedSinceMs = 900000;
        in.holdSinceMs = 0;
        in.holdReason = 0;
        in.gatherTimeoutMs = 60000;
        in.packHoldTimeoutMs = 30000;
        in.eliteHoldTimeoutMs = 120000;
        in.disarmHoldTimeoutMs = 1500;
        return in;
    }

    // ...and the same snapshot parked on the staging anchor with the gate shut.
    Inputs AtStagingUngathered()
    {
        Inputs in = Walking();
        in.cursorIndex = 0;
        in.distToCursor = 1.0f;
        in.gathered = false;
        in.quorumMet = false;
        in.topped = true;
        in.armedSinceMs = in.nowMs - 5000;
        return in;
    }

    std::vector<Anchor> Route()
    {
        // A straight 100yd line of six anchors, 20yd apart — enough shape for the
        // cursor tests without importing the real route's geometry.
        return { {0, 0, 0}, {20, 0, 0}, {40, 0, 0}, {60, 0, 0}, {80, 0, 0}, {100, 0, 0} };
    }
}

// --- walking --------------------------------------------------------------

TEST(DcSuppressionTransitTest, NothingInReachMeansWalk)
{
    Verdict const v = Decide(Walking());
    EXPECT_TRUE(v.advance);
    EXPECT_FALSE(v.complete);
    EXPECT_FALSE(v.cursorAdvance);
    EXPECT_EQ(v.hold, Hold::None);
    EXPECT_EQ(v.holdSinceMs, 0u);
    EXPECT_FALSE(v.timedOut);
}

TEST(DcSuppressionTransitTest, ReachingAnAnchorMovesTheCursorOn)
{
    Inputs in = Walking();
    in.distToCursor = in.arriveRadius - 0.1f;

    Verdict const v = Decide(in);
    EXPECT_TRUE(v.cursorAdvance);
    EXPECT_TRUE(v.advance) << "reaching an anchor with nothing in reach keeps walking";
}

// A hold does NOT freeze the cursor. The cursor is where the raid is GOING, and
// standing still for an elite is not a reason for the pack behind to form up on
// the leg the leader has already finished.
TEST(DcSuppressionTransitTest, AHoldStillLetsTheCursorMoveOn)
{
    Inputs in = Walking();
    in.distToCursor = 1.0f;
    in.nearestEliteDist = 8.0f;

    Verdict const v = Decide(in);
    EXPECT_FALSE(v.advance);
    EXPECT_EQ(v.hold, Hold::Elite);
    EXPECT_TRUE(v.cursorAdvance);
}

// --- completion -----------------------------------------------------------

TEST(DcSuppressionTransitTest, ReachingTheLastAnchorCompletesTheCrossing)
{
    Inputs in = Walking();
    in.cursorIndex = 19;  // the standoff
    in.distToCursor = 3.0f;

    Verdict const v = Decide(in);
    EXPECT_TRUE(v.complete);
    EXPECT_FALSE(v.advance);
    EXPECT_EQ(v.hold, Hold::None);
}

// Completion is the STANDOFF, not "no holds left". A raid mid-fight one anchor
// short of Broodlord has not crossed anything.
TEST(DcSuppressionTransitTest, NearTheLastAnchorIsNotComplete)
{
    Inputs in = Walking();
    in.cursorIndex = 19;
    in.distToCursor = in.arriveRadius + 0.1f;

    EXPECT_FALSE(Decide(in).complete);
}

TEST(DcSuppressionTransitTest, AnEmptyRouteDecidesNothing)
{
    Inputs in = Walking();
    in.anchorCount = 0;

    Verdict const v = Decide(in);
    EXPECT_FALSE(v.advance);
    EXPECT_FALSE(v.complete);
    EXPECT_EQ(v.hold, Hold::None);
}

// --- the gather gate ------------------------------------------------------

TEST(DcSuppressionTransitTest, TheGatherGateHoldsAtStagingUntilQuorum)
{
    Verdict const v = Decide(AtStagingUngathered());
    EXPECT_EQ(v.hold, Hold::Gathering);
    EXPECT_FALSE(v.advance);
    EXPECT_FALSE(v.openGatherGate);
    // The cursor stays on the staging anchor: that is where the pack forms.
    EXPECT_FALSE(v.cursorAdvance);
}

TEST(DcSuppressionTransitTest, QuorumOpensTheGateAndTheCrossingStartsTheSameTick)
{
    Inputs in = AtStagingUngathered();
    in.quorumMet = true;
    in.topped = true;

    Verdict const v = Decide(in);
    EXPECT_TRUE(v.openGatherGate);
    EXPECT_FALSE(v.timedOut);
    EXPECT_TRUE(v.advance) << "the gate opening must not cost a tick";
    EXPECT_TRUE(v.cursorAdvance);
}

// REGRESSION, tp-20260828-111227-1: the gate must NOT wait for the raid to top
// off. Staging is 40.8yd from a whelp on a 30s respawn, so a forty-man raid is
// never out of combat there and never tops; the clause bought sixty seconds of
// standing still on all four live gathers and then crossed anyway. A quorum that
// has formed up crosses NOW, empty or not — and it crosses on merit, not on a
// watchdog.
TEST(DcSuppressionTransitTest, AGatheredButUntoppedRaidCrossesAnyway)
{
    Inputs in = AtStagingUngathered();
    in.quorumMet = true;
    in.topped = false;

    Verdict const v = Decide(in);
    EXPECT_TRUE(v.openGatherGate);
    EXPECT_FALSE(v.timedOut) << "an untopped quorum must open the gate on merit";
    EXPECT_TRUE(v.advance);
}

// ...and quorum is still the thing being waited for: an untopped raid that has
// NOT formed up holds exactly as before.
TEST(DcSuppressionTransitTest, AnUntoppedRaidWithoutQuorumStillWaits)
{
    Inputs in = AtStagingUngathered();
    in.quorumMet = false;
    in.topped = false;

    Verdict const v = Decide(in);
    EXPECT_EQ(v.hold, Hold::Gathering);
    EXPECT_FALSE(v.openGatherGate);
}

// A muster that can hang eventually will. One bot that cannot path in must never
// hold thirty-nine others at the door.
TEST(DcSuppressionTransitTest, TheGatherGateTimesOutAndSaysSo)
{
    Inputs in = AtStagingUngathered();
    in.armedSinceMs = in.nowMs - in.gatherTimeoutMs;

    Verdict const v = Decide(in);
    EXPECT_TRUE(v.openGatherGate);
    EXPECT_TRUE(v.timedOut);
    EXPECT_TRUE(v.advance);
}

// The walk INTO staging is an ordinary advance. Holding a leader mid-approach
// because the column behind it has not arrived is how a raid never arrives.
TEST(DcSuppressionTransitTest, TheGateDoesNotHoldTheWalkIntoStaging)
{
    Inputs in = AtStagingUngathered();
    in.distToCursor = 30.0f;

    Verdict const v = Decide(in);
    EXPECT_TRUE(v.advance);
    EXPECT_EQ(v.hold, Hold::None);
    EXPECT_FALSE(v.openGatherGate);
}

TEST(DcSuppressionTransitTest, AnOpenedGateIsNotReopened)
{
    Inputs in = AtStagingUngathered();
    in.gathered = true;   // latched last tick
    in.quorumMet = false; // ...and the raid has since spread out

    Verdict const v = Decide(in);
    EXPECT_FALSE(v.openGatherGate);
    EXPECT_NE(v.hold, Hold::Gathering)
        << "re-gathering mid-crossing is the pack rung's job, not a walk back to staging";
}

// --- the three holds ------------------------------------------------------

TEST(DcSuppressionTransitTest, ATrailingPackHoldsTheLeader)
{
    Inputs in = Walking();
    in.trailDist = in.packLeash + 0.1f;

    Verdict const v = Decide(in);
    EXPECT_EQ(v.hold, Hold::PackTrailing);
    EXPECT_FALSE(v.advance);
    EXPECT_EQ(v.holdSinceMs, in.nowMs) << "a fresh hold starts its own clock";
}

// The pack hold is a QUORUM. Twenty-four followers all inside one leash at once
// is a bar no raid crossing a room that respawns every thirty seconds clears —
// measured at 87-91% of every driver tick in tp-20260828-121941-1 — and the only
// thing that released it was its own watchdog. A member that cannot keep up costs
// the raid a member (stranded recovery owns it), not the leg.
TEST(DcSuppressionTransitTest, AQuorumOfThePackInsideTheLeashDoesNotHold)
{
    Inputs in = Walking();
    in.packLiving = 24;
    in.packOutside = 3;  // 21 inside, and ceil(24 * 0.85) == 21
    in.packQuorum = 0.85f;
    in.trailDist = 400.0f;  // ...however far the worst straggler has got

    Verdict const v = Decide(in);
    EXPECT_TRUE(v.advance);
    EXPECT_EQ(v.hold, Hold::None);
}

TEST(DcSuppressionTransitTest, TooManyOutsideTheLeashStillHoldsTheLeader)
{
    Inputs in = Walking();
    in.packLiving = 24;
    in.packOutside = 4;  // 20 inside, one short of the quorum
    in.packQuorum = 0.85f;
    in.trailDist = in.packLeash + 1.0f;

    Verdict const v = Decide(in);
    EXPECT_FALSE(v.advance);
    EXPECT_EQ(v.hold, Hold::PackTrailing);
}

TEST(DcSuppressionTransitTest, TheOnlyFollowerMustKeepUp)
{
    Inputs in = Walking();
    in.packLiving = 1;
    in.packOutside = 1;
    in.packQuorum = 0.85f;  // ceil(1 * 0.85) == 1

    Verdict const v = Decide(in);
    EXPECT_EQ(v.hold, Hold::PackTrailing);
}

// A caller that cannot count heads must not silently switch the probe OFF.
TEST(DcSuppressionTransitTest, WithoutCountsThePackHoldFallsBackToTheFurthestMember)
{
    Inputs in = Walking();
    in.packLiving = 0;
    in.packOutside = 0;
    in.packQuorum = 0.0f;
    in.trailDist = in.packLeash + 0.1f;

    EXPECT_EQ(Decide(in).hold, Hold::PackTrailing);
}

TEST(DcSuppressionTransitTest, ALoneLeaderIsNeverHeldByThePack)
{
    Inputs in = Walking();
    in.trailDist = -1.0f;  // nobody else alive on the map

    EXPECT_TRUE(Decide(in).advance);
}

TEST(DcSuppressionTransitTest, AnEliteInReachHoldsTheLeader)
{
    Inputs in = Walking();
    in.nearestEliteDist = in.eliteHoldRadius;

    Verdict const v = Decide(in);
    EXPECT_EQ(v.hold, Hold::Elite);
    EXPECT_FALSE(v.advance);
}

TEST(DcSuppressionTransitTest, AnEliteOutOfReachDoesNotHold)
{
    Inputs in = Walking();
    in.nearestEliteDist = in.eliteHoldRadius + 0.1f;

    EXPECT_TRUE(Decide(in).advance);
}

TEST(DcSuppressionTransitTest, AnArmedDeviceInReachBuysTheDisarmRungATick)
{
    Inputs in = Walking();
    in.nearestArmedDeviceDist = 12.0f;

    Verdict const v = Decide(in);
    EXPECT_EQ(v.hold, Hold::Disarm);
    EXPECT_FALSE(v.advance);
}

TEST(DcSuppressionTransitTest, ADisarmRadiusOfZeroDisablesTheHold)
{
    Inputs in = Walking();
    in.nearestArmedDeviceDist = 0.5f;
    in.disarmHoldRadius = 0.0f;

    EXPECT_TRUE(Decide(in).advance)
        << "zero must disable the HOLD without pretending there is no device";
}

// Precedence: fighting an elite with half the raid a room behind is how the other
// half dies, and the disarm rung will still be there in ten seconds while the
// Taskmaster will not.
TEST(DcSuppressionTransitTest, HoldsFireInPrecedenceOrder)
{
    Inputs in = Walking();
    in.trailDist = in.packLeash + 5.0f;
    in.nearestEliteDist = 2.0f;
    in.nearestArmedDeviceDist = 1.0f;
    EXPECT_EQ(Decide(in).hold, Hold::PackTrailing);

    in.trailDist = 1.0f;
    EXPECT_EQ(Decide(in).hold, Hold::Elite);

    in.nearestEliteDist = -1.0f;
    EXPECT_EQ(Decide(in).hold, Hold::Disarm);
}

// --- the watchdogs --------------------------------------------------------

TEST(DcSuppressionTransitTest, EachHoldCarriesItsOwnWatchdog)
{
    struct Case { Hold hold; uint32 budget; };
    Inputs const base = Walking();
    Case const cases[] = {
        { Hold::PackTrailing, base.packHoldTimeoutMs },
        { Hold::Elite,        base.eliteHoldTimeoutMs },
        { Hold::Disarm,       base.disarmHoldTimeoutMs },
    };

    for (Case const& c : cases)
    {
        Inputs in = base;
        switch (c.hold)
        {
            case Hold::PackTrailing: in.trailDist = in.packLeash + 1.0f; break;
            case Hold::Elite:        in.nearestEliteDist = 5.0f; break;
            case Hold::Disarm:       in.nearestArmedDeviceDist = 5.0f; break;
            default: break;
        }
        in.holdReason = static_cast<uint8>(c.hold);

        // One millisecond short: still holding.
        in.holdSinceMs = in.nowMs - (c.budget - 1);
        Verdict held = Decide(in);
        EXPECT_EQ(held.hold, c.hold);
        EXPECT_FALSE(held.advance);
        EXPECT_FALSE(held.timedOut);

        // Expired: walk on, and say it was the watchdog. Whether the release
        // LATCHES is per hold — see the three tests below.
        in.holdSinceMs = in.nowMs - c.budget;
        Verdict released = Decide(in);
        EXPECT_TRUE(released.advance);
        EXPECT_TRUE(released.timedOut);
        if (DcSuppressionTransit::ReleaseLatches(c.hold))
        {
            EXPECT_EQ(released.hold, c.hold);
            EXPECT_EQ(released.holdSinceMs, in.nowMs - c.budget);
        }
        else
        {
            EXPECT_EQ(released.hold, Hold::None);
            EXPECT_EQ(released.holdSinceMs, 0u);
        }
    }
}

// AN ELITE RELEASE LATCHES. Returning hold=None/holdSinceMs=0 made the glue store
// "not holding", so the next tick re-stamped a fresh clock for the same unchanged
// condition and waited out the whole budget again. Because a hold never issues a
// stop packet, that bought exactly one anchor per 120s — over the 342yd crossing,
// the 18-24 minute leg.
TEST(DcSuppressionTransitTest, AnElitePastItsBudgetKeepsWalking)
{
    Inputs in = Walking();
    in.nearestEliteDist = 5.0f;                       // an elite that never dies
    in.holdReason = static_cast<uint8>(Hold::Elite);
    in.holdSinceMs = in.nowMs - in.eliteHoldTimeoutMs;

    Verdict last = Decide(in);
    ASSERT_TRUE(last.advance);
    ASSERT_TRUE(last.timedOut);

    // Feed the verdict back the way the driver does and step the clock on. The
    // condition has not changed, so the leg must keep walking, not re-hold.
    for (uint32 tick = 1; tick <= 10; ++tick)
    {
        in.holdReason = static_cast<uint8>(last.hold);
        in.holdSinceMs = last.holdSinceMs;
        in.nowMs += 1000;

        last = Decide(in);
        EXPECT_TRUE(last.advance) << "tick " << tick;
        EXPECT_TRUE(last.timedOut) << "tick " << tick;
    }
}

// ...and it un-latches the moment the condition clears, so the next hold gets a
// full budget rather than inheriting an expired clock.
TEST(DcSuppressionTransitTest, AClearedEliteDropsTheWatchdogLatch)
{
    Inputs in = Walking();
    in.nearestEliteDist = 5.0f;
    in.holdReason = static_cast<uint8>(Hold::Elite);
    in.holdSinceMs = in.nowMs - in.eliteHoldTimeoutMs;

    Verdict const released = Decide(in);
    ASSERT_TRUE(released.timedOut);

    // The elite dies; a straggler falls behind on the same tick.
    in.holdReason = static_cast<uint8>(released.hold);
    in.holdSinceMs = released.holdSinceMs;
    in.nearestEliteDist = -1.0f;
    in.trailDist = in.packLeash + 1.0f;

    Verdict const v = Decide(in);
    EXPECT_EQ(v.hold, Hold::PackTrailing);
    EXPECT_FALSE(v.advance);
    EXPECT_FALSE(v.timedOut);
    EXPECT_EQ(v.holdSinceMs, in.nowMs) << "a new hold reason starts its own clock";
}

// THE PACK HOLD IS THE EXCEPTION, deliberately: walking is what makes a straggler
// hold worse, so its release is a duty cycle — one anchor of progress, then wait
// the budget out again — rather than a latch that would march the leader to the
// Broodlord standoff on its own.
TEST(DcSuppressionTransitTest, APackReleaseIsOneAnchorNotALatch)
{
    Inputs in = Walking();
    in.trailDist = in.packLeash + 1.0f;               // a straggler that never closes
    in.holdReason = static_cast<uint8>(Hold::PackTrailing);
    in.holdSinceMs = in.nowMs - in.packHoldTimeoutMs;

    Verdict const released = Decide(in);
    ASSERT_TRUE(released.advance);
    ASSERT_TRUE(released.timedOut);
    ASSERT_EQ(released.hold, Hold::None);

    // Next tick, with the release stored back: the wait re-arms on a fresh clock.
    in.holdReason = static_cast<uint8>(released.hold);
    in.holdSinceMs = released.holdSinceMs;
    in.nowMs += 1000;

    Verdict const again = Decide(in);
    EXPECT_EQ(again.hold, Hold::PackTrailing);
    EXPECT_FALSE(again.advance);
    EXPECT_EQ(again.holdSinceMs, in.nowMs);
}

// The budgets bound ONE wait each. A party released from a 100-second elite fight
// straight into a straggler hold must get the straggler's full budget, not the
// remainder of the elite's.
TEST(DcSuppressionTransitTest, ChangingHoldReasonRestartsTheClock)
{
    Inputs in = Walking();
    in.holdReason = static_cast<uint8>(Hold::Elite);
    in.holdSinceMs = in.nowMs - 100000;   // long past the pack budget
    in.trailDist = in.packLeash + 1.0f;   // ...but the hold is now the PACK

    Verdict const v = Decide(in);
    EXPECT_EQ(v.hold, Hold::PackTrailing);
    EXPECT_FALSE(v.timedOut);
    EXPECT_EQ(v.holdSinceMs, in.nowMs);
}

TEST(DcSuppressionTransitTest, AContinuingHoldKeepsItsClock)
{
    Inputs in = Walking();
    in.nearestEliteDist = 5.0f;
    in.holdReason = static_cast<uint8>(Hold::Elite);
    in.holdSinceMs = in.nowMs - 40000;

    EXPECT_EQ(Decide(in).holdSinceMs, in.nowMs - 40000);
}

// getMSTime() wraps. Every clock here is compared as an unsigned interval for
// exactly this case, which a naive `now >= since + budget` gets backwards for the
// ~49 days on either side of the wrap.
TEST(DcSuppressionTransitTest, ClocksSurviveTheMillisecondWrap)
{
    uint32 const justBefore = 0xFFFFFF00u;
    EXPECT_FALSE(Expired(/*now*/ 0x00000010u, justBefore, /*budget*/ 1000u));
    EXPECT_TRUE(Expired(/*now*/ 0x00000400u, justBefore, /*budget*/ 1000u));
    // An unarmed clock and an unbounded budget both mean "never".
    EXPECT_FALSE(Expired(1000, 0, 100));
    EXPECT_FALSE(Expired(1000, 1, 0));
}

// --- the cursor -----------------------------------------------------------

TEST(DcSuppressionTransitTest, TheCursorIsTheAnchorAhead)
{
    std::vector<Anchor> const route = Route();
    // Standing at the start: the anchor ahead is 1.
    EXPECT_EQ(ResolveCursor(route, 0, 0, 0, /*stored*/ 0, 60.0f), 1u);
    // Halfway down the third leg (40 -> 60): the anchor ahead is 3.
    EXPECT_EQ(ResolveCursor(route, 50, 0, 0, /*stored*/ 0, 60.0f), 3u);
    // At the far end.
    EXPECT_EQ(ResolveCursor(route, 100, 0, 0, /*stored*/ 0, 60.0f), 5u);
}

// The two suppression rooms are stacked ~9yd apart and the route doubles back on
// itself between them, so a leader on the upper floor is closer to a lower-floor
// segment than the geometry deserves. Clamping to `stored` is what stops a cursor
// that has crossed the ramp from being dragged back down it.
TEST(DcSuppressionTransitTest, TheCursorNeverGoesBackwardsOnItsOwn)
{
    std::vector<Anchor> const route = Route();
    // Standing back at leg 1 with the cursor already at 4 — a float artefact, not
    // a fact — keeps 4.
    EXPECT_EQ(ResolveCursor(route, 25, 0, 0, /*stored*/ 4, /*resync*/ 60.0f), 4u);
}

// ...with the one escape: a leader that has died, corpse-run and come back is not
// where its stored cursor says, and inside a working crossing nothing is ever
// this far from the anchor it is walking to.
TEST(DcSuppressionTransitTest, AFarLeaderResyncsTheCursorBackwards)
{
    std::vector<Anchor> const route = Route();
    EXPECT_EQ(ResolveCursor(route, 5, 0, 0, /*stored*/ 5, /*resync*/ 20.0f), 1u);
    // ...and the same position with a resync distance it does not exceed keeps the
    // stored value, so the escape cannot fire inside a normal crossing.
    EXPECT_EQ(ResolveCursor(route, 5, 0, 0, /*stored*/ 5, /*resync*/ 200.0f), 5u);
}

TEST(DcSuppressionTransitTest, AStoredCursorPastTheEndIsClamped)
{
    std::vector<Anchor> const route = Route();
    EXPECT_LT(ResolveCursor(route, 100, 0, 0, /*stored*/ 99, 60.0f), route.size());
}

TEST(DcSuppressionTransitTest, ADegenerateRouteResolvesToZero)
{
    EXPECT_EQ(ResolveCursor({}, 0, 0, 0, 0, 60.0f), 0u);
    EXPECT_EQ(ResolveCursor({ {0, 0, 0} }, 0, 0, 0, 0, 60.0f), 0u);
}

// The route is a POLYLINE, not a set of points: the cursor has to follow the
// bends, or the pack forms on the wrong side of the ramp. A leader standing off
// the line beside leg 2 belongs to leg 2.
TEST(DcSuppressionTransitTest, TheCursorProjectsOntoTheNearestSegmentNotTheNearestAnchor)
{
    std::vector<Anchor> const route = Route();
    // (30, 8, 0) is 8yd off the middle of leg 2 (20 -> 40) and 12.8yd from the
    // nearest ANCHOR either side of it.
    EXPECT_EQ(ResolveCursor(route, 30, 8, 0, /*stored*/ 0, 60.0f), 2u);
}

// --- the gather radius floor ----------------------------------------------

// THE REGRESSION, tr-20260830-125018-2. The gather gate asked for 20yd while the
// pack rung parked the raid at 21-23yd of the cursor, so nobody was ever inside
// it: nineteen consecutive driver ticks read `2/25 near` with the raid otherwise
// perfectly formed (`0 of 24 outside` a 25yd leash, trail 21.5yd), the gate burnt
// its full 60s and the tank crossed with two members.
TEST(DcSuppressionTransitTest, TheGatherRadiusFloorCoversThePacksRestBand)
{
    // The shipped defaults: leash 25, margin 4, arrival leash 2.
    EXPECT_NEAR(GatherRadiusFloor(25.0f, 4.0f, 2.0f), 23.0f, 0.001f);

    // The floor must reach the OUTER edge of the band, not the point the rung
    // aims at — a member that stops one arrival-leash short is still parked.
    float const leash = 25.0f, margin = 4.0f, arrive = 2.0f;
    EXPECT_GE(GatherRadiusFloor(leash, margin, arrive), leash - margin + arrive);

    // A tighter leash tightens the floor with it, so the relationship holds under
    // tuning rather than only on the defaults.
    EXPECT_NEAR(GatherRadiusFloor(10.0f, 4.0f, 2.0f), 8.0f, 0.001f);

    // Degenerate tuning cannot produce a negative radius.
    EXPECT_FLOAT_EQ(GatherRadiusFloor(1.0f, 40.0f, 2.0f), 0.0f);
}

// --- the pack's hold point on the route ------------------------------------

namespace
{
    // The real climb out of Blackwing Lair's staging shelf, anchors 20-24 of the
    // Broodlord row. It is a C: the walkable floor bows ~7yd EAST around a hole in
    // the mesh, so x runs -7631 -> -7623 -> -7628 while y runs monotonically south.
    // Imported verbatim (rather than abstracted to a synthetic bend) because the
    // defect this pins is a property of THESE coordinates.
    std::vector<Anchor> CShapedClimb()
    {
        return { { -7630.90f, -915.50f, 437.30f },    // 0 staging
                 { -7627.03f, -926.86f, 440.63f },    // 1 north arm
                 { -7623.17f, -938.22f, 443.28f },    // 2 the east bulge
                 { -7627.83f, -953.50f, 440.78f },    // 3 back west, lower room
                 { -7633.00f, -968.64f, 440.81f } };  // 4
    }

    float Dist2D(Anchor const& a, float x, float y)
    {
        float const dx = a.x - x, dy = a.y - y;
        return std::sqrt(dx * dx + dy * dy);
    }
}

TEST(DcSuppressionTransitTest, AnchorIndexOfFindsThePublishedAnchor)
{
    std::vector<Anchor> const route = CShapedClimb();
    // The driver publishes hints[cursor] verbatim, so this is an exact match.
    EXPECT_EQ(AnchorIndexOf(route, -7623.17f, -938.22f, 443.28f), 2u);
    // ...and the tolerant form of the same question survives float drift.
    EXPECT_EQ(AnchorIndexOf(route, -7623.20f, -938.19f, 443.31f), 2u);
    EXPECT_EQ(AnchorIndexOf({}, 0, 0, 0), 0u);
}

TEST(DcSuppressionTransitTest, TheHoldPointWalksBackAlongThePolyline)
{
    std::vector<Anchor> const route = { {0, 0, 0}, {20, 0, 0}, {40, 0, 0}, {60, 0, 0} };

    // 21yd back from anchor 3 (x=60) is x=39 — one whole leg plus one yard.
    Anchor const back = PointBehindOnRoute(route, 3, 21.0f);
    EXPECT_NEAR(back.x, 39.0f, 0.01f);
    EXPECT_NEAR(back.y, 0.0f, 0.01f);

    // Zero back-distance is the cursor itself.
    EXPECT_NEAR(PointBehindOnRoute(route, 2, 0.0f).x, 40.0f, 0.01f);
}

// There is no polyline behind anchor 0, and the honest answer there is the head
// itself — which at the staging point is exactly where a gather wants everybody.
TEST(DcSuppressionTransitTest, TheHoldPointClampsToTheHeadOfTheRoute)
{
    std::vector<Anchor> const route = { {0, 0, 0}, {20, 0, 0}, {40, 0, 0} };
    EXPECT_NEAR(PointBehindOnRoute(route, 1, 500.0f).x, 0.0f, 0.01f);
    EXPECT_NEAR(PointBehindOnRoute(route, 0, 21.0f).x, 0.0f, 0.01f);
    EXPECT_NEAR(PointBehindOnRoute({}, 0, 21.0f).x, 0.0f, 0.01f);
    // A cursor past the end is clamped rather than read out of bounds.
    EXPECT_NEAR(PointBehindOnRoute(route, 99, 0.0f).x, 40.0f, 0.01f);
}

// THE REGRESSION, tr-20260830-125018-2. A follower on the north arm holding on a
// cursor down in the lower room used to be walked to the straight-line point 21yd
// from that cursor. On this C that point is (-7626.5, -932.5) — where the only
// navmesh surface is 2.3yd ABOVE it, two yards west of which there is no surface
// at all. The bot was handed PATHFIND_NORMAL, took the straight-line shortcut and
// walked into the ramp.
//
// The route walk-back cannot produce that point: every point it returns lies on a
// segment between two anchors the probe suite certifies as walkable. Pinned here
// as "it stays east, on the bulge" — the chord's defining property is that it
// does NOT.
TEST(DcSuppressionTransitTest, TheHoldPointRidesTheBulgeRatherThanCuttingTheC)
{
    std::vector<Anchor> const route = CShapedClimb();

    // Cursor on anchor 3 (the lower room), holding 21yd back.
    Anchor const hold = PointBehindOnRoute(route, 3, 21.0f);

    // 21yd of ROUTE distance back from the lower room is up on the east bulge,
    // which is the whole point: east of the chord, and above it. (Probed against
    // the real mmtiles the floor there is z 442.4 and the hold point is 442.2 —
    // see BlackwingLairSuppressionRouteProbe.TheChordAcrossTheClimbLeavesTheMesh.)
    EXPECT_GT(hold.x, -7627.0f) << "the hold point cut west across the void";
    EXPECT_GT(hold.z, 441.0f) << "the hold point sank below the ramp";

    // ...and it is genuinely ON the polyline, not merely near it: for SOME
    // segment, the distances to its two ends sum to that segment's own length.
    // Asserted over every segment rather than a named one so the test survives an
    // anchor being inserted, which would otherwise silently move which leg the
    // walk-back lands on without changing the property being pinned.
    float bestSlack = std::numeric_limits<float>::max();
    for (std::size_t i = 1; i < route.size(); ++i)
    {
        float const legLen = Dist2D(route[i - 1], route[i].x, route[i].y);
        // Slack rounds slightly NEGATIVE on the segment the point is actually on,
        // so a "-1 means unset" sentinel would discard the winner and report the
        // runner-up. Seeded with the float max instead.
        float const slack = Dist2D(route[i - 1], hold.x, hold.y) +
                            Dist2D(route[i], hold.x, hold.y) - legLen;
        bestSlack = std::min(bestSlack, slack);
    }
    EXPECT_LT(bestSlack, 0.05f) << "the hold point is not on any authored segment";

    // For contrast, the CHORD 21yd from the same cursor toward a follower up on
    // the north arm sits west of the corridor — inside the C's mouth.
    float const bx = -7630.0f, by = -920.0f;  // a follower on the north arm
    float const cdx = bx - route[3].x, cdy = by - route[3].y;
    float const clen = std::sqrt(cdx * cdx + cdy * cdy);
    float const chordX = route[3].x + cdx * (21.0f / clen);
    EXPECT_LT(chordX, -7626.0f) << "the chord no longer cuts the C — re-derive this test";
}
