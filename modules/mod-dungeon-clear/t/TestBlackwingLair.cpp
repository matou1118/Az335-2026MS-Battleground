/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "gtest/gtest.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "Position.h"

#include "Ai/Dungeon/DungeonClear/Data/DcTargetExclusionRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonClearRouteRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonEventRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"
#include "Ai/Dungeon/DungeonClear/Overrides/BossRosterRegistry.h"
#include "Ai/Dungeon/DungeonClear/Overrides/ObjectiveHookRegistry.h"
#include "Ai/Dungeon/DungeonClear/Util/DcDifficulty.h"
#include "Ai/Dungeon/DungeonClear/Util/DcNoStopZone.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonClearTuning.h"
#include "Ai/Dungeon/DungeonClear/Strategy/DcRelevance.h"
#include "Ai/Dungeon/DungeonClear/Util/DcRazorgoreDecision.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonPathFollower.h"

// Blackwing Lair (map 469) — Razorgore's orb and egg run.
//
// Two halves, both lintable without a world: the authored EVENT (whose flag set
// is the whole reason the encounter can be driven at all) and the pure KERNEL
// that decides what the driver does each tick.

namespace
{
    using namespace DcBlackwingLair;

    // A View in the middle of a healthy egg run: possessed, boss idle, an egg
    // elected and out of range. Every test below perturbs one field of it, so the
    // thing under test is always the single difference.
    DcRazorgore::View Driving()
    {
        DcRazorgore::View v;
        v.eventDone     = false;
        v.bossAlive     = true;
        v.eggsRemaining = 17;
        v.bossCharmed   = true;
        v.bossCasting   = false;
        v.orbGuardsAlive = false;
        v.orbGuardsEngaged = false;
        v.haveRunner    = true;
        v.runnerAtOrb   = true;
        v.runnerCanClick = true;
        v.haveEgg       = true;
        v.bossToEgg     = 30.0f;
        return v;
    }

    DcRazorgore::RunnerCandidate Bot(uint32 guid)
    {
        DcRazorgore::RunnerCandidate c;
        c.guidLow = guid;
        c.alive = true;
        c.isBot = true;
        return c;
    }
}

// --- the authored event ---------------------------------------------------

TEST(DungeonEventBlackwingLairTest, RazorgoreEventIsAnEncounterActiveCombatDriver)
{
    DungeonEvent const* ev = DungeonEventRegistry::Find(MAP_ID, EVENT_RAZORGORE_ORB);
    ASSERT_NE(ev, nullptr) << "Blackwing Lair (469) event 1 (Razorgore orb) is missing";

    EXPECT_EQ(ev->activation, EventActivation::Conditional);
    EXPECT_TRUE(static_cast<bool>(ev->condition)) << "the egg-run gate predicate must be bound";

    // THE flag. On a raid map DcBossStandDown makes every DC behaviour inert the
    // moment an encounter goes live, and FindDueConditionalEvent refuses any event
    // without this one outright — so dropping it does not degrade the encounter,
    // it silently removes it exactly when it has work to do.
    EXPECT_TRUE(ev->encounterActive)
        << "the orb event must be EncounterActive: it is the ONE exemption to the raid "
           "boss stand-down, and without it the driver never runs inside the fight";

    // The adds never stop until the last egg pops, so the party is in combat for
    // the whole run and a non-combat-only driver would never get a tick.
    EXPECT_TRUE(ev->drivesInCombat) << "the orb event must DrivesInCombat (continuous adds)";
    // The driver issues the POSSESSED BOSS's splines; the per-tick objective hold
    // would cancel them on the next tick.
    EXPECT_TRUE(ev->stepsOwnMovement) << "the orb event must StepsOwnMovement";
    // A phase-1 wipe soft-resets the encounter (the boss respawns in 30s and the
    // instance clears the field), so the event has to be able to come back.
    EXPECT_TRUE(ev->repeatable) << "the orb event must be Repeatable (the wipe path)";
    EXPECT_FALSE(ev->required) << "the orb event must be Optional (a timeout re-fires it)";

    // ONE Custom step: what this encounter needs is a preference re-decided every
    // tick as charms expire and runners die, not a sequence of gates.
    ASSERT_EQ(ev->steps.size(), 1u);
    EXPECT_EQ(ev->steps[0].kind, EventStepKind::Custom);
    EXPECT_EQ(ev->steps[0].hookId, HOOK_RAZORGORE_ORB);
    EXPECT_TRUE(ObjectiveHookRegistry::Has(HOOK_RAZORGORE_ORB))
        << "hook " << HOOK_RAZORGORE_ORB << " (DriveRazorgoreOrb) must be registered";
}

TEST(DungeonEventBlackwingLairTest, AuthoredIdsMatchTheWorldData)
{
    // Verified against the live world DB and Spell.dbc; a typo here is invisible
    // at runtime (a scan that finds nothing simply never fires).
    EXPECT_EQ(MAP_ID, 469u);
    EXPECT_EQ(NPC_RAZORGORE, 12435u);
    EXPECT_EQ(GO_ORB_OF_DOMINATION, 177808u);
    EXPECT_EQ(GO_BLACK_DRAGON_EGG, 177807u);
    EXPECT_EQ(EGG_COUNT, 30u);
    EXPECT_EQ(SPELL_MIND_CONTROL, 19832u);
    EXPECT_EQ(SPELL_MIND_EXHAUSTION, 23958u);
    EXPECT_EQ(SPELL_DESTROY_EGG, 19873u);

    // The commit band must stay well inside the spell's 10yd range: 19873 carries
    // interrupt flags 31, so arriving "just barely" in range is how a cast gets
    // eaten by the last half-yard of slide.
    EXPECT_LT(DcRazorgore::kCastBand, 10.0f);
    // ...and outside the re-path epsilon, or the driver would oscillate between
    // "close enough to cast" and "re-issue the spline".
    EXPECT_GT(DcRazorgore::kCastBand, DcRazorgore::kRepathEpsilon);
}

TEST(DungeonEventBlackwingLairTest, TheRaidCampsOffTheLedgeAndInHealRangeOfTheRunner)
{
    // The fix from the first live run: the orb and the egg run worked, and the
    // raid fought wherever the pull had left it, so every add that picked the
    // rooted runner arrived unopposed.
    //
    // The camp is on the FLOOR (column-probed: one walkable surface at 408.87),
    // not on the orb platform at 413 — a raid on a small ledge has nowhere to
    // spread and nothing between it and the floor the adds cross.
    EXPECT_LT(CAMP_Z, ORB_Z - 3.0f) << "the camp must be BELOW the orb platform";

    // Close enough that every healer covers the runner (heal range is 40yd) and a
    // melee bot can peel something off the ledge in a couple of steps...
    float const dx = CAMP_X - ORB_X, dy = CAMP_Y - ORB_Y, dz = CAMP_Z - ORB_Z;
    float const toOrb = std::sqrt(dx * dx + dy * dy + dz * dz);
    EXPECT_LT(toOrb, 20.0f) << "camp is too far from the orb to protect the runner";
    // ...and far enough out that the raid is not standing on the platform itself.
    EXPECT_GT(toOrb, 8.0f) << "camp has crept back onto the ledge";

    // Every add-spawn position the instance uses is a room's width away, so a wave
    // has to cross to the raid rather than landing on top of it.
    Position const spawns[] = {
        {-7661.207520f, -1043.268188f, 407.199554f}, {-7644.145020f, -1065.628052f, 407.204956f},
        {-7624.260742f, -1095.196899f, 407.205017f}, {-7608.501953f, -1116.077271f, 407.199921f},
        {-7531.841797f, -1063.765381f, 407.199615f}, {-7547.319336f, -1040.971924f, 407.205078f},
        {-7568.547852f, -1013.112488f, 407.204926f}, {-7584.175781f, -989.669128f, 407.199585f},
    };
    for (Position const& p : spawns)
        EXPECT_GT(p.GetExactDist(CAMP_X, CAMP_Y, CAMP_Z), 40.0f)
            << "add spawn (" << p.GetPositionX() << ", " << p.GetPositionY()
            << ") is close enough to the camp that its wave arrives on top of the raid";

    // ONE leash for the whole raid (the tank's separate tier is gone — 30 is
    // loose enough for anyone to step onto an add that reached the healers). It
    // is still a leash and not a free rein, and the bound that decides the number
    // is the RUNNER: it is rooted on the ledge and cannot come to a healer, so a
    // bot at the far edge of the camp must not be hopelessly out of range of it.
    EXPECT_GT(CAMP_LEASH, 0.0f);
    EXPECT_LT(CAMP_LEASH + toOrb, 45.0f)
        << "a bot at the far edge of the camp is out of any reach of the runner — "
           "the leash has stopped being a camp";

    // WHAT A LEASH THIS WIDE DECIDES: GRETHOK'S OWN SPAWN is inside it, so the
    // guard fight the tank pulls happens within the camp and the raid is not
    // walked off the pack it is killing on the first tick of the egg run. The
    // camp arms with that pull (razorDrivingMs), so this is not a nicety — a
    // leash that excluded the platform would fight the pull for every melee bot
    // in the raid. Re-read DungeonClearRazorgoreCampTrigger before "fixing" the
    // number.
    float const sdx = GRETHOK_X - CAMP_X, sdy = GRETHOK_Y - CAMP_Y,
                sdz = GRETHOK_Z - CAMP_Z;
    EXPECT_LT(std::sqrt(sdx * sdx + sdy * sdy + sdz * sdz), CAMP_LEASH)
        << "the camp yanks the raid off the guard pull it was just sent to make";

    // THE PING-PONG THIS ENCODES: the walk-back aims at the camp's near edge,
    // CAMP_HOLD_MARGIN inside the leash, so a bot the fight pushes out takes one
    // step back and holds. A margin of zero puts the hold point ON the boundary
    // and the next yard of drift re-arms the rung; a margin that reached the
    // leash puts it back at the centre and every correction is a lap across the
    // whole camp again. Both ends are the same bug, so both are asserted.
    EXPECT_GT(CAMP_HOLD_MARGIN, 0.0f)
        << "no margin is no hysteresis — the rung re-arms the moment it releases";
    EXPECT_LT(CAMP_HOLD_MARGIN, CAMP_LEASH)
        << "the margin has swallowed the tightest leash, so the walk-back aims at "
           "the camp centre again and the ping-pong is back";

    // And the step itself has to be a step: at the tightest leash the correction
    // is CAMP_HOLD_MARGIN yards, which must stay small next to the camp's own
    // radius or "hold at the edge" is indistinguishable from "walk to the middle".
    EXPECT_LT(CAMP_HOLD_MARGIN, CAMP_LEASH * 0.5f)
        << "the walk-back crosses more than half the camp — that is a lap, not a step";
}

