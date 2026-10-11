/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "gtest/gtest.h"

#include "TestRun/DcTestRunVerdict.h"

using DcTestRun::Classify;
using DcTestRun::Limits;
using DcTestRun::Observation;
using DcTestRun::Verdict;
using DcTestRun::VerdictName;

namespace
{
    // A healthy mid-run tick: nothing fired, everyone present, timers low.
    Observation Healthy()
    {
        Observation o;
        o.gmOnline = true;
        o.elapsedMs = 60 * 1000;
        o.sinceProgressMs = 30 * 1000;
        return o;
    }

    Limits DefaultLimits() { return Limits{}; }
}

// ---- the one continuing shape ---------------------------------------------------

TEST(DcTestRunVerdictTest, HealthyRunContinues)
{
    EXPECT_EQ(Classify(Healthy(), DefaultLimits()), Verdict::Continue);
}

// ---- disable outcomes are authoritative -----------------------------------------

TEST(DcTestRunVerdictTest, DisableWithAllClearedIsSuccess)
{
    Observation o = Healthy();
    o.disableFired = true;
    o.disableAllCleared = true;
    EXPECT_EQ(Classify(o, DefaultLimits()), Verdict::Success);
}

TEST(DcTestRunVerdictTest, DisableWithOtherReasonIsFailDisabled)
{
    Observation o = Healthy();
    o.disableFired = true;
    EXPECT_EQ(Classify(o, DefaultLimits()), Verdict::FailDisabled);
}

TEST(DcTestRunVerdictTest, SuccessBeatsEverySimultaneousTimer)
{
    // Disable and every watchdog trip on the same tick — the run's own
    // verdict must win, or a success could be recorded as a timeout.
    Observation o;
    o.disableFired = true;
    o.disableAllCleared = true;
    o.abortRequested = true;
    o.leaderMissing = true;
    o.gmOnline = false;
    o.paused = true;
    o.pausedForMs = 10'000'000;
    o.stalled = true;
    o.stalledForMs = 10'000'000;
    o.sinceProgressMs = 10'000'000;
    o.elapsedMs = 10'000'000;
    EXPECT_EQ(Classify(o, DefaultLimits()), Verdict::Success);
}

// ---- wipe ------------------------------------------------------------------------

TEST(DcTestRunVerdictTest, WipeInsideItsGraceStillContinues)
{
    // A battle rez / soulstone gets the grace period to un-wipe the run.
    Observation o = Healthy();
    o.partyWiped = true;
    o.wipedForMs = DefaultLimits().wipeGraceMs - 1;
    EXPECT_EQ(Classify(o, DefaultLimits()), Verdict::Continue);
}

TEST(DcTestRunVerdictTest, WipePastItsGraceFails)
{
    Observation o = Healthy();
    o.partyWiped = true;
    o.wipedForMs = DefaultLimits().wipeGraceMs;
    EXPECT_EQ(Classify(o, DefaultLimits()), Verdict::FailPartyWiped);
}

TEST(DcTestRunVerdictTest, WipeBeatsTheWatchdogsItAlsoTrips)
{
    // The regression this rung exists for: a corpse party stops progressing,
    // so the no-progress net used to claim the run — reporting a livelock
    // where the truth was a wipe. Same for the pause/stall trackers, which a
    // dead party pins high too. Wipe must outrank all three.
    Observation o = Healthy();
    o.partyWiped = true;
    o.wipedForMs = 10'000'000;
    o.paused = true;
    o.pausedForMs = 10'000'000;
    o.stalled = true;
    o.stalledForMs = 10'000'000;
    o.sinceProgressMs = 10'000'000;
    o.elapsedMs = 10'000'000;
    EXPECT_EQ(Classify(o, DefaultLimits()), Verdict::FailPartyWiped);
}

TEST(DcTestRunVerdictTest, DisableStillOutranksAWipe)
{
    // The tank dying with a survivor left routes through the in-run death
    // bailout, which disables with the death reason; that verdict is richer
    // than a bare "wipe" and must not be overwritten if both land at once.
    Observation o = Healthy();
    o.disableFired = true;
    o.partyWiped = true;
    o.wipedForMs = 10'000'000;
    EXPECT_EQ(Classify(o, DefaultLimits()), Verdict::FailDisabled);
}

