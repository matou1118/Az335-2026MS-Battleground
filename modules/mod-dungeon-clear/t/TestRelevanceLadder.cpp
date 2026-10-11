/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "gtest/gtest.h"

#include "Ai/Dungeon/DungeonClear/Strategy/DcRelevance.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonClearTuning.h"

// Executable form of the trigger relevance ladder (arch-review F3). Pins the
// ordering both strategies depend on and documents every same-value tie with the
// partition (role / engine / map / anchor-kind) that legitimizes it, so a future
// edit that reorders a rung — or lands a new feature on an occupied one — trips a
// red test instead of silently depending on trigger registration order.

// --- non-combat driving ladder, top to bottom ----------------------------

TEST(DungeonClearRelevanceTest, NonCombatLadderStrictlyDescends)
{
    // Every strict > below is an ordering the strategy relies on. Grouped as the
    // ladder reads top-down; ties are asserted separately (partitioned) below.
    EXPECT_GT(DcRel::Chat,            DcRel::LootRollPending);
    EXPECT_GT(DcRel::LootRollPending, DcRel::DoorReopened);
    EXPECT_GT(DcRel::DoorReopened,    DcRel::AllCleared);
    EXPECT_GT(DcRel::AllCleared,      DcRel::StrandedRecovery);
    EXPECT_GT(DcRel::StrandedRecovery, DcRel::HealReposition);
    EXPECT_GT(DcRel::HealReposition,  DcRel::HakkarSuppressor);
    EXPECT_GT(DcRel::HakkarSuppressor, DcRel::HakkarFlame);
    EXPECT_GT(DcRel::HakkarFlame,     DcRel::Pull);        // tie-broken (was ==)
    EXPECT_GT(DcRel::Pull,            DcRel::HakkarLootBlood);
    EXPECT_GT(DcRel::HakkarLootBlood, DcRel::RezParty);
    EXPECT_GT(DcRel::RezParty,        DcRel::EventDue);
    EXPECT_GT(DcRel::EventDue,        DcRel::AtBoss);
    EXPECT_GT(DcRel::AtBoss,          DcRel::AssistCamp);
    EXPECT_GT(DcRel::AssistCamp,      DcRel::HoldAtCamp);
    EXPECT_GT(DcRel::HoldAtCamp,      DcRel::NeedsRest);
    EXPECT_GT(DcRel::NeedsRest,       DcRel::RoomTrash);   // tie-broken (was ==)
    EXPECT_GT(DcRel::RoomTrash,       DcRel::BlockingTrash);
    EXPECT_GT(DcRel::BlockingTrash,   DcRel::LeaderAssist);
    EXPECT_GT(DcRel::LeaderAssist,    DcRel::DoorBlocked);
    EXPECT_GT(DcRel::DoorBlocked,     DcRel::Stalled);
    EXPECT_GT(DcRel::Stalled,         DcRel::RoomPreclearHold);
    EXPECT_GT(DcRel::RoomPreclearHold, DcRel::Advance);
    EXPECT_GT(DcRel::Advance,         DcRel::FilterLoot);
}

// --- key invariants named in the strategy comments ------------------------

TEST(DungeonClearRelevanceTest, EventDuePreemptsBossAndDoor)
{
    // A due conditional gate must preempt the boss engage AND the door-blocked stall.
    EXPECT_GT(DcRel::EventDue, DcRel::AtBoss);
    EXPECT_GT(DcRel::EventDue, DcRel::DoorBlocked);
}

TEST(DungeonClearRelevanceTest, LeaderAssistFillsGapBelowOwnEngageScans)
{
    // Leader-assist fills the out-of-sight gap: above advance/stall/door, below the
    // tank's own engage scans so a deliberate visible pull always wins.
    EXPECT_GT(DcRel::LeaderAssist, DcRel::DoorBlocked);
    EXPECT_GT(DcRel::LeaderAssist, DcRel::Stalled);
    EXPECT_GT(DcRel::LeaderAssist, DcRel::Advance);
    EXPECT_LT(DcRel::LeaderAssist, DcRel::BlockingTrash);
    EXPECT_LT(DcRel::LeaderAssist, DcRel::RoomTrash);
    EXPECT_LT(DcRel::LeaderAssist, DcRel::AtBoss);
}