TEST(DungeonEventBlackwingLairTest, OrbRungOutranksTheRaidStrategy)
{
    // mod-playerbots' `bwl` strategy nodes sit at ACTION_RAID (60) and
    // ACTION_RAID+1 (61) — and one of them, `bwl razorgore avoid aoe`, walks bots
    // out of the boss's frontal cone. It would walk the orb runner off the ledge
    // every tick of its trip if this rung did not outrank it.
    EXPECT_GT(DcRel::RazorgoreOrb, 61.0f);
    // Below the phantom-combat hatch, which must always win when it legitimately
    // fires.
    EXPECT_LT(DcRel::RazorgoreOrb, DcRel::BreakStuckCombat);

    // The raid's camp sits between the two: above the strategy nodes it has to
    // beat (`bwl razorgore avoid aoe` would walk bots straight out of the camp),
    // below the runner, because a bot that is somehow both has the more urgent job
    // at the orb.
    EXPECT_GT(DcRel::RazorgoreCamp, 61.0f);
    EXPECT_LT(DcRel::RazorgoreCamp, DcRel::RazorgoreOrb);
}

// --- the kernel: what the driver does this tick ---------------------------

TEST(DungeonEventBlackwingLairTest, CompletionOutranksEverything)
{
    // The dangerous tick is the one right after the thirtieth egg: the raid is now
    // trying to KILL Razorgore, and a driver that re-elected a runner and re-took
    // the orb there would mind-control the boss out of its own kill.
    DcRazorgore::View v = Driving();
    v.eventDone = true;
    EXPECT_EQ(DcRazorgore::Decide(v), DcRazorgore::Step::Done);

    v = Driving();
    v.eggsRemaining = 0;
    EXPECT_EQ(DcRazorgore::Decide(v), DcRazorgore::Step::Done);

    v = Driving();
    v.bossAlive = false;
    EXPECT_EQ(DcRazorgore::Decide(v), DcRazorgore::Step::Done);
}

TEST(DungeonEventBlackwingLairTest, PossessedBossWalksThenCasts)
{
    DcRazorgore::View v = Driving();
    v.bossToEgg = DcRazorgore::kCastBand + 0.1f;
    EXPECT_EQ(DcRazorgore::Decide(v), DcRazorgore::Step::MoveBoss);

    v.bossToEgg = DcRazorgore::kCastBand - 0.1f;
    EXPECT_EQ(DcRazorgore::Decide(v), DcRazorgore::Step::CastEgg);
}

TEST(DungeonEventBlackwingLairTest, ACastInFlightYieldsTheTick)
{
    // Destroy Egg is a 3s cast — thirty ticks. Claiming them would starve the
    // raid's combat engine for the whole egg run, which is how parties wipe to the
    // add wave while the driver is doing everything right.
    DcRazorgore::View v = Driving();
    v.bossCasting = true;
    v.bossToEgg = 1.0f;
    EXPECT_EQ(DcRazorgore::Decide(v), DcRazorgore::Step::WaitCast);
}

TEST(DungeonEventBlackwingLairTest, EveryEggSkippedAsksForAFreshPass)
{
    // Not Done: eggs remain, they are just all on the skip list this pass. Idle is
    // the caller's cue to clear it and try again — a skip is "not now", never
    // "never", and treating it as completion would end phase 1 with eggs standing.
    DcRazorgore::View v = Driving();
    v.haveEgg = false;
    EXPECT_EQ(DcRazorgore::Decide(v), DcRazorgore::Step::Idle);
}

TEST(DungeonEventBlackwingLairTest, TheMindControlCycleRotatesRunners)
{
    // Window over (90s expired, or the runner died): back to electing.
    DcRazorgore::View v = Driving();
    v.bossCharmed = false;
    v.haveRunner = false;
    EXPECT_EQ(DcRazorgore::Decide(v), DcRazorgore::Step::NeedRunner);

    // Elected but still walking the 78yd to the ledge.
    v.haveRunner = true;
    v.runnerAtOrb = false;
    EXPECT_EQ(DcRazorgore::Decide(v), DcRazorgore::Step::StageRunner);

    // Standing on it and legal: take the orb.
    v.runnerAtOrb = true;
    EXPECT_EQ(DcRazorgore::Decide(v), DcRazorgore::Step::ClickOrb);

    // Standing on it and NOT legal — the 60s Mind Exhaustion from its own last
    // window, or a pet it resummoned on the way. Someone else goes; waiting out a
    // lockout at the orb would cost the run a whole window.
    v.runnerCanClick = false;
    EXPECT_EQ(DcRazorgore::Decide(v), DcRazorgore::Step::NeedRunner);
}

// --- the kernel: who takes the orb ----------------------------------------

TEST(DungeonEventBlackwingLairTest, RunnerElectionEnforcesTheOrbsOwnRefusals)
{
    // go_orb_of_domination::GossipHello refuses a user with a pet or with Mind
    // Exhaustion up, and a corpse cannot click anything. A real player has no
    // PlayerbotAI for the rung to run on. The leader is the main tank and is the
    // only thing between the raid and an add every four seconds.
    std::vector<DcRazorgore::RunnerCandidate> pool;
    DcRazorgore::RunnerCandidate dead = Bot(1);      dead.alive = false;
    DcRazorgore::RunnerCandidate human = Bot(2);     human.isBot = false;
    DcRazorgore::RunnerCandidate pet = Bot(3);       pet.hasPet = true;
    DcRazorgore::RunnerCandidate tired = Bot(4);     tired.exhausted = true;
    DcRazorgore::RunnerCandidate leader = Bot(5);    leader.isLeader = true;
    pool = {dead, human, pet, tired, leader};
    EXPECT_EQ(DcRazorgore::SelectRunner(pool), -1)
        << "every candidate is barred; the driver must report no runner, not pick one";

    // One legal body is enough, whatever its role.
    DcRazorgore::RunnerCandidate healer = Bot(6);    healer.isHealer = true;
    pool.push_back(healer);
    EXPECT_EQ(DcRazorgore::SelectRunner(pool), 5);
}

TEST(DungeonEventBlackwingLairTest, RunnerElectionPrefersARangedDps)
{
    // The orb is on the upper ledge, 78yd from the boss and above every add spawn,
    // and the charmer is rooted there for 90 seconds. A ranged bot keeps DPSing
    // through its window; a melee bot is simply out of the fight, and a healer
    // parked there is how the raid dies to the adds.
    DcRazorgore::RunnerCandidate healer = Bot(1);  healer.isHealer = true;
    DcRazorgore::RunnerCandidate offtank = Bot(2); offtank.isTank = true;
    DcRazorgore::RunnerCandidate melee = Bot(3);
    DcRazorgore::RunnerCandidate ranged = Bot(4);  ranged.isRanged = true;

    EXPECT_EQ(DcRazorgore::SelectRunner({healer, offtank, melee, ranged}), 3);
    EXPECT_EQ(DcRazorgore::SelectRunner({healer, offtank, melee}), 2);
    EXPECT_EQ(DcRazorgore::SelectRunner({healer, offtank}), 1);
    EXPECT_EQ(DcRazorgore::SelectRunner({healer}), 0);
}

TEST(DungeonEventBlackwingLairTest, RunnerElectionIsStableAcrossContexts)
{
    // Every member computes the election independently (the leader publishes it,
    // but the runner's own rung re-derives eligibility); a tie broken by anything
    // but the lowest GUID would let two bots disagree about who is going.
    DcRazorgore::RunnerCandidate a = Bot(70); a.isRanged = true;
    DcRazorgore::RunnerCandidate b = Bot(12); b.isRanged = true;
    DcRazorgore::RunnerCandidate c = Bot(41); c.isRanged = true;
    EXPECT_EQ(DcRazorgore::SelectRunner({a, b, c}), 1);
    EXPECT_EQ(DcRazorgore::SelectRunner({c, a, b}), 2);
    EXPECT_EQ(DcRazorgore::SelectRunner({b, c, a}), 0);
}

// --- the phase-1 damage guard --------------------------------------------

TEST(DungeonEventBlackwingLairTest, NothingTouchesThePlatformBeforeTheTankPullsIt)
{
    // THE ORDERING THIS FILE EXISTS TO PIN. Grethok is a boss anchor, so the pull
    // is the tank's and the whole raid comes with it (advance, muster, standoff,
    // engage). Until that pull lands, the kernel must not elect anybody, must not
    // stage anybody, and must not click: each of those puts one DPS bot on a
    // ledge 78yd ahead of its raid, next to three level-62 elites. The shape that
    // staged "with the raid" needed a rung that glided forty bots at the ledge to
    // make it true — which is the footrace this replaced.
    DcRazorgore::View v = Driving();
    v.bossCharmed = false;
    v.orbGuardsAlive = true;
    v.orbGuardsEngaged = false;
    v.bossEngaged = false;

    v.haveRunner = false;
    EXPECT_EQ(DcRazorgore::Decide(v), DcRazorgore::Step::WaitPull)
        << "an election before the pull arms a runner's rung and sends it up alone";

    v.haveRunner = true;
    v.runnerAtOrb = false;
    EXPECT_EQ(DcRazorgore::Decide(v), DcRazorgore::Step::WaitPull)
        << "the staging walk ends on top of three elites nobody is fighting";

    v.runnerAtOrb = true;
    EXPECT_EQ(DcRazorgore::Decide(v), DcRazorgore::Step::WaitPull);

    // An empty platform is the other way out of the wait — a re-entry, or a raid
    // that killed the guards on some earlier attempt.
    v.orbGuardsAlive = false;
    EXPECT_EQ(DcRazorgore::Decide(v), DcRazorgore::Step::ClickOrb);
}

TEST(DungeonEventBlackwingLairTest, TheRunnerTakesOverTheMomentGrethokIsEngaged)
{
    // The other half of the same contract, and the reason the wait is a wait and
    // not a "clear the platform first": from the tag onward, Razorgore is already
    // in the fight (creature_formations, groupAI 7), the add pump is already
    // running (the instance promotes NOT_STARTED on the pull, not on the orb), and
    // the raid is already damaging a boss whose phase-1 death casts 20038 on all
    // forty of them. Waiting for the guards to die on top of that is pure loss.
    DcRazorgore::View v = Driving();
    v.bossCharmed = false;
    v.orbGuardsAlive = true;
    v.orbGuardsEngaged = true;   // the tank has Grethok
    v.bossEngaged = false;       // ...and the formation has not dragged him in YET

    v.haveRunner = false;
    EXPECT_EQ(DcRazorgore::Decide(v), DcRazorgore::Step::NeedRunner);

    v.haveRunner = true;
    v.runnerAtOrb = false;
    EXPECT_EQ(DcRazorgore::Decide(v), DcRazorgore::Step::StageRunner);

    v.runnerAtOrb = true;
    EXPECT_EQ(DcRazorgore::Decide(v), DcRazorgore::Step::ClickOrb);

    // A runner that cannot legally click is replaced immediately — the 60s
    // lockout does not pause for the guard fight.
    v.runnerCanClick = false;
    EXPECT_EQ(DcRazorgore::Decide(v), DcRazorgore::Step::NeedRunner);

    // And a boss already in combat is just as good an answer, whatever pulled
    // him: a stray add, a wipe recovery, or the formation link a tick after the
    // tag. Either flag releases the wait.
    v.runnerCanClick = true;
    v.orbGuardsEngaged = false;
    v.bossEngaged = true;
    EXPECT_EQ(DcRazorgore::Decide(v), DcRazorgore::Step::ClickOrb);
}

