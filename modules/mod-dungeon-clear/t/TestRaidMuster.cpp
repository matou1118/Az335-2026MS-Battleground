/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "gtest/gtest.h"

#include "Ai/Dungeon/DungeonClear/Util/DcRaidMusterDecision.h"

#include <cstdint>
#include <vector>

// The raid pre-boss muster phase machine (raid-support plan, Plan C):
// Idle -> Resting -> Rebuffing -> Ready, every phase timeout-bounded.

using DcRaidMusterDecision::Decide;
using DcRaidMusterDecision::Inputs;
using DcRaidMusterDecision::Phase;

namespace
{
    // The authored defaults (DcSettingsRegistry): 35s rest, 25s rebuff, 60s for
    // the whole muster. armedSinceMs defaults to the phase stamp — the tests
    // that care about the whole-muster ceiling set it apart explicitly.
    Inputs In(bool staged, bool topped, bool rebuffDone, std::uint32_t now,
              std::uint32_t since, std::uint32_t restMs = 35000, std::uint32_t rebuffMs = 25000,
              std::uint32_t totalMs = 60000, std::uint32_t armed = 0)
    {
        Inputs in;
        in.staged = staged;
        in.topped = topped;
        in.rebuffDone = rebuffDone;
        in.nowMs = now;
        in.phaseSinceMs = since;
        in.armedSinceMs = armed ? armed : since;
        in.restTimeoutMs = restMs;
        in.rebuffTimeoutMs = rebuffMs;
        in.totalTimeoutMs = totalMs;
        return in;
    }
}

TEST(DcRaidMusterTest, IdleArmsIntoRestingAndHolds)
{
    auto const v = Decide(Phase::Idle, In(false, false, false, 1000, 0));
    EXPECT_EQ(v.phase, Phase::Resting);
    EXPECT_TRUE(v.hold);
    EXPECT_FALSE(v.beginRebuff);
}

TEST(DcRaidMusterTest, RestingHoldsUntilStagedAndTopped)
{
    // Staged but not topped: hold.
    auto v = Decide(Phase::Resting, In(true, false, false, 5000, 1000));
    EXPECT_EQ(v.phase, Phase::Resting);
    EXPECT_TRUE(v.hold);
    // Topped but a straggler outside: hold — the boss gate is strict.
    v = Decide(Phase::Resting, In(false, true, false, 5000, 1000));
    EXPECT_EQ(v.phase, Phase::Resting);
    EXPECT_TRUE(v.hold);
    // Both: advance to the rebuff round, still holding this tick.
    v = Decide(Phase::Resting, In(true, true, false, 5000, 1000));
    EXPECT_EQ(v.phase, Phase::Rebuffing);
    EXPECT_TRUE(v.hold);
    EXPECT_TRUE(v.beginRebuff);
    EXPECT_FALSE(v.timedOut);
}

TEST(DcRaidMusterTest, RestingTimesOutIntoRebuff)
{
    // 35s of resting spent, but the 60s whole-muster budget still has room:
    // hand the rebuff round its share rather than skipping the buffs.
    auto const v = Decide(Phase::Resting, In(false, false, false,
                                             /*now*/ 36001, /*since*/ 1000));
    EXPECT_EQ(v.phase, Phase::Rebuffing);
    EXPECT_TRUE(v.beginRebuff);
    EXPECT_TRUE(v.timedOut);
    EXPECT_TRUE(v.hold);
    EXPECT_FALSE(v.cancelRebuff);
}

TEST(DcRaidMusterTest, RebuffHoldsUntilDoneThenReleases)
{
    auto v = Decide(Phase::Rebuffing, In(true, true, false, 10000, 6000));
    EXPECT_EQ(v.phase, Phase::Rebuffing);
    EXPECT_TRUE(v.hold);
    v = Decide(Phase::Rebuffing, In(true, true, true, 12000, 6000));
    EXPECT_EQ(v.phase, Phase::Ready);
    EXPECT_FALSE(v.hold);
    EXPECT_FALSE(v.timedOut);
}

