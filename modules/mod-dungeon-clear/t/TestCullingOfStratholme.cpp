/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

// The Culling of Stratholme (map 595).
//
// Two halves, the module's usual split:
//
//   1. THE WAVE KERNEL (DcCosWaves::Decide) — the whole of the wave controller's
//      per-tick reasoning, exercised without a map, a creature or an instance.
//      Unlike Halls of Reflection's, most of what it decides is when to TRAVEL:
//      nothing on this map comes to the party, so a driver that refused would
//      simply never fight.
//
//   2. THE AUTHORED DATA — the eight objectives plus the heroic ninth, the ten
//      event rows and their flags, the instance-data slots and progress values
//      hand-copied out of culling_of_stratholme.h, the crate geometry, and the
//      door rows.
//
// THE MOST IMPORTANT ASSERTIONS IN THIS FILE are the progress-value pins and the
// wave-entry list. Every gate on this map is a comparison against
// DATA_ARTHAS_EVENT, and a wrong value does not fail loudly — an off-by-one
// reads as "the party has not got there yet" for ever. And the wave census is
// only safe because three of its entries' static spawns are excluded by spawnId;
// an entry silently dropped from the list would read "the wave is dead" and
// advance nothing.

#include "gtest/gtest.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "Ai/Dungeon/DungeonClear/Data/DcEventDoorRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/DcNeverTargetRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonBossInfo.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonEventRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"
#include "Ai/Dungeon/DungeonClear/Overrides/BossRosterRegistry.h"
#include "Ai/Dungeon/DungeonClear/Overrides/ObjectiveHookRegistry.h"
#include "Ai/Dungeon/DungeonClear/Util/DcCosWaveDecision.h"

using namespace DcCullingOfStratholme;

namespace
{
    // --- the wave kernel's baseline ---------------------------------------
    //
    // Mid-phase with every probe healthy: the city intro is done (waves 1-5), one
    // wave is standing 120 yards away, nobody is in combat and the party is topped
    // off. Each test then breaks exactly one thing.
    DcCosWaves::Inputs WaveBase()
    {
        DcCosWaves::Inputs in;
        in.progress = PROGRESS_FINISHED_CITY_INTRO;
        in.progressWavesStart = PROGRESS_FINISHED_CITY_INTRO;
        in.progressMeathook = PROGRESS_KILLED_MEATHOOK;
        in.waveAlive = 4;
        in.nearestWaveDist = 120.0f;
        in.partyInCombat = false;
        in.waveEngaged = false;
        in.engageRange = WAVE_ENGAGE_RANGE;
        in.restSafeDist = WAVE_REST_SAFE_DIST;
        in.partyRecovered = true;
        in.nowMs = 1'000'000;
        in.standoffMs = WAVE_STANDOFF_MS;
        in.standoffSinceMs = 0;
        in.restBudgetMs = WAVE_REST_BUDGET_MS;
        in.restSinceMs = 0;
        in.state = static_cast<uint8>(DcCosWaves::State::Travel);
        in.stateSinceMs = 1'000'000 - 30'000;
        return in;
    }

    DungeonEvent const* Ev(uint32 id) { return DungeonEventRegistry::Find(MAP_ID, id); }

    // The Any-gated roster patch for map 595 (the seven shared objectives), and
    // the HeroicOnly one (the Corruptor). Both are looked up by gate rather than by
    // position so a future reordering of the table cannot silently swap them.
    BossRosterPatch const* Patch(DcDifficultyGate gate)
    {
        for (BossRosterPatch const& p : BossRosterRegistry::AllPatches())
            if (p.mapId == MAP_ID && p.gate == gate)
                return &p;
        return nullptr;
    }

    DungeonBossInfo const* Obj(BossRosterPatch const& p, uint32 seq)
    {
        uint32 const entry = BossRosterRegistry::ObjectiveEntry(seq);
        for (DungeonBossInfo const& b : p.add)
            if (b.entry == entry)
                return &b;
        return nullptr;
    }

    float Dist3(DungeonBossInfo const& a, float x, float y, float z)
    {
        float const dx = a.x - x, dy = a.y - y, dz = a.z - z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }
}

// ===========================================================================
//  1. THE PROGRESS COUNTER — the pins everything else is measured against
// ===========================================================================

// Hand-copied from culling_of_stratholme.h's `enum ArthasPhase` and `enum Data`.
// Both are plain unnumbered enums, so these are the values the compiler assigns,
// and getting one wrong is a silent permanent hold rather than an error.
TEST(CullingOfStratholme, InstanceDataSlotsAndProgressValuesMatchTheCore)
{
    EXPECT_EQ(MAP_ID, 595u);

    // enum Data, in declaration order: DATA_ARTHAS_EVENT is slot 0 and the guardian
    // timer is slot 1. Everything after them is a setter the instance owns.
    EXPECT_EQ(DATA_ARTHAS_EVENT, 0u);
    EXPECT_EQ(DATA_GUARDIANTIME_EVENT, 1u);

    // enum ArthasPhase, 0..11 with no gaps. The whole dungeon is this one counter.
    EXPECT_EQ(PROGRESS_NOT_STARTED, 0u);
    EXPECT_EQ(PROGRESS_CRATES_FOUND, 1u);
    EXPECT_EQ(PROGRESS_START_INTRO, 2u);
    EXPECT_EQ(PROGRESS_FINISHED_INTRO, 3u);
    EXPECT_EQ(PROGRESS_FINISHED_CITY_INTRO, 4u);
    EXPECT_EQ(PROGRESS_KILLED_MEATHOOK, 5u);
    EXPECT_EQ(PROGRESS_KILLED_SALRAMM, 6u);
    EXPECT_EQ(PROGRESS_REACHED_TOWN_HALL, 7u);
    EXPECT_EQ(PROGRESS_KILLED_EPOCH, 8u);
    EXPECT_EQ(PROGRESS_LAST_CITY, 9u);
    EXPECT_EQ(PROGRESS_BEFORE_MALGANIS, 10u);
    EXPECT_EQ(PROGRESS_FINISHED, 11u);

    // The four DungeonEncounter bits (DBC rows 293-296 normal / 297-300 heroic).
    // Mal'ganis's is the one that needs saying: his credit is a CAST_SPELL on
    // 58630, not a kill, and the spell exists as a spell_dbc row — so the bit
    // really does flip and 0xF is the expected final mask even though this map's
    // roster carries no boss rows.
    EXPECT_EQ(BIT_MEATHOOK, 0u);
    EXPECT_EQ(BIT_SALRAMM, 1u);
    EXPECT_EQ(BIT_EPOCH, 2u);
    EXPECT_EQ(BIT_MALGANIS, 3u);
}

// ===========================================================================
//  2. THE WAVE KERNEL
// ===========================================================================

// Outside the two-value window the kernel has nothing to say and must hand the
// tick back — including at PROGRESS_KILLED_SALRAMM, which is where the phase ends
// and the Town Hall objective takes over.
TEST(CullingOfStratholme, WaveKernelIsOnlyDueAtProgressFourAndFive)
{
    for (uint32 progress = 0; progress <= PROGRESS_FINISHED; ++progress)
    {
        DcCosWaves::Inputs in = WaveBase();
        in.progress = progress;
        DcCosWaves::Verdict const v = DcCosWaves::Decide(in);

        bool const inWindow = progress == PROGRESS_FINISHED_CITY_INTRO ||
                              progress == PROGRESS_KILLED_MEATHOOK;
        EXPECT_EQ(v.complete, !inWindow) << "at progress " << progress;
        if (!inWindow)
        {
            EXPECT_EQ(v.state, DcCosWaves::State::Complete) << "at progress " << progress;
            EXPECT_TRUE(v.yieldTick) << "at progress " << progress;
            EXPECT_FALSE(v.travel);
            EXPECT_FALSE(v.pullWave);
            EXPECT_EQ(v.standoffSinceMs, 0u)
                << "leaving the window must drop the standoff clock, or a party that "
                   "re-enters it opens its pull budget on a silence that already ended";
        }
    }
}

