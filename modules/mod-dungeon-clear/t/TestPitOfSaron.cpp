/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

// Pit of Saron (map 658).
//
// Two halves, and the split mirrors the module's:
//
//   1. THE DECISION KERNEL (DcPosGauntlet::Decide) — the whole of the gauntlet
//      driver's per-tick reasoning, exercised without a map, a creature or an
//      instance. This is where the map's genuinely dangerous behaviour lives: a
//      gate crossed out of turn is refused SILENTLY, so the states that decide
//      when to walk into one are the states that decide whether the run finishes.
//
//   2. THE AUTHORED DATA — the roster patch that makes the third boss exist at
//      all, the two event rows and their flags, the door row, and the geometry
//      invariants that are cheap enough to assert without a navmesh (the
//      route-probe suite owns the ones that are not).
//
// The single most important assertion in this file is the roster one, because the
// bug it pins does not look like a bug: with Scourgelord Tyrannus missing from the
// derived roster, a Pit of Saron run REPORTS SUCCESS at 2/2 bosses having never
// walked past the Krick outro.

#include "gtest/gtest.h"

#include <cmath>
#include <string>
#include <vector>

#include "Ai/Dungeon/DungeonClear/Data/DcEventDoorRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/DcHazardRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonBossInfo.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonEventRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"
#include "Ai/Dungeon/DungeonClear/Overrides/BossRosterRegistry.h"
#include "Ai/Dungeon/DungeonClear/Overrides/ObjectiveHookRegistry.h"
#include "Ai/Dungeon/DungeonClear/Util/DcPosGauntletDecision.h"

using namespace DcPitOfSaron;
using DcPosGauntlet::State;

namespace
{
    // A snapshot in the middle of the gauntlet with every probe healthy, which
    // each test then breaks in exactly one place. Progress 2 with the
    // orchestrator NOT yet in place is the state the party is really in the tick
    // Ick dies, so it is the natural baseline.
    DcPosGauntlet::Inputs Base()
    {
        DcPosGauntlet::Inputs in;
        in.progress = PROGRESS_FINISHED_KRICK_SCENE;
        in.bossesDone = true;
        in.tyrannusDone = false;
        in.rpInPlace = false;
        in.waveAlive = 0;
        in.waveArmed = 0;
        in.nearestWaveDist = -1.0f;
        in.waveEngageRange = WAVE_ENGAGE_RANGE;
        in.partyInCombat = false;
        in.distToStage = 0.0f;
        in.stageLeash = STAGE_LEASH;
        in.distToGate = 200.0f;
        in.gateLeash = GATE_LEASH;
        in.living = 5;
        in.nearGate = 5;
        in.gatherQuorum = GATHER_QUORUM;
        in.nowMs = 1'000'000;
        in.phase = PROGRESS_FINISHED_KRICK_SCENE;
        in.phaseSinceMs = 1'000'000 - 120'000;  // the outro has been running two minutes
        in.gateHoldSinceMs = 0;
        in.waveHoldSinceMs = 0;
        in.stallReported = false;
        in.waveGraceMs = WAVE_SPAWN_GRACE_MS;
        in.waveStandoffMs = WAVE_STANDOFF_MS;
        in.gateStallMs = GATE_STALL_MS;
        in.waveArmedQuorum = WAVE1_ARMED_QUORUM;
        in.waveArmMs = WAVE1_ARM_MS;

        // THE ARMING HOLD IS ALREADY SPENT in the baseline, because every test but
        // the arming block itself is asking about a wave the party has already been
        // released onto — and a latch left false would answer all of them with
        // "still waiting for the 4-packs". Section 4a below is where the latch is
        // false, and it is the only place it should ever be.
        in.waveArmedLatched = true;
        return in;
    }

    // The kernel's contract in one place: every verdict either claims the tick or
    // yields it, and a claim always names exactly one thing to do.
    void ExpectWellFormed(DcPosGauntlet::Verdict const& v)
    {
        int const acts = int(v.walkToStage) + int(v.walkToGate) + int(v.engageWave) +
                         int(v.pullWave) + int(v.holdStill);
        if (v.yieldTick)
            EXPECT_EQ(acts, 0) << "a yielded tick must not also issue movement";
        else
            EXPECT_EQ(acts, 1) << "a claimed tick must name exactly one action";
        if (v.forge)
            EXPECT_TRUE(DcPosGauntlet::IsGate(v.state))
                << "only a gate state may forge an areatrigger";
    }
}

// --- 1. the window ---------------------------------------------------------

// The gauntlet is EXACTLY progress 2..4 with both earlier bosses down and
// Tyrannus alive. Everything else is somebody else's leg, and the kernel says so
// by yielding rather than by holding — a hold outside the window would park the
// party wherever it happened to be standing.
TEST(DcPosGauntletTest, TheWindowIsProgressTwoToFourAndNothingElse)
{
    struct Case { char const* what; uint32 progress; bool bosses; bool tyrannus; };
    static constexpr Case kOut[] = {
        { "before the intro",        PROGRESS_NONE,                 true,  false },
        { "before Ick dies",         PROGRESS_FINISHED_INTRO,       true,  false },
        { "after the tunnel warn",   PROGRESS_AFTER_TUNNEL_WARN,    true,  false },
        { "during Tyrannus's intro", PROGRESS_TYRANNUS_INTRO,       true,  false },
        { "Tyrannus already dead",   PROGRESS_AFTER_WARN_1,         true,  true  },
        { "a boss still alive",      PROGRESS_AFTER_WARN_1,         false, false },
    };

    for (Case const& c : kOut)
    {
        DcPosGauntlet::Inputs in = Base();
        in.progress = c.progress;
        in.bossesDone = c.bosses;
        in.tyrannusDone = c.tyrannus;
        DcPosGauntlet::Verdict const v = DcPosGauntlet::Decide(in);

        EXPECT_EQ(v.state, State::Complete) << c.what;
        EXPECT_TRUE(v.complete) << c.what;
        EXPECT_TRUE(v.yieldTick) << c.what << ": outside its window the driver must hand the"
                                    " tick back, not park the party";
        EXPECT_FALSE(v.forge) << c.what;
        ExpectWellFormed(v);
    }

    // ...and inside it, it is not complete.
    for (uint32 p : { PROGRESS_FINISHED_KRICK_SCENE, PROGRESS_AFTER_WARN_1,
                      PROGRESS_AFTER_WARN_2 })
    {
        DcPosGauntlet::Inputs in = Base();
        in.progress = p;
        in.phase = p;
        DcPosGauntlet::Verdict const v = DcPosGauntlet::Decide(in);
        EXPECT_FALSE(v.complete) << "progress " << p << " is inside the gauntlet";
    }
}

// The bosses-done probe is NOT redundant with the progress one, and this pins
// why: npc_pos_tyrannus_eventsAI::SetData bails on Garfrost/Ick before it looks
// at which id it was handed, so a party at progress 3 with a boss somehow alive
// would forge gate 2 for ever with nothing able to accept it.
TEST(DcPosGauntletTest, ABossStillAliveEndsTheDriverEvenMidGauntlet)
{
    DcPosGauntlet::Inputs in = Base();
    in.progress = PROGRESS_AFTER_WARN_2;
    in.phase = PROGRESS_AFTER_WARN_2;
    in.bossesDone = false;
    in.distToGate = 1.0f;
    in.nearestWaveDist = -1.0f;

    DcPosGauntlet::Verdict const v = DcPosGauntlet::Decide(in);
    EXPECT_TRUE(v.complete);
    EXPECT_FALSE(v.forge);
}

// --- 2. the ARM state ------------------------------------------------------

// The orchestrator is still flying. The driver holds the party back and CLAIMS
// the tick — yielding here hands the leg to DcRel::Advance (15), which walks the
// party 88yd into an ambush trigger that will refuse them for a minute and a
// half.
TEST(DcPosGauntletTest, ArmHoldsThePartyBackUntilTheOrchestratorArrives)
{
    DcPosGauntlet::Inputs in = Base();
    in.rpInPlace = false;
    in.distToStage = 40.0f;

    DcPosGauntlet::Verdict v = DcPosGauntlet::Decide(in);
    EXPECT_EQ(v.state, State::Arm);
    EXPECT_TRUE(v.walkToStage) << "the leader is 40yd off the staging point";
    EXPECT_FALSE(v.yieldTick);
    EXPECT_FALSE(v.forge);
    ExpectWellFormed(v);

    // On the staging point: stand still, still claiming the tick.
    in.distToStage = STAGE_LEASH - 0.5f;
    v = DcPosGauntlet::Decide(in);
    EXPECT_EQ(v.state, State::Arm);
    EXPECT_FALSE(v.walkToStage);
    EXPECT_TRUE(v.holdStill);
    EXPECT_FALSE(v.yieldTick);
    ExpectWellFormed(v);
}