TEST(DungeonClearRelevanceTest, LeaderAssistOutranksAdvanceSoItMustYieldWhenItCannotAct)
{
    // The pairing that made tp-20260828-175353-1 unrecoverable, pinned so the
    // reasoning survives the next reader.
    //
    // Leader-assist sits ABOVE advance, and DC runs one action per tick. That is
    // correct — a tank must go help the fight rather than keep walking to the next
    // boss — but it means the rung is only allowed to claim the tick when it can
    // actually DO something. When it cannot, the cost of claiming anyway is not a
    // wasted tick, it is a stopped run: advance never gets to drive, the party
    // never leaves the ground that is flagging it, and the flag therefore never
    // clears either. All five BWL raids died in that loop, watchdogs clear,
    // 0.0yd from the "fight" they were closing on, for as long as anyone watched.
    EXPECT_GT(DcRel::LeaderAssist, DcRel::Advance);
    EXPECT_GT(DcRel::LeaderAssist, DcRel::Stalled);

    // Hence DungeonClearLeaderAssistAction's two exits: the co-located guard
    // (nothing reachable to assist and the flagged member is already here) and
    // reporting DcMoveTo's own refusal instead of discarding it. The guard's band
    // has to stay well under the ranges a REAL assist covers, or it would start
    // swallowing the case the rung exists for.
    EXPECT_LT(DC_LEADER_ASSIST_COLOCATED_RANGE, DC_ENGAGE_RANGE);
    EXPECT_GT(DC_LEADER_ASSIST_COLOCATED_RANGE, 0.0f);
}

TEST(DungeonClearRelevanceTest, StrandedRecoveryOutranksTheFrozenDrivingLadder)
{
    // The failsafe fires only after the leader driving ladder has, by definition,
    // been failing to make progress for minutes, so it must outrank every rung it
    // could otherwise be starved beneath — the boss/event drivers, the rez rung,
    // and the advance/stall fallbacks. Below AllCleared: a finished run should
    // congratulate + disable, not teleport.
    EXPECT_LT(DcRel::StrandedRecovery, DcRel::AllCleared);
    EXPECT_GT(DcRel::StrandedRecovery, DcRel::RezParty);
    EXPECT_GT(DcRel::StrandedRecovery, DcRel::EventDue);
    EXPECT_GT(DcRel::StrandedRecovery, DcRel::AtBoss);
    EXPECT_GT(DcRel::StrandedRecovery, DcRel::Pull);
    EXPECT_GT(DcRel::StrandedRecovery, DcRel::Stalled);
    EXPECT_GT(DcRel::StrandedRecovery, DcRel::Advance);
}

TEST(DungeonClearRelevanceTest, RezPartyOutranksEveryLadderItCanLandOn)
{
    // The elected rezzer may be the LEADER (a prot paladin raising its healer)
    // or any FOLLOWER, so the rung must beat both ladders: the leader's event/
    // boss drivers — recover the party before running a gate or pulling — and
    // every follower rung. It deliberately does NOT tie EventDue (31): both are
    // leader-armable on the same tick (a corpse plus a due conditional gate has
    // no role/engine/map partition), so the ordering must be strict.
    EXPECT_GT(DcRel::RezParty, DcRel::EventDue);
    EXPECT_GT(DcRel::RezParty, DcRel::AtBoss);
    EXPECT_GT(DcRel::RezParty, DcRel::AtObjective);
    EXPECT_GT(DcRel::RezParty, DcRel::AssistCamp);
    EXPECT_GT(DcRel::RezParty, DcRel::HoldAtCamp);
    EXPECT_GT(DcRel::RezParty, DcRel::NeedsRest);   // rez first; OOM rezzer's
                                                    // action defers, THEN drinks
    EXPECT_GT(DcRel::RezParty, DcRel::FollowTank);
    // Below the pull maneuver (inert during recovery via the readiness gate)
    // and the map-partitioned Hakkar band; below the terminal bailouts.
    EXPECT_LT(DcRel::RezParty, DcRel::HakkarLootBlood);
    EXPECT_LT(DcRel::RezParty, DcRel::Pull);
    EXPECT_LT(DcRel::RezParty, DcRel::PartyDied);
}