// A wave is alive and out of reach: walk to it. This is the state the driver spends
// most of the phase in, and the one thing it must never do instead is yield —
// there is nowhere for the party to be but at the wave.
TEST(CullingOfStratholme, WaveKernelTravelsToAWaveOutOfReach)
{
    DcCosWaves::Inputs in = WaveBase();
    in.nearestWaveDist = 260.0f;  // the Market Row -> Elders' Square leg
    DcCosWaves::Verdict const v = DcCosWaves::Decide(in);

    EXPECT_EQ(v.state, DcCosWaves::State::Travel);
    EXPECT_TRUE(v.travel);
    EXPECT_FALSE(v.yieldTick);
    EXPECT_FALSE(v.holdStill);
    EXPECT_FALSE(v.pullWave);
}

// Anything engaged yields the tick outright — whether the signal is the party's
// combat flag or the mob's. The driver sits above the stock combat movers, so a
// tick it claims during a fight is a tick the tank does not swing.
TEST(CullingOfStratholme, WaveKernelYieldsWhileAnythingIsEngaged)
{
    for (int which = 0; which < 2; ++which)
    {
        DcCosWaves::Inputs in = WaveBase();
        in.nearestWaveDist = 5.0f;
        if (which == 0)
            in.partyInCombat = true;
        else
            in.waveEngaged = true;

        DcCosWaves::Verdict const v = DcCosWaves::Decide(in);
        EXPECT_EQ(v.state, DcCosWaves::State::Fight) << "signal " << which;
        EXPECT_TRUE(v.yieldTick) << "signal " << which;
        EXPECT_FALSE(v.travel);
        EXPECT_FALSE(v.pullWave);
        EXPECT_FALSE(v.holdStill);
    }
}

// Fight beats everything, including a party that has not recovered: a tick spent
// deciding to rest while four mobs are swinging is a tick the tank does not swing.
TEST(CullingOfStratholme, WaveKernelFightOutranksRest)
{
    DcCosWaves::Inputs in = WaveBase();
    in.partyInCombat = true;
    in.partyRecovered = false;
    in.nearestWaveDist = 4.0f;

    DcCosWaves::Verdict const v = DcCosWaves::Decide(in);
    EXPECT_EQ(v.state, DcCosWaves::State::Fight);
    EXPECT_TRUE(v.yieldTick);
}

// Rest outranks TRAVEL, and that ordering is the design. There is no gap anywhere
// in the ten waves — the next four mobs are summoned in the same call as the
// previous wave's fourth death — so a party that does not top off before it walks
// into the next pack will not top off until Salramm is dead.
TEST(CullingOfStratholme, WaveKernelRestsBeforeWalkingToAFarWave)
{
    DcCosWaves::Inputs in = WaveBase();
    in.partyRecovered = false;
    in.nearestWaveDist = 200.0f;

    DcCosWaves::Verdict const v = DcCosWaves::Decide(in);
    EXPECT_EQ(v.state, DcCosWaves::State::Rest);
    EXPECT_TRUE(v.holdStill);
    EXPECT_FALSE(v.travel);
    // Never a yield out of combat: on this map that hands the leg to the advance
    // ladder, which walks the party off its rest and onto the Market Row anchor.
    EXPECT_FALSE(v.yieldTick);
}

// ...but it does NOT rest next to a live wave mob. Resting has to be allowed with a
// wave standing (there is no gap to use instead), so distance is the only
// safeguard there is.
TEST(CullingOfStratholme, WaveKernelDoesNotRestInsideTheRestSafeDistance)
{
    DcCosWaves::Inputs in = WaveBase();
    in.partyRecovered = false;
    in.nearestWaveDist = WAVE_REST_SAFE_DIST - 1.0f;

    DcCosWaves::Verdict const v = DcCosWaves::Decide(in);
    EXPECT_NE(v.state, DcCosWaves::State::Rest);
    EXPECT_EQ(v.state, DcCosWaves::State::Travel)
        << "inside the rest-safe distance but outside engage range, the answer is to "
           "close the last yards, not to sit down";
}

// An empty census is a HOLD and not a problem. The only windows it happens in are
// the 20 seconds before wave 1, the 10 seconds after an Arthas death, and the
// instant between a wave's last death and the next summon — and in all three the
// right answer is to stand still and wait for the census.
TEST(CullingOfStratholme, WaveKernelHoldsWhenTheCensusIsEmpty)
{
    DcCosWaves::Inputs in = WaveBase();
    in.waveAlive = 0;
    in.nearestWaveDist = -1.0f;

    DcCosWaves::Verdict const v = DcCosWaves::Decide(in);
    EXPECT_EQ(v.state, DcCosWaves::State::Hold);
    EXPECT_TRUE(v.holdStill);
    EXPECT_FALSE(v.travel);
    EXPECT_FALSE(v.pullWave);
    EXPECT_FALSE(v.yieldTick) << "a yield out of combat hands the leg to the advance "
                                 "ladder and walks the party to Market Row";
}

// The standoff breaker: a live mob inside engage range with nobody in combat starts
// a clock, holds until it expires, and then gets pulled. The clock must run from
// the START of the silence, not be re-stamped each tick.
TEST(CullingOfStratholme, WaveKernelBreaksAStandoffOnlyAfterTheBudget)
{
    DcCosWaves::Inputs in = WaveBase();
    in.nearestWaveDist = 8.0f;

    // Tick 1: the silence begins. Hold, and stamp the clock.
    DcCosWaves::Verdict first = DcCosWaves::Decide(in);
    EXPECT_EQ(first.state, DcCosWaves::State::Hold);
    EXPECT_TRUE(first.holdStill);
    EXPECT_EQ(first.standoffSinceMs, in.nowMs);

    // Part way through the budget: still holding, and the clock has NOT moved.
    DcCosWaves::Inputs mid = in;
    mid.standoffSinceMs = first.standoffSinceMs;
    mid.nowMs = in.nowMs + WAVE_STANDOFF_MS - 1;
    mid.state = first.storeState;
    DcCosWaves::Verdict const midV = DcCosWaves::Decide(mid);
    EXPECT_EQ(midV.state, DcCosWaves::State::Hold);
    EXPECT_EQ(midV.standoffSinceMs, first.standoffSinceMs)
        << "the clock must measure the CURRENT silence, not restart every tick";

    // Past the budget: pull.
    DcCosWaves::Inputs late = mid;
    late.nowMs = in.nowMs + WAVE_STANDOFF_MS;
    DcCosWaves::Verdict const lateV = DcCosWaves::Decide(late);
    EXPECT_EQ(lateV.state, DcCosWaves::State::Standoff);
    EXPECT_TRUE(lateV.pullWave);
    EXPECT_FALSE(lateV.yieldTick);
    EXPECT_FALSE(lateV.travel);
}

// ...and the clock is cleared by anything that ends the silence, so a later
// standoff is measured from its own start and not from an earlier one.
TEST(CullingOfStratholme, WaveKernelClearsTheStandoffClockWhenTheSilenceEnds)
{
    struct Case { char const* what; bool combat; float dist; };
    static constexpr Case kCases[] = {
        { "the party engaged",        true,  8.0f  },
        { "the mob is out of reach",  false, 60.0f },
    };

    for (Case const& c : kCases)
    {
        DcCosWaves::Inputs in = WaveBase();
        in.partyInCombat = c.combat;
        in.nearestWaveDist = c.dist;
        in.standoffSinceMs = in.nowMs - 999'999;  // a long-spent silence

        DcCosWaves::Verdict const v = DcCosWaves::Decide(in);
        EXPECT_EQ(v.standoffSinceMs, 0u) << c.what;
        EXPECT_FALSE(v.pullWave) << c.what;
    }
}