// The moment the orchestrator lands on PTSTyrannusWaitPos1 the ARM hold ends and
// the gate opens. This is the ONE transition on the map that is timing-derived
// nowhere: it is a live position read, so a slow outro costs seconds and not a
// run.
TEST(DcPosGauntletTest, ArmReleasesIntoGateOneWhenTheOrchestratorIsInPlace)
{
    DcPosGauntlet::Inputs in = Base();
    in.rpInPlace = true;
    in.distToGate = 90.0f;

    DcPosGauntlet::Verdict const v = DcPosGauntlet::Decide(in);
    EXPECT_EQ(v.state, State::Gate1);
    EXPECT_TRUE(v.walkToGate);
    EXPECT_FALSE(v.walkToStage);
    ExpectWellFormed(v);
}

// --- 3. the gates ----------------------------------------------------------

TEST(DcPosGauntletTest, AGateIsWalkedIntoThenForgedOnceThePartyIsFormed)
{
    DcPosGauntlet::Inputs in = Base();
    in.rpInPlace = true;

    // Far: walk, do not forge (a forge from outside the sphere is a no-op anyway,
    // but issuing one would muddy the "the gate has been forged and refused"
    // watchdog below).
    in.distToGate = 30.0f;
    DcPosGauntlet::Verdict v = DcPosGauntlet::Decide(in);
    EXPECT_TRUE(v.walkToGate);
    EXPECT_FALSE(v.forge);
    EXPECT_EQ(v.gateHoldSinceMs, 0u) << "the gate clock must not start while still walking";

    // In the gate but strung out: hold, do not forge. Crossing a gate short-handed
    // is how a party meets a ten-mob ambush with two of its five members.
    in.distToGate = 1.0f;
    in.nearGate = 2;  // 2 of 5 == 0.40, under the 0.75 quorum
    v = DcPosGauntlet::Decide(in);
    EXPECT_TRUE(DcPosGauntlet::IsGate(v.state));
    EXPECT_FALSE(v.quorumMet);
    EXPECT_FALSE(v.forge);
    EXPECT_TRUE(v.holdStill);
    EXPECT_GT(v.gateHoldSinceMs, 0u) << "the gate clock starts on ARRIVAL, not on the forge";
    ExpectWellFormed(v);

    // Formed up: forge.
    in.nearGate = 4;  // 4 of 5 == 0.80
    v = DcPosGauntlet::Decide(in);
    EXPECT_TRUE(v.quorumMet);
    EXPECT_TRUE(v.forge);
    EXPECT_TRUE(v.holdStill);
    ExpectWellFormed(v);
}

// A lone leader is trivially a quorum — a solo `.dc test` run and a party whose
// followers are all dead must not both deadlock at a gate. The rez ladder, not
// this kernel, is what gets them back up.
TEST(DcPosGauntletTest, ALoneLeaderIsAQuorum)
{
    DcPosGauntlet::Inputs in = Base();
    in.rpInPlace = true;
    in.distToGate = 1.0f;
    in.living = 1;
    in.nearGate = 1;

    DcPosGauntlet::Verdict const v = DcPosGauntlet::Decide(in);
    EXPECT_TRUE(v.quorumMet);
    EXPECT_TRUE(v.forge);
}

// Which trigger each phase is at. The kernel names the STATE and the glue resolves
// the trigger from the same progress value, so this pins the mapping the two
// halves have to agree on.
TEST(DcPosGauntletTest, EachPhaseNamesItsOwnGate)
{
    DcPosGauntlet::Inputs in = Base();
    in.rpInPlace = true;
    in.distToGate = 1.0f;

    in.progress = PROGRESS_FINISHED_KRICK_SCENE;
    in.phase = in.progress;
    EXPECT_EQ(DcPosGauntlet::Decide(in).state, State::Gate1);

    in.progress = PROGRESS_AFTER_WARN_1;
    in.phase = in.progress;
    in.waveAlive = 0;
    in.nearestWaveDist = -1.0f;
    in.phaseSinceMs = in.nowMs - (WAVE_SPAWN_GRACE_MS + 1000);
    EXPECT_EQ(DcPosGauntlet::Decide(in).state, State::Gate2);

    in.progress = PROGRESS_AFTER_WARN_2;
    in.phase = in.progress;
    EXPECT_EQ(DcPosGauntlet::Decide(in).state, State::Gate3);
}

// --- 4. the wave-summon grace ----------------------------------------------

// THE BUG THIS PREVENTS, spelled out because it is invisible from the source.
// SetData(1, 1) raises the counter to AFTER_WARN_1 and schedules the summons at
// 0ms; the two Deathbringers land on the next UpdateAI and the other eight at
// +3.0s / +3.5s behind a spline-arrival poll. Without the grace the driver reads
// an empty scan on the very tick the gate is accepted, walks straight to gate 2,
// and is refused there for killsLeft != 0 — leaving the party standing in an
// ambush it triggered with no way forward.
TEST(DcPosGauntletTest, AnEmptyScanInsideTheSummonGraceStillMeansTheWaveIsUp)
{
    DcPosGauntlet::Inputs in = Base();
    in.progress = PROGRESS_AFTER_WARN_1;
    in.phase = PROGRESS_AFTER_WARN_1;
    in.waveAlive = 0;
    in.nearestWaveDist = -1.0f;
    in.distToGate = 1.0f;

    // One tick after the gate was accepted: nothing has spawned yet.
    in.phaseSinceMs = in.nowMs - 200;
    DcPosGauntlet::Verdict v = DcPosGauntlet::Decide(in);
    EXPECT_EQ(v.state, State::Wave1);
    EXPECT_TRUE(v.holdStill) << "hold — yielding here lets Advance walk into gate 2";
    EXPECT_FALSE(v.forge);
    ExpectWellFormed(v);

    // Just inside the grace.
    in.phaseSinceMs = in.nowMs - (WAVE_SPAWN_GRACE_MS - 1);
    EXPECT_EQ(DcPosGauntlet::Decide(in).state, State::Wave1);

    // Past it with nothing alive: the wave really is dead.
    in.phaseSinceMs = in.nowMs - (WAVE_SPAWN_GRACE_MS + 1);
    v = DcPosGauntlet::Decide(in);
    EXPECT_EQ(v.state, State::Gate2);
    EXPECT_TRUE(v.forge);
}