TEST(DcRaidMusterTest, RebuffTimesOutIntoReady)
{
    // Rebuff phase entered at 6000 with 25s to spend; the muster armed at 5000
    // so the 60s ceiling is nowhere near — the phase bound is what fires, and
    // the still-open buff windows are cancelled with it.
    auto const v = Decide(Phase::Rebuffing, In(true, true, false,
                                               /*now*/ 31001, /*since*/ 6000,
                                               35000, 25000, 60000, /*armed*/ 5000));
    EXPECT_EQ(v.phase, Phase::Ready);
    EXPECT_FALSE(v.hold);
    EXPECT_TRUE(v.timedOut);
    EXPECT_TRUE(v.cancelRebuff);
}

TEST(DcRaidMusterTest, TotalBudgetReleasesFromResting)
{
    // A raid that can never top off: the rest bound would only hand it to the
    // buff round, but the whole-muster ceiling is spent — pull now.
    auto const v = Decide(Phase::Resting, In(false, false, false,
                                             /*now*/ 61001, /*since*/ 1000,
                                             /*rest*/ 0, 25000, 60000, /*armed*/ 1000));
    EXPECT_EQ(v.phase, Phase::Ready);
    EXPECT_FALSE(v.hold);
    EXPECT_TRUE(v.timedOut);
    EXPECT_TRUE(v.cancelRebuff);
    EXPECT_FALSE(v.beginRebuff);
}

TEST(DcRaidMusterTest, TotalBudgetReleasesFromRebuffing)
{
    // Buffs that will not land: 60s after the muster armed the pull goes out
    // and the open windows are cancelled, even though this phase's own 25s
    // bound has not expired.
    auto const v = Decide(Phase::Rebuffing, In(true, true, false,
                                               /*now*/ 61001, /*since*/ 50000,
                                               35000, 25000, 60000, /*armed*/ 1000));
    EXPECT_EQ(v.phase, Phase::Ready);
    EXPECT_FALSE(v.hold);
    EXPECT_TRUE(v.timedOut);
    EXPECT_TRUE(v.cancelRebuff);
}

TEST(DcRaidMusterTest, TotalBudgetNeverPreemptsAFinishedMuster)
{
    // The ceiling only fires from a pre-Ready phase, and a muster that finished
    // on its own merits never reports a cancel.
    auto v = Decide(Phase::Rebuffing, In(true, true, /*rebuffDone*/ true,
                                         /*now*/ 20000, /*since*/ 10000,
                                         35000, 25000, 60000, /*armed*/ 1000));
    EXPECT_EQ(v.phase, Phase::Ready);
    EXPECT_FALSE(v.timedOut);
    EXPECT_FALSE(v.cancelRebuff);

    v = Decide(Phase::Ready, In(false, false, false, /*now*/ 999999, /*since*/ 1000,
                                35000, 25000, 60000, /*armed*/ 1000));
    EXPECT_EQ(v.phase, Phase::Ready);
    EXPECT_FALSE(v.hold);
    EXPECT_FALSE(v.cancelRebuff);
}

TEST(DcRaidMusterTest, ZeroTotalBudgetNeverExpires)
{
    auto const v = Decide(Phase::Rebuffing, In(true, true, false,
                                               /*now*/ 999999, /*since*/ 999000,
                                               35000, /*rebuff*/ 0, /*total*/ 0,
                                               /*armed*/ 1));
    EXPECT_EQ(v.phase, Phase::Rebuffing);
    EXPECT_TRUE(v.hold);
    EXPECT_FALSE(v.cancelRebuff);
}

TEST(DcRaidMusterTest, IdleArmingIgnoresAStaleTotalClock)
{
    // Re-arming for a new boss clears the armed stamp; a 0 stamp must never
    // read as "budget spent" and skip the muster outright.
    auto const v = Decide(Phase::Idle, In(false, false, false, /*now*/ 999999,
                                          /*since*/ 0, 35000, 25000, 60000,
                                          /*armed*/ 0));
    EXPECT_EQ(v.phase, Phase::Resting);
    EXPECT_TRUE(v.hold);
}

