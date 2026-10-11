/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "gtest/gtest.h"

#include <algorithm>
#include <limits>
#include <map>
#include <set>

#include "TestRun/DcTestPlan.h"
#include "TestRun/DcTestPlanSummary.h"

using namespace DcTestPlan;

namespace
{
    constexpr std::uint32_t kUnlimited = std::numeric_limits<std::uint32_t>::max();

    Spec PoolSpec(std::vector<std::string> const& tokens, PickMode pick = PickMode::Bag)
    {
        Spec s;
        s.isPool = true;
        s.endless = true;
        s.concurrent = 2;
        s.pick = pick;
        for (std::string const& t : tokens)
        {
            PoolEntry e;
            e.token = t;
            s.pool.push_back(e);
        }
        return s;
    }

    std::vector<std::int32_t> Draw(PickState& st, Spec const& s, std::size_t n,
                                   std::vector<bool> const& eligible = {})
    {
        std::vector<std::int32_t> out;
        for (std::size_t i = 0; i < n; ++i)
            out.push_back(PickNext(st, s, eligible));
        return out;
    }

    // 2026-09-27 00:00:00 UTC.
    constexpr std::uint64_t kMidnightUtc = 1790467200ull;
}

// ---- pool= parsing -----------------------------------------------------------------

TEST(DcTestPlanPoolParseTest, PoolEntriesWithDifficultySuffixes)
{
    ParseResult const r = ParseStartArgs("pool=rfc,wc,kara,uk:heroic,ramparts:hc concurrent=3");
    ASSERT_TRUE(r.ok) << r.err;
    EXPECT_TRUE(r.spec.isPool);
    EXPECT_TRUE(r.spec.endless);  // no total= with a pool = run until stopped
    EXPECT_EQ(r.spec.concurrent, 3u);
    EXPECT_EQ(r.spec.pick, PickMode::Bag);
    ASSERT_EQ(r.spec.pool.size(), 5u);
    EXPECT_EQ(r.spec.pool[0].token, "rfc");
    EXPECT_FALSE(r.spec.pool[0].heroic);
    EXPECT_EQ(r.spec.pool[3].token, "uk");
    EXPECT_TRUE(r.spec.pool[3].heroic);
    EXPECT_EQ(r.spec.pool[4].Key(), "ramparts:heroic");
    EXPECT_TRUE(r.spec.dungeonToken.empty());
}

TEST(DcTestPlanPoolParseTest, TotalZeroIsEndlessTotalNIsFinite)
{
    ParseResult const endless = ParseStartArgs("pool=rfc,wc total=0");
    ASSERT_TRUE(endless.ok) << endless.err;
    EXPECT_TRUE(endless.spec.endless);

    ParseResult const finite = ParseStartArgs("pool=rfc,wc total=30");
    ASSERT_TRUE(finite.ok) << finite.err;
    EXPECT_FALSE(finite.spec.endless);
    EXPECT_EQ(finite.spec.total, 30u);
}

TEST(DcTestPlanPoolParseTest, TotalZeroWithoutPoolIsRefused)
{
    // A typo'd single-dungeon plan must not run forever.
    ParseResult const r = ParseStartArgs("deadmines total=0");
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.err.find("needs pool="), std::string::npos) << r.err;
    // ...and the positional form still insists on a total.
    EXPECT_FALSE(ParseStartArgs("deadmines").ok);
}

TEST(DcTestPlanPoolParseTest, PositionalFormIsUnchanged)
{
    ParseResult const r = ParseStartArgs("ramparts heroic total=10 size=10");
    ASSERT_TRUE(r.ok) << r.err;
    EXPECT_FALSE(r.spec.isPool);
    EXPECT_FALSE(r.spec.endless);
    EXPECT_TRUE(r.spec.pool.empty());  // the manager builds the one entry
    EXPECT_EQ(r.spec.dungeonToken, "ramparts");
    EXPECT_TRUE(r.spec.heroic);
    EXPECT_EQ(r.spec.size, 10u);
}