TEST(DungeonClearRelevanceTest, RezPartyRankAloneDoesNotProtectTheApproach)
{
    // A COMPANION WARNING TO THE TEST ABOVE, because reading that one on its own
    // is what cost run tr-20260807-080834-115 its recovery: outranking the
    // follower stack does NOT mean the follower stack cannot run during a
    // recovery, and the rez approach is not safe merely because 31.5 > 25.
    //
    // Two holes, both real:
    //
    //  1. Engine::DoNextAction only stops walking the queue when an action
    //     RETURNS TRUE. A rez rung that returns false — as it did on every tick
    //     after the first, because DcMoveTo reports a duplicate destination while
    //     its own glide is running — hands the tick straight down to scout-lag,
    //     whose in-the-bubble branch stops the bot. Fix: the approach branch owns
    //     the tick unconditionally (DcFollowerActions).
    //
    //  2. HealReposition sits ABOVE RezParty and is registered in the non-combat
    //     engine too, so on a healer — the class the election PREFERS — it takes
    //     the tick outright and walks toward the tank for line of sight while the
    //     corpse lies the other way.
    //
    // This assertion pins hole 2 as a deliberate ordering rather than an
    // oversight. Do not "fix" it by lifting RezParty over HealReposition: that
    // ordering is load-bearing the rest of the time (a healer must reposition to
    // heal), and StrandedRecovery/HazardVacate sit above it for the same reason.
    // The stand-down gate — DcRezRecovery::IsElectedRezzer, consulted by the
    // follower movers — is what makes the rez rung reachable, not the number.
    EXPECT_GT(DcRel::HealReposition, DcRel::RezParty);
}

TEST(DungeonClearRelevanceTest, HakkarSuppressorOutranksFlameOutranksBlood)
{
    EXPECT_GT(DcRel::HakkarSuppressor, DcRel::HakkarFlame);
    EXPECT_GT(DcRel::HakkarFlame,      DcRel::HakkarLootBlood);
    // Combat side mirrors the order at the higher band.
    EXPECT_GT(DcRel::HakkarSuppressorCombat, DcRel::HakkarFlameCombat);
    EXPECT_GT(DcRel::HakkarFlameCombat,      DcRel::HakkarLootBloodCombat);
}

// --- combat engine ladder -------------------------------------------------