TEST(DungeonEventBlackwingLairTest, AGuardRespawnNeverAbortsALivePossession)
{
    // The guard gate is BEHIND the possessed branch, and behind completion. Once
    // the mind control is up the run has 90 seconds of egg to break and nothing
    // about the platform can be worth spending them on — the driver must keep
    // driving.
    DcRazorgore::View v = Driving();
    v.orbGuardsAlive = true;
    EXPECT_EQ(DcRazorgore::Decide(v), DcRazorgore::Step::MoveBoss);

    v.bossToEgg = 1.0f;
    EXPECT_EQ(DcRazorgore::Decide(v), DcRazorgore::Step::CastEgg);

    // ...and a guard standing after the last egg is still Done, not a new job.
    v.eggsRemaining = 0;
    EXPECT_EQ(DcRazorgore::Decide(v), DcRazorgore::Step::Done);
}

TEST(DungeonEventBlackwingLairTest, GrethokIsBossZeroAndTheRealBossesShiftBehindHim)
{
    // The roster patch IS the fix for "the tank runs in and leaves the raid
    // behind": with no anchor of his own, Grethok was pulled by a bespoke rung
    // that glided every bot at the ledge; as boss #0 he is pulled by the ordinary
    // pipeline, which musters the raid and stages it first.
    std::vector<DungeonBossInfo> base;
    uint32 const entries[] = {12435, 13020, 12017, 11983, 14601, 11981, 14020, 11583};
    for (uint32 i = 0; i < 8; ++i)
    {
        DungeonBossInfo b;
        b.entry = entries[i];
        b.encounterIndex = i;   // instance_encounters 610-617, bits 0-7
        b.mapId = MAP_ID;
        base.push_back(b);
    }

    auto const out = BossRosterRegistry::Apply(MAP_ID, DcDiffKey::Raid(0), base);
    ASSERT_EQ(out.size(), 9u);

    // Grethok leads, and he is a BOSS anchor — an objective would complete on
    // arrival and never pull anything.
    ASSERT_EQ(out.front().entry, NPC_GRETHOK_THE_CONTROLLER);
    EXPECT_EQ(out.front().kind, DungeonAnchorKind::Boss);
    EXPECT_EQ(BossOrderKey(out.front()), 0u);
    // His anchor is his spawn: the pull pipeline measures its standoff from the
    // live creature, but the advance walks the raid to this point to find him.
    EXPECT_FLOAT_EQ(out.front().x, GRETHOK_X);
    EXPECT_FLOAT_EQ(out.front().y, GRETHOK_Y);
    EXPECT_FLOAT_EQ(out.front().z, GRETHOK_Z);

    // COMPLETION is borrowed from Razorgore. Grethok has no DungeonEncounter row
    // of his own, so without this his row would carry bit 0 by default anyway —
    // the inheritance says so on purpose, and it must resolve to a REAL bit
    // rather than being left as a dangling "inherit from" marker.
    EXPECT_EQ(out.front().encounterIndex, 0u);
    EXPECT_EQ(out.front().inheritCompletionFrom, 0u)
        << "the inheritance must be resolved by Apply, not carried into the run";

    // ...and the eight real bosses keep their DBC kill-bits while shifting to
    // 1..8, so nothing about their completion detection changes.
    for (uint32 i = 0; i < 8; ++i)
    {
        EXPECT_EQ(out[i + 1].entry, entries[i]) << i;
        EXPECT_EQ(out[i + 1].encounterIndex, i) << i;
        EXPECT_EQ(BossOrderKey(out[i + 1]), i + 1) << i;
    }

    // Razorgore specifically must sort AFTER Grethok — a tie would leave the
    // order to the list's own sort and the raid would walk to the boss it is
    // forbidden to kill.
    EXPECT_LT(BossOrderKey(out.front()), BossOrderKey(out[1]));
}

TEST(DungeonEventBlackwingLairTest, TheCampRungSitsBetweenTheRaidStrategyAndTheRunner)
{
    // Two rungs left on this map, and the ordering between them and the raid
    // strategy is the whole statement of who owns a bot's tick during the egg
    // run. Above ACTION_RAID+1 (61): `bwl razorgore avoid aoe` would walk bots
    // out of the camp. Below the runner (62): a bot that is somehow both has the
    // more urgent job at the orb.
    EXPECT_GT(DcRel::RazorgoreCamp, 61.0f);
    EXPECT_LT(DcRel::RazorgoreCamp, DcRel::RazorgoreOrb);
}

TEST(DungeonEventBlackwingLairTest, TheCampOutranksTheApproachLadderNotJustTheRaidStrategy)
{
    // The camp is registered in BOTH engines now, and the reason is the ladder it
    // has to beat in the NON-combat one. The egg run has plenty of out-of-combat
    // ticks — the wave dies, the possessed boss is attacking nobody — and on
    // those the driving ladder had the raid: the next boss is Razorgore, our own
    // runner is walking him egg to egg, and the advance chased that moving anchor
    // and parked at the engage range with the followers in tow (live, 23:14:41
    // and 23:15:47: 45yd and 42yd splines issued at the possessed boss, between
    // "within engage range of Razorgore the Untamed ... -> holding for at-boss").
    //
    // Those rungs stand down for the whole egg run (DcBlackwingLair::
    // EggRunHoldsTheRaid), and the camp outranks every one of them anyway, so a
    // stand-down that ever misses a tick still cannot outvote the camp.
    EXPECT_GT(DcRel::RazorgoreCamp, DcRel::Advance);
    EXPECT_GT(DcRel::RazorgoreCamp, DcRel::AtBoss);
    EXPECT_GT(DcRel::RazorgoreCamp, DcRel::Pull);
    EXPECT_GT(DcRel::RazorgoreCamp, DcRel::BlockingTrash);
    EXPECT_GT(DcRel::RazorgoreCamp, DcRel::FollowTank);

    // Including the rez rung, which is the one genuine cost of putting it this
    // high in the non-combat engine: a corpse outside the leash is not walked to
    // until phase 1 ends. The raid fights AT the camp, so its corpses are inside
    // it; a corpse across the chamber is one nobody may cross the room for while
    // the runner is rooted on the ledge.
    EXPECT_GT(DcRel::RazorgoreCamp, DcRel::RezParty);

    // ...and it still yields to the runner, which is the older statement of the
    // same rule: the orb is the more urgent job.
    EXPECT_LT(DcRel::RazorgoreCamp, DcRel::RazorgoreOrb);
}

TEST(DungeonEventBlackwingLairTest, ACastingRunnerIsNotDisqualified)
{
    // A DPS bot is casting most ticks. When "not mid-cast" was one of the
    // election's gates, the elected runner read unusable almost every tick, the
    // FSM asked for another, and the rotation cycled a fresh runner every three
    // seconds for the whole window without a single click landing. The cast is
    // interrupted at the orb instead, where it is one call.
    //
    // What runnerCanClick means now is the orb script's own three refusals, and
    // those still send the job to somebody else...
    DcRazorgore::View v = Driving();
    v.bossCharmed = false;
    v.runnerAtOrb = true;
    v.runnerCanClick = false;
    EXPECT_EQ(DcRazorgore::Decide(v), DcRazorgore::Step::NeedRunner);

    // ...and a runner that merely has a spell in flight is not one of them: it is
    // still the runner, and it still clicks.
    v.runnerCanClick = true;
    EXPECT_EQ(DcRazorgore::Decide(v), DcRazorgore::Step::ClickOrb);
}

TEST(DungeonEventBlackwingLairTest, RazorgoreIsATargetExclusionRowOnHisOwnMap)
{
    // The guard lives in dungeon-clear, not in mod-playerbots' `bwl` strategy,
    // because GatherStrategyTargetExclusions walks EVERY strategy on the bot's
    // combat engine — DungeonClearCombatStrategy included. Keeping it here puts
    // the combat guard in the same module as the driver whose work it protects.
    EXPECT_TRUE(DcTargetExclusionRegistry::HasRowsFor(MAP_ID));

    // ...and nowhere else. HasRowsFor is the cheap gate mod-playerbots caches per
    // engine; a stray map here would make every bot on it rebuild its combat
    // strategy list on every target pick.
    EXPECT_FALSE(DcTargetExclusionRegistry::HasRowsFor(409));  // Molten Core
    EXPECT_FALSE(DcTargetExclusionRegistry::HasRowsFor(608));  // The Violet Hold
    EXPECT_FALSE(DcTargetExclusionRegistry::HasRowsFor(0));

    // The row is keyed to Razorgore specifically — the adds around him are
    // ordinary targets and must stay killable.
    EXPECT_FALSE(DcTargetExclusionRegistry::IsExcluded(nullptr, MAP_ID, 12422u));
    EXPECT_FALSE(DcTargetExclusionRegistry::IsExcluded(nullptr, MAP_ID, 12416u));
    // Right entry, wrong map: never excluded.
    EXPECT_FALSE(DcTargetExclusionRegistry::IsExcluded(nullptr, 409u, NPC_RAZORGORE));
}

// --- the drake hall, pulled through a ceiling -----------------------------

TEST(DungeonEventBlackwingLairTest, TheApproachWalksTheRaidDirectlyUnderFiremaw)
{
    // The geometry the exclusion rows exist for, pinned so a route re-author
    // cannot quietly move the raid back under him without this failing.
    //
    // Firemaw's spawn is (-7520.2, -1025.8, 449.1). Approach anchor 7 — the long
    // hall from Vaelastrasz — is (-7520.5, -1023.3, 424.5): 2.5yd away in plan
    // view and 24.6yd below. A level-63 boss aggros at about 25yd in 3D, so the
    // route is INSIDE his radius, and DoZoneInCombat does the rest.
    std::vector<WaypointHint> const* row = DungeonClearRouteRegistry::Get(
        MAP_ID, DUNGEON_DIFFICULTY_NORMAL, NPC_BROODLORD_LASHLAYER);
    ASSERT_NE(row, nullptr);
    ASSERT_GT(row->size(), 8u);

    constexpr float kFiremawX = -7520.2f, kFiremawY = -1025.8f, kFiremawZ = 449.1f;

    float nearest = 1e9f;
    for (std::size_t i = 0; i < DcBlackwingLair::TRANSIT_STAGE_ANCHOR_INDEX; ++i)
    {
        WaypointHint const& a = (*row)[i];
        float const dx = a.x - kFiremawX, dy = a.y - kFiremawY, dz = a.z - kFiremawZ;
        nearest = std::min(nearest, std::sqrt(dx * dx + dy * dy + dz * dz));
    }

    // Not an assertion that this is ACCEPTABLE — it is a record that it is true,
    // and the reason the rows below are not optional. If a future route moves the
    // hall out of his bubble this becomes a free win and the number can change.
    EXPECT_LT(nearest, 30.0f)
        << "the approach no longer passes under Firemaw — re-read the exclusion "
           "rows' justification before trusting them to be load-bearing";
}

