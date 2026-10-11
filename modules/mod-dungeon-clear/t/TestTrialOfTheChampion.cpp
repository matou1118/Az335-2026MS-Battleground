/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

// Trial of the Champion (map 650).
//
// Two halves, the module's usual split:
//
//   1. THE DRIVER KERNEL (DcTocDriver::Decide) — the whole of hook 37's per-tick
//      reasoning, exercised without a map: the muster, the three clicks and their
//      options, the joust's hold-don't-yield rule, the side-pack pulls, the
//      Knight's intro and his evade strand, and the wipe rewinds.
//
//   2. THE AUTHORED DATA — three objectives and no boss rows, the four event rows
//      and their flags, the progress pins hand-copied out of trial_of_the_champion.h,
//      and the glue rows (doors, hazard, exclusion, never-target).
//
// THE MOST IMPORTANT ASSERTIONS are the progress pins and the gossip options.
// Every gate on this map is a comparison against DATA_INSTANCE_PROGRESS, and a
// wrong value is a silent permanent hold; and an option off by one at progress 0
// is the long ninety-second version at best and a dead click at worst.

#include "gtest/gtest.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "Ai/Dungeon/DungeonClear/Data/DcEventDoorRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/DcHazardRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/DcNeverTargetRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/DcTargetExclusionRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonBossInfo.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonEventRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"
#include "Ai/Dungeon/DungeonClear/Overrides/BossRosterRegistry.h"
#include "Ai/Dungeon/DungeonClear/Overrides/ObjectiveHookRegistry.h"
#include "Ai/Dungeon/DungeonClear/Util/DcTocDriverDecision.h"

using namespace DcTrialOfTheChampion;
using DcTocDriver::Action;
using DcTocDriver::State;

namespace
{
    constexpr uint32 NOW = 1'000'000;

    // --- the kernel's baselines -----------------------------------------------
    //
    // Progress 0, a full party of five all mounted for a minute, the announcer
    // flagged and 3yd away, nobody in combat, everyone topped off. Each test then
    // breaks exactly one thing.
    DcTocDriver::Inputs AtClick()
    {
        DcTocDriver::Inputs in;
        in.progress = PROGRESS_INITIAL;
        in.alive = true;
        in.partyInCombat = false;
        in.partyReady = true;
        in.tankMounted = true;
        in.mountedMembers = 5;
        in.partySize = 5;
        in.walkingChampions = 0;
        in.distToPost = 2.0f;
        in.announcerPresent = true;
        in.announcerFlagged = true;
        in.announcerDist = 3.0f;
        in.nowMs = NOW;
        in.mountedSinceMs = NOW - 60'000;
        return in;
    }

    // Progress 6, the champions just "died", the party on foot, the announcer
    // flagged again and 3yd away, no soldiers yet.
    DcTocDriver::Inputs AtArgent()
    {
        DcTocDriver::Inputs in = AtClick();
        in.progress = PROGRESS_CHAMPIONS_DEAD;
        in.tankMounted = false;
        in.mountedMembers = 0;
        in.mountedSinceMs = 0;
        return in;
    }

    DcTocDriver::Inputs AtKnight()
    {
        DcTocDriver::Inputs in = AtArgent();
        in.progress = PROGRESS_ARGENT_CHALLENGE_DIED;
        return in;
    }

    DungeonEvent const* Ev(uint32 id) { return DungeonEventRegistry::Find(MAP_ID, id); }