TEST(DungeonClearRelevanceTest, CombatLadderStrictlyDescends)
{
    // Phantom-combat force-clear sits at the very top of the combat band so the
    // recovery always wins the tick when it fires.
    EXPECT_GT(DcRel::BreakStuckCombat,       DcRel::HakkarSuppressorCombat);
    EXPECT_GT(DcRel::HakkarSuppressorCombat, DcRel::HakkarFlameCombat);
    EXPECT_GT(DcRel::HakkarFlameCombat,      DcRel::HakkarLootBloodCombat);
    // The wave-encounter event driver (Black Morass) sits between the Hakkar band
    // and the camp owners. It has to outrank the camp owners and every stock
    // combat mover, because its entire job is to take the tank OFF the pack it is
    // tanking and walk it to the next rift — the thing the non-combat-only rung
    // could never do, which is why two open rifts used to end the run. It stays
    // below Hakkar (different map, can never contend) and below the phantom-combat
    // hatch. See DungeonEvent::drivesInCombat.
    EXPECT_GT(DcRel::HakkarLootBloodCombat,  DcRel::EventDueCombat);
    EXPECT_GT(DcRel::EventDueCombat,         DcRel::PullManeuver);
    EXPECT_GT(DcRel::EventDueCombat,         DcRel::AssistCampCombat);
    EXPECT_GT(DcRel::HakkarLootBloodCombat,  DcRel::PullManeuver);
    // Hazard vacate (survival, any role) sits below the camp owners (60) and above
    // the role repositions — it must win over normal combat movement but not fight
    // the camp/Hakkar orchestration.
    EXPECT_GT(DcRel::PullManeuver,           DcRel::HazardVacate);
    EXPECT_GT(DcRel::HazardVacate,         DcRel::HealReposition);
    EXPECT_GT(DcRel::HealReposition,         DcRel::AssistCampCombat);
    EXPECT_GT(DcRel::AssistCampCombat,       DcRel::RegroupCombat);
}

// Hazard vacate is dual-engine. In the NON-combat engine it must outrank the whole
// driving ladder (so a bot leaves the Destroyed Sentinel's pulse instead of
// looting/resting/advancing on the death spot) but stay below the terminal death/
// chat bailouts.
TEST(DungeonClearRelevanceTest, HazardVacateOutranksNonCombatDrivers)
{
    EXPECT_GT(DcRel::HazardVacate, DcRel::AllCleared);
    EXPECT_GT(DcRel::HazardVacate, DcRel::HealReposition);
    EXPECT_GT(DcRel::HazardVacate, DcRel::Advance);
    EXPECT_GT(DcRel::HazardVacate, DcRel::FilterLoot);
    EXPECT_GT(DcRel::HazardVacate, DcRel::NeedsRest);
    EXPECT_LT(DcRel::HazardVacate, DcRel::PartyDied);
    EXPECT_LT(DcRel::HazardVacate, DcRel::Chat);
}

// HALLS OF REFLECTION's forward step sits ONE RUNG ABOVE the generic vacate, and
// the gap has to be exactly that: they answer the SAME danger — the Lich King's
// Remorseless Winter ring — with opposite bearings, so whichever wins the tick is
// the direction the bot actually moves.
//
// The generic vacate retreats radially, away from the emitter. On this encounter
// the party is walking a path that runs -x -y with the Lich King following, and
// every 2 seconds each player whose (p.x - lk.x) + (p.y - lk.y) exceeds 20 takes
// 10 000 damage plus a KNOCKBACK THAT THROWS THEM FURTHER BEHIND. So for a bot
// that is already behind him, "away from him" is deeper into a self-reinforcing
// rule. The forward step has to outrank it, on the one map where both are armed.
//
// It stays under the camp owners (60), which never contend — both of map 668's
// events carry OwnsThePull, so there is no camp — and under the terminal bailouts.
TEST(DungeonClearRelevanceTest, TheHorForwardStepOutranksTheRadialVacateItReplaces)
{
    EXPECT_GT(DcRel::HorStayAhead, DcRel::HazardVacate)
        << "the two answer the same emitter with opposite bearings; if the radial vacate "
           "wins the tick it walks a bot that is behind the Lich King further behind him";
    EXPECT_LT(DcRel::HorStayAhead, DcRel::PullManeuver);
    EXPECT_LT(DcRel::HorStayAhead, DcRel::StayAtCamp);
    EXPECT_LT(DcRel::HorStayAhead, DcRel::PartyDied);
    EXPECT_LT(DcRel::HorStayAhead, DcRel::Chat);

    // Above everything it has to beat to move a follower mid-fight: the stock
    // combat movers, and DC's own role repositions.
    EXPECT_GT(DcRel::HorStayAhead, DcRel::HealReposition);
    EXPECT_GT(DcRel::HorStayAhead, DcRel::AssistCampCombat);
    EXPECT_GT(DcRel::HorStayAhead, DcRel::Advance);
}