// THE REST HOLD IS BOUNDED, and this is the deadlock it bounds. `partyRecovered`
// is a predicate the party can fail for ever — a bot out of water, a member the rez
// ladder cannot reach, a rest target above what drinking delivers — and an
// unbounded hold on it would stop the dungeon in a gap between waves with every
// watchdog reporting a healthy party standing still.
TEST(CullingOfStratholme, WaveKernelStopsRestingOnceTheBudgetExpires)
{
    DcCosWaves::Inputs in = WaveBase();
    in.partyRecovered = false;
    in.nearestWaveDist = 200.0f;

    // Tick 1: the window opens. Rest, and stamp the clock.
    DcCosWaves::Verdict const first = DcCosWaves::Decide(in);
    ASSERT_EQ(first.state, DcCosWaves::State::Rest);
    EXPECT_EQ(first.restSinceMs, in.nowMs);
    EXPECT_FALSE(first.restBudgetExpired);

    // Part way through: still resting, clock unmoved.
    DcCosWaves::Inputs mid = in;
    mid.restSinceMs = first.restSinceMs;
    mid.nowMs = in.nowMs + WAVE_REST_BUDGET_MS - 1;
    mid.state = first.storeState;
    DcCosWaves::Verdict const midV = DcCosWaves::Decide(mid);
    EXPECT_EQ(midV.state, DcCosWaves::State::Rest);
    EXPECT_EQ(midV.restSinceMs, first.restSinceMs)
        << "the rest clock must measure the CURRENT window, not restart every tick";

    // Past the budget: give up and walk to the wave, and say so exactly once. One
    // window can still spend the whole budget on its own — the accumulator only
    // adds LATER windows to it.
    DcCosWaves::Inputs late = mid;
    late.nowMs = in.nowMs + WAVE_REST_BUDGET_MS;
    DcCosWaves::Verdict const lateV = DcCosWaves::Decide(late);
    EXPECT_EQ(lateV.state, DcCosWaves::State::Travel);
    EXPECT_TRUE(lateV.travel);
    EXPECT_TRUE(lateV.restBudgetExpired);

    // ...and on the NEXT tick, with the stored state now Travel, it does not
    // re-report, and it does not flap back into Rest on a fresh clock either.
    DcCosWaves::Inputs after = late;
    after.state = lateV.storeState;
    after.restSinceMs = lateV.restSinceMs;
    after.nowMs = late.nowMs + 100;
    DcCosWaves::Verdict const afterV = DcCosWaves::Decide(after);
    EXPECT_EQ(afterV.state, DcCosWaves::State::Travel)
        << "the clock survives the transition, so the expired window stays expired — "
           "otherwise the party inches toward the wave one tick per budget";
    EXPECT_FALSE(afterV.restBudgetExpired);
}

// A fight still CLOSES the rest window — but it no longer forgives it. The window
// is banked instead of zeroed, which is the whole fix: see the accumulator test
// below for the deadlock that used to leak through here.
TEST(CullingOfStratholme, WaveKernelClosesTheRestWindowWhenRestingStops)
{
    DcCosWaves::Inputs in = WaveBase();
    in.partyRecovered = false;
    in.nearestWaveDist = 200.0f;
    in.restSinceMs = in.nowMs - 5000;  // a window open for five seconds

    // A fight closes it, and those five seconds go into the bank.
    DcCosWaves::Inputs fighting = in;
    fighting.partyInCombat = true;
    DcCosWaves::Verdict const f = DcCosWaves::Decide(fighting);
    EXPECT_EQ(f.state, DcCosWaves::State::Fight);
    EXPECT_EQ(f.restSinceMs, 0u);
    EXPECT_EQ(f.restSpentMs, 5000u) << "an interrupted rest bought nothing — it counts";

    // So does a wave mob coming inside the rest-safe distance, which is the other
    // way a rest stops being a rest.
    DcCosWaves::Inputs crowded = in;
    crowded.nearestWaveDist = WAVE_REST_SAFE_DIST - 1.0f;
    DcCosWaves::Verdict const c = DcCosWaves::Decide(crowded);
    EXPECT_EQ(c.restSinceMs, 0u);
    EXPECT_EQ(c.restSpentMs, 5000u);

    // And leaving the window clears it too, for the same reason the standoff clock
    // is cleared there: a run that comes back must not inherit a spent budget.
    DcCosWaves::Inputs done = in;
    done.progress = PROGRESS_KILLED_SALRAMM;
    EXPECT_EQ(DcCosWaves::Decide(done).restSinceMs, 0u);
}

// THE BUDGET IS CUMULATIVE ACROSS THE PHASE, and this is the live deadlock that
// made it so. Live (tp-20260910-091724-1): a prot-paladin tank drops below the 65%
// mana floor every wave fight and cannot climb back in a gap, so the party rested,
// spent the whole budget, gave up, walked, fought for five seconds — and the fight
// zeroed the clock, handing it a fresh full budget. Six times in run -9 (714s of
// resting) and four in run -10 (381s), against 1-34s for the eight runs whose tank
// was not a mana user. A per-window budget is an unbounded hold in disguise as soon
// as anything interrupts it.
TEST(CullingOfStratholme, WaveKernelRestBudgetAccumulatesAcrossWaveFights)
{
    DcCosWaves::Inputs in = WaveBase();
    in.partyRecovered = false;
    in.nearestWaveDist = 200.0f;

    // Two thirds of the budget spent, then a wave fight interrupts.
    uint32 const twoThirds = WAVE_REST_BUDGET_MS * 2 / 3;
    in.restSinceMs = in.nowMs - twoThirds;
    DcCosWaves::Inputs fight = in;
    fight.partyInCombat = true;
    DcCosWaves::Verdict const fv = DcCosWaves::Decide(fight);
    ASSERT_EQ(fv.restSpentMs, twoThirds);

    // The party comes out of that fight still short and sits down again. It gets
    // the REMAINDER, not a fresh budget: one third in, the bound bites.
    DcCosWaves::Inputs again = in;
    again.restSpentMs = fv.restSpentMs;
    again.restSinceMs = 0;
    again.nowMs = in.nowMs + 1000;
    DcCosWaves::Verdict const openV = DcCosWaves::Decide(again);
    EXPECT_EQ(openV.state, DcCosWaves::State::Rest) << "there is budget left, so it rests";

    DcCosWaves::Inputs spent = again;
    spent.restSinceMs = openV.restSinceMs;
    spent.state = openV.storeState;
    spent.nowMs = again.nowMs + (WAVE_REST_BUDGET_MS - twoThirds);
    DcCosWaves::Verdict const spentV = DcCosWaves::Decide(spent);
    EXPECT_EQ(spentV.state, DcCosWaves::State::Travel)
        << "the second window may only spend what the first one left";
    EXPECT_TRUE(spentV.restBudgetExpired);

    // ...and from here another fight buys nothing back. The fight is what CLOSES
    // the still-open window, banking every second of it,
    DcCosWaves::Inputs fightAgain = spent;
    fightAgain.restSpentMs = spentV.restSpentMs;
    fightAgain.restSinceMs = spentV.restSinceMs;
    fightAgain.state = spentV.storeState;
    fightAgain.partyInCombat = true;
    fightAgain.nowMs = spent.nowMs + 5000;
    DcCosWaves::Verdict const fa = DcCosWaves::Decide(fightAgain);
    EXPECT_EQ(fa.restSpentMs, WAVE_REST_BUDGET_MS) << "banked, and held at the budget";

    // ...so the party comes out of it to a budget that is still gone. This is the
    // loop that ran six times live.
    DcCosWaves::Inputs loop = fightAgain;
    loop.partyInCombat = false;
    loop.restSpentMs = fa.restSpentMs;
    loop.restSinceMs = fa.restSinceMs;
    loop.state = fa.storeState;
    loop.nowMs = fightAgain.nowMs + 5000;
    EXPECT_EQ(DcCosWaves::Decide(loop).state, DcCosWaves::State::Travel)
        << "a wave fight must not launder a spent rest budget";
}