TEST(DcTestPlanPoolParseTest, DuplicateAndMalformedEntriesAreRefused)
{
    EXPECT_FALSE(ParseStartArgs("pool=rfc,wc,rfc").ok);
    EXPECT_FALSE(ParseStartArgs("pool=uk:hc,uk:heroic").ok);   // same entry, two spellings
    EXPECT_TRUE(ParseStartArgs("pool=uk,uk:heroic").ok);       // two difficulties = two entries
    EXPECT_FALSE(ParseStartArgs("pool=").ok);
    EXPECT_FALSE(ParseStartArgs("pool=rfc,,wc").ok);
    EXPECT_FALSE(ParseStartArgs("pool=rfc,").ok);
    EXPECT_FALSE(ParseStartArgs("pool=uk:mythic").ok);
    EXPECT_FALSE(ParseStartArgs("pool=:heroic").ok);
    EXPECT_FALSE(ParseStartArgs("pool=rfc pool=wc").ok);
}

TEST(DcTestPlanPoolParseTest, PoolExcludesTheSingleDungeonKnobs)
{
    EXPECT_FALSE(ParseStartArgs("deadmines pool=rfc,wc").ok);
    EXPECT_FALSE(ParseStartArgs("pool=rfc,wc heroic").ok);
    EXPECT_FALSE(ParseStartArgs("pool=rfc,mc size=20").ok);
}

TEST(DcTestPlanPoolParseTest, PickMode)
{
    ParseResult const r = ParseStartArgs("pool=rfc,wc pick=random seed=5");
    ASSERT_TRUE(r.ok) << r.err;
    EXPECT_EQ(r.spec.pick, PickMode::Random);
    EXPECT_EQ(r.spec.seedBase, 5u);
    EXPECT_TRUE(ParseStartArgs("pool=rfc,wc pick=bag").ok);
    EXPECT_FALSE(ParseStartArgs("pool=rfc,wc pick=weighted").ok);
}

// ---- edit args -------------------------------------------------------------------

TEST(DcTestPlanEditParseTest, PoolAndConcurrent)
{
    EditParse const r = ParseEditArgs("tp-20260928-1 pool=rfc,uk:hc concurrent=4");
    ASSERT_TRUE(r.ok) << r.err;
    EXPECT_EQ(r.edit.planId, "tp-20260928-1");
    EXPECT_TRUE(r.edit.hasPool);
    ASSERT_EQ(r.edit.pool.size(), 2u);
    EXPECT_TRUE(r.edit.pool[1].heroic);
    EXPECT_TRUE(r.edit.hasConcurrent);
    EXPECT_EQ(r.edit.concurrent, 4u);

    EditParse const only = ParseEditArgs("tp-1 concurrent=1");
    ASSERT_TRUE(only.ok) << only.err;
    EXPECT_FALSE(only.edit.hasPool);
}

TEST(DcTestPlanEditParseTest, Refusals)
{
    EXPECT_FALSE(ParseEditArgs("").ok);
    EXPECT_FALSE(ParseEditArgs("tp-1").ok);                 // nothing to change
    EXPECT_FALSE(ParseEditArgs("concurrent=2").ok);         // no planId
    EXPECT_FALSE(ParseEditArgs("tp-1 concurrent=0").ok);
    EXPECT_FALSE(ParseEditArgs("tp-1 concurrent=x").ok);
    EXPECT_FALSE(ParseEditArgs("tp-1 pool=rfc,rfc").ok);
    EXPECT_FALSE(ParseEditArgs("tp-1 total=5").ok);
    EXPECT_FALSE(ParseEditArgs("tp-1 tp-2 concurrent=2").ok);
}

// ---- endless scheduling ----------------------------------------------------------

TEST(DcTestPlanEndlessTest, LaunchesAreBoundedOnlyByConcurrency)
{
    Spec s = PoolSpec({"rfc", "wc"});
    s.concurrent = 3;
    Counters c;
    c.launched = 100000;
    c.activeNow = 1;
    EXPECT_EQ(LaunchesWanted(s, c, kUnlimited, 0), 2u);
    EXPECT_EQ(LaunchesWanted(s, c, 1, 0), 1u);
    EXPECT_EQ(LaunchesWanted(s, c, kUnlimited, 500), 0u);
}

TEST(DcTestPlanEndlessTest, FinishesOnlyWhenStoppedAndDrained)
{
    Spec const s = PoolSpec({"rfc"});
    Counters c;
    c.launched = 50;
    c.succeeded = 50;
    EXPECT_FALSE(IsFinished(s, c, false));
    EXPECT_TRUE(IsFinished(s, c, true));
    c.activeNow = 1;
    EXPECT_FALSE(IsFinished(s, c, true));
}

// ---- picker ----------------------------------------------------------------------