TEST(DcTestRunVerdictTest, AbortStillOutranksAWipe)
{
    Observation o = Healthy();
    o.abortRequested = true;
    o.partyWiped = true;
    o.wipedForMs = 10'000'000;
    EXPECT_EQ(Classify(o, DefaultLimits()), Verdict::FailAborted);
}

// ---- abort cluster ---------------------------------------------------------------

TEST(DcTestRunVerdictTest, AbortRequestFails)
{
    Observation o = Healthy();
    o.abortRequested = true;
    EXPECT_EQ(Classify(o, DefaultLimits()), Verdict::FailAborted);
}

TEST(DcTestRunVerdictTest, LeaderMissingFails)
{
    Observation o = Healthy();
    o.leaderMissing = true;
    EXPECT_EQ(Classify(o, DefaultLimits()), Verdict::FailAborted);
}

TEST(DcTestRunVerdictTest, GmOfflineFails)
{
    Observation o = Healthy();
    o.gmOnline = false;
    EXPECT_EQ(Classify(o, DefaultLimits()), Verdict::FailAborted);
}

TEST(DcTestRunVerdictTest, AbortBeatsPauseTimeout)
{
    Observation o = Healthy();
    o.abortRequested = true;
    o.paused = true;
    o.pausedForMs = 10'000'000;
    EXPECT_EQ(Classify(o, DefaultLimits()), Verdict::FailAborted);
}

// ---- pause grace -----------------------------------------------------------------

TEST(DcTestRunVerdictTest, PauseInsideGraceContinues)
{
    Observation o = Healthy();
    o.paused = true;
    o.pausedForMs = DefaultLimits().pauseGraceMs - 1;
    EXPECT_EQ(Classify(o, DefaultLimits()), Verdict::Continue);
}

TEST(DcTestRunVerdictTest, PauseAtGraceBoundaryFails)
{
    Observation o = Healthy();
    o.paused = true;
    o.pausedForMs = DefaultLimits().pauseGraceMs;
    EXPECT_EQ(Classify(o, DefaultLimits()), Verdict::FailPausedTimeout);
}

TEST(DcTestRunVerdictTest, UnpausedIgnoresPauseTimer)
{
    // pausedForMs is stale bookkeeping once the run resumed; paused=false
    // must gate it off entirely.
    Observation o = Healthy();
    o.paused = false;
    o.pausedForMs = 10'000'000;
    EXPECT_EQ(Classify(o, DefaultLimits()), Verdict::Continue);
}

// ---- stall grace -----------------------------------------------------------------

TEST(DcTestRunVerdictTest, StallInsideGraceContinues)
{
    Observation o = Healthy();
    o.stalled = true;
    o.stalledForMs = DefaultLimits().stallGraceMs - 1;
    EXPECT_EQ(Classify(o, DefaultLimits()), Verdict::Continue);
}

TEST(DcTestRunVerdictTest, StallAtGraceBoundaryFails)
{
    Observation o = Healthy();
    o.stalled = true;
    o.stalledForMs = DefaultLimits().stallGraceMs;
    EXPECT_EQ(Classify(o, DefaultLimits()), Verdict::FailStalledTimeout);
}

TEST(DcTestRunVerdictTest, PauseTimeoutBeatsStallTimeout)
{
    Observation o = Healthy();
    o.paused = true;
    o.pausedForMs = 10'000'000;
    o.stalled = true;
    o.stalledForMs = 10'000'000;
    EXPECT_EQ(Classify(o, DefaultLimits()), Verdict::FailPausedTimeout);
}

// ---- progress + overall timers ---------------------------------------------------

TEST(DcTestRunVerdictTest, NoProgressAtLimitFails)
{
    Observation o = Healthy();
    o.sinceProgressMs = DefaultLimits().noProgressMs;
    EXPECT_EQ(Classify(o, DefaultLimits()), Verdict::FailNoProgress);
}

TEST(DcTestRunVerdictTest, OverallTimeoutAtLimitFails)
{
    Observation o = Healthy();
    o.elapsedMs = DefaultLimits().overallTimeoutMs;
    EXPECT_EQ(Classify(o, DefaultLimits()), Verdict::FailOverallTimeout);
}

TEST(DcTestRunVerdictTest, NoProgressBeatsOverallTimeout)
{
    Observation o = Healthy();
    o.sinceProgressMs = 10'000'000;
    o.elapsedMs = 10'000'000;
    EXPECT_EQ(Classify(o, DefaultLimits()), Verdict::FailNoProgress);
}