// ...but ACTUALLY RECOVERING does forgive it. The bound is on futile rest, never on
// a party that keeps reaching its target — otherwise one spent budget early in the
// phase would deny a legitimate rest for the rest of it.
TEST(CullingOfStratholme, WaveKernelRecoveringClearsTheSpentRestBudget)
{
    DcCosWaves::Inputs in = WaveBase();
    in.nearestWaveDist = 200.0f;
    in.restSpentMs = WAVE_REST_BUDGET_MS;  // wholly spent
    in.partyRecovered = true;

    DcCosWaves::Verdict const v = DcCosWaves::Decide(in);
    EXPECT_EQ(v.restSpentMs, 0u) << "recovery zeroes the bank";
    EXPECT_EQ(v.restSinceMs, 0u);

    // So the next time the party is short it may rest again on a full budget.
    DcCosWaves::Inputs later = in;
    later.restSpentMs = v.restSpentMs;
    later.partyRecovered = false;
    later.nowMs = in.nowMs + 1000;
    EXPECT_EQ(DcCosWaves::Decide(later).state, DcCosWaves::State::Rest);
}

// The state clock re-stamps on a transition and holds still otherwise — the
// telemetry prints one line per transition rather than one per tick.
TEST(CullingOfStratholme, WaveKernelStampsTheStateClockOnlyOnATransition)
{
    DcCosWaves::Inputs in = WaveBase();
    in.state = static_cast<uint8>(DcCosWaves::State::Travel);
    in.stateSinceMs = in.nowMs - 5000;

    DcCosWaves::Verdict const same = DcCosWaves::Decide(in);
    EXPECT_EQ(same.state, DcCosWaves::State::Travel);
    EXPECT_EQ(same.stateSinceMs, in.stateSinceMs);

    in.state = static_cast<uint8>(DcCosWaves::State::Hold);
    DcCosWaves::Verdict const changed = DcCosWaves::Decide(in);
    EXPECT_EQ(changed.state, DcCosWaves::State::Travel);
    EXPECT_EQ(changed.stateSinceMs, in.nowMs);
}