// THE OCULUS rider rung flies every member's drake. It has to outrank every DC
// rung a rider could otherwise be handed in either engine, and it sits half a rung
// over the Hakkar suppressor rather than on it so the ladder carries no new tie.
TEST(DungeonClearRelevanceTest, TheOculusRiderOutranksEveryLadderItFliesOver)
{
    EXPECT_GT(DcRel::OcRider, DcRel::HakkarSuppressorCombat) << "a tie with no partition test";
    EXPECT_GT(DcRel::OcRider, DcRel::EventDueCombat);
    EXPECT_GT(DcRel::OcRider, DcRel::PullManeuver);
    EXPECT_GT(DcRel::OcRider, DcRel::StayAtCamp);
    EXPECT_GT(DcRel::OcRider, DcRel::HazardVacate);
    EXPECT_GT(DcRel::OcRider, DcRel::StrandedRecovery);
    EXPECT_GT(DcRel::OcRider, DcRel::RezParty);
    EXPECT_GT(DcRel::OcRider, DcRel::EventDue);
    EXPECT_GT(DcRel::OcRider, DcRel::FollowTank);
    EXPECT_GT(DcRel::OcRider, DcRel::Advance);
    EXPECT_LT(DcRel::OcRider, DcRel::BreakStuckCombat);
    EXPECT_LT(DcRel::OcRider, DcRel::PartyDied);
    EXPECT_LT(DcRel::OcRider, DcRel::Chat);
}

// The pull maneuver is dual-engine for the same reason HazardVacate and
// BreakStuckCombat are: stock `drop target` (99) can move a still-flagged bot onto
// the NON-combat engine, and every watchdog the maneuver owns lives inside its
// action — so a combat-only registration lets an LOS-break drag freeze in a holding
// phase with no clock running at all. In the non-combat ladder 60 must clear the
// driving rungs it is unblocking and stay under the phantom-combat hatch.
TEST(DungeonClearRelevanceTest, PullManeuverIsDualEngineAndClearsBothLadders)
{
    // Above every non-combat driving rung it now shares an engine with.
    EXPECT_GT(DcRel::PullManeuver, DcRel::HazardVacate);
    EXPECT_GT(DcRel::PullManeuver, DcRel::StrandedRecovery);
    EXPECT_GT(DcRel::PullManeuver, DcRel::HealReposition);
    EXPECT_GT(DcRel::PullManeuver, DcRel::Pull);
    EXPECT_GT(DcRel::PullManeuver, DcRel::AssistCamp);
    EXPECT_GT(DcRel::PullManeuver, DcRel::HoldAtCamp);
    EXPECT_GT(DcRel::PullManeuver, DcRel::Advance);
    // Below the phantom-combat hatch, which is inert whenever anything is fightable
    // and must win the tick when it does fire.
    EXPECT_LT(DcRel::PullManeuver, DcRel::BreakStuckCombat);
    // Below the terminal bailouts on both sides.
    EXPECT_LT(DcRel::PullManeuver, DcRel::Chat);
    EXPECT_LT(DcRel::PullManeuver, DcRel::PartyDied);
}