TEST(DungeonEventBlackwingLairTest, TheDrakeHallIsBarredWhileBroodlordStands)
{
    // Four rows, one window. The bar is not "killing this wastes damage" — it is
    // "answering this walks the raid two floors up through a ceiling the navmesh
    // cannot cross", which is what DoZoneInCombat makes possible from 250yd with
    // no line of sight.
    for (uint32 const entry : { DcBlackwingLair::NPC_FIREMAW, DcBlackwingLair::NPC_EBONROC,
                                DcBlackwingLair::NPC_FLAMEGOR,
                                DcBlackwingLair::NPC_CHROMAGGUS })
    {
        // Windowed, not permanent: with no bot to read the instance from, the row
        // must not answer yes — a flat always-on would block the kills the clear
        // is sequencing, exactly as it would for Razorgore.
        EXPECT_FALSE(DcTargetExclusionRegistry::IsExcluded(nullptr, MAP_ID, entry))
            << "entry " << entry << " must carry a live gate, not a flat bar";

        // Right entry, wrong map.
        EXPECT_FALSE(DcTargetExclusionRegistry::IsExcluded(nullptr, 409u, entry));
    }

    // Broodlord himself is NOT a row: he is the objective this sequences toward.
    EXPECT_FALSE(
        DcTargetExclusionRegistry::IsExcluded(nullptr, MAP_ID, NPC_BROODLORD_LASHLAYER));
}

TEST(DungeonEventBlackwingLairTest, NoFloorHeightCanSeparateTheDrakeHallFromTheGauntlet)
{
    // Why the out-of-order bar cannot key on z, pinned as geometry.
    //
    // The clause used to be `bot->GetPositionZ() >= 445` — below the drake hall's
    // floor you cannot be fighting something standing on it, so the bar lifted
    // once the raid was legitimately up there. That is right about the APPROACH
    // (anchors 0..TRANSIT_STAGE-1 run 24.6yd directly BENEATH Firemaw) and wrong
    // about the crossing, because the upper suppression room and Broodlord's own
    // standoff are on FIREMAW'S OWN FLOOR. The raid therefore spends the whole
    // legitimate second half of the gauntlet above any such threshold with the
    // bar lifted and Firemaw a short walk away — which is how two of five raids
    // killed him out of order in tp-20260828-132333-1.
    std::vector<WaypointHint> const* row = DungeonClearRouteRegistry::Get(
        MAP_ID, DUNGEON_DIFFICULTY_NORMAL, NPC_BROODLORD_LASHLAYER);
    ASSERT_NE(row, nullptr);
    ASSERT_GT(row->size(), DcBlackwingLair::TRANSIT_STAGE_ANCHOR_INDEX);

    constexpr float kFiremawZ = 449.1f;

    // The half the old clause got right: every approach anchor is well below the
    // hall, so a threshold anywhere between them covers the approach.
    for (std::size_t i = 0; i < DcBlackwingLair::TRANSIT_STAGE_ANCHOR_INDEX; ++i)
        EXPECT_LT((*row)[i].z, kFiremawZ - 5.0f)
            << "approach anchor " << i << " should run beneath the drake hall";

    // The half it got wrong: the crossing ENDS on Firemaw's floor. Any threshold
    // low enough to cover the approach is also low enough to be cleared here, so
    // no single z can be both. That is the whole reason the clause is now an
    // operator-skip question instead.
    WaypointHint const& standoff = row->back();
    EXPECT_NEAR(standoff.z, kFiremawZ, 5.0f)
        << "the Broodlord standoff shares the drake hall's floor";

    // ...and it is not distance either: Broodlord and Firemaw are ~54yd apart,
    // well inside the band a corridor scan or an aggro radius would call near.
    float const dx = standoff.x - (-7520.2f);
    float const dy = standoff.y - (-1025.8f);
    EXPECT_LT(std::sqrt(dx * dx + dy * dy), 80.0f)
        << "no plan-view bound can separate the standoff from Firemaw either";
}

TEST(DungeonEventBlackwingLairTest, ARouteCursorResetToZeroIsUnrecoverableOnThisRoute)
{
    // Why InstallLongPath must SEED the follower cursor by projection instead of
    // flatly resetting it to 0 (DungeonPathFollower::SeedCursor).
    //
    // Resnap is forward-only and searches RESNAP_WINDOW flattened points from the
    // cursor. Anchored segments collapse to ONE polyline point each, so on this
    // route a window of 24 reaches anchor 24 of ~40 — it cannot see the crossing
    // at all from anchor 0. A raid standing at the standoff whose cursor was just
    // reset therefore gets "Resnap FAILED -> rebuild -> reset to 0" forever, and
    // the rejoin walks it back to Vaelastrasz's chamber. 180 such rejoins in one
    // run of tp-20260828-132333-1.
    std::vector<WaypointHint> const* row = DungeonClearRouteRegistry::Get(
        MAP_ID, DUNGEON_DIFFICULTY_NORMAL, NPC_BROODLORD_LASHLAYER);
    ASSERT_NE(row, nullptr);

    EXPECT_GT(row->size(), DungeonPathFollower::RESNAP_WINDOW)
        << "if the whole route fitted in one Resnap window a cursor reset would "
           "self-heal and SeedCursor would be unnecessary";

    // And the reset is not merely slow to recover — the route START is far enough
    // from the crossing that no candidate is even inside RESNAP_RADIUS, so the
    // fallback (walk to the cursor) heads all the way back.
    WaypointHint const& start = row->front();
    WaypointHint const& standoff = row->back();
    float const dx = standoff.x - start.x;
    float const dy = standoff.y - start.y;
    float const dz = standoff.z - start.z;
    EXPECT_GT(std::sqrt(dx * dx + dy * dy + dz * dz), DungeonPathFollower::RESNAP_RADIUS)
        << "the route start must be out of resnap range of the far end, or the "
           "reset would be harmless";
}

TEST(DungeonEventBlackwingLairTest, TheTankCarveOutIsPerRowNotBlanket)
{
    // Razorgore's row leaves the TANK pick alone on purpose — somebody has to hold
    // him between mind controls while the raid is barred from killing him. An
    // out-of-order boss is the opposite case: a tank that answers Firemaw is
    // precisely how twenty-four other bots end up in his room. The distinction
    // lives on the row, so pin that the two rows disagree about it.
    //
    // Both are read here through the same nullptr-bot path used above, so what is
    // being pinned is the ROW SHAPE — that the tank question is asked per row at
    // all — rather than a live verdict, which needs an InstanceScript.
    EXPECT_FALSE(DcTargetExclusionRegistry::IsExcluded(nullptr, MAP_ID, NPC_RAZORGORE,
                                                       /*forTank*/ true));
    EXPECT_FALSE(DcTargetExclusionRegistry::IsExcluded(nullptr, MAP_ID,
                                                       DcBlackwingLair::NPC_FIREMAW,
                                                       /*forTank*/ true));

    // ...and the map gate still holds for the new rows.
    EXPECT_TRUE(DcTargetExclusionRegistry::HasRowsFor(MAP_ID));
    EXPECT_FALSE(DcTargetExclusionRegistry::HasRowsFor(469u + 1u));
}

// --- the brake that does not go through the raid icon ---------------------

TEST(DungeonEventBlackwingLairTest, HoldFireOutranksTheRaidStrategyAndYieldsToPositioning)
{
    // The rung has to beat what actually points the raid's damage during phase 1,
    // which is not the DC ladder: `bwl razorgore mark boss` sits at ACTION_RAID+1
    // (61) and paints the moon icon on Razorgore, and both stock DPS pickers
    // return the icon's unit BEFORE the target-exclusion pass runs. So something
    // above 61 has to take the target back off the DPS.
    EXPECT_GT(DcRel::HoldFire, 61.0f);

    // ...and yield to both BWL positioning rungs. A bot in the wrong place is the
    // more urgent problem, and letting go of a target it is no longer shooting at
    // costs nothing to defer by a tick.
    EXPECT_LT(DcRel::HoldFire, DcRel::RazorgoreCamp);
    EXPECT_LT(DcRel::HoldFire, DcRel::RazorgoreOrb);

    // Below the phantom-combat hatch, like everything else in this band: when
    // nothing is fightable at all, that rung has to win.
    EXPECT_LT(DcRel::HoldFire, DcRel::BreakStuckCombat);
}

TEST(DungeonEventBlackwingLairTest, TheExclusionRowCoversTheWholeEggPhaseNotJustTheGap)
{
    // The user-facing contract: Razorgore is killed once the eggs are gone, and
    // not before. The row's window is therefore the PHASE — DATA_EGG_EVENT != DONE
    // — and not "while somebody holds the possession". Between one mind control
    // breaking and the next runner reaching the ledge there is a gap of ten to
    // fifteen seconds in which Razorgore is loose, in combat, and being tanked;
    // that gap is exactly when a window-scoped guard would open and exactly when
    // the raid would kill him.
    //
    // Nothing in this file can evaluate the row's live predicate (it reads an
    // InstanceScript), so what is pinned here is the shape: the row exists, it is
    // his map and his entry, and it is windowed rather than permanent — a
    // permanent row would block the phase-2 kill, which is the objective.
    EXPECT_TRUE(DcTargetExclusionRegistry::HasRowsFor(MAP_ID));
    EXPECT_FALSE(DcTargetExclusionRegistry::IsExcluded(nullptr, MAP_ID, NPC_RAZORGORE))
        << "the row must carry a live gate, not a flat always-on: a permanent bar "
           "would also block the phase-2 kill this whole encounter is aiming at";
}

// --- shortening the gap between windows -----------------------------------

TEST(DungeonEventBlackwingLairTest, TheElectionBreaksTiesOnTheWalkToTheOrb)
{
    // Every second between one possession ending and the next beginning is a
    // second of a freed Razorgore, and the new runner's WALK is nearly all of it.
    // Among candidates of equal rank the election therefore takes the one already
    // closest to the ledge.
    DcRazorgore::RunnerCandidate near = Bot(90);  near.isRanged = true;  near.distToOrb = 5.0f;
    DcRazorgore::RunnerCandidate far = Bot(10);   far.isRanged = true;   far.distToOrb = 70.0f;
    EXPECT_EQ(DcRazorgore::SelectRunner({near, far}), 0)
        << "the lower GUID is 70yd away; distance decides before GUID does";
    EXPECT_EQ(DcRazorgore::SelectRunner({far, near}), 1);

    // RANK STILL WINS OUTRIGHT. A healer standing on the orb does not displace a
    // ranged DPS across the room: losing the healer to a 90s root is how the raid
    // dies to the add wave, and thirty yards of walking is the cheaper price.
    DcRazorgore::RunnerCandidate healerAtOrb = Bot(1);
    healerAtOrb.isHealer = true;
    healerAtOrb.distToOrb = 0.0f;
    EXPECT_EQ(DcRazorgore::SelectRunner({healerAtOrb, far}), 1);

    // And the bucket is coarse on purpose: two bots standing together must not
    // trade the job back and forth as they shuffle, so inside one bucket the GUID
    // decides and the answer is stable.
    DcRazorgore::RunnerCandidate a = Bot(70); a.isRanged = true; a.distToOrb = 1.0f;
    DcRazorgore::RunnerCandidate b = Bot(12); b.isRanged = true; b.distToOrb = 9.0f;
    EXPECT_EQ(DcRazorgore::SelectRunner({a, b}), 1);
    EXPECT_EQ(DcRazorgore::SelectRunner({b, a}), 0);
}


// --- Vaelastrasz the Corrupt: the raid starts the encounter ----------------