TEST(DcTestPlanPickTest, BagRunsEveryEntryOncePerCycle)
{
    Spec const s = PoolSpec({"a", "b", "c", "d", "e"});
    PickState st;
    SeedPicker(st, 42, 0);
    for (int cycle = 0; cycle < 4; ++cycle)
    {
        std::set<std::int32_t> seen;
        for (std::int32_t idx : Draw(st, s, 5))
        {
            ASSERT_GE(idx, 0);
            EXPECT_TRUE(seen.insert(idx).second) << "entry " << idx << " twice in cycle " << cycle;
        }
        EXPECT_EQ(seen.size(), 5u);
    }
}

TEST(DcTestPlanPickTest, SeededPickOrderIsReproducible)
{
    Spec const s = PoolSpec({"a", "b", "c", "d", "e", "f", "g", "h"});
    PickState a;
    PickState b;
    SeedPicker(a, 7, 111);
    SeedPicker(b, 7, 999);  // entropy is ignored when a seed is given
    EXPECT_EQ(Draw(a, s, 40), Draw(b, s, 40));

    Spec r = s;
    r.pick = PickMode::Random;
    SeedPicker(a, 7, 0);
    SeedPicker(b, 7, 0);
    EXPECT_EQ(Draw(a, r, 40), Draw(b, r, 40));
}

TEST(DcTestPlanPickTest, RandomDrawsOnlyEligibleEntries)
{
    Spec const s = PoolSpec({"a", "b", "c"}, PickMode::Random);
    PickState st;
    SeedPicker(st, 3, 0);
    std::map<std::int32_t, int> counts;
    for (std::int32_t idx : Draw(st, s, 300, {true, false, true}))
        ++counts[idx];
    EXPECT_EQ(counts.count(1), 0u);
    EXPECT_GT(counts[0], 0);
    EXPECT_GT(counts[2], 0);
}

TEST(DcTestPlanPickTest, NothingEligibleOrEmptyPoolPicksNone)
{
    PickState st;
    SeedPicker(st, 1, 0);
    EXPECT_EQ(PickNext(st, PoolSpec({}), {}), -1);
    EXPECT_EQ(PickNext(st, PoolSpec({"a", "b"}), {false, false}), -1);
    EXPECT_EQ(PickNext(st, PoolSpec({"a", "b"}, PickMode::Random), {false, false}), -1);
}

TEST(DcTestPlanPickTest, SkippedEntryKeepsItsTurnForLater)
{
    // A guarded heroic is not dropped from the cycle: it waits in the bag and
    // runs as soon as it is eligible again — exactly once, however many
    // cycles the others went through meanwhile.
    Spec const s = PoolSpec({"a", "hc", "b"});
    PickState st;
    SeedPicker(st, 9, 0);
    std::vector<bool> const guarded{true, false, true};
    for (std::int32_t idx : Draw(st, s, 10, guarded))
        EXPECT_NE(idx, 1);
    EXPECT_EQ(std::count(st.bag.begin(), st.bag.end(), 1u), 1);

    // Guard lifts: the waiting entry comes up before the next full cycle ends.
    std::vector<std::int32_t> const after = Draw(st, s, 3);
    EXPECT_NE(std::find(after.begin(), after.end(), 1), after.end());
}

TEST(DcTestPlanPickTest, PeekMatchesTheNextBagDraw)
{
    Spec const s = PoolSpec({"a", "b", "c"});
    PickState st;
    SeedPicker(st, 11, 0);
    PickNext(st, s, {});  // fills the bag
    for (int i = 0; i < 8; ++i)
    {
        std::int32_t const peek = PeekNext(st, s, {});
        std::int32_t const next = PickNext(st, s, {});
        if (peek >= 0)
            EXPECT_EQ(peek, next);
    }
    Spec r = s;
    r.pick = PickMode::Random;
    EXPECT_EQ(PeekNext(st, r, {}), -1);
}

TEST(DcTestPlanPickTest, PoolShrinkDropsStaleBagIndices)
{
    Spec s = PoolSpec({"a", "b", "c", "d"});
    PickState st;
    SeedPicker(st, 5, 0);
    PickNext(st, s, {});
    s.pool.resize(2);
    for (std::int32_t idx : Draw(st, s, 6))
    {
        EXPECT_GE(idx, 0);
        EXPECT_LT(idx, 2);
    }
}

// ---- instance-reset guard --------------------------------------------------------