// Contribution-gated combat regroup (Option B) is pinned BELOW the stock combat
// movers, unlike the old distance-tether rung that had to sit above them. Stock
// ACTION_MOVE / MoveChase is 30; the regroup only fires when stock movement has no
// target to chase, so anything stock can do wins the tick.
TEST(DungeonClearRelevanceTest, RegroupCombatSitsBelowStockMovers)
{
    constexpr float kStockMove = 30.0f;  // ACTION_MOVE / MoveChase (stock playerbots)
    EXPECT_LT(DcRel::RegroupCombat,    kStockMove);
    EXPECT_LT(kStockMove,              DcRel::AssistCampCombat);
    EXPECT_LT(DcRel::AssistCampCombat, DcRel::HealReposition);
    // The hazard vacate must out-drive stock melee movement (else the bot chases
    // its target right back into the pulse) and out-rank the role repositions.
    EXPECT_LT(kStockMove,              DcRel::HazardVacate);
    EXPECT_LT(DcRel::HealReposition,   DcRel::HazardVacate);
    // The combat-side objective engage (stealthed-sapper break) sits ABOVE stock
    // movers so it owns the tick and walks the tank onto the undetected sapper, and
    // BELOW the follower assist / camp owners (35) which never contend (leader-only).
    EXPECT_LT(kStockMove,                     DcRel::ObjectiveEngageCombat);
    EXPECT_LT(DcRel::ObjectiveEngageCombat,   DcRel::AssistCampCombat);
    // And the whole documented chain: RegroupCombat < move < assist < heal-reposition.
    EXPECT_LT(DcRel::RegroupCombat, DcRel::AssistCampCombat);
    EXPECT_LT(DcRel::AssistCampCombat, DcRel::HealReposition);
}

// --- ties, each with the partition that makes them safe -------------------

TEST(DungeonClearRelevanceTest, PartitionedTiesAreEqualByDesign)
{
    // Distinct trigger conditions (keyword vs death), both terminal.
    EXPECT_FLOAT_EQ(DcRel::Chat, DcRel::PartyDied);
    // Mutually exclusive by the anchor-kind check in each trigger (boss vs objective).
    EXPECT_FLOAT_EQ(DcRel::AtBoss, DcRel::AtObjective);
    // Leader-only vs follower-only.
    EXPECT_FLOAT_EQ(DcRel::BlockingTrash, DcRel::FollowTank);
    // Combat: leader-only (drag) vs follower-only (pin) — role peers.
    EXPECT_FLOAT_EQ(DcRel::PullManeuver, DcRel::StayAtCamp);
    // Engine-partitioned: RegroupCombat (29) runs ONLY in the combat engine;
    // AssistCamp (29) ONLY in the non-combat engine. They can never arbitrate on
    // the same tick, so the shared value is safe (contribution-gate rework).
    EXPECT_FLOAT_EQ(DcRel::RegroupCombat, DcRel::AssistCamp);
    // MAP-partitioned: the transit pack rung (36) is Blackwing Lair's and its
    // first test is `GetMapId() == 469`; the Hakkar suppressor (36) is the Sunken
    // Temple's. Two maps cannot both be under a bot's feet.
    EXPECT_FLOAT_EQ(DcRel::TransitPack, DcRel::HakkarSuppressor);
}

// The transit pack rung is the one that keeps the raid one body while the leader
// crosses Blackwing Lair's Suppression Rooms — and a strung-out raid there is not
// untidy, it is a run with no driver at all (every straggler holds the WHOLE party
// in combat, and DcCombatFlag::MayDrive is false while anything is engaged). So
// what it outranks is load-bearing, and so is what outranks it.
TEST(DungeonClearRelevanceTest, TransitPackOutranksTheRungsThatStringTheRaidOut)
{
    // The movers that would take a follower off the column: a whelp 40yd off the
    // line is precisely the string-out this exists to stop.
    EXPECT_GT(DcRel::TransitPack, DcRel::AssistCampCombat);   // combat engine
    EXPECT_GT(DcRel::TransitPack, DcRel::RegroupCombat);
    EXPECT_GT(DcRel::TransitPack, DcRel::AssistCamp);         // non-combat engine
    EXPECT_GT(DcRel::TransitPack, DcRel::HoldAtCamp);
    EXPECT_GT(DcRel::TransitPack, DcRel::FollowTank);
    EXPECT_GT(DcRel::TransitPack, DcRel::Advance);
    // ...and the rez rung, deliberately: walking a rezzer back through the
    // gauntlet for a corpse is a second death, not a recovery. Same call the
    // Razorgore camp (61.5) makes one room over.
    EXPECT_GT(DcRel::TransitPack, DcRel::RezParty);

    // What must still win. Stranded recovery is the floor under the whole ladder,
    // heal-reposition is healer-only and above every driver, and in the combat
    // engine the camp owners / hazard vacate / the phantom hatch all outrank it.
    EXPECT_GT(DcRel::StrandedRecovery, DcRel::TransitPack);
    EXPECT_GT(DcRel::HealReposition,   DcRel::TransitPack);
    EXPECT_GT(DcRel::HazardVacate,     DcRel::TransitPack);
    EXPECT_GT(DcRel::PullManeuver,     DcRel::TransitPack);
    EXPECT_GT(DcRel::StayAtCamp,       DcRel::TransitPack);
    EXPECT_GT(DcRel::BreakStuckCombat, DcRel::TransitPack);

    // The leader's own transit driver runs on the COMBAT event rung (61) and must
    // outrank the pack rung: the leader is exempt from the pack anyway, but if the
    // two ever landed on one bot, moving the column is what matters.
    EXPECT_GT(DcRel::EventDueCombat, DcRel::TransitPack);
}