TEST(DcRaidMusterTest, ReadyStaysReleased)
{
    auto const v = Decide(Phase::Ready, In(false, false, false, 99000, 50000));
    EXPECT_EQ(v.phase, Phase::Ready);
    EXPECT_FALSE(v.hold);
}

TEST(DcRaidMusterTest, ZeroTimeoutNeverExpires)
{
    auto const v = Decide(Phase::Resting,
                          In(false, false, false, 999999, 1, /*rest*/ 0,
                             /*rebuff*/ 25000, /*total*/ 0));
    EXPECT_EQ(v.phase, Phase::Resting);
    EXPECT_TRUE(v.hold);
}

// --- THE BUDGET ONLY EXISTS WHERE THE KERNEL IS ASKED -----------------------
//
// Every bound here is `now - since >= budget`, evaluated on the tick the glue
// calls Decide. That makes the CALL SITE part of the contract, and it is the
// half that was wrong live: the muster was hosted only in
// DungeonClearEngageBossAction, behind DungeonClearAtBossTrigger, whose last
// gate was DcPartyState::IsBetweenPullsReady — a gate the muster itself made
// unsatisfiable by pushing RestHealthPct/RestManaPct to 100 (IsPartyReady is
// strict on every healer, and the priest single-target buffing the raid is a
// healer who is never at full mana). Vaelastrasz, 2026-08-28: the kernel was
// asked three times in 126s against a 60s ceiling, and four sibling raids stood
// there 77s, 89s, 116s and 218s.
//
// These two tests pin both halves: sparse sampling releases late no matter how
// correct the kernel is, and per-tick sampling releases on the budget. The fix
// is the call site (DungeonClearAtBossTrigger ticks DcRaidMuster::Holds every
// tick and gates on its verdict); this is the statement of why it has to.
namespace
{
    // Drive the machine from Idle over a list of evaluation offsets (ms from
    // the arming tick), with a raid that never comes staged/topped and buffs
    // that never finish. Returns how long after arming the tick that first read
    // Ready happened. Offsets are taken from a non-zero epoch because
    // Expired() treats a zero stamp as "unset".
    std::uint32_t ReleaseDelayOverSamples(std::vector<std::uint32_t> const& offsetsMs)
    {
        constexpr std::uint32_t kEpoch = 1000;
        Phase phase = Phase::Idle;
        std::uint32_t phaseSince = 0;
        std::uint32_t armed = 0;
        for (std::uint32_t off : offsetsMs)
        {
            std::uint32_t const now = kEpoch + off;
            Inputs in = In(false, false, false, now, phaseSince, 35000, 25000, 60000,
                           /*armed*/ 1);
            in.armedSinceMs = armed;
            auto const v = Decide(phase, in);
            if (v.phase != phase)
            {
                phase = v.phase;
                phaseSince = now;
                if (phase == Phase::Resting)
                    armed = now;   // the glue stamps the ceiling on the arming tick
            }
            if (phase == Phase::Ready)
                return now - armed;
        }
        return 0;
    }
}

TEST(DcRaidMusterTest, SparseSamplingReleasesLongPastTheCeiling)
{
    // The live Vaelastrasz sample times (13:28:40 / 13:29:32 / 13:30:46, in ms
    // from the arming tick). The kernel does the right thing at every one of
    // them and the raid still stands there 126s on a 60s budget.
    EXPECT_EQ(ReleaseDelayOverSamples({0, 52000, 126000}), 126000u);
}

TEST(DcRaidMusterTest, PerTickSamplingReleasesOnTheCeiling)
{
    std::vector<std::uint32_t> ticks;
    for (std::uint32_t t = 0; t <= 200000; t += 100)
        ticks.push_back(t);
    // 35s rest bound hands to the buff round, then the 60s ceiling fires from
    // Rebuffing — released within one tick of the budget, not two minutes past.
    EXPECT_EQ(ReleaseDelayOverSamples(ticks), 60000u);
}