TEST(DcTestPlanResetGuardTest, NextResetIsUtcHourStrictlyAfterNow)
{
    EXPECT_EQ(NextResetS(kMidnightUtc, 4), kMidnightUtc + 4 * 3600);
    EXPECT_EQ(NextResetS(kMidnightUtc + 4 * 3600, 4), kMidnightUtc + 28 * 3600);
    EXPECT_EQ(NextResetS(kMidnightUtc + 5 * 3600, 4), kMidnightUtc + 28 * 3600);
    EXPECT_EQ(NextResetS(kMidnightUtc, 0), kMidnightUtc + 24 * 3600);
}

TEST(DcTestPlanResetGuardTest, GuardWindowSpansBeforeAndJustAfterTheReset)
{
    std::uint64_t const reset = kMidnightUtc + 4 * 3600;
    std::uint64_t const until = reset + kResetGuardAfterS;
    EXPECT_EQ(ResetGuardUntil(reset - 46 * 60, 4, 45), 0u);
    EXPECT_EQ(ResetGuardUntil(reset - 45 * 60, 4, 45), until);
    EXPECT_EQ(ResetGuardUntil(reset - 1, 4, 45), until);
    EXPECT_EQ(ResetGuardUntil(reset, 4, 45), until);
    EXPECT_EQ(ResetGuardUntil(reset + kResetGuardAfterS - 1, 4, 45), until);
    EXPECT_EQ(ResetGuardUntil(reset + kResetGuardAfterS, 4, 45), 0u);
    EXPECT_EQ(ResetGuardUntil(reset - 60, 4, 0), 0u);  // 0 = no guard
}

TEST(DcTestPlanResetGuardTest, OnlyResetSensitiveEntriesAreHeld)
{
    Spec s = PoolSpec({"rfc", "uk", "kara"});
    s.pool[1].heroic = true;
    s.pool[1].resetSensitive = true;
    s.pool[2].resetSensitive = true;  // raid
    EXPECT_EQ(EligibleEntries(s, false), (std::vector<bool>{true, true, true}));
    EXPECT_EQ(EligibleEntries(s, true), (std::vector<bool>{true, false, false}));

    PickState st;
    SeedPicker(st, 2, 0);
    for (std::int32_t idx : Draw(st, s, 5, EligibleEntries(s, true)))
        EXPECT_EQ(idx, 0);
}

TEST(DcTestPlanResetGuardTest, AllSensitivePoolHolds)
{
    Spec s = PoolSpec({"uk", "kara"});
    for (PoolEntry& e : s.pool)
        e.resetSensitive = true;
    std::vector<bool> const eligible = EligibleEntries(s, true);
    EXPECT_TRUE(std::none_of(eligible.begin(), eligible.end(), [](bool b) { return b; }));
    PickState st;
    SeedPicker(st, 2, 0);
    EXPECT_EQ(PickNext(st, s, eligible), -1);
}

// ---- bounded accumulator ---------------------------------------------------------

TEST(DcTestPlanAccumulatorTest, MatchesTheBatchBuild)
{
    std::vector<RunOutcome> outcomes;
    for (std::uint32_t i = 0; i < 9; ++i)
    {
        RunOutcome o;
        o.runId = "tr-" + std::to_string(i);
        o.result = i % 3 ? "success" : "wipe";
        o.failReason = i % 3 ? "" : "wiped on boss";
        o.durationS = 600 + i * 37;
        o.bossRoster = {"A", "B"};
        o.bossKills = i % 3 ? std::vector<std::string>{"A", "B"} : std::vector<std::string>{"A"};
        if (!(i % 3))
        {
            o.wipeOpponent = "B";
            o.wipeOnBoss = true;
        }
        o.pulls = {{3, 4 + i % 2, false, false}, {2, 2, true, !(i % 3)}};
        outcomes.push_back(o);
    }

    DcTestPlanSummary::Accumulator acc;
    for (RunOutcome const& o : outcomes)
        acc.Add(o);
    DcTestPlanSummary::Header h;
    h.planId = "tp-x";
    EXPECT_EQ(DcTestPlanSummary::ToJsonl(h, acc.Build()),
              DcTestPlanSummary::ToJsonl(h, DcTestPlanSummary::Build(outcomes)));
    EXPECT_EQ(acc.Launched(), 9u);
    EXPECT_EQ(acc.Succeeded(), 6u);
    EXPECT_EQ(acc.Failed(), 3u);
}