// Exactly one action per verdict, in every state the kernel can report. A verdict
// that set two would have the driver walk and pull in the same tick; one that set
// none would have it claim a tick and do nothing with it.
TEST(CullingOfStratholme, WaveKernelNeverProposesTwoActionsAtOnce)
{
    struct Case { char const* what; void (*mutate)(DcCosWaves::Inputs&); };
    static Case const kCases[] = {
        { "out of the window",  [](DcCosWaves::Inputs& i) { i.progress = PROGRESS_KILLED_SALRAMM; } },
        { "fighting",           [](DcCosWaves::Inputs& i) { i.partyInCombat = true; } },
        { "travelling",         [](DcCosWaves::Inputs& i) { i.nearestWaveDist = 150.0f; } },
        { "resting",            [](DcCosWaves::Inputs& i) { i.partyRecovered = false;
                                                            i.nearestWaveDist = 150.0f; } },
        { "rest budget spent",  [](DcCosWaves::Inputs& i) { i.partyRecovered = false;
                                                            i.nearestWaveDist = 150.0f;
                                                            i.restSinceMs = i.nowMs - 999'999; } },
        { "census empty",       [](DcCosWaves::Inputs& i) { i.waveAlive = 0;
                                                            i.nearestWaveDist = -1.0f; } },
        { "standoff",           [](DcCosWaves::Inputs& i) { i.nearestWaveDist = 8.0f;
                                                            i.standoffSinceMs = i.nowMs - 60'000; } },
    };

    for (Case const& c : kCases)
    {
        DcCosWaves::Inputs in = WaveBase();
        c.mutate(in);
        DcCosWaves::Verdict const v = DcCosWaves::Decide(in);

        int const actions = (v.travel ? 1 : 0) + (v.pullWave ? 1 : 0) +
                            (v.holdStill ? 1 : 0) + (v.yieldTick ? 1 : 0);
        EXPECT_EQ(actions, 1) << c.what << " proposed " << actions << " actions";
    }
}

// ===========================================================================
//  3. THE AUTHORED DATA
// ===========================================================================

// Seven objectives on both difficulties, plus the heroic Corruptor, with event ids
// and order keys on ONE scale: objective N is OBJ(N) with eventId N and
// orderOverride N. Six is deliberately absent from a normal run.
TEST(CullingOfStratholme, RosterIsEightObjectivesPlusTheHeroicCorruptor)
{
    BossRosterPatch const* any = Patch(DcDifficultyGate::Any);
    ASSERT_NE(any, nullptr) << "map 595 has no Any-gated roster patch — every run would "
                               "fail at setup with 'no boss roster for this map'";
    EXPECT_TRUE(any->remove.empty())
        << "there is nothing to remove: BossSpawnIndex derives an EMPTY list for this map "
           "because all four encounters are script TempSummons with no creature row";
    EXPECT_TRUE(any->reorder.empty()) << "...and nothing to reorder, for the same reason";
    EXPECT_EQ(any->add.size(), 8u);

    struct Row { uint32 seq; uint32 order; char const* name; float x, y, z; };
    static constexpr Row kRows[] = {
        { EVENT_CRATES,    ORDER_CRATES,    "Chromie and the plagued grain",
          CHROMIE_X, CHROMIE_Y, CHROMIE_Z },
        { EVENT_START_RP,  ORDER_START_RP,  "Chromie: start the Royal Escort",
          CHROMIE_MID_X, CHROMIE_MID_Y, CHROMIE_MID_Z },
        { EVENT_CITY_GATE, ORDER_CITY_GATE, "Arthas: the bridge and the city gate",
          BRIDGE_X, BRIDGE_Y, BRIDGE_Z },
        { EVENT_MEATHOOK,  ORDER_MEATHOOK,  "Waves 1-5 and Meathook",
          MARKET_ROW_X, MARKET_ROW_Y, MARKET_ROW_Z },
        { EVENT_SALRAMM,   ORDER_SALRAMM,   "Waves 6-10 and Salramm the Fleshcrafter",
          MARKET_ROW_X, MARKET_ROW_Y, MARKET_ROW_Z },
        { EVENT_TOWN_HALL, ORDER_TOWN_HALL, "Arthas: the Town Hall and Chrono-Lord Epoch",
          TOWN_HALL_MEET_X, TOWN_HALL_MEET_Y, TOWN_HALL_MEET_Z },
        { EVENT_FIRE_STREET, ORDER_FIRE_STREET,
          "Arthas: the secret passage and Fire Street",
          PASSAGE_X, PASSAGE_Y, PASSAGE_Z },
        { EVENT_MALGANIS,  ORDER_MALGANIS, "Arthas: Mal'ganis",
          MARKET_X, MARKET_Y, MARKET_Z },
    };

    for (Row const& r : kRows)
    {
        DungeonBossInfo const* o = Obj(*any, r.seq);
        ASSERT_NE(o, nullptr) << "objective " << r.seq << " (" << r.name << ") is missing";
        EXPECT_EQ(o->kind, DungeonAnchorKind::Objective) << r.name;
        EXPECT_EQ(o->mapId, MAP_ID) << r.name;
        EXPECT_EQ(o->eventId, r.seq)
            << r.name << ": objective N must carry eventId N — one scale, no cross-wiring";
        EXPECT_EQ(o->orderOverride, static_cast<int32>(r.order)) << r.name;
        EXPECT_EQ(o->onArriveHook, 0u)
            << r.name << ": every objective on this map is driven by its EVENT, not by an "
                         "arrival hook";
        // An objective's encounterIndex is an ordering hint only, and on this map it
        // must stay 0: given a boss's real bit, the completed-mask check would read
        // the objective as done the moment that boss died — which for the two wave
        // rows is the same instant their own gate clears.
        EXPECT_EQ(o->encounterIndex, 0u) << r.name;
        EXPECT_LT(Dist3(*o, r.x, r.y, r.z), 0.1f)
            << r.name << " is not anchored where its constants say";
        EXPECT_GT(o->arriveRadius, 0.0f) << r.name;
    }

    // The heroic patch, and it has to be a SECOND patch: on a normal run the
    // Corruptor does not exist, so a row for him would be an objective the party
    // can never satisfy.
    BossRosterPatch const* heroic = Patch(DcDifficultyGate::HeroicOnly);
    ASSERT_NE(heroic, nullptr) << "the Infinite Corruptor has no HeroicOnly roster patch";
    EXPECT_EQ(heroic->add.size(), 1u);
    DungeonBossInfo const* corruptor = Obj(*heroic, EVENT_CORRUPTOR);
    ASSERT_NE(corruptor, nullptr);
    EXPECT_EQ(corruptor->eventId, EVENT_CORRUPTOR);
    EXPECT_EQ(corruptor->orderOverride, static_cast<int32>(ORDER_CORRUPTOR));
    EXPECT_LT(Dist3(*corruptor, CORRUPTOR_X, CORRUPTOR_Y, CORRUPTOR_Z), 0.1f);

    // THE ORDERING IS GEOGRAPHY, NOT PREFERENCE, and the straight line lies about
    // it. The Corruptor stands in the Market district, which this city's street
    // graph does not connect to the wave streets at all: measured on the real
    // mmtiles he is 927yd from Market Row on foot (81 in a straight line), 1139
    // from King's Square, 1223 from where the waves start — and 143 from Arthas's
    // waypoint-54 stop. So the ONLY boundary he can be reached from is the one
    // between the Fire Street leg and Mal'ganis, which is where he sits.
    EXPECT_GT(ORDER_CORRUPTOR, ORDER_FIRE_STREET);
    EXPECT_LT(ORDER_CORRUPTOR, ORDER_MALGANIS);
}

// No boss rows anywhere, on either difficulty. This is the Old Hillsbrad decision:
// an independently navigable boss anchor would have the clear walk the party AT
// Meathook's spawn while the escort is still 300 yards back with Arthas.
TEST(CullingOfStratholme, RosterHasNoBossAnchors)
{
    for (BossRosterPatch const& p : BossRosterRegistry::AllPatches())
    {
        if (p.mapId != MAP_ID)
            continue;
        for (DungeonBossInfo const& b : p.add)
            EXPECT_EQ(b.kind, DungeonAnchorKind::Objective)
                << "entry " << b.entry << " ('" << b.name << "') is a boss anchor. All four"
                   " encounters on this map are killed INSIDE the escort and wave events;"
                   " a navigable boss anchor would fight them.";
    }
}

// Ten events, all on map 595, with the nine anchored ones matching their
// objectives one-for-one and the tenth being the conditional wave driver.
TEST(CullingOfStratholme, EventRowsAndFlagsAreAsAuthored)
{
    for (uint32 id = EVENT_CRATES; id <= EVENT_WAVES; ++id)
        ASSERT_NE(Ev(id), nullptr) << "event " << id << " is missing";

    // The eight anchored events that carry no difficulty gate.
    for (uint32 id : { EVENT_CRATES, EVENT_START_RP, EVENT_CITY_GATE, EVENT_MEATHOOK,
                       EVENT_SALRAMM, EVENT_TOWN_HALL, EVENT_FIRE_STREET,
                       EVENT_MALGANIS })
    {
        DungeonEvent const* ev = Ev(id);
        EXPECT_EQ(ev->activation, EventActivation::Anchored) << "event " << id;
        EXPECT_EQ(ev->gate, DcDifficultyGate::Any) << "event " << id;
        // PERSISTENT, every one of them. The whole dungeon is one scripted chain
        // whose legs each span several combat gaps and minutes of cutscene; a
        // non-persistent event would rewind to step 0 on every one of them.
        EXPECT_TRUE(ev->persistent)
            << "event " << id << " ('" << ev->name << "') must be Persistent — every leg of"
               " this dungeon spans combat gaps that would otherwise rewind it";
        EXPECT_FALSE(ev->drivesInCombat)
            << "event " << id << ": the flag only has an effect on Conditional events";
        EXPECT_FALSE(ev->repeatable) << "event " << id;
    }

    // The heroic Corruptor: gated, and OPTIONAL. Optional is what makes a missed
    // 26-minute clock a skipped bonus rather than a stalled run.
    DungeonEvent const* corruptor = Ev(EVENT_CORRUPTOR);
    EXPECT_EQ(corruptor->activation, EventActivation::Anchored);
    EXPECT_EQ(corruptor->gate, DcDifficultyGate::HeroicOnly)
        << "the Infinite Corruptor is a heroic-only bonus boss; on normal he is never"
           " summoned at all";
    EXPECT_FALSE(corruptor->required)
        << "the Corruptor event must be Optional: when the 26-minute timer has already"
           " expired he is despawned, and a Required event would stall the run on a"
           " bonus the party can no longer win";

    // The wave controller's five flags. Each of them is load-bearing and the
    // reasoning is at the call site; this pins the set.
    DungeonEvent const* waves = Ev(EVENT_WAVES);
    EXPECT_EQ(waves->activation, EventActivation::Conditional);
    EXPECT_TRUE(waves->condition) << "a Conditional event with no predicate never fires";
    EXPECT_TRUE(waves->repeatable);
    EXPECT_TRUE(waves->persistent);
    EXPECT_TRUE(waves->ownsThePull);
    EXPECT_TRUE(waves->drivesInCombat)
        << "there is NO inter-wave timer on this map — the next wave is summoned in the"
           " same call as the previous wave's fourth death — so an out-of-combat-only"
           " rung would get the 20 seconds before wave 1 and nothing after";
    EXPECT_TRUE(waves->stepsOwnMovement);
    EXPECT_FALSE(waves->required);
    EXPECT_EQ(waves->gate, DcDifficultyGate::Any)
        << "the waves are identical on both difficulties";
    // Neither panel hint: the two wave objectives already give the phase its rows,
    // and PanelBeforeBoss on a REPEATABLE (never-latched) event would suppress the
    // dynamic pull near that boss permanently.
    EXPECT_EQ(waves->panelGatesBossEntry, 0u);
    EXPECT_EQ(waves->panelSortAfterBossEntry, 0u);

    ASSERT_EQ(waves->steps.size(), 1u);
    EXPECT_EQ(waves->steps[0].kind, EventStepKind::Custom);
    EXPECT_EQ(waves->steps[0].hookId, HOOK_COS_WAVES);
    EXPECT_TRUE(ObjectiveHookRegistry::Has(HOOK_COS_WAVES))
        << "hook " << HOOK_COS_WAVES << " is not registered — the objective would latch"
           " Blocked and the whole wave phase would never be driven";
}

// The crate event: a leading MoveTo, a gossip, then five UseItemAt steps, each
// pointing at its own crate with the receipt GO and the item's own use-spell.
TEST(CullingOfStratholme, CrateEventUsesTheDisruptorAtEachOfTheFiveCrates)
{
    DungeonEvent const* ev = Ev(EVENT_CRATES);
    ASSERT_NE(ev, nullptr);
    ASSERT_EQ(ev->steps.size(), 7u) << "a MoveTo, one gossip, and five crates";

    // Step 0: the leading MoveTo that engages the persistent sticky latch before
    // the crate steps walk the tank 330 yards off the anchor. Old Hillsbrad's step
    // 0 verbatim — see the event's note.
    EXPECT_EQ(ev->steps[0].kind, EventStepKind::MoveTo)
        << "without a leading MoveTo the sticky latch waits for stepIndex >= 1, and the"
           " tank can close the at-objective gate on itself in the meantime";
    EXPECT_FLOAT_EQ(ev->steps[0].x, CHROMIE_X);

    // Step 1: Chromie, skippable. The crate steps grant the Disruptor themselves,
    // so a dead or missing Chromie must cost nothing but her dialogue.
    EXPECT_EQ(ev->steps[1].kind, EventStepKind::Gossip);
    EXPECT_EQ(ev->steps[1].creatureEntry, NPC_CHROMIE_START);
    EXPECT_EQ(ev->steps[1].gossipOption, 0);
    EXPECT_TRUE(ev->steps[1].skipIfMissing);

    for (std::size_t i = 0; i < 5; ++i)
    {
        EventStep const& s = ev->steps[i + 2];
        EXPECT_EQ(s.kind, EventStepKind::UseItemAt) << "crate " << (i + 1);
        EXPECT_EQ(s.itemId, ITEM_ARCANE_DISRUPTOR) << "crate " << (i + 1);
        EXPECT_EQ(s.spellId, SPELL_ARCANE_DISRUPTION) << "crate " << (i + 1);
        // The RECEIPT, not a cast target: the helper's SpellHit deletes the
        // Suspicious crate and summons a Plagued one in its place.
        EXPECT_EQ(s.goEntry, GO_PLAGUED_CRATE) << "crate " << (i + 1);
        EXPECT_FLOAT_EQ(s.x, CRATES[i].x) << "crate " << (i + 1);
        EXPECT_FLOAT_EQ(s.y, CRATES[i].y) << "crate " << (i + 1);
        EXPECT_FLOAT_EQ(s.z, CRATES[i].z) << "crate " << (i + 1);
        EXPECT_EQ(s.timeoutMs, CRATE_TIMEOUT_MS) << "crate " << (i + 1);
    }
}

// The five crate anchors are far enough apart that the step's receipt latch and
// arrival radius can never confuse one for another. This is the assertion that
// keeps the UseItemAt step honest: the latch radius is anchor-relative, so the
// NEAREST PAIR is what bounds it.
TEST(CullingOfStratholme, CrateAnchorsAreWellSeparated)
{
    float nearest = 1e9f;
    for (std::size_t i = 0; i < 5; ++i)
        for (std::size_t j = i + 1; j < 5; ++j)
        {
            float const dx = CRATES[i].x - CRATES[j].x;
            float const dy = CRATES[i].y - CRATES[j].y;
            float const dz = CRATES[i].z - CRATES[j].z;
            nearest = std::min(nearest, std::sqrt(dx * dx + dy * dy + dz * dz));
        }

    EXPECT_GT(nearest, 20.0f)
        << "the two nearest crate anchors are " << nearest << "yd apart. The UseItemAt"
           " receipt latch matches a Plagued Grain Crate within 5yd of the ANCHOR, so"
           " anchors closer than that would let crate N latch on crate N-1's receipt and"
           " the count would come up short with nothing in the log to say so.";
}

// The four escort legs: a leading MoveTo, then one EscortCreature on Arthas gated
// on the counter, with each leg's completion value and threat radius.
TEST(CullingOfStratholme, EscortLegsAreGatedOnTheCounterBehindALeadingMoveTo)
{
    // `search` is per-leg because the Town Hall leg is the only one whose escortee
    // is already moving when the step starts — see ESCORT_SEARCH_TOWN_HALL.
    struct Leg { uint32 id; uint32 doneMin; int32 doneBit; float threat; float search;
                 char const* what; };
    static constexpr Leg kLegs[] = {
        { EVENT_CITY_GATE, PROGRESS_FINISHED_CITY_INTRO, -1,
          ESCORT_THREAT_CITY, ESCORT_SEARCH, "the bridge and the city gate" },
        { EVENT_TOWN_HALL, PROGRESS_KILLED_EPOCH, static_cast<int32>(BIT_EPOCH),
          ESCORT_THREAT_TOWN_HALL, ESCORT_SEARCH_TOWN_HALL, "the Town Hall and Epoch" },
        { EVENT_FIRE_STREET, PROGRESS_BEFORE_MALGANIS, -1,
          ESCORT_THREAT_LAST_CITY, ESCORT_SEARCH, "the passage and Fire Street" },
        { EVENT_MALGANIS, PROGRESS_FINISHED, static_cast<int32>(BIT_MALGANIS),
          ESCORT_THREAT_LAST_CITY, ESCORT_SEARCH, "Mal'ganis" },
    };

    for (Leg const& leg : kLegs)
    {
        DungeonEvent const* ev = Ev(leg.id);
        ASSERT_NE(ev, nullptr) << leg.what;
        ASSERT_GE(ev->steps.size(), 2u) << leg.what;

        // THE LEADING STEP IS A MoveTo (or a garrison MoveTo, which is the same
        // kind) AND IT IS NOT DECORATION. A persistent anchored event only goes
        // sticky — letting the tank roam far from its anchor while the event drives
        // — once stepIndex >= 1 or its FRONT step is a MoveTo. An escort step is
        // neither: it never advances its index until the whole escort is over. So
        // without this the at-objective trigger goes false the moment the escort
        // walks the tank out of the arrive radius, Advance hauls him back, and the
        // pull system never stands down.
        EXPECT_EQ(ev->steps.front().kind, EventStepKind::MoveTo)
            << leg.what << ": the front step must be a MoveTo or the persistent sticky"
                           " latch can never engage and the leg deadlocks at its anchor";

        EventStep const& escort = ev->steps.back();
        EXPECT_EQ(escort.kind, EventStepKind::EscortCreature) << leg.what;
        EXPECT_EQ(escort.creatureEntry, NPC_ARTHAS) << leg.what;
        // Arthas is faction 2076 throughout, so the driver's START branch (which
        // needs the idle faction 35) never fires and the RESUME branch does all the
        // work. RESUME requires BOTH a non-negative instanceDataId and a
        // non-negative gossip option.
        EXPECT_EQ(escort.gossipOption, 0) << leg.what;
        EXPECT_EQ(escort.instanceDataId, static_cast<int32>(DATA_ARTHAS_EVENT)) << leg.what;
        EXPECT_EQ(escort.instanceDataMin, leg.doneMin) << leg.what;
        EXPECT_EQ(escort.escortDoneBit, leg.doneBit) << leg.what;
        EXPECT_EQ(escort.escortDoneEntry, 0u)
            << leg.what << ": completion is the counter (and the bit), never a boss's"
                           " existence — all four bosses are TempSummons that appear and"
                           " vanish inside the leg";
        EXPECT_FLOAT_EQ(escort.escortThreatRadius, leg.threat) << leg.what;
        EXPECT_FLOAT_EQ(escort.radius, leg.search) << leg.what;
    }

    // Only the Town Hall leg searches wider, and it must: its escortee sets off on
    // a 10s timer after Salramm and covers the 275yd to WP20 whether or not the
    // party has reached the meeting point yet.
    EXPECT_GT(ESCORT_SEARCH_TOWN_HALL, ESCORT_SEARCH);

    // The Town Hall leg's threat radius is the widest, and it has to be: Epoch is
    // summoned 40yd from Arthas's stop and walks in to about 30. The city leg's is
    // the tightest, for the cosmetic Mal'ganis standing 27yd from his stop.
    EXPECT_GT(ESCORT_THREAT_TOWN_HALL, ESCORT_THREAT_LAST_CITY);
    EXPECT_GT(ESCORT_THREAT_LAST_CITY, ESCORT_THREAT_CITY);
}

// The two wave objectives hold on the counter, at Market Row, where both bosses
// spawn — which is why they cost nothing: the party is already standing on them.
TEST(CullingOfStratholme, WaveObjectivesAreCounterHoldsAtMarketRow)
{
    struct Row { uint32 id; uint32 min; };
    static constexpr Row kRows[] = {
        { EVENT_MEATHOOK, PROGRESS_KILLED_MEATHOOK },
        { EVENT_SALRAMM,  PROGRESS_KILLED_SALRAMM  },
    };

    for (Row const& r : kRows)
    {
        DungeonEvent const* ev = Ev(r.id);
        ASSERT_NE(ev, nullptr);
        ASSERT_EQ(ev->steps.size(), 1u) << "event " << r.id << " is bookkeeping, not a"
                                           " sequence — event 9 does the fighting";
        EventStep const& s = ev->steps[0];
        EXPECT_EQ(s.kind, EventStepKind::MoveTo);
        EXPECT_EQ(s.instanceDataId, static_cast<int32>(DATA_ARTHAS_EVENT));
        EXPECT_EQ(s.instanceDataMin, r.min);
        EXPECT_FLOAT_EQ(s.x, MARKET_ROW_X);
        EXPECT_FLOAT_EQ(s.y, MARKET_ROW_Y);
        EXPECT_FLOAT_EQ(s.z, MARKET_ROW_Z);
        EXPECT_EQ(s.timeoutMs, WAVE_HOLD_TIMEOUT_MS);
    }

    // Meathook's gate is strictly below Salramm's, so the two latch in order even
    // though they share an anchor.
    EXPECT_LT(PROGRESS_KILLED_MEATHOOK, PROGRESS_KILLED_SALRAMM);
}

// THE REGRESSION GATE FOR BOTH WAYS THIS LEG HAS FAILED LIVE, which are opposite
// mistakes about the same NPC and are best read together.
//
// tp-20260909-224741-1 (0/10): anchored at the Town Hall DOOR. Arthas is a DB
// spawn, so Creature::IsUpdateNeeded gives him no exemption: with every player
// further than the instance grid activation range (Visibility.Distance.Instances,
// 170yd) he is dropped from the map's update list and stops ticking entirely — no
// unpause timer, no waypoints, no SetData. The party garrisoned at the door, 265yd
// east of his WP11 stop, and waited for a counter only he could advance. Ten runs
// read `data(0)=6 (need >= 7)` until the watchdog ended them.
//
// tp-20260910-073710-1 (0/10): anchored at WP12, his first step out of the wave
// stop. That fixed the wake-up and broke the meeting. He does not wait to be
// collected — ACTION_KILLED_SALRAMM schedules a 10s resume — and the party's own
// approach is what wakes him, so he ran east on WP13..WP20 while they walked west
// to where he had been. They passed on the road. Every run reached an empty anchor
// and stalled with "I can't keep up with the escort", the escortee-ABSENT wording.
//
// So the anchor has to be a MEETING POINT on his route, satisfying both bounds at
// once: inside 170yd of his wave stop (or he never starts) and inside the leg's
// escort search radius of the Town Hall door (or he finishes before he is seen).
TEST(CullingOfStratholme, TownHallLegMeetsArthasOnTheRoadAndEscortsHimIn)
{
    DungeonEvent const* ev = Ev(EVENT_TOWN_HALL);
    ASSERT_NE(ev, nullptr);
    ASSERT_EQ(ev->steps.size(), 2u);

    // A plain leading MoveTo, NOT a garrison: no instance-data gate on step 0. A
    // gate here is the old deadlock, because the party would stand on it waiting
    // for Arthas while being the reason he cannot move.
    EventStep const& lead = ev->steps[0];
    EXPECT_EQ(lead.kind, EventStepKind::MoveTo);
    EXPECT_EQ(lead.instanceDataId, -1)
        << "step 0 gates on instance data again — that is the tp-20260909-224741-1"
           " garrison, and it deadlocks because Arthas cannot advance the counter"
           " while the party stands 265yd away waiting for him to";
    EXPECT_FLOAT_EQ(lead.x, TOWN_HALL_MEET_X);
    EXPECT_FLOAT_EQ(lead.y, TOWN_HALL_MEET_Y);
    EXPECT_FLOAT_EQ(lead.z, TOWN_HALL_MEET_Z);

    constexpr float kArthasStopX = 2092.15f;  // Arthas WP11 / LeaderIntroPos2special
    constexpr float kArthasStopY = 1276.65f;
    float const toArthas = std::sqrt(
        (TOWN_HALL_MEET_X - kArthasStopX) * (TOWN_HALL_MEET_X - kArthasStopX) +
        (TOWN_HALL_MEET_Y - kArthasStopY) * (TOWN_HALL_MEET_Y - kArthasStopY));
    float const toDoor = std::sqrt(
        (TOWN_HALL_MEET_X - TOWN_HALL_X) * (TOWN_HALL_MEET_X - TOWN_HALL_X) +
        (TOWN_HALL_MEET_Y - TOWN_HALL_Y) * (TOWN_HALL_MEET_Y - TOWN_HALL_Y));

    // BOUND ONE — the wake-up. 170yd is the grid activation range that decides
    // whether he ticks at all, and it is measured from his stop, not from the door.
    EXPECT_LT(toArthas, 170.0f)
        << "the anchor is " << toArthas << "yd from Arthas's stop, past the 170yd"
           " instance grid activation range: he would stop being updated and never"
           " walk to WP20 (the tp-20260909-224741-1 deadlock)";

    // BOUND TWO — seeing him afterwards. His run ends at the door, so if the door
    // is inside the search radius then so is every point of the run, however long
    // the party took to arrive.
    EXPECT_LT(toDoor, ESCORT_SEARCH_TOWN_HALL)
        << "the Town Hall door is " << toDoor << "yd away, outside this leg's "
        << ESCORT_SEARCH_TOWN_HALL << "yd escort search: a party delayed by loot or"
           " a rest would arrive after Arthas had finished his run and would never"
           " find him (the tp-20260910-073710-1 stall)";

    // And the anchor must not slide back onto his stop, which is the WP12
    // authoring that let him run out from under the party.
    EXPECT_GT(toArthas, 100.0f)
        << "the anchor has drifted back to Arthas's wave stop (" << toArthas << "yd)."
           " The leg meets him ON the road; it does not wait where he starts";

    // The escort is the second step and it owns the whole walk: it must NOT carry a
    // flat timeout, because EscortCreature is watchdogOwned in the executor and its
    // distance-based dead-air clock is what covers Epoch's 32s immunity.
    EventStep const& escort = ev->steps[1];
    EXPECT_EQ(escort.kind, EventStepKind::EscortCreature);
    EXPECT_EQ(escort.instanceDataMin, PROGRESS_KILLED_EPOCH);

    // The wider search is what bound two is asserted against, so it has to be the
    // radius this step actually carries — not the module default.
    EXPECT_FLOAT_EQ(escort.radius, ESCORT_SEARCH_TOWN_HALL)
        << "the Town Hall leg dropped back to the default escort search radius; the"
           " meeting point is only safe because this leg searches wider";
}

// The heroic Corruptor event: wait for him, then seek and kill him.
TEST(CullingOfStratholme, CorruptorEventWaitsThenEngages)
{
    DungeonEvent const* ev = Ev(EVENT_CORRUPTOR);
    ASSERT_NE(ev, nullptr);
    ASSERT_EQ(ev->steps.size(), 2u);

    // The wait's real job is the case that looks like a formality: he has been
    // standing in the city for ten minutes, so a timeout means the 26-minute clock
    // expired and he has DESPAWNED — and because the event is Optional, a timeout
    // skips the objective and the clear carries on.
    EXPECT_EQ(ev->steps[0].kind, EventStepKind::WaitForSpawn);
    EXPECT_EQ(ev->steps[0].creatureEntry, NPC_INFINITE_CORRUPTOR);
    EXPECT_TRUE(ev->steps[0].wantAlive);
    EXPECT_EQ(ev->steps[0].timeoutMs, CORRUPTOR_SPAWN_TIMEOUT_MS);

    // ENGAGE, not a bare gate: he does not move and never aggros from his spawn,
    // so waiting for him to come to a held party would deadlock.
    EXPECT_EQ(ev->steps[1].kind, EventStepKind::KillCreature);
    EXPECT_EQ(ev->steps[1].creatureEntry, NPC_INFINITE_CORRUPTOR);
    EXPECT_TRUE(ev->steps[1].engage);
    EXPECT_EQ(ev->steps[1].count, 1u);
    EXPECT_EQ(ev->steps[1].timeoutMs, CORRUPTOR_KILL_TIMEOUT_MS);
}

// The Chromie-middle wait IS the crate count's audit: she is summoned 20 seconds
// after the fifth crate and not before, so waiting for her is how a short count
// fails loudly instead of silently.
TEST(CullingOfStratholme, StartRpEventWaitsForChromieMiddleThenGossipsHer)
{
    DungeonEvent const* ev = Ev(EVENT_START_RP);
    ASSERT_NE(ev, nullptr);
    ASSERT_EQ(ev->steps.size(), 2u);

    EXPECT_EQ(ev->steps[0].kind, EventStepKind::WaitForSpawn);
    EXPECT_EQ(ev->steps[0].creatureEntry, NPC_CHROMIE_MIDDLE);
    EXPECT_TRUE(ev->steps[0].wantAlive);
    EXPECT_EQ(ev->steps[0].timeoutMs, CHROMIE_MID_TIMEOUT_MS);
    EXPECT_GT(CHROMIE_MID_TIMEOUT_MS, 20000u)
        << "she is summoned 20s after the fifth crate — a timeout at or under that would"
           " fail a healthy run";

    EXPECT_EQ(ev->steps[1].kind, EventStepKind::Gossip);
    EXPECT_EQ(ev->steps[1].creatureEntry, NPC_CHROMIE_MIDDLE);
    EXPECT_EQ(ev->steps[1].gossipOption, 0);
    // She is a SUMMON and may still be settling; her script refuses the select
    // outright while the counter is not CRATES_FOUND, so a mid-walk click is a
    // silent no-op rather than an error.
    EXPECT_TRUE(ev->steps[1].waitForStill);
    // NOT skippable, unlike the entrance Chromie: her gossip is the only thing in
    // the game that sets START_INTRO, so the run genuinely cannot continue without
    // it and must stall for the human rather than march on.
    EXPECT_FALSE(ev->steps[1].skipIfMissing);
}

// The wave census list: complete, and containing both bosses.
TEST(CullingOfStratholme, WaveEntryListIsCompleteAndIncludesBothWaveBosses)
{
    std::vector<uint32> const& entries = CosWaveEntries();

    // The eight trash entries from npc_arthasAI's WavesLocations tables...
    for (uint32 e : { NPC_RISEN_ZOMBIE, NPC_DEVOURING_GHOUL, NPC_DARK_NECROMANCER,
                      NPC_TOMB_STALKER, NPC_CRYPT_FIEND, NPC_BILE_GOLEM,
                      NPC_ENRAGING_GHOUL, NPC_PATCHWORK_CONSTRUCT })
        EXPECT_NE(std::find(entries.begin(), entries.end(), e), entries.end())
            << "wave entry " << e << " is missing from the census list. A missing entry"
               " reads as 'the wave is dead', so the driver holds while four mobs stand"
               " 200 yards away and the wave counter never moves.";

    // ...plus the two bosses, which to the driver are simply wave 5 and wave 10:
    // they spawn at a cluster with no movement script and have to be walked to.
    EXPECT_NE(std::find(entries.begin(), entries.end(), NPC_MEATHOOK), entries.end());
    EXPECT_NE(std::find(entries.begin(), entries.end(), NPC_SALRAMM), entries.end());

    EXPECT_EQ(entries.size(), 10u);

    // And the entries that must NOT be there. Epoch, Mal'ganis and the Corruptor
    // are not wave mobs; counting one would send the party across the city mid-wave.
    for (uint32 e : { NPC_EPOCH, NPC_MALGANIS, NPC_INFINITE_CORRUPTOR,
                      NPC_GUARDIAN_OF_TIME, NPC_TIME_RIFT, NPC_ARTHAS })
        EXPECT_EQ(std::find(entries.begin(), entries.end(), e), entries.end())
            << "entry " << e << " is not a wave mob and must not be in the census";
}

// The four wave clusters are genuinely far apart — which is the fact that makes the
// driver necessary and the one that sizes its census radius.
TEST(CullingOfStratholme, WaveScanRadiusCoversTheWidestClusterPair)
{
    struct C { float x, y, z; char const* name; };
    static constexpr C kClusters[] = {
        { CLUSTER_KS_X, CLUSTER_KS_Y, CLUSTER_KS_Z, "King's Square" },
        { CLUSTER_FL_X, CLUSTER_FL_Y, CLUSTER_FL_Z, "Festival Lane" },
        { CLUSTER_MR_X, CLUSTER_MR_Y, CLUSTER_MR_Z, "Market Row" },
        { CLUSTER_ES_X, CLUSTER_ES_Y, CLUSTER_ES_Z, "Elders' Square" },
    };

    float widest = 0.0f;
    for (C const& a : kClusters)
        for (C const& b : kClusters)
        {
            float const d = std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) +
                                      (a.z - b.z) * (a.z - b.z));
            widest = std::max(widest, d);
        }

    EXPECT_GT(widest, 200.0f)
        << "the clusters are only " << widest << "yd apart at their widest — if that is"
           " really true the travel driver is no longer needed and this test is the"
           " place to say so";
    EXPECT_GT(WAVE_SCAN, widest)
        << "WAVE_SCAN (" << WAVE_SCAN << ") must clear the widest cluster pair (" << widest
        << "yd) from either end, or a live wave at one cluster is invisible from the other"
           " and the driver holds for ever";

    // And the travel leash has to be strictly inside engage range, or there is a
    // dead band: the kernel would ask for a walk that TravelTo refuses because the
    // leader is already inside the leash, and the driver would yield every tick.
    EXPECT_LT(WAVE_TRAVEL_LEASH, WAVE_ENGAGE_RANGE);
    // ...and the rest-safe distance has to be strictly outside it, or a party that
    // needs to rest inside engage range has no state to be in.
    EXPECT_GT(WAVE_REST_SAFE_DIST, WAVE_ENGAGE_RANGE);
    // The rest budget has to be comfortably longer than a real drink (about 30s to
    // the stock HighMana threshold) or it would cut healthy recovery short, and
    // comfortably shorter than the event's own timeout or it would not be the thing
    // that breaks a stuck rest.
    EXPECT_GT(WAVE_REST_BUDGET_MS, 60000u);
    EXPECT_LT(WAVE_REST_BUDGET_MS, WAVES_TIMEOUT_MS);
}

// The two doors the instance owns, and the four city gates it does not.
TEST(CullingOfStratholme, DoorRowsAreAsAuthored)
{
    // The bookcase needs BOTH rows — see the registry rows and the route probe.
    EXPECT_TRUE(DcEventDoorRegistry::IsScriptOnly(GO_SHKAF_GATE));
    EXPECT_TRUE(DcEventDoorRegistry::IsNavigationIgnored(GO_SHKAF_GATE));

    // The exit gate is never on the critical path (it opens after Mal'ganis), so
    // script-only alone is right.
    EXPECT_TRUE(DcEventDoorRegistry::IsScriptOnly(GO_EXIT_GATE));
    EXPECT_FALSE(DcEventDoorRegistry::IsNavigationIgnored(GO_EXIT_GATE));

    // The crates are NOT doors and must never appear in a door row.
    EXPECT_FALSE(DcEventDoorRegistry::IsScriptOnly(GO_SUSPICIOUS_CRATE));
    EXPECT_FALSE(DcEventDoorRegistry::IsScriptOnly(GO_PLAGUED_CRATE));
}

// No never-target rows on this map, and that is a decision rather than an omission.
TEST(CullingOfStratholme, NoNeverTargetRowsAreNeededOrAuthored)
{
    // The three creatures that look like they would want one do not:
    //   * the Time Rift (28409) is NOT_SELECTABLE + IMMUNE_TO_PC;
    //   * the Guardian of Time (32281) is faction 35;
    //   * Arthas (26499) is faction 2076 — friendly, so no attacker scan proposes him.
    // Every one is rejected by the ordinary filters.
    for (uint32 e : { NPC_TIME_RIFT, NPC_GUARDIAN_OF_TIME, NPC_ARTHAS })
        EXPECT_FALSE(DcNeverTargetRegistry::IsNeverTarget(MAP_ID, e))
            << "entry " << e << " has a never-target row. None is needed on this map; if"
               " one has become necessary, the comment here should say why.";

    // And Mal'ganis (26533) must NOT have one even though the city intro summons a
    // COSMETIC copy of him: the rows are static per entry, so a row would also hide
    // the real boss at the end of the dungeon. The cosmetic one is
    // IMMUNE_TO_PC|IMMUNE_TO_NPC and REACT_PASSIVE by template, which is what makes
    // the row unnecessary.
    EXPECT_FALSE(DcNeverTargetRegistry::IsNeverTarget(MAP_ID, NPC_MALGANIS))
        << "a never-target row on 26533 would hide the FINAL Mal'ganis as well as the"
           " cosmetic one — the rows are per entry and both copies share it";
}