TEST(DungeonClearRelevanceTest, HealRepositionAboveLeaderDriversIsRolePartitioned)
{
    // In BOTH engines heal-reposition (41, healer-only) sits above the leader-only
    // pull/hakkar/at-boss drivers. It never contends because of the healer-vs-leader
    // role split — asserted so a future non-healer trigger at 41 is caught.
    //
    // THE SPLIT DOES NOT COVER EVERYTHING BELOW 41. RezParty runs on ALL bots,
    // healers first, so on the elected rezzer these two are the same bot and DO
    // contend — see RezPartyRankAloneDoesNotProtectTheApproach. Role partitioning
    // is a claim about the rungs listed here, not a general one.
    EXPECT_GT(DcRel::HealReposition, DcRel::Pull);
    EXPECT_GT(DcRel::HealReposition, DcRel::HakkarSuppressor);
    EXPECT_GT(DcRel::HealReposition, DcRel::AtBoss);
}

TEST(DungeonClearRelevanceTest, PreviouslyTiedRungsAreNowStrictlyOrdered)
{
    // The two ties that were NOT role/map/anchor partitioned are broken so
    // ordering never depends on trigger registration order.
    EXPECT_NE(DcRel::HakkarFlame, DcRel::Pull);
    EXPECT_NE(DcRel::NeedsRest,   DcRel::RoomTrash);
}

// The pull rung OUTRANKS the objective rung, which is correct — a pull in flight
// must not be interrupted by the objective driver — but it is also what turns any
// stuck pull verdict into a stalled run: while the pull action owns the tick, an
// anchored event never gets to drive its own steps.
//
// tr-20260817-100413-43/44/45 (Shattered Halls) is the live case. The governor
// latched decision == PatrolHold, then the persistent "Sweep the assassin hallway"
// event armed the pull stand-down — which stopped the governor from ever running
// again, so nothing could move the code off PatrolHold. The pull trigger keeps its
// rung live on that code, the pull action re-planted the tank every tick, and the
// event's own rung 5 points below never fired again: 913s parked, no_progress.
//
// The fix is that the stand-down now CLEARS the verdict (DcPullContext::
// ClearDynamicVerdict) and the trigger reads the same stand-down. This test pins
// the ordering the fix assumes: pull above objective, so "the pull rung is live"
// always means "the event is not driving".
TEST(DungeonClearRelevanceTest, PullOutranksTheObjectiveDriverItCanStarve)
{
    EXPECT_GT(DcRel::Pull, DcRel::AtObjective);
    EXPECT_GT(DcRel::Pull, DcRel::AtBoss);
    // The event-driven rungs sit in the same band, so a stuck pull starves them
    // all alike — the stand-down has to be authoritative, not advisory.
    EXPECT_GT(DcRel::Pull, DcRel::EventDue);
}