TEST(DcTestRunVerdictTest, CustomLimitsAreHonored)
{
    Limits l;
    l.overallTimeoutMs = 1000;
    Observation o = Healthy();
    o.elapsedMs = 1000;
    o.sinceProgressMs = 0;
    EXPECT_EQ(Classify(o, l), Verdict::FailOverallTimeout);
}

// ---- names -----------------------------------------------------------------------

TEST(DcTestRunVerdictTest, EveryTerminalVerdictHasAStableName)
{
    EXPECT_STREQ(VerdictName(Verdict::Success), "success");
    EXPECT_STREQ(VerdictName(Verdict::FailDisabled), "disabled");
    EXPECT_STREQ(VerdictName(Verdict::FailPartyWiped), "wipe");
    EXPECT_STREQ(VerdictName(Verdict::FailPausedTimeout), "paused_timeout");
    EXPECT_STREQ(VerdictName(Verdict::FailStalledTimeout), "stalled_timeout");
    EXPECT_STREQ(VerdictName(Verdict::FailNoProgress), "no_progress");
    EXPECT_STREQ(VerdictName(Verdict::FailOverallTimeout), "overall_timeout");
    EXPECT_STREQ(VerdictName(Verdict::FailAborted), "aborted");
    EXPECT_STREQ(VerdictName(Verdict::Continue), "continue");
}

// ---- scenarios: predicate + grace (Karazhan chess plan, T1) ------------------------

using DcTestRun::ProgressClock;
using DcTestRun::ScenarioGrace;
using DcTestRun::SuccessBy;

TEST(DcTestRunVerdictTest, ScenarioPredicateInsideGraceContinues)
{
    Observation o = Healthy();
    o.scenarioSuccess = true;
    o.graceExpired = false;
    EXPECT_EQ(Classify(o, DefaultLimits()), Verdict::Continue);
}

TEST(DcTestRunVerdictTest, ScenarioGraceExpiryIsSuccess)
{
    Observation o = Healthy();
    o.scenarioSuccess = true;
    o.graceExpired = true;
    Verdict const v = Classify(o, DefaultLimits());
    EXPECT_EQ(v, Verdict::Success);
    EXPECT_STREQ(SuccessBy(o, v), "grace");
}

TEST(DcTestRunVerdictTest, AllClearedBeatsTheGraceOnTheSameTick)
{
    Observation o = Healthy();
    o.disableFired = true;
    o.disableAllCleared = true;
    o.scenarioSuccess = true;
    o.graceExpired = true;
    Verdict const v = Classify(o, DefaultLimits());
    EXPECT_EQ(v, Verdict::Success);
    EXPECT_STREQ(SuccessBy(o, v), "allCleared");
}

TEST(DcTestRunVerdictTest, AllClearedInsideTheGraceIsAllCleared)
{
    Observation o = Healthy();
    o.disableFired = true;
    o.disableAllCleared = true;
    o.scenarioSuccess = true;
    Verdict const v = Classify(o, DefaultLimits());
    EXPECT_EQ(v, Verdict::Success);
    EXPECT_STREQ(SuccessBy(o, v), "allCleared");
}

TEST(DcTestRunVerdictTest, DisableAfterThePredicateHeldIsStillSuccess)
{
    // A death during the chest loot (or a dc off) disables the run with a
    // non-all-cleared reason — the objective was already met.
    Observation o = Healthy();
    o.disableFired = true;
    o.scenarioSuccess = true;
    Verdict const v = Classify(o, DefaultLimits());
    EXPECT_EQ(v, Verdict::Success);
    EXPECT_STREQ(SuccessBy(o, v), "predicate");
}

TEST(DcTestRunVerdictTest, WatchdogsInsideTheGraceResolveToSuccess)
{
    Observation o = Healthy();
    o.scenarioSuccess = true;
    o.partyWiped = true;
    o.wipedForMs = 10'000'000;
    o.sinceProgressMs = 10'000'000;
    o.elapsedMs = 10'000'000;
    Verdict const v = Classify(o, DefaultLimits());
    EXPECT_EQ(v, Verdict::Success);
    EXPECT_STREQ(SuccessBy(o, v), "predicate");
}

TEST(DcTestRunVerdictTest, AbortIsNotBentByThePredicate)
{
    Observation o = Healthy();
    o.scenarioSuccess = true;
    o.abortRequested = true;
    EXPECT_EQ(Classify(o, DefaultLimits()), Verdict::FailAborted);
}