// The grace belongs to the PHASE, not to the sub-state, and this is the case that
// proves it matters: the grace window has to open on the tick the gate is
// ACCEPTED, which is the same tick the sub-state flips from Gate to Wave. A clock
// stamped on the sub-state transition would be indistinguishable here — and would
// then restart again the moment the wave died, re-opening a spent window.
TEST(DcPosGauntletTest, ThePhaseClockRestampsOnlyWhenTheProgressCounterMoves)
{
    DcPosGauntlet::Inputs in = Base();
    in.rpInPlace = true;
    in.distToGate = 1.0f;

    // The stored clock belongs to phase 2; the world has moved to 3.
    in.progress = PROGRESS_AFTER_WARN_1;
    in.phase = PROGRESS_FINISHED_KRICK_SCENE;
    in.phaseSinceMs = in.nowMs - 500'000;
    in.gateHoldSinceMs = in.nowMs - 500'000;
    in.stallReported = true;

    DcPosGauntlet::Verdict v = DcPosGauntlet::Decide(in);
    EXPECT_EQ(v.phase, PROGRESS_AFTER_WARN_1);
    EXPECT_EQ(v.phaseSinceMs, in.nowMs) << "a progress bump re-stamps the phase clock";
    EXPECT_FALSE(v.stallReported) << "...and gives the new phase its own right to complain once";
    EXPECT_EQ(v.state, State::Wave1) << "and the fresh grace window keeps the wave up";

    // Same phase, same clock: nothing is re-stamped.
    in.phase = PROGRESS_AFTER_WARN_1;
    in.phaseSinceMs = in.nowMs - 500'000;
    in.stallReported = true;
    v = DcPosGauntlet::Decide(in);
    EXPECT_EQ(v.phaseSinceMs, in.nowMs - 500'000u);
    EXPECT_TRUE(v.stallReported);
}

// --- 4a. wave 1 has to ARM before the party climbs the ramp -----------------

// THE REGRESSION THIS PINS, in the words it was reported in: "the party moves to
// the correct spot to wait for the event to start, but it proceeds up the ramp too
// soon. It needs to wait until the two 4-pack group of mobs are spawned and
// hostile before going up the ramp."
//
// It is not the ARM hold ending early — that hold ended exactly right, on the
// orchestrator reaching its wait position. It is the state AFTER it. Gate 1 is
// accepted, progress goes to 3, and pit_of_saron.cpp's event 30 lands two
// Deathbringers at (950.6, 50.9, 567.9) — the far top of the ramp, ~100yd off —
// REACT_PASSIVE and NON_ATTACKABLE, on a spline down to their homes. The two
// 4-packs do not exist yet; they are summoned by events 33/34 when each
// Deathbringer's spline ENDS, plus 3s, and go aggressive 3.5s after that.
//
// For those ~15-20 seconds the only thing a wave scan can see is two mobs that
// cannot be attacked, at the top of a ramp. The distant-mob rule below then walks
// the party at them — up the ramp, over the ground both packs are about to land
// on. The armed count is what tells the two situations apart.
TEST(DcPosGauntletTest, ThePartyHoldsAtTheRampFootUntilBothFourPacksAreHostile)
{
    DcPosGauntlet::Inputs in = Base();
    in.progress = PROGRESS_AFTER_WARN_1;
    in.phase = PROGRESS_AFTER_WARN_1;
    in.phaseSinceMs = in.nowMs - 500;  // gate 1 was accepted half a second ago
    in.waveArmedLatched = false;

    // Event 30 has fired: two Deathbringers alive at the top of the ramp, neither
    // attackable. This is the exact tick the bug walked.
    in.waveAlive = 2;
    in.waveArmed = 0;
    in.nearestWaveDist = 103.0f;  // gate 1's stand point to the summon point

    DcPosGauntlet::Verdict v = DcPosGauntlet::Decide(in);
    EXPECT_EQ(v.state, State::Wave1);
    EXPECT_TRUE(v.waveArming);
    EXPECT_TRUE(v.holdStill);
    EXPECT_FALSE(v.engageWave) << "THE BUG: this is the tick that climbed the ramp "
                                  "at a mob that cannot be attacked";
    EXPECT_FALSE(v.waveArmedLatched);
    ExpectWellFormed(v);

    // The Deathbringers finish their splines and the UPPER 4-pack lands (event 33).
    // Four escorts plus their Deathbringer arm together — five of ten. STILL NOT
    // BOTH PACKS, and the lower one is the pack that spawns beside the party.
    in.waveAlive = 7;
    in.waveArmed = 5;
    in.nearestWaveDist = 60.0f;
    v = DcPosGauntlet::Decide(in);
    EXPECT_TRUE(v.waveArming) << "five is one pack; the quorum is deliberately eight";
    EXPECT_TRUE(v.holdStill);
    EXPECT_FALSE(v.engageWave);

    // Event 34: the lower 4-pack lands 20yd up-slope of the party and arms. The
    // ambush is real, and the hold releases.
    in.waveAlive = 10;
    in.waveArmed = 10;
    in.nearestWaveDist = 20.0f;
    v = DcPosGauntlet::Decide(in);
    EXPECT_FALSE(v.waveArming);
    EXPECT_TRUE(v.waveArmedLatched);
    EXPECT_TRUE(v.holdStill) << "in reach and silent — the standoff clock takes over";
    EXPECT_EQ(v.waveHoldSinceMs, in.nowMs);
    ExpectWellFormed(v);
}

// THE LATCH IS WHY THIS IS NOT JUST A QUORUM. Eight armed mobs become five as the
// party kills them, and killsLeft is not readable — so "five armed" is
// indistinguishable from the mid-cascade case above by any world fact available to
// the driver. Only the history separates them. A party that dropped combat for one
// tick between pulls, inside the arming budget, would otherwise be sent back to
// standing still while five live Ymirjar chewed on it.
TEST(DcPosGauntletTest, TheArmingHoldNeverReArmsOnceTheWaveHasBeenReleased)
{
    DcPosGauntlet::Inputs in = Base();
    in.progress = PROGRESS_AFTER_WARN_1;
    in.phase = PROGRESS_AFTER_WARN_1;
    in.phaseSinceMs = in.nowMs - 20'000;  // armed, and still inside WAVE1_ARM_MS
    in.waveArmedLatched = true;
    in.waveAlive = 4;
    in.waveArmed = 4;  // six of the ten are already dead
    in.nearestWaveDist = 30.0f;
    in.partyInCombat = false;

    DcPosGauntlet::Verdict const v = DcPosGauntlet::Decide(in);
    EXPECT_FALSE(v.waveArming);
    EXPECT_TRUE(v.engageWave) << "half a dead wave is fetched, not waited on";
    EXPECT_TRUE(v.waveArmedLatched);
    ExpectWellFormed(v);
}

// The hold is BOUNDED, and this is the shape that needs it: events 31/32 poll every
// 500ms for a Deathbringer's spline to end, and a Deathbringer that never reaches
// its home leaves them polling for ever — NEITHER 4-pack is summoned, and a quorum
// of eight can never be met. Holding on it would be the six-minute standoff again
// in a new costume.
TEST(DcPosGauntletTest, TheArmingHoldReleasesOnItsBudgetWhenTheCascadeWedges)
{
    DcPosGauntlet::Inputs in = Base();
    in.progress = PROGRESS_AFTER_WARN_1;
    in.phase = PROGRESS_AFTER_WARN_1;
    in.waveArmedLatched = false;
    in.waveAlive = 2;
    in.waveArmed = 0;
    in.nearestWaveDist = 103.0f;

    // One millisecond short of the budget: still holding.
    in.phaseSinceMs = in.nowMs - (WAVE1_ARM_MS - 1);
    DcPosGauntlet::Verdict v = DcPosGauntlet::Decide(in);
    EXPECT_TRUE(v.waveArming);
    EXPECT_FALSE(v.waveArmedLatched);

    // One more, and the driver goes and gets them anyway.
    in.phaseSinceMs = in.nowMs - WAVE1_ARM_MS;
    v = DcPosGauntlet::Decide(in);
    EXPECT_FALSE(v.waveArming);
    EXPECT_TRUE(v.waveArmedLatched);
    EXPECT_TRUE(v.engageWave);
    ExpectWellFormed(v);
}

// Combat beats the quorum. If a Deathbringer's one relocation look lands on a bot
// on the way down, the party is in a fight — and continuing to hold would leave
// whoever it aggroed to soak it alone while the rest stand at the gate.
TEST(DcPosGauntletTest, TheArmingHoldYieldsImmediatelyIfTheWaveFindsThePartyFirst)
{
    DcPosGauntlet::Inputs in = Base();
    in.progress = PROGRESS_AFTER_WARN_1;
    in.phase = PROGRESS_AFTER_WARN_1;
    in.phaseSinceMs = in.nowMs - 2'000;
    in.waveArmedLatched = false;
    in.waveAlive = 2;
    in.waveArmed = 1;
    in.nearestWaveDist = 6.0f;
    in.partyInCombat = true;

    DcPosGauntlet::Verdict const v = DcPosGauntlet::Decide(in);
    EXPECT_FALSE(v.waveArming);
    EXPECT_TRUE(v.waveArmedLatched);
    EXPECT_TRUE(v.yieldTick) << "in reach and fighting — that is a combat tick";
    ExpectWellFormed(v);
}

// A quorum of ZERO is how wave 2 opts out, and it must not merely be a quorum that
// happens to be satisfied — it must skip the hold outright. Wave 2's six (twelve
// heroic) mobs are summoned in one go by event 60, ride one spline to a fixed home,
// and carry no scripted passive window; a hold there would be a way to stand still
// while a wave that is already coming arrives.
TEST(DcPosGauntletTest, WaveTwoOptsOutOfTheArmingHoldWithAQuorumOfZero)
{
    DcPosGauntlet::Inputs in = Base();
    in.progress = PROGRESS_AFTER_WARN_2;
    in.phase = PROGRESS_AFTER_WARN_2;
    in.phaseSinceMs = in.nowMs - 500;
    in.waveArmedLatched = false;
    in.waveArmedQuorum = 0;  // what the glue passes outside progress 3
    in.waveAlive = 6;
    in.waveArmed = 0;        // ...and it does not matter that nothing reads armed
    in.nearestWaveDist = 40.0f;

    DcPosGauntlet::Verdict const v = DcPosGauntlet::Decide(in);
    EXPECT_EQ(v.state, State::Wave2);
    EXPECT_FALSE(v.waveArming);
    EXPECT_TRUE(v.waveArmedLatched);
    EXPECT_TRUE(v.engageWave);
    ExpectWellFormed(v);
}

// The latch is PHASE-SCOPED, exactly like the stall report: a progress bump clears
// it, so wave 2 gets its own decision, and a wipe-and-retry that re-enters the
// window re-enters the cascade holding nothing. Leaving the window clears it too.
TEST(DcPosGauntletTest, TheArmingLatchIsClearedByThePhaseBumpAndByLeavingTheWindow)
{
    DcPosGauntlet::Inputs in = Base();
    in.waveArmedLatched = true;

    // Wave 1 is dead, gate 2 is accepted, and the world has moved to phase 4 while
    // the stored latch still belongs to phase 3.
    in.progress = PROGRESS_AFTER_WARN_2;
    in.phase = PROGRESS_AFTER_WARN_1;
    in.phaseSinceMs = in.nowMs - 200'000;
    in.distToGate = 200.0f;
    in.waveAlive = 0;
    in.nearestWaveDist = -1.0f;
    EXPECT_FALSE(DcPosGauntlet::Decide(in).waveArmedLatched)
        << "a progress bump re-stamps the phase clock and the latch with it";

    // And the whole block is dropped on the way out of the window.
    in.progress = PROGRESS_AFTER_TUNNEL_WARN;
    in.phase = PROGRESS_AFTER_TUNNEL_WARN;
    DcPosGauntlet::Verdict const v = DcPosGauntlet::Decide(in);
    EXPECT_TRUE(v.complete);
    EXPECT_FALSE(v.waveArmedLatched);
}

// The arming quorum is not a number somebody liked. It is "both 4-packs", and the
// summon order is what makes the count say so: each 4-pack arms together with its
// own Deathbringer, so ONE pack tops out at five and eight cannot be reached
// without both. The margin above eight is the two Deathbringers, which is what
// keeps a single failed summon from wedging the hold on its budget.
TEST(DcPosGauntletTest, TheArmingQuorumIsBothFourPacksAndNotOne)
{
    EXPECT_GT(WAVE1_ARMED_QUORUM, 5u) << "one 4-pack plus its Deathbringer is five";
    EXPECT_LE(WAVE1_ARMED_QUORUM, 8u) << "both 4-packs are eight — a quorum above it "
                                         "would depend on a Deathbringer surviving "
                                         "its own summon";

    // And the budget has to outlast the whole cascade: two spline descents (the
    // longer is 93yd), a 500ms arrival poll, +3s to the pack summon and +3.5s to
    // the react flip. ~20s of chain, and the budget is a deadlock breaker rather
    // than a schedule, so it wants a wide margin over that and none at all over the
    // run's own timeout.
    EXPECT_GE(WAVE1_ARM_MS, 30'000u);
    EXPECT_LE(WAVE1_ARM_MS, 90'000u);
    EXPECT_GT(WAVE1_ARM_MS, WAVE_SPAWN_GRACE_MS)
        << "the summon grace only covers an EMPTY scan; the cascade runs far past it";
}

// --- 5. fighting a wave ----------------------------------------------------

// A wave mob out of reach is a wave that never clears: killsLeft only moves on a
// DEATH, and both waves contain creatures that spline to a fixed home and go
// aggressive in place 30-60yd from where the party is standing.
TEST(DcPosGauntletTest, ADistantWaveMobIsWalkedAt)
{
    DcPosGauntlet::Inputs in = Base();
    in.progress = PROGRESS_AFTER_WARN_1;
    in.phase = PROGRESS_AFTER_WARN_1;
    in.phaseSinceMs = in.nowMs - 60'000;
    in.waveAlive = 3;
    in.nearestWaveDist = WAVE_ENGAGE_RANGE + 20.0f;

    DcPosGauntlet::Verdict const v = DcPosGauntlet::Decide(in);
    EXPECT_EQ(v.state, State::Wave1);
    EXPECT_TRUE(v.engageWave);
    EXPECT_FALSE(v.yieldTick) << "the driver is steering, so it claims the tick";
    ExpectWellFormed(v);
}

// In reach and fighting: YIELD. This is the load-bearing half of the whole event
// row's StepsOwnMovement flag — the driver sits above the stock combat movers, and
// a tick it claims while it has nothing to steer is a tick the tank does not
// swing.
TEST(DcPosGauntletTest, AWaveInReachYieldsTheTickToTheCombatEngine)
{
    DcPosGauntlet::Inputs in = Base();
    in.progress = PROGRESS_AFTER_WARN_1;
    in.phase = PROGRESS_AFTER_WARN_1;
    in.phaseSinceMs = in.nowMs - 60'000;
    in.waveAlive = 6;
    in.nearestWaveDist = 5.0f;
    in.partyInCombat = true;

    DcPosGauntlet::Verdict v = DcPosGauntlet::Decide(in);
    EXPECT_EQ(v.state, State::Wave1);
    EXPECT_TRUE(v.yieldTick);
    EXPECT_FALSE(v.engageWave);
    ExpectWellFormed(v);

    // In reach but NOT yet fighting: hold rather than yield. Both Ymirjar entries
    // spend their first seconds REACT_PASSIVE and NON_ATTACKABLE while they emerge
    // from the ground, and handing the leg back to Advance for those seconds walks
    // the party through the ambush and into the next gate.
    in.partyInCombat = false;
    v = DcPosGauntlet::Decide(in);
    EXPECT_FALSE(v.yieldTick);
    EXPECT_TRUE(v.holdStill);
    EXPECT_EQ(v.waveHoldSinceMs, in.nowMs) << "and the standoff clock starts here";
    ExpectWellFormed(v);
}

// THE STANDOFF BREAKER, and the regression that produced it.
//
// tp-20260907-113722-1: two of five runs stood 8yd from ten live wave-1 Ymirjar,
// whole party at full health, out of combat, for 6m23s — the log line frozen to
// the decimal — until the event timeout stalled the run. The wave never aggroed
// and never would have. Wave 1's eight escorts (36840 / 36893) carry
// `smart_scripts` AI_INIT -> REACT_PASSIVE plus a ONE-SHOT 3500ms update ->
// REACT_AGGRESSIVE; the two Deathbringers (36892) get the same window from
// pit_of_saron.cpp. SetReactState runs no aggro scan, and AzerothCore's proximity
// aggro fires only from a relocation notifier, so the mob's single look at the
// party is spent while it is still passive — and after the flip nothing moves,
// because the driver's own hold is what parked the party.
//
// So the hold is a BUDGET, not a state. Past it the kernel stops waiting to be
// attacked and says so.
TEST(DcPosGauntletTest, ASilentWaveInReachIsPulledOncePastTheStandoffBudget)
{
    DcPosGauntlet::Inputs in = Base();
    in.progress = PROGRESS_AFTER_WARN_1;
    in.phase = PROGRESS_AFTER_WARN_1;
    in.phaseSinceMs = in.nowMs - 60'000;
    in.waveAlive = 10;
    in.nearestWaveDist = 8.5f;  // the measured standoff distance, to the decimal
    in.partyInCombat = false;

    // First tick in reach: the clock starts and the party waits out the emerge.
    DcPosGauntlet::Verdict v = DcPosGauntlet::Decide(in);
    EXPECT_EQ(v.state, State::Wave1);
    EXPECT_TRUE(v.holdStill);
    EXPECT_FALSE(v.pullWave);
    EXPECT_EQ(v.waveHoldSinceMs, in.nowMs);
    ExpectWellFormed(v);

    // One millisecond short of the budget is still a hold — the emerge window is
    // 3500ms and an ordinary aggro must never be pre-empted.
    in.waveHoldSinceMs = v.waveHoldSinceMs;
    in.nowMs += WAVE_STANDOFF_MS - 1;
    v = DcPosGauntlet::Decide(in);
    EXPECT_TRUE(v.holdStill);
    EXPECT_FALSE(v.pullWave);
    EXPECT_EQ(v.waveHoldSinceMs, in.nowMs - (WAVE_STANDOFF_MS - 1))
        << "and a hold does not restamp its own clock";
    ExpectWellFormed(v);

    // One more millisecond and the driver starts the fight itself.
    in.nowMs += 1;
    v = DcPosGauntlet::Decide(in);
    EXPECT_TRUE(v.pullWave);
    EXPECT_FALSE(v.holdStill);
    EXPECT_FALSE(v.yieldTick) << "the pull claims the tick — it IS the action";
    EXPECT_EQ(v.waveHoldSinceMs, in.nowMs)
        << "and the clock re-arms, so a pull that does not take is retried on the "
           "same budget rather than every tick";
    ExpectWellFormed(v);

    // The pull lands: back to yielding, and the clock is spent.
    in.waveHoldSinceMs = v.waveHoldSinceMs;
    in.partyInCombat = true;
    v = DcPosGauntlet::Decide(in);
    EXPECT_TRUE(v.yieldTick);
    EXPECT_FALSE(v.pullWave);
    EXPECT_EQ(v.waveHoldSinceMs, 0u);
    ExpectWellFormed(v);
}

// The budget is not a substitute for walking. A wave mob out of reach is fetched
// as before, and fetching restarts the clock — the standoff being measured is
// "in reach and silent", not "the wave has been up a while".
TEST(DcPosGauntletTest, TheStandoffClockOnlyRunsWhileAMobIsActuallyInReach)
{
    DcPosGauntlet::Inputs in = Base();
    in.progress = PROGRESS_AFTER_WARN_1;
    in.phase = PROGRESS_AFTER_WARN_1;
    in.phaseSinceMs = in.nowMs - 60'000;
    in.waveAlive = 2;
    in.nearestWaveDist = 8.0f;
    in.partyInCombat = false;
    in.waveHoldSinceMs = in.nowMs - 60'000;  // in reach and silent for a minute

    // It splines away out of reach (wave 1's Deathbringers do exactly this on
    // their way to two homes 30-60yd apart): walk, and drop the clock.
    in.nearestWaveDist = WAVE_ENGAGE_RANGE + 1.0f;
    DcPosGauntlet::Verdict v = DcPosGauntlet::Decide(in);
    EXPECT_TRUE(v.engageWave);
    EXPECT_FALSE(v.pullWave);
    EXPECT_EQ(v.waveHoldSinceMs, 0u);
    ExpectWellFormed(v);

    // An empty scan inside the summon grace clears it too — there is nothing to
    // pull, and a clock left running here would fire on the first mob to land.
    in.waveHoldSinceMs = in.nowMs - 60'000;
    in.phaseSinceMs = in.nowMs - 1'000;  // inside the grace, so the wave is still "up"
    in.waveAlive = 0;
    in.nearestWaveDist = -1.0f;
    v = DcPosGauntlet::Decide(in);
    EXPECT_EQ(v.state, State::Wave1);
    EXPECT_TRUE(v.holdStill);
    EXPECT_EQ(v.waveHoldSinceMs, 0u);
    ExpectWellFormed(v);

    // And a gate is only ever reached with the wave dead, so it is spent there.
    in.waveHoldSinceMs = in.nowMs - 60'000;
    in.phaseSinceMs = in.nowMs - WAVE_SPAWN_GRACE_MS - 1'000;
    in.distToGate = 1.0f;
    v = DcPosGauntlet::Decide(in);
    EXPECT_EQ(v.state, State::Gate2);
    EXPECT_EQ(v.waveHoldSinceMs, 0u);
    ExpectWellFormed(v);
}

// Wave 2 behaves identically, and heroic changes nothing here — the difficulty
// difference is six more mobs inside the same volume, which is the glue's problem
// (the scan) and not the kernel's.
TEST(DcPosGauntletTest, WaveTwoIsTheSameShapeAsWaveOne)
{
    DcPosGauntlet::Inputs in = Base();
    in.progress = PROGRESS_AFTER_WARN_2;
    in.phase = PROGRESS_AFTER_WARN_2;
    in.phaseSinceMs = in.nowMs - 60'000;
    in.waveAlive = 12;
    in.nearestWaveDist = 3.0f;
    in.partyInCombat = true;

    DcPosGauntlet::Verdict v = DcPosGauntlet::Decide(in);
    EXPECT_EQ(v.state, State::Wave2);
    EXPECT_TRUE(v.yieldTick);

    in.waveAlive = 0;
    in.nearestWaveDist = -1.0f;
    in.distToGate = 1.0f;
    v = DcPosGauntlet::Decide(in);
    EXPECT_EQ(v.state, State::Gate3);
    EXPECT_TRUE(v.forge);
}

// --- 6. the stall watchdog -------------------------------------------------

// A gate that has been forged from inside its own sphere for GATE_STALL_MS and
// still has not moved the counter cannot be fixed from in here — the orchestrator
// is dead or wedged, or a wave mob despawned instead of dying and left killsLeft
// stuck. So the kernel says so ONCE and keeps forging; the event's own timeout is
// what eventually stalls the run for a human. A silent forever-loop is the failure
// this exists to make visible.
TEST(DcPosGauntletTest, TheGateStallIsReportedExactlyOncePerGate)
{
    DcPosGauntlet::Inputs in = Base();
    in.rpInPlace = true;
    in.distToGate = 1.0f;
    in.gateHoldSinceMs = in.nowMs - (GATE_STALL_MS + 1000);

    DcPosGauntlet::Verdict v = DcPosGauntlet::Decide(in);
    EXPECT_TRUE(v.stalled);
    EXPECT_TRUE(v.reportStall);
    EXPECT_TRUE(v.stallReported);
    EXPECT_TRUE(v.forge) << "a stalled gate keeps being forged — giving up would be worse";

    // Next tick, with the flag stored back: still stalled, no second line.
    in.stallReported = v.stallReported;
    v = DcPosGauntlet::Decide(in);
    EXPECT_TRUE(v.stalled);
    EXPECT_FALSE(v.reportStall);
}

// The gate clock measures the time spent FORGING, not the time spent walking —
// and emphatically not the ninety seconds of Krick outro that precede gate 1. A
// watchdog hung off the phase clock would fire on every single run.
TEST(DcPosGauntletTest, TheGateClockIgnoresTheOutroAndTheWalkIn)
{
    DcPosGauntlet::Inputs in = Base();
    in.rpInPlace = true;
    in.phaseSinceMs = in.nowMs - 150'000;  // the outro took two and a half minutes
    in.distToGate = 60.0f;                 // and the party is still walking in

    DcPosGauntlet::Verdict v = DcPosGauntlet::Decide(in);
    EXPECT_FALSE(v.stalled);
    EXPECT_EQ(v.gateHoldSinceMs, 0u);

    // It starts on arrival.
    in.distToGate = 1.0f;
    v = DcPosGauntlet::Decide(in);
    EXPECT_EQ(v.gateHoldSinceMs, in.nowMs);
    EXPECT_FALSE(v.stalled);

    // ...and is dropped again if the party is dragged back out of the sphere, so a
    // camp-drag cannot bank stall time it did not spend forging.
    in.gateHoldSinceMs = v.gateHoldSinceMs;
    in.distToGate = 60.0f;
    v = DcPosGauntlet::Decide(in);
    EXPECT_EQ(v.gateHoldSinceMs, 0u);
}

// --- 7. the roster: the boss that reports success by not existing ----------

TEST(DungeonEventPitOfSaronTest, ScourgelordTyrannusIsPatchedInWithHisRealKillBit)
{
    BossRosterPatch const* patch = nullptr;
    for (BossRosterPatch const& p : BossRosterRegistry::AllPatches())
        if (p.mapId == MAP_ID)
            patch = &p;

    ASSERT_NE(patch, nullptr)
        << "map 658 has no roster patch. Without one, BossSpawnIndex::Build derives TWO bosses "
           "for a three-boss dungeon — 36658 has no `creature` spawn row anywhere (he is "
           "Rimefang's vehicle accessory) — and a run REPORTS SUCCESS at 2/2 having never "
           "walked past the Krick outro.";

    DungeonBossInfo const* tyrannus = nullptr;
    DungeonBossInfo const* ledge = nullptr;
    for (DungeonBossInfo const& e : patch->add)
    {
        if (e.entry == NPC_TYRANNUS)
            tyrannus = &e;
        if (e.entry == BossRosterRegistry::ObjectiveEntry(1))
            ledge = &e;
    }

    ASSERT_NE(tyrannus, nullptr) << "Scourgelord Tyrannus (36658) is not added by the patch";
    EXPECT_EQ(tyrannus->kind, DungeonAnchorKind::Boss);
    EXPECT_EQ(tyrannus->encounterIndex, BIT_TYRANNUS)
        << "bit 2 is a plain ENCOUNTER_CREDIT_KILL_CREATURE row on BOTH difficulties "
           "(DungeonEncounter.dbc 837 and 838); only the derivation failed, so the completion "
           "bit rides the ordinary KillRewarder path and must be his own";
    EXPECT_EQ(tyrannus->orderOverride, ORDER_TYRANNUS);
    EXPECT_EQ(tyrannus->inheritCompletionFrom, 0u)
        << "there is nothing to inherit from — the boss is missing from the base list, which is "
           "the whole reason this row exists";
    EXPECT_LT(tyrannus->doneBossStateIndex, 0)
        << "he has a real DBC bit, so completion must NOT come off an instance boss-state slot";

    // HIS EXIT POSITION, NOT HIS SPAWN. He rides Rimefang at z 642.93 — 14.7yd up
    // — until DoAction(1) MoveJump()s him to exitPos, and NavmeshSnap's vertical
    // extent is a FIXED 10 regardless of snap radius, so an anchor at the riding
    // height would fail to snap and the row would be DROPPED AT LOAD.
    EXPECT_NEAR(tyrannus->z, 628.2f, 0.5f)
        << "Tyrannus must be anchored on the arena floor he jumps down to, not on Rimefang";

    ASSERT_NE(ledge, nullptr) << "the ledge objective is missing";
    EXPECT_EQ(ledge->kind, DungeonAnchorKind::Objective);
    EXPECT_EQ(ledge->eventId, EVENT_TYRANNUS_LEDGE);
    EXPECT_EQ(ledge->orderOverride, ORDER_LEDGE);
    EXPECT_EQ(ledge->encounterIndex, 0u)
        << "an objective carries no kill-bit; it orders by orderOverride";

    // The two reorders exist only to make an integer slot for the objective.
    bool garfrost = false, ick = false;
    for (auto const& r : patch->reorder)
    {
        if (r.first == NPC_GARFROST) { garfrost = true; EXPECT_EQ(r.second, ORDER_GARFROST); }
        if (r.first == NPC_ICK)      { ick = true;      EXPECT_EQ(r.second, ORDER_KRICK);    }
    }
    EXPECT_TRUE(garfrost);
    EXPECT_TRUE(ick) << "the second encounter's kill credit is ICK (36476), not Krick (36477)";

    EXPECT_TRUE(patch->remove.empty()) << "nothing on this map is mis-derived, only missing";
    EXPECT_TRUE(patch->skipByDesign.empty());
}

// The clear order has to be a contiguous 1..4 with the objective between Ick and
// the boss it gates. A hole here is not cosmetic: the objective is what walks the
// party to the ledge, and an objective ordered after its own boss never runs.
TEST(DungeonEventPitOfSaronTest, TheClearOrderIsContiguousAndTheLedgePrecedesTheBoss)
{
    EXPECT_EQ(ORDER_GARFROST, 1);
    EXPECT_EQ(ORDER_KRICK, 2);
    EXPECT_EQ(ORDER_LEDGE, 3);
    EXPECT_EQ(ORDER_TYRANNUS, 4);
    EXPECT_LT(ORDER_LEDGE, ORDER_TYRANNUS);
}

// --- 8. the two event rows -------------------------------------------------

TEST(DungeonEventPitOfSaronTest, TheGauntletIsAConditionalInCombatDriverThatOwnsThePull)
{
    DungeonEvent const* ev = DungeonEventRegistry::Find(MAP_ID, EVENT_GAUNTLET);
    ASSERT_NE(ev, nullptr) << "map 658 event 1 'Run the Ymirjar gauntlet' is missing";

    EXPECT_EQ(ev->activation, EventActivation::Conditional);
    EXPECT_NE(ev->condition, nullptr);

    EXPECT_TRUE(ev->drivesInCombat)
        << "from the tick gate 1 is accepted the party is fighting continuously; a non-combat "
           "rung would get its ticks before the first gate and essentially never again";
    EXPECT_TRUE(ev->stepsOwnMovement)
        << "the hook issues every metre of the leg itself, and the per-tick hold would cancel "
           "each spline the tick after it is issued";
    EXPECT_TRUE(ev->ownsThePull)
        << "a camp dragged backwards leaves wave 2's measurement volume and re-enters a spent "
           "gate sphere";
    EXPECT_TRUE(ev->repeatable)
        << "the condition going false is the only completion; a latch would not survive a wipe";
    EXPECT_TRUE(ev->persistent)
        << "on this leg a combat gap is one Ymirjar dying";
    EXPECT_TRUE(ev->required)
        << "without the three gates Tyrannus is never attackable — a quiet skip costs the last "
           "boss, so a stall that names the problem is the correct failure";

    ASSERT_EQ(ev->steps.size(), 1u) << "one Custom step: this leg is a standing preference "
                                       "re-decided every tick, not a sequence";
    EXPECT_EQ(ev->steps[0].kind, EventStepKind::Custom);
    EXPECT_EQ(ev->steps[0].hookId, HOOK_POS_GAUNTLET);
    EXPECT_TRUE(ObjectiveHookRegistry::Has(HOOK_POS_GAUNTLET));

    // PanelAfterBoss(Ick) and NEVER PanelBeforeBoss(Tyrannus): panelGatesBossEntry
    // also keys DcTargeting::HasPendingSummonEvent, which reads an unlatched
    // gating event as "this boss must still be SUMMONED" and suppresses the
    // dynamic pull within 80yd of him — and a REPEATABLE event is never latched,
    // so the suppression would be permanent.
    EXPECT_EQ(ev->panelGatesBossEntry, 0u)
        << "PanelBeforeBoss on a Repeatable event permanently suppresses the pull at that boss "
           "([[dc-panelbeforeboss-repeatable-permanent-hold]])";
    EXPECT_EQ(ev->panelSortAfterBossEntry, NPC_ICK);
}

TEST(DungeonEventPitOfSaronTest, TheLedgeIsAnAnchoredGateThatOwnsItsWalkIn)
{
    DungeonEvent const* ev = DungeonEventRegistry::Find(MAP_ID, EVENT_TYRANNUS_LEDGE);
    ASSERT_NE(ev, nullptr) << "map 658 event 2 'Tyrannus's ledge' is missing";

    EXPECT_EQ(ev->activation, EventActivation::Anchored);
    EXPECT_TRUE(ev->persistent);
    EXPECT_TRUE(ev->stepsOwnMovement)
        << "the anchor is deliberately OUTSIDE areatrigger 5633's sphere, so the hook has to "
           "walk the leader in — and the per-tick hold would cancel that glide";
    EXPECT_FALSE(ev->ownsThePull)
        << "the flag is conditional-only; on an anchored row it is a silent no-op";
    EXPECT_FALSE(ev->drivesInCombat)
        << "there is no anchored combat rung to set it on";

    ASSERT_EQ(ev->steps.size(), 1u);
    EXPECT_EQ(ev->steps[0].kind, EventStepKind::Custom);
    EXPECT_EQ(ev->steps[0].hookId, HOOK_POS_TYRANNUS_LEDGE);
    EXPECT_TRUE(ObjectiveHookRegistry::Has(HOOK_POS_TYRANNUS_LEDGE));
}

// --- 9. the geometry invariants that need no navmesh -----------------------

// The three gate stand points must be inside the spheres they fire, with room for
// the arrival leash. A forge from outside is a SILENT no-op: the core re-tests
// Player::IsInAreaTriggerRadius on the packet, and for a radius trigger that is a
// 3D distance against the DBC centre. The route probe repeats this against a real
// mesh; this copy runs everywhere.
TEST(DungeonEventPitOfSaronTest, EveryGateIsForgedFromInsideItsOwnSphere)
{
    struct G { char const* name; float sx, sy, sz, cx, cy, cz, r; };
    static constexpr G kGates[] = {
        { "gate 1", GATE_1_X, GATE_1_Y, GATE_1_Z, AT_WARN_1_X, AT_WARN_1_Y, AT_WARN_1_Z, AT_WARN_1_R },
        { "gate 2", GATE_2_X, GATE_2_Y, GATE_2_Z, AT_WARN_2_X, AT_WARN_2_Y, AT_WARN_2_Z, AT_WARN_2_R },
        { "gate 3", GATE_3_X, GATE_3_Y, GATE_3_Z, AT_TUNNEL_X, AT_TUNNEL_Y, AT_TUNNEL_Z, AT_TUNNEL_R },
        { "the arena", ARENA_X, ARENA_Y, ARENA_Z, AT_TYRANNUS_X, AT_TYRANNUS_Y, AT_TYRANNUS_Z, AT_TYRANNUS_R },
    };

    for (G const& g : kGates)
    {
        float const dx = g.sx - g.cx, dy = g.sy - g.cy, dz = g.sz - g.cz;
        float const d = std::sqrt(dx * dx + dy * dy + dz * dz);
        EXPECT_LT(d + GATE_LEASH, g.r)
            << g.name << " stands " << d << "yd from its trigger centre (radius " << g.r
            << "): a leader that settles on the far side of its arrival leash would forge from "
               "OUTSIDE, and the refusal is silent";
    }
}

// ...and the mirror: the ledge anchor must be OUTSIDE 5633, or arriving at the
// objective IS entering the encounter and the gather that exists to prevent a
// strung-out entry never runs.
TEST(DungeonEventPitOfSaronTest, TheLedgeAnchorCannotItselfTripTheTyrannusTrigger)
{
    float const dx = LEDGE_X - AT_TYRANNUS_X;
    float const dy = LEDGE_Y - AT_TYRANNUS_Y;
    float const dz = LEDGE_Z - AT_TYRANNUS_Z;
    float const d = std::sqrt(dx * dx + dy * dy + dz * dz);
    EXPECT_GT(d, AT_TYRANNUS_R + LEDGE_ARRIVE)
        << "the ledge is " << d << "yd from areatrigger " << AREATRIGGER_TYRANNUS
        << " (radius " << AT_TYRANNUS_R << ") — inside it, and the objective fires the encounter "
           "on arrival with the party strung out down the tunnel";
}

// The ARM staging point must be clear of gate 1's sphere on every bearing. It is
// the whole content of the ARM state: hold here for the 85-100s the Krick outro
// needs, rather than stand in an ambush trigger that will refuse the party the
// entire time.
// THE ARM HOLD IS NO LONGER MEASURED AGAINST THE TRIGGER SPHERE, and the swap is
// the point of the test rather than an exemption from it.
//
// The old bar was "40yd clear of areatrigger 5578 on every bearing", written when
// the hold sat in the middle of Krick's arena. That hold meant resuming from the
// arena, and the resume line cut the shoulder at (848, 79) — 3.14yd of rock,
// because the escort spline between anchors is linear. The hold now sits south of
// the gauntlet on the flat plain, which puts it 33.81yd from the trigger's centre:
// INSIDE the 34.54yd sphere by 0.73yd, on the rim.
//
// That is safe because the driver FORGES the trigger (HandleAreaTriggerOpcode from
// a bot on the gate point) and bots have no client to fire it by walking in, so
// sitting inside the sphere consumes nothing and a refused forge is silent and
// retried. What the hold still has to be clear of is the ground the AMBUSH lands
// on, and that is what this now asserts.
TEST(DungeonEventPitOfSaronTest, TheArmHoldIsClearOfTheAmbushGround)
{
    // The first anchor of the ambush ramp — where wave 1 arrives.
    float const rx = 871.83f, ry = 52.31f, rz = 523.03f;
    float const toRamp = std::sqrt((STAGE_X - rx) * (STAGE_X - rx) +
                                   (STAGE_Y - ry) * (STAGE_Y - ry) +
                                   (STAGE_Z - rz) * (STAGE_Z - rz));
    EXPECT_GT(toRamp, 40.0f)
        << "the ARM hold is only " << toRamp << "yd from the wave-1 ramp anchor";

    // And clear of the gate stand point itself, so the hold never parks the party
    // on the ground the forge is issued from.
    float const toGate = std::sqrt((STAGE_X - GATE_1_X) * (STAGE_X - GATE_1_X) +
                                   (STAGE_Y - GATE_1_Y) * (STAGE_Y - GATE_1_Y) +
                                   (STAGE_Z - GATE_1_Z) * (STAGE_Z - GATE_1_Z));
    EXPECT_GT(toGate, STAGE_LEASH + 20.0f)
        << "the ARM hold is only " << toGate << "yd from gate 1's stand point";

    // The relaxation is bounded: on the rim is fine, deep inside is not.
    float const dx = STAGE_X - AT_WARN_1_X, dy = STAGE_Y - AT_WARN_1_Y, dz = STAGE_Z - AT_WARN_1_Z;
    float const toTrigger = std::sqrt(dx * dx + dy * dy + dz * dz);
    EXPECT_GT(toTrigger, AT_WARN_1_R - 5.0f)
        << "the ARM hold is " << toTrigger << "yd from areatrigger " << AREATRIGGER_WARN_1
        << " (radius " << AT_WARN_1_R << ") — that is well inside the sphere, not on its rim";
}

// WAVE 2'S VOLUME IS THE ONE PIECE OF AUTHORED GEOMETRY ON THIS MAP THAT CAN FAIL
// QUIETLY IN BOTH DIRECTIONS, and this is its proof.
//
// Too small and a heroic run's second spawn group — which ends 68yd north of the
// normal one and 17yd lower — never counts, so the driver walks to gate 3 with six
// mobs alive and is refused for killsLeft != 0. Too large and it swallows the
// eight STATIC Fallen Warriors in the icicle tunnel, or Gorkun's endless pump
// behind the boss, and the wave never reads as dead at all.
TEST(DungeonEventPitOfSaronTest, TheWaveTwoVolumeCoversBothDifficultiesAndNoStaticFallenWarrior)
{
    auto inVolume = [](float x, float y, float z)
    {
        float const dx = x - WAVE2_X, dy = y - WAVE2_Y;
        return dx * dx + dy * dy <= WAVE2_RADIUS * WAVE2_RADIUS &&
               std::fabs(z - WAVE2_Z) <= WAVE2_ZBAND;
    };

    // Every position npc_pos_tyrannus_eventsAI event 60 actually uses.
    struct P { char const* what; float x, y, z; };
    static constexpr P kWave[] = {
        { "normal spawn line", 927.11f, -72.60f, 592.2f },
        { "normal spawn line", 934.52f, -72.52f, 592.1f },
        { "normal home",       926.10f, -46.63f, 591.2f },
        { "heroic spawn line", 921.77f, -65.10f, 592.5f },
        { "heroic mid",        928.43f, -29.31f, 589.0f },
        { "heroic home",       937.80f,  21.20f, 574.6f },
    };
    for (P const& p : kWave)
        EXPECT_TRUE(inVolume(p.x, p.y, p.z))
            << p.what << " (" << p.x << ", " << p.y << ", " << p.z
            << ") is outside the wave-2 volume — those mobs would never be counted and the "
               "driver would walk to gate 3 while killsLeft is still non-zero";

    // ...and every 36841 it must exclude: the eight static tunnel spawns and
    // Gorkun's summon point.
    static constexpr P kNot[] = {
        { "static tunnel",  997.3f, -139.3f, 615.9f },
        { "static tunnel", 1000.4f, -127.9f, 616.2f },
        { "static tunnel", 1042.2f, -104.3f, 630.0f },
        { "static tunnel", 1049.8f, -113.3f, 629.8f },
        { "static tunnel", 1059.2f,   95.9f, 630.8f },
        { "static tunnel", 1062.2f,  -29.9f, 633.9f },
        { "static tunnel", 1069.9f,  100.0f, 631.1f },
        { "static tunnel", 1073.6f,  -31.0f, 633.4f },
        { "Gorkun's pump", 1060.95f, 102.79f, 630.2f },
    };
    for (P const& p : kNot)
        EXPECT_FALSE(inVolume(p.x, p.y, p.z))
            << p.what << " (" << p.x << ", " << p.y << ", " << p.z
            << ") is INSIDE the wave-2 volume — 36841 also spawns statically in the tunnel and "
               "is pumped for the whole Tyrannus fight, so the wave would never read as dead";
}

// --- 10. the door ----------------------------------------------------------

TEST(DungeonEventPitOfSaronTest, TheIceWallIsScriptOnlyAndStillVisibleToNavigation)
{
    EXPECT_TRUE(DcEventDoorRegistry::IsScriptOnly(GO_ICE_WALL))
        << "201885 is a lock-free GAMEOBJECT_TYPE_DOOR — exactly what BotCanOpenDoorLikePlayer "
           "will happily open — and it is instance_pit_of_saron's sole property";
    EXPECT_FALSE(DcEventDoorRegistry::IsNavigationIgnored(GO_ICE_WALL))
        << "it is a real gate on a designed leg. By the time the module reaches it, it is "
           "already open (both bosses are DONE), so a run that DOES pause here has regressed "
           "somewhere — and hiding it from navigation would mask that rather than prevent it";
}

// --- 11. the poison on the floor -------------------------------------------
//
// 69024 (Krick) and 70274 (Plagueborn Horror) are the same spell twice: Spell.dbc
// gives both Effect[0] = 27 SPELL_EFFECT_PERSISTENT_AREA_AURA applying aura 89
// SPELL_AURA_PERIODIC_DAMAGE_PERCENT at EffectRadiusIndex 26 = 4.0yd, amplitude
// 2000ms, DurationIndex 1 = 10000ms, EffectImplicitTargetA 53
// TARGET_DEST_TARGET_ENEMY. Percent-of-max-health is what makes them worth a row
// rather than a healing budget: 15%/2s and 10%/2s do not get survivable with gear.

namespace
{
    constexpr uint32 MAP_PIT_OF_SARON       = 658;
    constexpr uint32 SPELL_TOXIC_WASTE_KRICK = 69024;
    constexpr uint32 SPELL_TOXIC_WASTE_HORROR = 70274;
}

TEST(DungeonEventPitOfSaronTest, BothToxicWastePoolsAreRegisteredAndSizedAgainstTheirAura)
{
    for (uint32 spellId : { SPELL_TOXIC_WASTE_KRICK, SPELL_TOXIC_WASTE_HORROR })
    {
        DcGroundHazard const* row = DcHazardRegistry::FindGround(MAP_PIT_OF_SARON, spellId);
        ASSERT_NE(row, nullptr)
            << "Toxic Waste (" << spellId << ") has no DcGroundHazard row — a "
               "PERSISTENT_AREA_AURA the bots cannot see is a pool they stand in";

        EXPECT_EQ(row->mapId, MAP_PIT_OF_SARON);
        EXPECT_EQ(row->spellId, spellId);

        // vacateRadius is the RAW 4.0yd aura, the rule every ground row follows,
        // so the retreat's aim point lands outside this row's own keep-out.
        EXPECT_FLOAT_EQ(row->vacateRadius, 4.0f);
        // ...and the placement keep-out keeps the same 3yd budget over it that the
        // other eight pool rows leave for NavmeshSnap pulling a candidate back in.
        EXPECT_FLOAT_EQ(row->radius, 7.0f);
        EXPECT_FLOAT_EQ(row->radius - row->vacateRadius, 3.0f);

        // The pool cannot be fought — there is no unit to target — so the row must
        // drive the active retreat and not merely bias placement.
        EXPECT_GT(row->vacateRadius, 0.0f);

        // The retreat aims vacate + slack; that point must read clean against this
        // row's own geometry or the vacate action rejects every candidate it makes.
        float const aim = row->vacateRadius + row->retreatSlack;
        EXPECT_GT(aim, row->radius);
        EXPECT_FALSE(DcHazardRegistry::PointInside(*row, 0.0f, 0.0f, 0.0f, aim, 0.0f, 0.0f));
        EXPECT_TRUE(DcHazardRegistry::PointInside(*row, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f));
    }

    // The map now carries ground pools and still carries neither of the other two
    // hazard kinds, so the combined probe every live predicate gates on says yes.
    EXPECT_TRUE(DcHazardRegistry::HasGroundHazards(MAP_PIT_OF_SARON));
    EXPECT_TRUE(DcHazardRegistry::HasAnyHazard(MAP_PIT_OF_SARON));
    EXPECT_FALSE(DcHazardRegistry::HasEmitters(MAP_PIT_OF_SARON));
    EXPECT_FALSE(DcHazardRegistry::HasTrapHazards(MAP_PIT_OF_SARON));
}

TEST(DungeonEventPitOfSaronTest, ToxicWasteKeepOutStaysOnTheArenaFloor)
{
    DcGroundHazard const* row = DcHazardRegistry::FindGround(MAP_PIT_OF_SARON, SPELL_TOXIC_WASTE_KRICK);
    ASSERT_NE(row, nullptr);

    // Every Plagueborn Horror spawn and Ick himself sit between z 509.5 and
    // z 512.5, so a pool's cylinder tops out at z 518.5. The north bridge
    // DcNavPenaltyRegistry fences is z520-531 and the ramp above the arena runs
    // z522-565: a pool on the floor must not sterilise either of them.
    constexpr float kPoolZ = 512.5f;   // the highest Horror spawn
    EXPECT_FALSE(DcHazardRegistry::PointInside(*row, 850.0f, 100.0f, kPoolZ,
                                               850.0f, 100.0f, 520.0f))
        << "a floor pool reaches the north-bridge band — the bridge fence and the "
           "hazard keep-out would fight each other over the same polys";
    EXPECT_FALSE(DcHazardRegistry::PointInside(*row, 850.0f, 100.0f, kPoolZ,
                                               850.0f, 100.0f, 530.0f));

    // On the floor itself it does exactly what it is for: inside the keep-out at
    // 6.5yd, clear of it at 7.5yd.
    EXPECT_TRUE(DcHazardRegistry::PointInside(*row, 850.0f, 100.0f, 510.0f,
                                              856.5f, 100.0f, 510.0f));
    EXPECT_FALSE(DcHazardRegistry::PointInside(*row, 850.0f, 100.0f, 510.0f,
                                               857.5f, 100.0f, 510.0f));

    // And a camp leg that walks straight through a pool is rejected even when
    // both of its endpoints are clear.
    EXPECT_TRUE(DcHazardRegistry::SegmentClips(*row, 850.0f, 100.0f, 510.0f,
                                               830.0f, 100.0f, 510.0f,
                                               870.0f, 100.0f, 510.0f));
    EXPECT_FALSE(DcHazardRegistry::SegmentClips(*row, 850.0f, 100.0f, 510.0f,
                                                830.0f, 130.0f, 510.0f,
                                                870.0f, 130.0f, 510.0f));
}

TEST(DungeonEventPitOfSaronTest, NoOtherPitOfSaronSpellLeavesARegisteredPool)
{
    // The two Toxic Wastes are the only SPELL_EFFECT_PERSISTENT_AREA_AURA spells
    // on map 658. These are the near-misses, and every one of them must stay OUT:
    //   68989 Poison Nova       — instant 15yd nova + DoT, no persistent leg.
    //   69012 / 69263 Explosive Barrage — periodic trigger summoning Exploding Orb
    //                             creatures; a moving one-shot, not a patch.
    //   69015 / 69017 / 69019   — the orb's summon, visual and 6yd detonation.
    //   69021 Mighty Kick, 69028 Shadow Bolt, 68987 Pursuit — single-target.
    //   69581 Pustulant Flesh   — nuke + DoT on one target, no persistent leg.
    //   69582 Blight Bomb       — one-shot 20yd death explosion at 15% HP.
    //   69424 Icicle            — the ONE genuine near-miss: it really does carry
    //                             a PERSISTENT_AREA_AURA leg, but at aura 4
    //                             SPELL_AURA_DUMMY, radius 0.0yd, no amplitude —
    //                             the telegraph decal, not the falling icicle. A
    //                             row here would read as "icicles handled"; they
    //                             are deferred on purpose (PitOfSaronEvents.cpp).
    for (uint32 spellId : { 68987u, 68989u, 69012u, 69015u, 69017u, 69019u,
                            69021u, 69028u, 69263u, 69424u, 69426u, 69428u,
                            69581u, 69582u })
    {
        EXPECT_EQ(DcHazardRegistry::FindGround(MAP_PIT_OF_SARON, spellId), nullptr)
            << spellId << " has no PERSISTENT_AREA_AURA leg and must not hold a "
               "DcGroundHazard row — DungeonClearGroundHazardsValue keys on what "
               "DynamicObject::GetSpellId() returns, and no DynamicObject is ever made";
    }

    // And the rows are keyed on the map, not loose spell ids.
    EXPECT_EQ(DcHazardRegistry::FindGround(604, SPELL_TOXIC_WASTE_KRICK), nullptr);
    EXPECT_EQ(DcHazardRegistry::FindGround(658, 55627u), nullptr);
}