TEST(DcTestPlanAccumulatorTest, RunIdCapKeepsTheMostRecent)
{
    DcTestPlanSummary::Accumulator acc(3);
    for (int i = 0; i < 5; ++i)
    {
        RunOutcome o;
        o.runId = "r" + std::to_string(i);
        o.result = "success";
        acc.Add(o);
    }
    DcTestPlanSummary::Stats const s = acc.Build();
    EXPECT_EQ(s.runIds, (std::vector<std::string>{"r2", "r3", "r4"}));
    EXPECT_TRUE(s.runIdsRecent);
    EXPECT_EQ(s.launched, 5u);
}

TEST(DcTestPlanAccumulatorTest, DistinctFailReasonsAreBounded)
{
    DcTestPlanSummary::Accumulator acc;
    std::size_t const n = DcTestPlanSummary::Accumulator::kMaxDistinctKeys + 20;
    for (std::size_t i = 0; i < n; ++i)
    {
        RunOutcome o;
        o.runId = "r";
        o.result = "stalled";
        o.failReason = "stuck at " + std::to_string(i);
        acc.Add(o);
    }
    DcTestPlanSummary::Stats const s = acc.Build();
    EXPECT_EQ(s.failReasons.size(), DcTestPlanSummary::Accumulator::kMaxDistinctKeys + 1);
    EXPECT_EQ(s.failReasons.front().key, "(other)");
    EXPECT_EQ(s.failReasons.front().count, 20u);
}

TEST(DcTestPlanAccumulatorTest, PoolLineCarriesPerEntryStats)
{
    DcTestPlanSummary::Header h;
    h.planId = "tp-p";
    h.dungeon = "pool";
    h.endless = true;
    h.checkpoint = true;
    h.pick = "bag";
    h.result = "running";
    DcTestPlanSummary::Accumulator rfc;
    RunOutcome o;
    o.runId = "r1";
    o.result = "success";
    o.durationS = 300;
    rfc.Add(o);
    std::string const line = DcTestPlanSummary::ToJsonl(
        h, rfc.Build(), {{"rfc", false, 0, true, rfc.Build()}});
    EXPECT_NE(line.find("\"endless\":true,\"checkpoint\":true,\"pick\":\"bag\",\"pool\":["
                        "{\"token\":\"rfc\",\"heroic\":false,\"size\":0,\"inPool\":true,"
                        "\"runs\":{\"launched\":1,\"succeeded\":1,\"failed\":0}"),
              std::string::npos)
        << line;
    EXPECT_EQ(line.find('\n'), std::string::npos);

    // A single-dungeon plan's line has no pool key at all.
    EXPECT_EQ(DcTestPlanSummary::ToJsonl(h, rfc.Build()).find("\"pool\":["), std::string::npos);
}

// ---- chunked pools (a console line has a length limit) ---------------------------

TEST(DcTestPlanPoolParseTest, PausedBareWordStartsThePlanPaused)
{
    ParseResult const r = ParseStartArgs("pool=rfc,wc total=0 paused");
    ASSERT_TRUE(r.ok) << r.err;
    EXPECT_TRUE(r.spec.startPaused);
    EXPECT_FALSE(ParseStartArgs("pool=rfc,wc").spec.startPaused);
    // Still exactly one positional token: `paused` is a flag, not a dungeon.
    ParseResult const single = ParseStartArgs("deadmines paused total=3");
    ASSERT_TRUE(single.ok) << single.err;
    EXPECT_EQ(single.spec.dungeonToken, "deadmines");
}

TEST(DcTestPlanEditParseTest, AddAppendsAndExcludesPool)
{
    EditParse const r = ParseEditArgs("tp-1 add=uk:hc,kara");
    ASSERT_TRUE(r.ok) << r.err;
    EXPECT_TRUE(r.edit.hasAdd);
    EXPECT_FALSE(r.edit.hasPool);
    ASSERT_EQ(r.edit.add.size(), 2u);
    EXPECT_EQ(r.edit.add[0].Key(), "uk:heroic");
    EXPECT_FALSE(ParseEditArgs("tp-1 pool=rfc add=wc").ok);
    EXPECT_FALSE(ParseEditArgs("tp-1 add=rfc,rfc").ok);
    EXPECT_FALSE(ParseEditArgs("tp-1 add=").ok);
}