TEST(DungeonEventBlackwingLairTest, VaelastraszRouseIsAPlainOutOfCombatGossip)
{
    DungeonEvent const* ev = DungeonEventRegistry::Find(MAP_ID, EVENT_VAELASTRASZ_ROUSE);
    ASSERT_NE(ev, nullptr) << "Blackwing Lair (469) event 2 (rouse Vaelastrasz) is missing";

    EXPECT_EQ(ev->activation, EventActivation::Conditional);
    EXPECT_TRUE(static_cast<bool>(ev->condition))
        << "the muster gate predicate must be bound, or the raid never talks to him";

    // NONE of the Razorgore flags. This event runs BETWEEN fights, on the
    // ordinary out-of-combat rung: Vaelastrasz is friendly and passive until his
    // intro ends, and the instance only flips his encounter to IN_PROGRESS when
    // he engages — so there is no combat to drive in, no stand-down to be exempt
    // from, and no movement of our own to protect.
    EXPECT_FALSE(ev->drivesInCombat)
        << "nothing is fighting: taking combat ticks here would only starve the "
           "combat engine during the ~63s intro";
    EXPECT_FALSE(ev->encounterActive)
        << "EncounterActive is the stand-down exemption; there is no live encounter "
           "before the gossip, and after it the fight belongs to the raid strategy";
    EXPECT_FALSE(ev->stepsOwnMovement)
        << "the Gossip step's own walk-in is a plain MoveTo the at-objective hold "
           "does not cancel";

    // REQUIRED: an undrivable gossip is a dead run, and the human should be told
    // rather than left watching forty bots stand in front of a sleeping dragon.
    EXPECT_TRUE(ev->required);
    EXPECT_FALSE(ev->repeatable)
        << "an unfinished step list is never latched, so a missed click simply "
           "re-fires next tick — repeatability would buy nothing and could re-run "
           "a gossip that has already been answered";

    // ONE step, and it is the arrival step that keeps this off the Stratholme #5
    // "latched complete from across the map" lint.
    ASSERT_EQ(ev->steps.size(), 1u);
    EXPECT_EQ(ev->steps[0].kind, EventStepKind::Gossip);
    EXPECT_EQ(ev->steps[0].creatureEntry, NPC_VAELASTRASZ);
    EXPECT_EQ(ev->steps[0].gossipOption, VAEL_GOSSIP_OPTION);
    EXPECT_FALSE(ev->steps[0].skipIfMissing)
        << "a missing Vaelastrasz is not an optional NPC to walk past — it is the "
           "boss, and the predicate already refuses to fire without him";
}

TEST(DungeonEventBlackwingLairTest, VaelastraszRouseNeverClaimsToSummonHim)
{
    DungeonEvent const* ev = DungeonEventRegistry::Find(MAP_ID, EVENT_VAELASTRASZ_ROUSE);
    ASSERT_NE(ev, nullptr);

    // panelGatesBossEntry reads like panel cosmetics and is not:
    // DcTargeting::HasPendingSummonEvent keys the "the party must SUMMON this
    // boss" hold off it, and IsHoldingForSummonEvent then stands the whole pull
    // pipeline down within 80yd of the anchor. On map 469 that radius is the
    // entire Razorgore -> Vaelastrasz corridor (four Death Talon packs and seven
    // Blackwing Warlocks spawn 25-51yd from him), and Vaelastrasz is a world
    // spawn nobody summons — so the hold would be pure regression. The sort hint
    // must be the cosmetic-only one.
    EXPECT_EQ(ev->panelGatesBossEntry, 0u)
        << "setting this would suppress the dynamic pull for the whole approach";
    EXPECT_EQ(ev->panelSortAfterBossEntry, NPC_RAZORGORE)
        << "the rouse belongs between Razorgore and Vaelastrasz in the panel";
}

TEST(DungeonEventBlackwingLairTest, VaelastraszAuthoredIdsMatchTheWorldData)
{
    // creature_template 13020 / creature guid 84512, read off the live world DB.
    EXPECT_EQ(NPC_VAELASTRASZ, 13020u);
    EXPECT_FLOAT_EQ(VAEL_X, -7483.79f);
    EXPECT_FLOAT_EQ(VAEL_Y, -1015.99f);
    EXPECT_FLOAT_EQ(VAEL_Z, 408.652f);

    // The gossip chain is 21333 -> 21334 -> 21332, every level offering exactly
    // one option at OptionID 0, and boss_vaelastrasz answers the 21334 one.
    // SelectGossip drills submenus by selecting option 0 repeatedly, so a single
    // authored 0 walks the whole chain.
    EXPECT_EQ(VAEL_GOSSIP_OPTION, 0);

    // The due range has to cover the boss standoff the approach parks the tank
    // at (the Gossip step walks the last yards in itself) without reaching back
    // into Razorgore's chamber, which is ~114yd away.
    EXPECT_GT(VAEL_DUE_RANGE, 40.0f);
    EXPECT_LT(VAEL_DUE_RANGE, 114.0f);
    // ...and the grid scan must comfortably outreach the due range, or the
    // predicate could be due at a distance the scan cannot resolve him from.
    EXPECT_GT(VAEL_SCAN, VAEL_DUE_RANGE);

    // Per-map event ids: Razorgore's orb is 1, the rouse is 2.
    EXPECT_EQ(EVENT_RAZORGORE_ORB, 1u);
    EXPECT_EQ(EVENT_VAELASTRASZ_ROUSE, 2u);
    EXPECT_NE(EVENT_VAELASTRASZ_ROUSE, EVENT_RAZORGORE_ORB);
}

TEST(DungeonEventBlackwingLairTest, VaelastraszStaysWhereHeIsInTheRoster)
{
    // The rouse deliberately adds NO anchor. Vaelastrasz carries a real
    // kill-credit row, so BossSpawnIndex derives him and the ordinary pipeline
    // already walks the raid to him, stands it off and MUSTERS it — and the
    // muster is exactly what the rouse waits on. A separate objective anchor
    // would have gained the walk and LOST the muster (the raid pre-boss gate
    // arms at Boss anchors only), which is the one thing this feature is for.
    std::vector<DungeonBossInfo> base;
    uint32 const entries[] = {12435, 13020, 12017, 11983, 14601, 11981, 14020, 11583};
    for (uint32 i = 0; i < 8; ++i)
    {
        DungeonBossInfo b;
        b.entry = entries[i];
        b.encounterIndex = i;
        b.mapId = MAP_ID;
        base.push_back(b);
    }

    auto const out = BossRosterRegistry::Apply(MAP_ID, DcDiffKey::Raid(0), base);
    // Grethok plus the eight statics — the rouse adds nothing.
    ASSERT_EQ(out.size(), 9u);

    auto const vael = std::find_if(out.begin(), out.end(), [](DungeonBossInfo const& b)
                                   { return b.entry == NPC_VAELASTRASZ; });
    ASSERT_NE(vael, out.end());
    EXPECT_EQ(vael->kind, DungeonAnchorKind::Boss)
        << "he must stay a BOSS anchor: that is what arms the raid muster";
    EXPECT_EQ(vael->eventId, 0u)
        << "the rouse is Conditional, not anchored to him";
    EXPECT_EQ(BossOrderKey(*vael), 2u) << "Grethok 0, Razorgore 1, Vaelastrasz 2";
}

// --- the Suppression Rooms transit (Vaelastrasz -> Broodlord) --------------

TEST(DungeonEventBlackwingLairTest, TheTransitIsACombatDriverThatOwnsItsOwnMovement)
{
    DungeonEvent const* ev = DungeonEventRegistry::Find(MAP_ID, EVENT_SUPPRESSION_TRANSIT);
    ASSERT_NE(ev, nullptr) << "Blackwing Lair (469) event 3 (suppression transit) is missing";

    EXPECT_EQ(ev->activation, EventActivation::Conditional);
    EXPECT_TRUE(static_cast<bool>(ev->condition)) << "the corridor gate predicate must be bound";

    // THE flag, and the reason the leg needs an event at all. A hundred whelps
    // inside 20yd of the route on a 30s respawn means AnyPartyEngagement is true
    // essentially without a break — DcCombatFlag::MayDrive is therefore false, and
    // Advance lives only in the NON-combat engine. Drop this and the clear does
    // not cross the leg slowly; it does not cross it at all.
    EXPECT_TRUE(ev->drivesInCombat)
        << "the transit must DrivesInCombat: with the whelps up there are no "
           "out-of-combat ticks to drive it on";

    // The driver walks the leader down the route on its own long-range splines,
    // and the per-tick objective hold runs BEFORE the hook — so without this each
    // spline is cancelled the tick after it is issued and the raid creeps a tick
    // at a time down 342yd while every log line reports a healthy issue.
    EXPECT_TRUE(ev->stepsOwnMovement) << "the transit must StepsOwnMovement";

    // The crossing is not a thing that completes once: the condition going false
    // (the leader reaches the standoff, leaves the corridor, or Broodlord dies) is
    // the only "done", and a leader shoved back into the rooms has to re-arm.
    EXPECT_TRUE(ev->repeatable) << "the transit must be Repeatable";

    // On this leg a combat "gap" is one whelp wave dying, several times a minute.
    // A non-persistent event would rewind its step list on each of them.
    EXPECT_TRUE(ev->persistent) << "the transit must be Persistent";

    // Deliberately NOT optional: every hold is watchdog-bounded from inside, so
    // the step timeout can only fire when a watchdog itself failed to release —
    // and skipping quietly at that point hands the leg back to a clear that
    // provably cannot cross it.
    EXPECT_TRUE(ev->required)
        << "the transit must NOT be Optional — a timeout here is a broken watchdog, "
           "and the human should hear about it";

    // ...and NOT EncounterActive: this is the leg BETWEEN two encounters, which is
    // exactly where DC is supposed to work. Claiming the exemption would let it
    // run inside Broodlord's own fight.
    EXPECT_FALSE(ev->encounterActive)
        << "no encounter is in progress on this leg; the stand-down exemption is not ours";

    // ONE Custom step: what the leg needs is a preference re-decided every tick
    // (walk / hold for the pack / hold for an elite / hold for the disarm rung),
    // not a sequence. The staging hop and the gather gate are the controller's
    // first two states, not two steps that happen to come first.
    ASSERT_EQ(ev->steps.size(), 1u);
    EXPECT_EQ(ev->steps[0].kind, EventStepKind::Custom);
    EXPECT_EQ(ev->steps[0].hookId, HOOK_SUPPRESSION_TRANSIT);
    EXPECT_EQ(ev->steps[0].timeoutMs, TRANSIT_TIMEOUT_MS);
    EXPECT_TRUE(ObjectiveHookRegistry::Has(HOOK_SUPPRESSION_TRANSIT))
        << "hook " << HOOK_SUPPRESSION_TRANSIT
        << " (DriveSuppressionTransit) must be registered";

    // Hook ids are one flat space across every dungeon; a collision silently drops
    // one dungeon's driver.
    EXPECT_NE(HOOK_SUPPRESSION_TRANSIT, HOOK_RAZORGORE_ORB);
    EXPECT_NE(EVENT_SUPPRESSION_TRANSIT, EVENT_RAZORGORE_ORB);
    EXPECT_NE(EVENT_SUPPRESSION_TRANSIT, EVENT_VAELASTRASZ_ROUSE);
}