TEST(DcTestRunVerdictTest, GraceExpiredWithoutThePredicateMeansNothing)
{
    // graceExpired is only meaningful with scenarioSuccess; a stray flag on a
    // full-dungeon run must not manufacture a success.
    Observation o = Healthy();
    o.graceExpired = true;
    EXPECT_EQ(Classify(o, DefaultLimits()), Verdict::Continue);
}

TEST(DcTestRunVerdictTest, PlainSuccessIsAllCleared)
{
    Observation o = Healthy();
    o.disableFired = true;
    o.disableAllCleared = true;
    EXPECT_STREQ(SuccessBy(o, Classify(o, DefaultLimits())), "allCleared");
    EXPECT_STREQ(SuccessBy(o, Verdict::FailNoProgress), "");
}

TEST(DcTestRunVerdictTest, ScenarioGraceLatchesAndCounts)
{
    ScenarioGrace g;
    g.Step(false, 1000, 5000);
    EXPECT_FALSE(g.held);
    EXPECT_FALSE(g.Expired(60'000));

    g.Step(true, 1000, 6000);  // first held: clock starts at 0
    EXPECT_TRUE(g.held);
    EXPECT_EQ(g.heldAtMs, 6000u);
    EXPECT_FALSE(g.Expired(60'000));

    // The latch holds even if the predicate reads false afterwards.
    for (int i = 0; i < 59; ++i)
        g.Step(false, 1000, 7000 + i * 1000);
    EXPECT_TRUE(g.held);
    EXPECT_FALSE(g.Expired(60'000));
    g.Step(false, 1000, 66'000);
    EXPECT_TRUE(g.Expired(60'000));
}

TEST(DcTestRunVerdictTest, ZeroGraceExpiresOnTheFirstHold)
{
    ScenarioGrace g;
    g.Step(true, 1000, 1000);
    EXPECT_TRUE(g.Expired(0));
}

// ---- no-progress clock with the event progress sequence --------------------------

TEST(DcTestRunVerdictTest, EventSequenceChangeResetsNoProgress)
{
    ProgressClock c;
    EXPECT_FALSE(c.Step(1000, false, 0));
    EXPECT_FALSE(c.Step(1000, false, 0));
    EXPECT_EQ(c.sinceMs, 2000u);

    // A chess move: no kill, no anchor, no combat change — still progress.
    EXPECT_TRUE(c.Step(1000, false, 1));
    EXPECT_EQ(c.sinceMs, 0u);

    // Same value next tick: the change was consumed.
    EXPECT_FALSE(c.Step(1000, false, 1));
    EXPECT_EQ(c.sinceMs, 1000u);
}

TEST(DcTestRunVerdictTest, EventSequenceFirstSampleOnlySeeds)
{
    // A counter already non-zero when monitoring opens is not progress.
    ProgressClock c;
    c.sinceMs = 5000;
    EXPECT_FALSE(c.Step(1000, false, 42));
    EXPECT_EQ(c.sinceMs, 6000u);
}

TEST(DcTestRunVerdictTest, EventSequenceWrapStillCounts)
{
    ProgressClock c;
    c.Step(1000, false, 0xFFFFFFFFu);
    EXPECT_TRUE(c.Step(1000, false, 0u));
}

TEST(DcTestRunVerdictTest, OtherProgressStillResetsWithoutTheSequence)
{
    ProgressClock c;
    c.Step(1000, false, 7);
    c.Step(1000, false, 7);
    EXPECT_TRUE(c.Step(1000, true, 7));
    EXPECT_EQ(c.sinceMs, 0u);
}

TEST(DcTestRunVerdictTest, LongEventWithSequenceBumpsNeverTripsNoProgress)
{
    // A 10-minute game: one move every 20s, nothing else moving. Fed through
    // the clock and into the kernel at a 300s scenario budget, it never fails.
    Limits l;
    l.noProgressMs = 300 * 1000;
    ProgressClock c;
    std::uint32_t seq = 0;
    for (std::uint32_t t = 1; t <= 600; ++t)
    {
        if (t % 20 == 0)
            ++seq;
        c.Step(1000, false, seq);
        Observation o = Healthy();
        o.sinceProgressMs = c.sinceMs;
        o.elapsedMs = t * 1000;
        ASSERT_EQ(Classify(o, l), Verdict::Continue) << "t=" << t;
    }
}