    BossRosterPatch const* Patch()
    {
        for (BossRosterPatch const& p : BossRosterRegistry::AllPatches())
            if (p.mapId == MAP_ID)
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
}

// ===========================================================================
//  1. THE PROGRESS COUNTER — the pins everything else is measured against
// ===========================================================================

// Hand-copied from trial_of_the_champion.h's `enum eData` / `enum eProgress`,
// both plain enums counted from their first member.
TEST(TrialOfTheChampion, InstanceDataSlotsAndProgressValuesMatchTheCore)
{
    EXPECT_EQ(MAP_ID, 650u);

    // enum eData: BOSS_* 0-2, MAX_ENCOUNTER 3, DATA_INSTANCE_PROGRESS 4,
    // DATA_ANNOUNCER 5, then 6..13 setters, DATA_TEAMID_IN_INSTANCE 14,
    // DATA_PALETRESS 15.
    EXPECT_EQ(DATA_INSTANCE_PROGRESS, 4u);
    EXPECT_EQ(DATA_ANNOUNCER, 5u);
    EXPECT_EQ(DATA_TEAMID_IN_INSTANCE, 14u);
    EXPECT_EQ(DATA_ARGENT_CHAMPION, 15u);

    EXPECT_EQ(PROGRESS_INITIAL, 0u);
    EXPECT_EQ(PROGRESS_REACHED_DEST, 1u);
    EXPECT_EQ(PROGRESS_GROUP_DIED_1, 2u);
    EXPECT_EQ(PROGRESS_GROUP_DIED_2, 3u);
    EXPECT_EQ(PROGRESS_GROUP_DIED_3, 4u);
    EXPECT_EQ(PROGRESS_CHAMPIONS_UNMOUNTED, 5u);
    EXPECT_EQ(PROGRESS_CHAMPIONS_DEAD, 6u);
    EXPECT_EQ(PROGRESS_SOLDIERS_DIED, 7u);
    EXPECT_EQ(PROGRESS_ARGENT_CHALLENGE_DIED, 8u);
    EXPECT_EQ(PROGRESS_FINISHED, 9u);

    EXPECT_EQ(BIT_GRAND_CHAMPIONS, 0u);
    EXPECT_EQ(BIT_ARGENT_CHALLENGE, 1u);
    EXPECT_EQ(BIT_BLACK_KNIGHT, 2u);
}

// menu 10614: at progress 0 the items are OptionIDs 0 and 3, and SelectGossip
// takes a POSITION — so the short version is position 1. At 6 and 8 there is one.
TEST(TrialOfTheChampion, GossipOptionIsOneAtZeroAndZeroLater)
{
    EXPECT_EQ(GOSSIP_OPTION_SHORT_JOUST, 1);
    EXPECT_EQ(GOSSIP_OPTION_NEXT_PHASE, 0);

    DcTocDriver::Verdict const v0 = DcTocDriver::Decide(AtClick());
    EXPECT_EQ(v0.action, Action::Gossip);
    EXPECT_EQ(v0.gossipOption, GOSSIP_OPTION_SHORT_JOUST)
        << "position 0 at progress 0 is the LONG version: two Tirion speeches, three "
           "gate cycles and three waypoint walks before wave 1";

    DcTocDriver::Verdict const v6 = DcTocDriver::Decide(AtArgent());
    EXPECT_EQ(v6.action, Action::Gossip);
    EXPECT_EQ(v6.gossipOption, GOSSIP_OPTION_NEXT_PHASE);

    DcTocDriver::Verdict const v8 = DcTocDriver::Decide(AtKnight());
    EXPECT_EQ(v8.action, Action::Gossip);
    EXPECT_EQ(v8.gossipOption, GOSSIP_OPTION_NEXT_PHASE);
}

// ===========================================================================
//  2. THE DRIVER KERNEL
// ===========================================================================

// Complete at 9 and beyond, and only there.
TEST(TrialOfTheChampion, KernelIsCompleteOnlyOnceTheKnightIsDead)
{
    for (uint32 p = 0; p <= PROGRESS_FINISHED + 1; ++p)
    {
        DcTocDriver::Inputs in = AtClick();
        in.progress = p;
        DcTocDriver::Verdict const v = DcTocDriver::Decide(in);
        EXPECT_EQ(v.complete, p >= PROGRESS_FINISHED) << "progress " << p;
        if (v.complete)
            EXPECT_EQ(v.action, Action::Yield) << "progress " << p;
    }
}

TEST(TrialOfTheChampion, KernelYieldsForADeadLeader)
{
    for (uint32 p = 0; p < PROGRESS_FINISHED; ++p)
    {
        DcTocDriver::Inputs in = AtClick();
        in.progress = p;
        in.alive = false;
        DcTocDriver::Verdict const v = DcTocDriver::Decide(in);
        EXPECT_EQ(v.state, State::Down) << "progress " << p;
        EXPECT_EQ(v.action, Action::Yield) << "progress " << p;
    }
}

// The tank on foot at 0: mod-playerbots' `toc lance` (65) and `toc mount` (64)
// outrank the event rung and do it. The driver HOLDS rather than yields, so the
// ticks those decline (a stock MoveTo returns false while its last move is in
// flight) are not handed to Advance, which would walk the tank off to OBJ(1)
// underneath them — but it issues nothing.
TEST(TrialOfTheChampion, KernelHoldsWithoutMovingWhileStockMountsTheTank)
{
    DcTocDriver::Inputs in = AtClick();
    in.tankMounted = false;
    in.mountedMembers = 0;
    in.mountedSinceMs = 0;

    DcTocDriver::Verdict const v = DcTocDriver::Decide(in);
    EXPECT_EQ(v.state, State::Mounting);
    EXPECT_EQ(v.action, Action::Hold);
    EXPECT_EQ(v.mountedSinceMs, 0u);
    EXPECT_EQ(v.unmountedSinceMs, NOW) << "the mount-warning clock starts on the first on-foot tick";
}

// Four riders or ninety seconds, whichever comes first — measured from the TANK's
// own mount, so a party that never musters still gets its click.
TEST(TrialOfTheChampion, MusterWaitsForFourRidersThenTimesOut)
{
    DcTocDriver::Inputs in = AtClick();
    in.mountedMembers = 3;
    in.mountedSinceMs = NOW - 10'000;
    DcTocDriver::Verdict v = DcTocDriver::Decide(in);
    EXPECT_EQ(v.state, State::Mustering);
    EXPECT_EQ(v.action, Action::Hold);

    in.mountedMembers = MUSTER_QUORUM;
    v = DcTocDriver::Decide(in);
    EXPECT_EQ(v.action, Action::Gossip) << "four of five is the quorum";

    in.mountedMembers = 1;
    in.mountedSinceMs = NOW - MUSTER_TIMEOUT_MS;
    v = DcTocDriver::Decide(in);
    EXPECT_EQ(v.action, Action::Gossip) << "the muster is bounded — at the timeout the tank goes alone";

    // The clock starts the tick the tank is first seen mounted.
    in.mountedSinceMs = 0;
    v = DcTocDriver::Decide(in);
    EXPECT_EQ(v.mountedSinceMs, NOW);
    EXPECT_EQ(v.state, State::Mustering);
}

// A smaller party musters everyone it has.
TEST(TrialOfTheChampion, MusterQuorumShrinksWithTheParty)
{
    EXPECT_EQ(DcTocDriver::MusterQuorum(5), MUSTER_QUORUM);
    EXPECT_EQ(DcTocDriver::MusterQuorum(3), 3u);
    EXPECT_EQ(DcTocDriver::MusterQuorum(1), 1u);
    EXPECT_EQ(DcTocDriver::MusterQuorum(0), 1u) << "the tank always counts itself";

    DcTocDriver::Inputs in = AtClick();
    in.partySize = 3;
    in.mountedMembers = 3;
    in.mountedSinceMs = NOW;
    EXPECT_EQ(DcTocDriver::Decide(in).action, Action::Gossip);
}

// Every Gossip verdict needs the flag and the reach, and at 0 it also needs the
// tank on a horse (unmounted, the menu comes back with no options at all).
TEST(TrialOfTheChampion, GossipNeedsFlagAndReachAndAVehicleAtZero)
{
    DcTocDriver::Inputs in = AtClick();

    in.announcerFlagged = false;
    DcTocDriver::Verdict v = DcTocDriver::Decide(in);
    EXPECT_NE(v.action, Action::Gossip) << "a flagless announcer is a silent no-op forever";
    EXPECT_EQ(v.state, State::AwaitGossip);
    EXPECT_EQ(v.action, Action::Hold) << "mounted at 0: never hand the tank to the DC ladder";

    in = AtClick();
    in.announcerPresent = false;
    v = DcTocDriver::Decide(in);
    EXPECT_EQ(v.action, Action::Hold);

    in = AtClick();
    in.announcerDist = GOSSIP_REACH + 0.5f;
    v = DcTocDriver::Decide(in);
    EXPECT_EQ(v.state, State::Approach);
    EXPECT_EQ(v.action, Action::ApproachAnnouncer);

    in = AtClick();
    in.tankMounted = false;
    v = DcTocDriver::Decide(in);
    EXPECT_NE(v.action, Action::Gossip);

    // The reach is inside the core's own INTERACTION_DISTANCE (5.5), and the
    // approach aims inside the reach.
    EXPECT_LT(GOSSIP_REACH, 5.5f);
    EXPECT_LT(GOSSIP_STANDOFF, GOSSIP_REACH);
}

// Progress 1-4 in combat is the joust, and the joust is mod-playerbots'.
TEST(TrialOfTheChampion, DriverYieldsInCombatDuringTheJoust)
{
    for (uint32 p = PROGRESS_REACHED_DEST; p <= PROGRESS_GROUP_DIED_3; ++p)
    {
        DcTocDriver::Inputs in = AtClick();
        in.progress = p;
        in.partyInCombat = true;
        in.walkingChampions = 1;
        in.distToPost = 40.0f;
        DcTocDriver::Verdict const v = DcTocDriver::Decide(in);
        EXPECT_EQ(v.state, State::Joust) << "progress " << p;
        EXPECT_EQ(v.action, Action::Yield) << "progress " << p;
    }
}

// OUT of combat during the joust the driver never yields: the DC ladder under it
// would spline the passenger. It rides the horse back to the post, or holds.
TEST(TrialOfTheChampion, JoustNeverYieldsOutOfCombat)
{
    for (uint32 p = PROGRESS_INITIAL; p <= PROGRESS_GROUP_DIED_3; ++p)
        for (bool mounted : { false, true })
            for (uint32 walking : { 0u, 2u })
                for (float post : { 0.0f, 30.0f })
                    for (bool flagged : { false, true })
                    {
                        DcTocDriver::Inputs in = AtClick();
                        in.progress = p;
                        in.tankMounted = mounted;
                        in.walkingChampions = walking;
                        in.distToPost = post;
                        in.announcerFlagged = flagged;
                        DcTocDriver::Verdict const v = DcTocDriver::Decide(in);
                        EXPECT_TRUE(v.Claims())
                            << "progress " << p << " mounted " << mounted << " walking " << walking
                            << " post " << post << " — a yield here hands a mounted tank to Advance";
                    }
}

// A champion walking to a horse: `toc mounted` shadows him from both engines and
// outranks this; the driver holds so nothing in DC rides the tank away.
TEST(TrialOfTheChampion, WalkingChampionOutOfCombatHoldsWithoutMoving)
{
    DcTocDriver::Inputs in = AtClick();
    in.progress = PROGRESS_GROUP_DIED_3;
    in.walkingChampions = 1;
    in.distToPost = 35.0f;
    DcTocDriver::Verdict const v = DcTocDriver::Decide(in);
    EXPECT_EQ(v.state, State::TrampleCover);
    EXPECT_EQ(v.action, Action::Hold)
        << "riding back to the post here would pull the trampler off the champion";
}

TEST(TrialOfTheChampion, MountedTankRidesBackToThePostBetweenWaves)
{
    DcTocDriver::Inputs in = AtClick();
    in.progress = PROGRESS_GROUP_DIED_1;
    in.distToPost = POST_RADIUS + 5.0f;
    DcTocDriver::Verdict v = DcTocDriver::Decide(in);
    EXPECT_EQ(v.state, State::Regroup);
    EXPECT_EQ(v.action, Action::ReturnToPost);

    in.distToPost = POST_RADIUS - 1.0f;
    v = DcTocDriver::Decide(in);
    EXPECT_EQ(v.state, State::BetweenWaves);
    EXPECT_EQ(v.action, Action::Hold);

    // A dead horse: `toc mount` remounts; the driver holds for it.
    in.tankMounted = false;
    in.distToPost = POST_RADIUS + 5.0f;
    v = DcTocDriver::Decide(in);
    EXPECT_EQ(v.state, State::Mounting);
    EXPECT_EQ(v.action, Action::Hold);
}

// Follow-tank stands down for a follower on foot through the joust, and ONLY then:
// in the saddle it follows (with the horse), after progress 4 there are no horses,
// and on any other map the question does not arise. tr-20260910-223555-1 is the
// ping-pong this closes — two DPS turned back to the tank on every tick their walk
// to a far horse was in flight.
TEST(TrialOfTheChampion, FollowTankStandsDownOnlyForAFollowerOnFootInTheJoust)
{
    for (uint32 p = PROGRESS_INITIAL; p <= PROGRESS_GROUP_DIED_3; ++p)
    {
        EXPECT_TRUE(DcTocDriver::FollowerMountsItself(MAP_ID, p, /*alive*/ true, /*onVehicle*/ false))
            << "progress " << p;
        EXPECT_FALSE(DcTocDriver::FollowerMountsItself(MAP_ID, p, true, /*onVehicle*/ true))
            << "progress " << p << ": a rider follows with its horse";
        EXPECT_FALSE(DcTocDriver::FollowerMountsItself(MAP_ID, p, /*alive*/ false, false))
            << "progress " << p;
    }
    for (uint32 p = PROGRESS_CHAMPIONS_UNMOUNTED; p <= PROGRESS_FINISHED; ++p)
        EXPECT_FALSE(DcTocDriver::FollowerMountsItself(MAP_ID, p, true, false))
            << "progress " << p << ": the horses are gone for good";
    EXPECT_FALSE(DcTocDriver::FollowerMountsItself(595, PROGRESS_INITIAL, true, false));
}

// Two minutes on foot through the joust is reported; a remount clears it.
TEST(TrialOfTheChampion, NeverMountedIsReportedAfterTheBudget)
{
    DcTocDriver::Inputs in = AtClick();
    in.tankMounted = false;
    in.unmountedSinceMs = NOW - MOUNT_WARN_MS + 1;
    EXPECT_FALSE(DcTocDriver::Decide(in).neverMounted);

    in.unmountedSinceMs = NOW - MOUNT_WARN_MS;
    EXPECT_TRUE(DcTocDriver::Decide(in).neverMounted);

    in.tankMounted = true;
    DcTocDriver::Verdict const v = DcTocDriver::Decide(in);
    EXPECT_FALSE(v.neverMounted);
    EXPECT_EQ(v.unmountedSinceMs, 0u);

    // Not a joust concern after 4.
    in = AtArgent();
    in.unmountedSinceMs = NOW - 10 * MOUNT_WARN_MS;
    EXPECT_FALSE(DcTocDriver::Decide(in).neverMounted);
}

TEST(TrialOfTheChampion, OnFootChampionsAreAnOrdinaryFight)
{
    DcTocDriver::Inputs in = AtArgent();
    in.progress = PROGRESS_CHAMPIONS_UNMOUNTED;
    for (bool combat : { false, true })
    {
        in.partyInCombat = combat;
        DcTocDriver::Verdict const v = DcTocDriver::Decide(in);
        EXPECT_EQ(v.state, State::OnFoot);
        EXPECT_EQ(v.action, Action::Yield);
    }
}

// Soldiers: hold the 12.5s intro, pull only when nothing is engaged, never pull a
// short party.
TEST(TrialOfTheChampion, SoldierPullOnlyWhenNoneAreEngaged)
{
    DcTocDriver::Inputs in = AtArgent();
    in.announcerFlagged = false;
    in.soldiersAlive = 9;
    in.soldiersAttackable = 0;
    DcTocDriver::Verdict v = DcTocDriver::Decide(in);
    EXPECT_EQ(v.state, State::ArgentIntro);
    EXPECT_EQ(v.action, Action::Hold);

    in.soldiersAttackable = 9;
    v = DcTocDriver::Decide(in);
    EXPECT_EQ(v.state, State::ArgentPull);
    EXPECT_EQ(v.action, Action::EngageSoldier);

    in.soldiersEngaged = true;
    v = DcTocDriver::Decide(in);
    EXPECT_EQ(v.action, Action::Yield) << "a pack is already coming — let the stock engine fight it";

    in.soldiersEngaged = false;
    in.partyInCombat = true;
    v = DcTocDriver::Decide(in);
    EXPECT_EQ(v.action, Action::Yield);

    in.partyInCombat = false;
    in.soldiersAlive = 6;
    in.soldiersAttackable = 6;
    in.partyReady = false;
    v = DcTocDriver::Decide(in);
    EXPECT_EQ(v.state, State::Resting);
    EXPECT_EQ(v.action, Action::Yield) << "rest between packs: the anchored hold yields to NeedsRest";
}

// The click at 6 and 8 waits for the rest gate; at 0 it never asks (nobody can
// drink in the saddle).
TEST(TrialOfTheChampion, PhaseClicksWaitForTheRestGate)
{
    for (DcTocDriver::Inputs in : { AtArgent(), AtKnight() })
    {
        in.partyReady = false;
        DcTocDriver::Verdict const v = DcTocDriver::Decide(in);
        EXPECT_EQ(v.state, State::Resting) << "progress " << in.progress;
        EXPECT_EQ(v.action, Action::Yield) << "progress " << in.progress;
    }

    DcTocDriver::Inputs in = AtClick();
    in.partyReady = false;
    EXPECT_EQ(DcTocDriver::Decide(in).action, Action::Gossip);
}

// Between phases, with the announcer's flag still down (it returns 15s after the
// phase), the driver yields: OBJ(1) latches and OBJ(2)'s arrival walks the party.
TEST(TrialOfTheChampion, UnflaggedAnnouncerAfterAPhaseIsAYield)
{
    for (DcTocDriver::Inputs in : { AtArgent(), AtKnight() })
    {
        in.announcerFlagged = false;
        DcTocDriver::Verdict const v = DcTocDriver::Decide(in);
        EXPECT_EQ(v.state, State::AwaitGossip) << "progress " << in.progress;
        EXPECT_EQ(v.action, Action::Yield) << "progress " << in.progress;
    }
}

// Progress 7: the boss walks in and zone-combats on her own. Only an EVADED,
// idle, attackable boss after a partial wipe is pulled.
TEST(TrialOfTheChampion, ArgentBossIsPulledOnlyAfterAnEvade)
{
    DcTocDriver::Inputs in = AtArgent();
    in.progress = PROGRESS_SOLDIERS_DIED;
    in.argentBossPresent = true;
    in.argentBossAttackable = false;  // walking in, NON_ATTACKABLE for 3s
    DcTocDriver::Verdict v = DcTocDriver::Decide(in);
    EXPECT_EQ(v.action, Action::Yield);

    in.argentBossAttackable = true;
    in.argentBossEngaged = true;
    v = DcTocDriver::Decide(in);
    EXPECT_EQ(v.action, Action::Yield);

    in.argentBossEngaged = false;
    v = DcTocDriver::Decide(in);
    EXPECT_EQ(v.state, State::ArgentRepull);
    EXPECT_EQ(v.action, Action::EngageBoss);

    in.partyReady = false;
    v = DcTocDriver::Decide(in);
    EXPECT_EQ(v.action, Action::Yield) << "rest before re-pulling";
}

// The Knight: ~50s of speeches NON_ATTACKABLE (hold), then an ordinary fight.
TEST(TrialOfTheChampion, KnightIntroHoldsUntilAttackable)
{
    DcTocDriver::Inputs in = AtKnight();
    in.announcerPresent = false;  // he killed him on the way in
    in.knightPresent = true;
    in.knightAttackable = false;
    DcTocDriver::Verdict v = DcTocDriver::Decide(in);
    EXPECT_EQ(v.state, State::KnightIntro);
    EXPECT_EQ(v.action, Action::Hold);
    EXPECT_EQ(v.strandSinceMs, 0u) << "a Knight on the field is not a strand";

    in.knightAttackable = true;
    v = DcTocDriver::Decide(in);
    EXPECT_EQ(v.state, State::KnightFight);
    EXPECT_EQ(v.action, Action::Yield);

    // His feign-death transitions read NON_ATTACKABLE mid-fight; combat wins.
    in.knightAttackable = false;
    in.partyInCombat = true;
    v = DcTocDriver::Decide(in);
    EXPECT_EQ(v.action, Action::Yield);
}

// Evade = despawn, with the announcer already dead: reported after the grace,
// never retried, never clicked.
TEST(TrialOfTheChampion, EvadedKnightIsReportedNotRetried)
{
    DcTocDriver::Inputs in = AtKnight();
    in.announcerPresent = false;
    in.knightPresent = false;

    DcTocDriver::Verdict v = DcTocDriver::Decide(in);
    EXPECT_EQ(v.strandSinceMs, NOW);
    EXPECT_FALSE(v.stranded);
    EXPECT_NE(v.action, Action::Gossip);

    in.strandSinceMs = NOW - STRAND_GRACE_MS;
    v = DcTocDriver::Decide(in);
    EXPECT_TRUE(v.stranded);
    EXPECT_EQ(v.state, State::KnightStranded);
    EXPECT_EQ(v.action, Action::Yield);

    // A full wipe respawns the announcer: the strand clock clears and the click
    // comes back.
    in.announcerPresent = true;
    v = DcTocDriver::Decide(in);
    EXPECT_EQ(v.strandSinceMs, 0u);
    EXPECT_FALSE(v.stranded);
    EXPECT_EQ(v.action, Action::Gossip);
}

// The rewinds: 1-4 -> 0 (re-mount, re-click the SHORT option), 7 -> 6, and 8
// stays 8. Nothing latched, so the earlier phase's answer simply comes back.
TEST(TrialOfTheChampion, WipeRewindReclicksAtZeroSixAndEight)
{
    DcTocDriver::Inputs in = AtClick();
    in.progress = PROGRESS_GROUP_DIED_2;
    EXPECT_NE(DcTocDriver::Decide(in).action, Action::Gossip);
    in.progress = PROGRESS_INITIAL;  // cleanup: announcer re-homed with gossip
    DcTocDriver::Verdict v = DcTocDriver::Decide(in);
    EXPECT_EQ(v.action, Action::Gossip);
    EXPECT_EQ(v.gossipOption, GOSSIP_OPTION_SHORT_JOUST);

    in = AtArgent();
    in.progress = PROGRESS_SOLDIERS_DIED;
    EXPECT_NE(DcTocDriver::Decide(in).action, Action::Gossip);
    in.progress = PROGRESS_CHAMPIONS_DEAD;  // 7 -> 6, soldiers despawned
    v = DcTocDriver::Decide(in);
    EXPECT_EQ(v.action, Action::Gossip);
    EXPECT_EQ(v.gossipOption, GOSSIP_OPTION_NEXT_PHASE);

    in = AtKnight();  // 8 -> 8, gryphon and Knight despawned, announcer back
    v = DcTocDriver::Decide(in);
    EXPECT_EQ(v.action, Action::Gossip);
    EXPECT_EQ(v.gossipOption, GOSSIP_OPTION_NEXT_PHASE);
}

// One action per tick, and its payload is consistent with it: a gossip option
// only on Gossip, and a Gossip only with the flag up and the announcer in reach.
TEST(TrialOfTheChampion, NeverProposesTwoActions)
{
    for (uint32 p = 0; p <= PROGRESS_FINISHED; ++p)
        for (bool combat : { false, true })
            for (bool mounted : { false, true })
                for (bool flagged : { false, true })
                    for (float dist : { 2.0f, 20.0f })
                        for (uint32 soldiers : { 0u, 9u })
                            for (bool ready : { false, true })
                            {
                                DcTocDriver::Inputs in = AtClick();
                                in.progress = p;
                                in.partyInCombat = combat;
                                in.tankMounted = mounted;
                                in.announcerFlagged = flagged;
                                in.announcerDist = dist;
                                in.soldiersAlive = soldiers;
                                in.soldiersAttackable = soldiers;
                                in.partyReady = ready;
                                DcTocDriver::Verdict const v = DcTocDriver::Decide(in);

                                EXPECT_EQ(v.gossipOption >= 0, v.action == Action::Gossip)
                                    << "progress " << p;
                                if (v.action == Action::Gossip)
                                {
                                    EXPECT_TRUE(flagged) << "progress " << p;
                                    EXPECT_LE(dist, GOSSIP_REACH) << "progress " << p;
                                    EXPECT_FALSE(combat) << "progress " << p;
                                }
                                if (combat)
                                    EXPECT_EQ(v.action, Action::Yield)
                                        << "progress " << p << ": the driver never steers under fire";
                            }
}

// ===========================================================================
//  3. THE AUTHORED DATA
// ===========================================================================

TEST(TrialOfTheChampion, RosterIsThreeObjectivesAndNoBossRows)
{
    BossRosterPatch const* p = Patch();
    ASSERT_NE(p, nullptr) << "map 650 has no roster patch — every run fails at setup with "
                             "'no boss roster for this map'";
    EXPECT_EQ(p->gate, DcDifficultyGate::Any) << "heroic changes nothing structural";
    EXPECT_TRUE(p->remove.empty()) << "the derived list is EMPTY: nothing to remove";
    EXPECT_TRUE(p->reorder.empty());
    ASSERT_EQ(p->add.size(), 3u);

    for (DungeonBossInfo const& b : p->add)
        EXPECT_EQ(b.kind, DungeonAnchorKind::Objective)
            << "'" << b.name << "' is a boss anchor. Two of the three bosses never die and "
               "the third does not exist until a gossip click.";

    // Only one patch for this map.
    uint32 patches = 0;
    for (BossRosterPatch const& q : BossRosterRegistry::AllPatches())
        if (q.mapId == MAP_ID)
            ++patches;
    EXPECT_EQ(patches, 1u);
}

TEST(TrialOfTheChampion, ObjectiveOrderIsOneTwoThree)
{
    BossRosterPatch const* p = Patch();
    ASSERT_NE(p, nullptr);

    struct Row { uint32 seq; uint32 order; char const* name; float x, y, z; };
    static constexpr Row kRows[] = {
        { EVENT_CHAMPIONS,    ORDER_CHAMPIONS,    "The Grand Champions",
          CHAMPIONS_X, CHAMPIONS_Y, CHAMPIONS_Z },
        { EVENT_ARGENT,       ORDER_ARGENT,       "The Argent Challenge",
          ARGENT_X, ARGENT_Y, ARGENT_Z },
        { EVENT_BLACK_KNIGHT, ORDER_BLACK_KNIGHT, "The Black Knight",
          KNIGHT_X, KNIGHT_Y, KNIGHT_Z },
    };

    for (Row const& r : kRows)
    {
        DungeonBossInfo const* o = Obj(*p, r.seq);
        ASSERT_NE(o, nullptr) << r.name;
        EXPECT_EQ(o->mapId, MAP_ID) << r.name;
        EXPECT_EQ(o->eventId, r.seq) << r.name;
        EXPECT_EQ(o->orderOverride, static_cast<int32>(r.order)) << r.name;
        EXPECT_EQ(o->onArriveHook, 0u) << r.name << ": driven by its event, not an arrival hook";
        EXPECT_EQ(o->encounterIndex, 0u)
            << r.name << ": a real bit would self-complete on the encounter credit";
        EXPECT_NEAR(o->x, r.x, 0.01f) << r.name;
        EXPECT_NEAR(o->y, r.y, 0.01f) << r.name;
        EXPECT_NEAR(o->z, r.z, 0.01f) << r.name;
        EXPECT_FLOAT_EQ(o->arriveRadius, OBJ_ARRIVE) << r.name;
    }

    EXPECT_LT(ORDER_CHAMPIONS, ORDER_ARGENT);
    EXPECT_LT(ORDER_ARGENT, ORDER_BLACK_KNIGHT);
}

// Every anchor inside the bowl, and OBJ(2) and OBJ(3) far enough apart to be
// distinct arrivals.
TEST(TrialOfTheChampion, AnchorsAreInsideTheBowl)
{
    struct P { float x, y; char const* name; };
    for (P const& a : { P{ CHAMPIONS_X, CHAMPIONS_Y, "OBJ1" }, P{ ARGENT_X, ARGENT_Y, "OBJ2" },
                        P{ KNIGHT_X, KNIGHT_Y, "OBJ3" } })
        EXPECT_LT(std::hypot(a.x - ARENA_X, a.y - ARENA_Y), 20.0f) << a.name;

    EXPECT_GT(std::hypot(ARGENT_X - KNIGHT_X, ARGENT_Y - KNIGHT_Y), 4.0f);
    // The joust post's radius has to keep a parked horse off the wave convergence
    // point (ARENA_*, where each wave walks to) by at least a lance's reach.
    EXPECT_GT(std::hypot(CHAMPIONS_X - ARENA_X, CHAMPIONS_Y - ARENA_Y), 6.0f);
    // The census has to cover the far wall from the entrance.
    EXPECT_GT(ARENA_SCAN, std::hypot(805.23f - ARENA_X, 618.04f - ARENA_Y) + ARENA_RADIUS);

    // ...where the wall is measured off the four gates, which stand in it.
    struct G { float x, y; };
    for (G const& g : { G{ 746.70f, 677.47f }, G{ 746.65f, 556.93f }, G{ 685.51f, 618.06f },
                        G{ 807.84f, 618.06f } })
        EXPECT_LE(std::hypot(g.x - ARENA_X, g.y - ARENA_Y), ARENA_RADIUS + 3.0f);
}

TEST(TrialOfTheChampion, EventFlagsAreAsAuthored)
{
    for (uint32 id = EVENT_CHAMPIONS; id <= EVENT_DRIVER; ++id)
        ASSERT_NE(Ev(id), nullptr) << "event " << id << " is missing";

    for (uint32 id : { EVENT_CHAMPIONS, EVENT_ARGENT, EVENT_BLACK_KNIGHT })
    {
        DungeonEvent const* ev = Ev(id);
        EXPECT_EQ(ev->activation, EventActivation::Anchored) << "event " << id;
        EXPECT_EQ(ev->gate, DcDifficultyGate::Any) << "event " << id;
        EXPECT_TRUE(ev->persistent) << "event " << id << ": every phase spans fights";
        EXPECT_TRUE(ev->required) << "event " << id;
        EXPECT_FALSE(ev->repeatable) << "event " << id;
        EXPECT_FALSE(ev->drivesInCombat) << "event " << id;
        EXPECT_FALSE(ev->stepsOwnMovement) << "event " << id;
        EXPECT_FALSE(ev->ownsThePull) << "event " << id << ": inferred from Persistent on anchored events";
    }

    DungeonEvent const* d = Ev(EVENT_DRIVER);
    EXPECT_EQ(d->activation, EventActivation::Conditional);
    EXPECT_TRUE(d->condition) << "a Conditional event with no predicate never fires";
    EXPECT_TRUE(d->repeatable);
    EXPECT_TRUE(d->persistent);
    EXPECT_FALSE(d->required);
    EXPECT_TRUE(d->ownsThePull);
    EXPECT_TRUE(d->drivesInCombat);
    EXPECT_TRUE(d->stepsOwnMovement)
        << "the hook rides the tank's horse itself; the per-tick hold would cancel it";
    EXPECT_EQ(d->gate, DcDifficultyGate::Any);
    EXPECT_EQ(d->panelGatesBossEntry, 0u);
    EXPECT_EQ(d->panelSortAfterBossEntry, 0u);

    ASSERT_EQ(d->steps.size(), 1u);
    EXPECT_EQ(d->steps[0].kind, EventStepKind::Custom);
    EXPECT_EQ(d->steps[0].hookId, HOOK_TOC_DRIVER);
    EXPECT_EQ(d->steps[0].timeoutMs, DRIVER_TIMEOUT_MS);
    EXPECT_TRUE(ObjectiveHookRegistry::Has(HOOK_TOC_DRIVER))
        << "hook 37 is not registered — the driver would latch Blocked and nothing would "
           "ever click the announcer";
}

// Each hold: a leading MoveTo onto the anchor (the sticky rule), then a garrison
// on the counter at exactly the value that ends its encounter.
TEST(TrialOfTheChampion, HoldsGateOnSixEightNine)
{
    struct Row { uint32 id; uint32 min; float x, y, z; uint32 timeout; };
    static constexpr Row kRows[] = {
        { EVENT_CHAMPIONS,    PROGRESS_CHAMPIONS_DEAD,        CHAMPIONS_X, CHAMPIONS_Y, CHAMPIONS_Z,
          CHAMPIONS_TIMEOUT_MS },
        { EVENT_ARGENT,       PROGRESS_ARGENT_CHALLENGE_DIED, ARGENT_X, ARGENT_Y, ARGENT_Z,
          ARGENT_TIMEOUT_MS },
        { EVENT_BLACK_KNIGHT, PROGRESS_FINISHED,              KNIGHT_X, KNIGHT_Y, KNIGHT_Z,
          KNIGHT_TIMEOUT_MS },
    };

    for (Row const& r : kRows)
    {
        DungeonEvent const* ev = Ev(r.id);
        ASSERT_NE(ev, nullptr);
        ASSERT_EQ(ev->steps.size(), 2u) << "event " << r.id;

        EventStep const& lead = ev->steps[0];
        EXPECT_EQ(lead.kind, EventStepKind::MoveTo)
            << "event " << r.id << ": without a leading MoveTo the persistent sticky latch "
               "never engages and the fights across the bowl close the at-objective gate";
        EXPECT_LT(lead.instanceDataId, 0) << "event " << r.id << ": the lead is a plain MoveTo";
        EXPECT_FLOAT_EQ(lead.x, r.x);

        EventStep const& hold = ev->steps[1];
        EXPECT_EQ(hold.kind, EventStepKind::MoveTo);
        EXPECT_EQ(hold.instanceDataId, static_cast<int32>(DATA_INSTANCE_PROGRESS)) << "event " << r.id;
        EXPECT_EQ(hold.instanceDataMin, r.min) << "event " << r.id;
        EXPECT_FLOAT_EQ(hold.x, r.x);
        EXPECT_FLOAT_EQ(hold.y, r.y);
        EXPECT_FLOAT_EQ(hold.z, r.z);
        EXPECT_FLOAT_EQ(hold.radius, HOLD_RADIUS);
        EXPECT_EQ(hold.timeoutMs, r.timeout) << "event " << r.id;
        EXPECT_EQ(hold.hookId, 0u) << "event " << r.id << ": the hold does nothing but hold";
    }

    // The thresholds no rewind can reach early: 1-4 falls to 0 and 7 to 6, all
    // below the holds they would otherwise satisfy.
    EXPECT_GT(PROGRESS_CHAMPIONS_DEAD, PROGRESS_GROUP_DIED_3);
    EXPECT_GT(PROGRESS_ARGENT_CHALLENGE_DIED, PROGRESS_SOLDIERS_DIED);
}

TEST(TrialOfTheChampion, EntryListsAreComplete)
{
    std::vector<uint32> const& champs = TocChampionEntries();
    EXPECT_EQ(champs.size(), 10u) << "five champions per side";
    for (uint32 e : { NPC_MOKRA, NPC_ERESSEA, NPC_RUNOK, NPC_ZULTORE, NPC_VISCERI,
                      NPC_JACOB, NPC_AMBROSE, NPC_COLOSOS, NPC_JAELYNE, NPC_LANA })
        EXPECT_NE(std::find(champs.begin(), champs.end(), e), champs.end()) << e;

    std::vector<uint32> const& soldiers = TocSoldierEntries();
    EXPECT_EQ(soldiers.size(), 3u);
    for (uint32 e : { NPC_ARGENT_MONK, NPC_ARGENT_PRIESTESS, NPC_ARGENT_LIGHTWIELDER })
        EXPECT_NE(std::find(soldiers.begin(), soldiers.end(), e), soldiers.end()) << e;
    EXPECT_EQ(std::find(soldiers.begin(), soldiers.end(), NPC_FOUNTAIN_OF_LIGHT), soldiers.end())
        << "the fountain is not a soldier: it would hold the pull on a PACIFIED totem";
}

TEST(TrialOfTheChampion, DoorRowsCoverAllFourGates)
{
    for (uint32 go : { GO_MAIN_GATE, GO_EAST_PORTCULLIS, GO_SOUTH_PORTCULLIS, GO_NORTH_PORTCULLIS })
    {
        EXPECT_TRUE(DcEventDoorRegistry::IsScriptOnly(go)) << go;
        EXPECT_TRUE(DcEventDoorRegistry::IsNavigationIgnored(go))
            << go << ": the entrance gate is 2.6yd from the landing point and shuts at progress 1";
    }
    EXPECT_FALSE(DcEventDoorRegistry::IsScriptOnly(GO_LANCE_RACK));
    EXPECT_FALSE(DcEventDoorRegistry::IsNavigationIgnored(GO_LANCE_RACK));
}

// Paletress, for the Reflective Shield window only, and never for the tank.
TEST(TrialOfTheChampion, PaletressExclusionIsDpsOnlyAndConditional)
{
    EXPECT_TRUE(DcTargetExclusionRegistry::HasRowsFor(MAP_ID));

    EXPECT_FALSE(DcTargetExclusionRegistry::IsExcluded(nullptr, MAP_ID, NPC_PALETRESS, /*forTank*/ true))
        << "the tank keeps her — she is the one hitting people";
    EXPECT_FALSE(DcTargetExclusionRegistry::IsExcluded(nullptr, MAP_ID, NPC_PALETRESS, /*forTank*/ false))
        << "with no live shield the row must stand down: it is WINDOWED, never permanent";

    EXPECT_FALSE(DcTargetExclusionRegistry::IsExcluded(nullptr, MAP_ID, NPC_EADRIC))
        << "Eadric has no unkillable window";
    EXPECT_FALSE(DcTargetExclusionRegistry::IsExcluded(nullptr, MAP_ID, NPC_BLACK_KNIGHT));
}

// Desecration: the leave-once shape. Vacate the RAW 8yd pulse, keep out past it,
// and aim the retreat outside the keep-out cylinder so it always finds a spot.
TEST(TrialOfTheChampion, DesecrationEmitterVacatesTheEightYardPulse)
{
    EXPECT_TRUE(DcHazardRegistry::HasEmitters(MAP_ID));
    DcHazardEmitter const* e = DcHazardRegistry::Find(MAP_ID, NPC_DESECRATION_STALKER);
    ASSERT_NE(e, nullptr);
    EXPECT_FLOAT_EQ(e->vacateRadius, DESECRATION_RADIUS);
    EXPECT_GT(e->radius, e->vacateRadius) << "the keep-out pads the pulse for placement drift";
    EXPECT_GT(e->retreatSlack, e->holdBand);
    EXPECT_GT(e->vacateRadius + e->retreatSlack, e->radius)
        << "a retreat aimed inside the keep-out cylinder can never find a spot it accepts";

    EXPECT_EQ(DcHazardRegistry::Find(MAP_ID, NPC_BLACK_KNIGHT), nullptr);
}

TEST(TrialOfTheChampion, NeverTargetRowsAreTheTwoSummonsOnly)
{
    EXPECT_TRUE(DcNeverTargetRegistry::IsNeverTarget(MAP_ID, NPC_DESECRATION_STALKER))
        << "unit_flags 0, faction 14: to the clear it is the nearest hostile, at 0yd";
    EXPECT_TRUE(DcNeverTargetRegistry::IsNeverTarget(MAP_ID, NPC_FOUNTAIN_OF_LIGHT));

    for (uint32 e : { NPC_PALETRESS, NPC_EADRIC, NPC_BLACK_KNIGHT, NPC_ARGENT_MONK,
                      NPC_ARGENT_PRIESTESS, NPC_ARGENT_LIGHTWIELDER, NPC_RISEN_CHAMPION,
                      NPC_RISEN_JAEREN, NPC_RISEN_ARELAS })
        EXPECT_FALSE(DcNeverTargetRegistry::IsNeverTarget(MAP_ID, e))
            << e << " is a fight the party has to win";
}