TEST(DungeonEventBlackwingLairTest, TheTransitAuthoredIdsMatchTheWorldData)
{
    EXPECT_EQ(NPC_BROODLORD_LASHLAYER, 12017u);
    // instance_blackwing_lair's own BWLEncounter enum, which GetBossState is keyed
    // on — NOT the roster order, which puts Grethok in front of everything.
    EXPECT_EQ(BROODLORD_ENCOUNTER_INDEX, 2u);

    // The four Corrupted Whelps and the two elites, as the never-target rows and
    // the driver's elite scan read them.
    EXPECT_EQ(NPC_CORRUPTED_RED_WHELP, 14022u);
    EXPECT_EQ(NPC_CORRUPTED_GREEN_WHELP, 14023u);
    EXPECT_EQ(NPC_CORRUPTED_BLUE_WHELP, 14024u);
    EXPECT_EQ(NPC_CORRUPTED_BRONZE_WHELP, 14025u);
    EXPECT_EQ(NPC_BLACKWING_TASKMASTER, 12458u);
    EXPECT_EQ(NPC_DEATH_TALON_HATCHER, 12468u);

    // go_suppression_device — the GO the bot disarm rung turns off.
    EXPECT_EQ(GO_SUPPRESSION_DEVICE, 179784u);
}

// The corridor box is the transit's real gate: one axis-aligned test standing
// between a rung on every bot's combat engine and the other seven encounters of
// this raid. Both ends of the crossing must be inside it, and the two encounters
// immediately behind the gauntlet must not be.
TEST(DungeonEventBlackwingLairTest, TheTransitCorridorHoldsTheLegAndNothingElse)
{
    auto inside = [](float x, float y, float z)
    {
        return x >= TRANSIT_BOX_MIN_X && x <= TRANSIT_BOX_MAX_X &&
               y >= TRANSIT_BOX_MIN_Y && y <= TRANSIT_BOX_MAX_Y &&
               z >= TRANSIT_BOX_MIN_Z && z <= TRANSIT_BOX_MAX_Z;
    };

    EXPECT_TRUE(inside(TRANSIT_STAGE_X, TRANSIT_STAGE_Y, TRANSIT_STAGE_Z));
    EXPECT_TRUE(inside(TRANSIT_END_X, TRANSIT_END_Y, TRANSIT_END_Z));

    EXPECT_FALSE(inside(VAEL_X, VAEL_Y, VAEL_Z)) << "Vaelastrasz";
    EXPECT_FALSE(inside(ORB_X, ORB_Y, ORB_Z)) << "the Orb of Domination";
    EXPECT_FALSE(inside(CAMP_X, CAMP_Y, CAMP_Z)) << "the Razorgore camp";
    EXPECT_FALSE(inside(GRETHOK_X, GRETHOK_Y, GRETHOK_Z)) << "Grethok's platform";
}

// The three watchdogs bound three different KINDS of wait. Collapsing them onto
// one number would either release the disarm hold before mod-playerbots' rung had
// a tick, or park the leg behind a wedged straggler for the whole elite budget.
TEST(DungeonEventBlackwingLairTest, TheTransitHoldBudgetsAreOrderedByWhatTheyWaitFor)
{
    EXPECT_LT(TRANSIT_DISARM_HOLD_TIMEOUT_MS, TRANSIT_PACK_HOLD_TIMEOUT_MS);
    EXPECT_LT(TRANSIT_PACK_HOLD_TIMEOUT_MS, TRANSIT_ELITE_HOLD_TIMEOUT_MS);
    // ...and every one of them well inside the whole crossing's bound, or the
    // step timeout would fire before the watchdog it exists to backstop.
    EXPECT_LT(TRANSIT_ELITE_HOLD_TIMEOUT_MS, TRANSIT_TIMEOUT_MS);

    // The hold margin is the pack leash's hysteresis and has to stay well under
    // the leash's own clamp floor (10) — a margin that reached the leash would put
    // the hold point back at the cursor and bring the cross-the-pack lap with it.
    EXPECT_GT(TRANSIT_PACK_HOLD_MARGIN, 0.0f);
    EXPECT_LT(TRANSIT_PACK_HOLD_MARGIN, 10.0f);
}

// The drake hall is stacked DIRECTLY over the Broodlord approach corridor, and
// that is the whole reason the followers' assist picker needs a reachability gate
// rather than a better distance metric (tp-20260828-142623-1: the raid crossed
// from the approach hall into Firemaw's room and never killed Broodlord).
//
// Coordinates are world-DB spawns; if a data update moves them, the justification
// for the gate moves with them and this test should be the thing that says so.
TEST(DungeonEventBlackwingLairTest, TheDrakeHallSitsDirectlyOverTheBroodlordApproach)
{
    // Route anchor 11 of the authored Broodlord approach — where the raid stood.
    constexpr float HALL_X = -7523.75f, HALL_Y = -974.98f, HALL_Z = 424.95f;
    // Blackwing Warlock (12459, guid 84560), on the drake-hall floor above.
    constexpr float WARLOCK_X = -7538.6f, WARLOCK_Y = -983.2f, WARLOCK_Z = 449.4f;
    // Death Talon Seether (12464, guid 84524) — a LEGITIMATE corridor pack, on the
    // approach hall's own floor, and what the party should have been fighting.
    constexpr float SEETHER_X = -7534.2f, SEETHER_Y = -926.7f, SEETHER_Z = 428.0f;

    auto dist2d = [](float ax, float ay, float bx, float by)
    { return std::sqrt((ax - bx) * (ax - bx) + (ay - by) * (ay - by)); };
    auto dist3d = [&](float ax, float ay, float az, float bx, float by, float bz)
    { return std::sqrt(dist2d(ax, ay, bx, by) * dist2d(ax, ay, bx, by) +
                       (az - bz) * (az - bz)); };

    float const warlock2d = dist2d(HALL_X, HALL_Y, WARLOCK_X, WARLOCK_Y);
    float const seether2d = dist2d(HALL_X, HALL_Y, SEETHER_X, SEETHER_Y);
    float const warlock3d =
        dist3d(HALL_X, HALL_Y, HALL_Z, WARLOCK_X, WARLOCK_Y, WARLOCK_Z);
    float const seether3d =
        dist3d(HALL_X, HALL_Y, HALL_Z, SEETHER_X, SEETHER_Y, SEETHER_Z);

    // Straight overhead: almost all of the separation is vertical.
    EXPECT_LT(warlock2d, 20.0f) << "the Warlock is nearly on top of the corridor in plan view";
    EXPECT_GT(WARLOCK_Z - HALL_Z, 20.0f) << "...and a full floor above it";

    // THE POINT: a distance metric cannot separate these. The mob a floor up is
    // nearer than the pack on our own floor in 2D *and* in 3D, so switching the
    // assist rank from GetExactDist2d to GetExactDist would not have prevented the
    // ceiling walk. Only reachability tells them apart.
    EXPECT_LT(warlock2d, seether2d) << "2D rank prefers the mob upstairs";
    EXPECT_LT(warlock3d, seether3d) << "and so does 3D — the metric is not the fix";

    // The gate does engage here: IsLevelReachable only short-circuits inside
    // DC_Z_LEVEL_TOLERANCE, and this is five times that, so the probe runs and the
    // detour bound gets its say.
    EXPECT_GT(std::fabs(WARLOCK_Z - HALL_Z), DC_Z_LEVEL_TOLERANCE);

    // ...and it says no by a wide margin. The real walk from the approach hall to
    // the drake hall is the whole authored route — down the switchback, through
    // both suppression rooms, over the Taskmaster ramp — against a bound of
    // max(straight * 2, straight + 20) on a ~30yd straight line.
    float const bound =
        DcDetourBound(warlock3d, DC_TRASH_DETOUR_RATIO, DC_TRASH_DETOUR_SLACK);
    EXPECT_LT(bound, 65.0f) << "a 30yd straight line buys at most ~60yd of detour";

    std::vector<WaypointHint> const* route = DungeonClearRouteRegistry::Get(
        MAP_ID, DUNGEON_DIFFICULTY_NORMAL, NPC_BROODLORD_LASHLAYER);
    ASSERT_NE(route, nullptr) << "the Broodlord approach route must be registered";
    ASSERT_GT(route->size(), 1u);
    float routeLen = 0.0f;
    for (size_t i = 1; i < route->size(); ++i)
        routeLen += dist3d((*route)[i - 1].x, (*route)[i - 1].y, (*route)[i - 1].z,
                           (*route)[i].x, (*route)[i].y, (*route)[i].z);
    EXPECT_GT(routeLen, 4.0f * bound)
        << "the walkable route between these two floors dwarfs the detour bound";
}

// ---------------------------------------------------------------------------
// The NO_STOP span over the approach hall (tp-20260828-175353-1).
//
// The hall from Vaelastrasz to the staging shelf runs ~24yd UNDER the upper
// suppression room, and the trash up there aggros the raid through the floor,
// cannot path down, and evades where it stands. The party cannot fight its way
// out of that and must not try; the route says so with AnchorFlag::NO_STOP, and
// these tests pin the span against the coordinates the five failed raids
// actually died on, so a re-author cannot quietly drop the cover.
// ---------------------------------------------------------------------------

namespace
{
    // Where each of the five raids of tp-20260828-175353-1 was standing when the
    // operator gave up on it. Every one must be inside the span.
    struct StallSite { char const* run; float x, y, z; };
    constexpr StallSite BWL_STALL_SITES[] = {
        { "tr-20260828-175358-1", -7531.9f, -972.3f, 425.0f },
        { "tr-20260828-175358-2", -7544.6f, -949.0f, 428.0f },
        { "tr-20260828-175358-3", -7562.9f, -945.0f, 428.1f },
        { "tr-20260828-175358-4", -7560.6f, -942.5f, 428.1f },
        { "tr-20260828-175358-5", -7559.8f, -939.1f, 428.1f },
    };

    float NoStopDist3d(float ax, float ay, float az, float bx, float by, float bz)
    {
        float const dx = ax - bx, dy = ay - by, dz = az - bz;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    std::vector<WaypointHint> const& BroodlordRoute()
    {
        std::vector<WaypointHint> const* route = DungeonClearRouteRegistry::Get(
            MAP_ID, DUNGEON_DIFFICULTY_NORMAL, NPC_BROODLORD_LASHLAYER);
        EXPECT_NE(route, nullptr) << "the Broodlord approach route must be registered";
        static std::vector<WaypointHint> const empty;
        return route ? *route : empty;
    }
}

TEST(DungeonEventBlackwingLairTest, TheApproachHallCarriesANoStopSpan)
{
    std::vector<WaypointHint> const& route = BroodlordRoute();
    ASSERT_GT(route.size(), 13u);

    size_t flagged = 0;
    size_t firstFlagged = route.size();
    size_t lastFlagged = 0;
    for (size_t i = 0; i < route.size(); ++i)
        if (HasFlag(route[i].flags, AnchorFlag::NO_STOP))
        {
            ++flagged;
            firstFlagged = std::min(firstFlagged, i);
            lastFlagged = std::max(lastFlagged, i);
        }

    EXPECT_GT(flagged, 0u) << "the overhead-exposed hall must be flagged NO_STOP";

    // The span is CONTIGUOUS. A hole in the middle is worse than no span at all:
    // the pull system would re-arm inside the exposed band, plant a camp there,
    // and the drag would pull the raid back into the hole it just crossed.
    for (size_t i = firstFlagged; i <= lastFlagged; ++i)
        EXPECT_TRUE(HasFlag(route[i].flags, AnchorFlag::NO_STOP))
            << "anchor " << i << " punches a hole in the NO_STOP span";

    // It lives entirely in the APPROACH half. The crossing half (anchor 20 on) is
    // the suppression transit's business and is sliced away from this route by
    // BwlTransitRoute; a NO_STOP flag there would be a second, silent driver.
    EXPECT_LT(lastFlagged, static_cast<size_t>(DcBlackwingLair::TRANSIT_STAGE_ANCHOR_INDEX))
        << "NO_STOP must not reach into the transit's half of the route";
}

TEST(DungeonEventBlackwingLairTest, TheNoStopSpanCoversWhereEveryFailedRaidDied)
{
    std::vector<WaypointHint> const& route = BroodlordRoute();
    ASSERT_FALSE(route.empty());

    for (StallSite const& s : BWL_STALL_SITES)
        EXPECT_TRUE(DcNoStopZone::CoversPoint(route, s.x, s.y, s.z, DC_NO_STOP_CORRIDOR))
            << s.run << " stalled at (" << s.x << ", " << s.y << ", " << s.z
            << ") — the span that exists to stop that must cover it";
}

TEST(DungeonEventBlackwingLairTest, TheNoStopSpanDoesNotCageTheRestOfTheLeg)
{
    std::vector<WaypointHint> const& route = BroodlordRoute();
    ASSERT_GT(route.size(), static_cast<size_t>(DcBlackwingLair::TRANSIT_STAGE_ANCHOR_INDEX));

    // A zone that never releases is a run that never pulls anything again. The
    // Death Talon pack (12463/12464/12465) sits on the party's OWN floor from
    // anchor 14 on and is a perfectly good pull — the span must be off by then.
    EXPECT_FALSE(DcNoStopZone::CoversPoint(route, route[14].x, route[14].y,
                                           route[14].z, DC_NO_STOP_CORRIDOR))
        << "the same-floor pack at anchors 14+ must still be pullable";

    // And the staging shelf, where the suppression transit's own gather happens.
    size_t const stage = static_cast<size_t>(DcBlackwingLair::TRANSIT_STAGE_ANCHOR_INDEX);
    EXPECT_FALSE(DcNoStopZone::CoversPoint(route, route[stage].x, route[stage].y,
                                           route[stage].z, DC_NO_STOP_CORRIDOR))
        << "staging is clean ground — the span must have released well before it";

    // Vaelastrasz's chamber, behind the switchback, is ordinary clearing ground.
    EXPECT_FALSE(DcNoStopZone::CoversPoint(route, route[1].x, route[1].y,
                                           route[1].z, DC_NO_STOP_CORRIDOR))
        << "the chamber the raid arrives from is not part of the crossing";
}

TEST(DungeonEventBlackwingLairTest, NoStopCoverageIsACapsuleNotAnAnchorSphere)
{
    std::vector<WaypointHint> const& route = BroodlordRoute();
    ASSERT_GT(route.size(), 12u);

    // The midpoint of a flagged leg is the point furthest from both its anchors —
    // an anchor-radius test would leave it uncovered on legs this long, which is
    // exactly where run 1 was standing. Assert the geometry is segment-based by
    // checking a midpoint that no anchor is within DC_NO_STOP_CORRIDOR of.
    WaypointHint const& a = route[11];
    WaypointHint const& b = route[12];
    float const mx = (a.x + b.x) * 0.5f, my = (a.y + b.y) * 0.5f, mz = (a.z + b.z) * 0.5f;

    float const toA = NoStopDist3d(mx, my, mz, a.x, a.y, a.z);
    ASSERT_GT(toA, DC_NO_STOP_CORRIDOR * 0.5f)
        << "pick a longer leg — this one cannot distinguish the two shapes";

    EXPECT_TRUE(DcNoStopZone::CoversPoint(route, mx, my, mz, DC_NO_STOP_CORRIDOR))
        << "mid-leg must be inside the zone, not just the anchors";

    // Straight up through the ceiling is NOT in the zone: the span is about the
    // ground the party walks, and the floor above it is where the hostiles are.
    EXPECT_FALSE(DcNoStopZone::CoversPoint(route, mx, my, mz + 24.3f,
                                           DC_NO_STOP_CORRIDOR))
        << "the upper suppression room is not part of the approach span";
}

TEST(DungeonEventBlackwingLairTest, DistToSegmentClampsToTheEndpoints)
{
    // The projection has to clamp, or a point off the END of a leg reads as
    // near-zero distance to the infinite line and the zone leaks arbitrarily far
    // in both directions along the corridor.
    WaypointHint const a{ 0.0f, 0.0f, 0.0f };
    WaypointHint const b{ 10.0f, 0.0f, 0.0f };

    EXPECT_NEAR(DcNoStopZone::DistSqToSegment(5.0f, 3.0f, 0.0f, a, b), 9.0f, 0.01f);
    EXPECT_NEAR(DcNoStopZone::DistSqToSegment(-4.0f, 0.0f, 0.0f, a, b), 16.0f, 0.01f);
    EXPECT_NEAR(DcNoStopZone::DistSqToSegment(17.0f, 0.0f, 0.0f, a, b), 49.0f, 0.01f);

    // A degenerate leg (duplicate anchors) must not divide by zero.
    EXPECT_NEAR(DcNoStopZone::DistSqToSegment(3.0f, 4.0f, 0.0f, a, a), 25.0f, 0.01f);
}

// --- Chromaggus' cage (the lever) -----------------------------------------

TEST(DungeonEventBlackwingLairTest, ChromaggusCageIsASingleReportUseClick)
{
    DungeonEvent const* ev = DungeonEventRegistry::Find(MAP_ID, EVENT_CHROMAGGUS_CAGE);
    ASSERT_NE(ev, nullptr) << "Blackwing Lair (469) event 4 (open the cage) is missing";

    EXPECT_EQ(ev->activation, EventActivation::Conditional);
    EXPECT_TRUE(static_cast<bool>(ev->condition))
        << "the muster gate predicate must be bound, or the raid never pulls the lever";

    // The rouse's flag set, for the rouse's reasons: the party is out of combat,
    // no encounter is in progress (the instance only flips DATA_CHROMAGGUS when
    // he engages), and the step's walk-in is an ordinary MoveTo.
    EXPECT_FALSE(ev->drivesInCombat);
    EXPECT_FALSE(ev->encounterActive);
    EXPECT_FALSE(ev->stepsOwnMovement);
    EXPECT_TRUE(ev->required);
    EXPECT_FALSE(ev->repeatable)
        << "the lever is a permanent one-way latch; after a wipe the cage is open "
           "and the raid re-pulls him the ordinary way";

    ASSERT_EQ(ev->steps.size(), 1u);
    EventStep const& click = ev->steps[0];
    EXPECT_EQ(click.kind, EventStepKind::UseGameObject);
    EXPECT_EQ(click.goEntry, GO_CHROMAGGUS_LEVER);

    // THE flag. GameObject::Use() passes reportUse=false, and
    // go_chromaggus_lever's GossipHello opens the portcullis only under
    // reportUse=true while stamping itself spent (NOT_SELECTABLE | IN_USE,
    // GO_STATE_ACTIVE) either way. A plain Use() therefore burns the lever
    // without freeing Chromaggus — an immune boss behind a shut door with
    // nothing left to click.
    EXPECT_TRUE(click.reportUse)
        << "a plain Use() would spend the lever without opening the cage";

    // The search radius has to reach the lever from anywhere the due range
    // admits; the step's own 20yd default would never see it from the standoff.
    EXPECT_FLOAT_EQ(click.radius, CHROMA_LEVER_SEARCH);
    EXPECT_GE(CHROMA_LEVER_SEARCH, CHROMA_DUE_RANGE);
}

TEST(DungeonEventBlackwingLairTest, ChromaggusCageNeverClaimsToSummonHim)
{
    DungeonEvent const* ev = DungeonEventRegistry::Find(MAP_ID, EVENT_CHROMAGGUS_CAGE);
    ASSERT_NE(ev, nullptr);

    // Same trap as the rouse: panelGatesBossEntry is not cosmetic —
    // DcTargeting::HasPendingSummonEvent keys the "must be SUMMONED" hold off it
    // and IsHoldingForSummonEvent stands the pull down within 80yd. Chromaggus is
    // a world spawn sitting in his cage from map load, and his chamber still has
    // Death Talon packs to clear.
    EXPECT_EQ(ev->panelGatesBossEntry, 0u)
        << "setting this would suppress the dynamic pull across his whole chamber";
    EXPECT_EQ(ev->panelSortAfterBossEntry, NPC_FLAMEGOR)
        << "the cage belongs between Flamegor and Chromaggus in the panel";
}

TEST(DungeonEventBlackwingLairTest, ChromaggusAuthoredIdsMatchTheWorldData)
{
    // gameobject guid 56161 / entry 179148 ('Lever', ScriptName
    // go_chromaggus_lever) and guid 75161 / entry 179116 (the cage portcullis),
    // read off the live world DB.
    EXPECT_EQ(GO_CHROMAGGUS_LEVER, 179148u);
    EXPECT_EQ(NPC_CHROMAGGUS_CAGE_DOOR, 179116u);
    EXPECT_EQ(NPC_CHROMAGGUS, 14020u);
    EXPECT_FLOAT_EQ(CHROMA_LEVER_X, -7510.98f);
    EXPECT_FLOAT_EQ(CHROMA_LEVER_Y, -1094.69f);
    EXPECT_FLOAT_EQ(CHROMA_LEVER_Z, 476.555f);

    // boss_chromaggus::homePos — where the lever's waypoint walk puts him.
    EXPECT_FLOAT_EQ(CHROMA_HOME_X, -7491.1587f);
    EXPECT_FLOAT_EQ(CHROMA_HOME_Y, -1069.718f);
    EXPECT_FLOAT_EQ(CHROMA_HOME_Z, 476.59094f);

    // instance_blackwing_lair's DATA_CHROMAGGUS, and the DBC bit it coincides
    // with on this map.
    EXPECT_EQ(CHROMAGGUS_ENCOUNTER_INDEX, 6u);

    // Per-map event ids stay unique.
    EXPECT_EQ(EVENT_CHROMAGGUS_CAGE, 4u);
    EXPECT_NE(EVENT_CHROMAGGUS_CAGE, EVENT_RAZORGORE_ORB);
    EXPECT_NE(EVENT_CHROMAGGUS_CAGE, EVENT_VAELASTRASZ_ROUSE);
    EXPECT_NE(EVENT_CHROMAGGUS_CAGE, EVENT_SUPPRESSION_TRANSIT);
    EXPECT_NE(EVENT_CHROMAGGUS_CAGE, EVENT_NEFARIAN_START);

    // The due range is measured from the LEVER and has to contain the anchor the
    // raid musters at (31.9yd away) with the muster spread on top, while stopping
    // well short of the drake hall behind it (Flamegor is ~130yd back).
    float const leverToHome = std::hypot(CHROMA_LEVER_X - CHROMA_HOME_X,
                                         CHROMA_LEVER_Y - CHROMA_HOME_Y);
    EXPECT_NEAR(leverToHome, 31.9f, 1.0f);
    EXPECT_GT(CHROMA_DUE_RANGE, leverToHome + 40.0f);
    EXPECT_LT(CHROMA_DUE_RANGE, 130.0f);
    // ...and the creature scan must outreach the cage from anywhere in it.
    EXPECT_GT(CHROMA_SCAN, CHROMA_DUE_RANGE * 0.5f);

    // THE FLOOR BAND. A 90yd 2D radius around the lever also covers the Broodlord
    // floor 27yd below it — the Suppression Rooms corridor passes within 36yd
    // (2D) — so the band must be tight enough to exclude z 449 while still
    // admitting the whole chamber (the lever at 476.6, the cage door sill at
    // 480.0 and Chromaggus' own spawn at 476.7).
    EXPECT_LT(CHROMA_FLOOR_BAND, std::fabs(CHROMA_FLOOR_Z - 449.3f));
    EXPECT_GT(CHROMA_FLOOR_BAND, std::fabs(CHROMA_FLOOR_Z - 480.03f));
    EXPECT_LT(std::fabs(CHROMA_LEVER_Z - CHROMA_FLOOR_Z), CHROMA_FLOOR_BAND);
}

TEST(DungeonEventBlackwingLairTest, ChromaggusIsAnchoredWhereTheLeverSendsHimNotInTheCage)
{
    // His DB spawn is a holding pen sealed behind the portcullis, sitting
    // directly above the z-449 Broodlord floor; boss_chromaggus::homePos is where
    // his scripted walk-out ends and where the fight happens. The advance routes
    // at the LIVE creature once his grid is up, so this governs the far approach
    // and every panel/diag distance — which is exactly the reading that must not
    // aim a 130yd walk at a point inside a cage on the wrong floor.
    std::vector<DungeonBossInfo> base;
    uint32 const entries[] = {12435, 13020, 12017, 11983, 14601, 11981, 14020, 11583};
    for (uint32 i = 0; i < 8; ++i)
    {
        DungeonBossInfo b;
        b.entry = entries[i];
        b.encounterIndex = i;
        b.mapId = MAP_ID;
        // The derived coords for Chromaggus are his creature-table spawn.
        if (b.entry == NPC_CHROMAGGUS)
        {
            b.x = -7515.34f;
            b.y = -1029.62f;
            b.z = 476.73f;
        }
        base.push_back(b);
    }

    auto const out = BossRosterRegistry::Apply(MAP_ID, DcDiffKey::Raid(0), base);

    auto const chroma = std::find_if(out.begin(), out.end(), [](DungeonBossInfo const& b)
                                     { return b.entry == NPC_CHROMAGGUS; });
    ASSERT_NE(chroma, out.end());
    EXPECT_EQ(std::count_if(out.begin(), out.end(), [](DungeonBossInfo const& b)
                            { return b.entry == NPC_CHROMAGGUS; }), 1)
        << "remove + re-add, never two anchors for one boss";

    EXPECT_EQ(chroma->kind, DungeonAnchorKind::Boss)
        << "he must stay a BOSS anchor: that is what arms the raid muster the "
           "cage event waits on";
    // The anchor must sit on the CHAMBER floor the due gate admits, not on the
    // cage's or the one below it.
    EXPECT_LT(std::fabs(chroma->z - CHROMA_FLOOR_Z), CHROMA_FLOOR_BAND);
    EXPECT_FLOAT_EQ(chroma->x, CHROMA_HOME_X);
    EXPECT_FLOAT_EQ(chroma->y, CHROMA_HOME_Y);
    EXPECT_FLOAT_EQ(chroma->z, CHROMA_HOME_Z);
    EXPECT_EQ(BossOrderKey(*chroma), 7u) << "Flamegor 6, Chromaggus 7";

    // The re-add must keep his REAL kill-bit — a dropped inheritance would leave
    // encounterIndex 0 and complete him off Razorgore's death.
    EXPECT_EQ(chroma->encounterIndex, 6u);
    EXPECT_EQ(chroma->inheritCompletionFrom, 0u)
        << "the inheritance must be resolved by Apply, not carried into the run";
}

// --- Nefarian (starting the encounter) ------------------------------------

TEST(DungeonEventBlackwingLairTest, NefarianStartIsARepeatableSummonGate)
{
    DungeonEvent const* ev = DungeonEventRegistry::Find(MAP_ID, EVENT_NEFARIAN_START);
    ASSERT_NE(ev, nullptr) << "Blackwing Lair (469) event 5 (start Nefarian) is missing";

    EXPECT_EQ(ev->activation, EventActivation::Conditional);
    EXPECT_TRUE(static_cast<bool>(ev->condition));

    // REPEATABLE carries two jobs here. Victor's gossip flag comes back after a
    // failed attempt (EVENT_RESPAWN_NEFARIUS, 15min), so the raid gets to start
    // him again; and a repeatable conditional event is never latched, which is
    // what keeps DcTargeting::HasPendingSummonEvent answering true.
    EXPECT_TRUE(ev->repeatable)
        << "a latched event would let TryBossNotPresentStall abort the run the "
           "moment the advance reached a Nefarian who does not exist yet";

    // ...and PanelBeforeBoss is what that pending-summon lookup keys on. This is
    // the one row on this map that WANTS the pull suppression it brings: the
    // lair holds no trash, and the same flag is what suppresses the not-spawned
    // stall.
    EXPECT_EQ(ev->panelGatesBossEntry, NPC_NEFARIAN);
    EXPECT_EQ(ev->panelSortAfterBossEntry, 0u);

    EXPECT_FALSE(ev->drivesInCombat)
        << "DC's part ends at the click; the wave fight is the raid strategy's";
    EXPECT_FALSE(ev->encounterActive);
    EXPECT_FALSE(ev->stepsOwnMovement);

    ASSERT_EQ(ev->steps.size(), 1u);
    EventStep const& talk = ev->steps[0];
    EXPECT_EQ(talk.kind, EventStepKind::Gossip);
    EXPECT_EQ(talk.creatureEntry, NPC_VICTOR_NEFARIUS);
    EXPECT_EQ(talk.gossipOption, NEFARIUS_GOSSIP_OPTION);
    EXPECT_FALSE(talk.skipIfMissing)
        << "a missing Victor is not an optional NPC to walk past — he is the "
           "encounter, and the predicate already refuses to fire without him";

    // The step has to ACQUIRE him from the raid anchor, which is 86yd away.
    float const anchorToVictor = std::hypot(NEFARIAN_X - NEFARIUS_X,
                                            NEFARIAN_Y - NEFARIUS_Y);
    EXPECT_NEAR(anchorToVictor, 85.9f, 1.0f);
    EXPECT_GT(talk.radius, anchorToVictor);

    // The due range is a window. Wide enough to arm from the anchor the advance
    // parks the raid at (86yd, plus slack for the at-boss hold's standoff)...
    EXPECT_GT(NEFARIUS_DUE_RANGE, anchorToVictor + 20.0f);
    // ...and short of the lair's entrance portcullis (GO 176966 at
    // (-7488.1, -1150.7, 476.5)), 149yd from Victor: an event due at the door
    // preempts the advance there and marches the tank across the room while the
    // raid is still filing in behind it.
    float const doorToVictor = std::hypot(-7488.1f - NEFARIUS_X,
                                          -1150.7f - NEFARIUS_Y);
    EXPECT_NEAR(doorToVictor, 148.9f, 1.0f);
    EXPECT_LT(NEFARIUS_DUE_RANGE, doorToVictor);
}

TEST(DungeonEventBlackwingLairTest, NefarianAuthoredIdsMatchTheWorldData)
{
    // creature_template 10162 (gossip_menu_id 21330, ScriptName
    // boss_victor_nefarius) / creature guid 85785, read off the live world DB.
    EXPECT_EQ(NPC_VICTOR_NEFARIUS, 10162u);
    EXPECT_EQ(NPC_NEFARIAN, 11583u);
    EXPECT_FLOAT_EQ(NEFARIUS_X, -7587.76f);
    EXPECT_FLOAT_EQ(NEFARIUS_Y, -1261.43f);
    EXPECT_FLOAT_EQ(NEFARIUS_Z, 482.21f);

    // waypoint_data path 11583, final point — where his intro flight lands.
    EXPECT_FLOAT_EQ(NEFARIAN_X, -7502.0f);
    EXPECT_FLOAT_EQ(NEFARIAN_Y, -1256.5f);
    EXPECT_FLOAT_EQ(NEFARIAN_Z, 476.758f);

    // DungeonEncounter.dbc row 617: mapId 469, encounterIndex 7. NOT the
    // instance script's DATA_ enum, which only happens to agree on this map.
    EXPECT_EQ(NEFARIAN_ENCOUNTER_INDEX, 7u);

    // 21330 -> 21331 -> 21332, one option each at OptionID 0, no conditions rows.
    // SelectGossip drills the chain from the single authored 0.
    EXPECT_EQ(NEFARIUS_GOSSIP_OPTION, 0);

    EXPECT_EQ(EVENT_NEFARIAN_START, 5u);
    EXPECT_GT(NEFARIUS_SCAN, NEFARIUS_DUE_RANGE);
}

TEST(DungeonEventBlackwingLairTest, NefarianIsAddedToARosterThatCannotDeriveHim)
{
    // He has a kill-credit row (instance_encounters 617) but NO creature spawn,
    // and BossSpawnIndex walks the spawn table — so the derived list really does
    // stop at Chromaggus and the run would declare itself finished one boss
    // short. This base is the honest one: seven bosses, no Nefarian.
    std::vector<DungeonBossInfo> base;
    uint32 const entries[] = {12435, 13020, 12017, 11983, 14601, 11981, 14020};
    for (uint32 i = 0; i < 7; ++i)
    {
        DungeonBossInfo b;
        b.entry = entries[i];
        b.encounterIndex = i;
        b.mapId = MAP_ID;
        base.push_back(b);
    }

    auto const out = BossRosterRegistry::Apply(MAP_ID, DcDiffKey::Raid(0), base);
    // Grethok + the seven derived + Nefarian.
    ASSERT_EQ(out.size(), 9u);

    auto const nef = std::find_if(out.begin(), out.end(), [](DungeonBossInfo const& b)
                                  { return b.entry == NPC_NEFARIAN; });
    ASSERT_NE(nef, out.end()) << "the clear must not end at Chromaggus";
    EXPECT_EQ(nef->kind, DungeonAnchorKind::Boss);
    EXPECT_EQ(nef->encounterIndex, NEFARIAN_ENCOUNTER_INDEX)
        << "completion rides GetCompletedEncounterMask bit 7, so the bit must be "
           "the real DBC one and not a default 0 that would read as Razorgore";
    EXPECT_EQ(nef->doneBossStateIndex, -1)
        << "he has a real DungeonEncounter row; this is not the boss-state case";
    EXPECT_EQ(nef->inheritCompletionFrom, 0u);
    EXPECT_FLOAT_EQ(nef->x, NEFARIAN_X);
    EXPECT_FLOAT_EQ(nef->y, NEFARIAN_Y);
    EXPECT_FLOAT_EQ(nef->z, NEFARIAN_Z);

    // He sorts LAST, behind Chromaggus.
    EXPECT_EQ(BossOrderKey(*nef), 8u);
    EXPECT_EQ(out.back().entry, NPC_NEFARIAN);
}
