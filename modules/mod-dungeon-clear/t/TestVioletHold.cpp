/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "gtest/gtest.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <vector>

#include "Ai/Dungeon/DungeonClear/Data/DcEventDoorRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/DcHazardRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/DcNeverTargetRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonBossInfo.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonEventRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"
#include "Ai/Dungeon/DungeonClear/Overrides/BossRosterRegistry.h"
#include "Ai/Dungeon/DungeonClear/Overrides/ObjectiveHookRegistry.h"
#include "TestRun/DcTestDungeonRegistry.h"

// The Violet Hold (map 608) — the authored-data lints for a dungeon whose whole
// clear is four objectives and one wave driver.
//
// Suite name deliberately begins DungeonEvent so it is picked up by the
// `DungeonEvent*` filter that t/run_tests.sh and .github/workflows/tests.yml both
// use; a suite named VioletHold* would build, pass locally, and never run in CI.
//
// Every number checked here is either read out of the core
// (src/server/scripts/Northrend/VioletHold/*) or measured against the live 608
// mmtile / world DB. The reasoning lives in Data/Events/VioletHoldEvents.cpp and
// Overrides/VioletHoldDriver.cpp; these tests exist so an edit that drops one of
// those properties fails loudly at author time instead of silently costing a run.

namespace
{
    constexpr uint32 VH_MAP = 608;

    // instance_violet_hold's goData, plus the four empty cells that are on the map
    // and never opened. Every one is GAMEOBJECT_TYPE_DOOR with lockId 0.
    constexpr uint32 kVhDoors[] =
    {
        191556, 191557, 191558, 191559, 191560, 191562,
        191563, 191564, 191565, 191566, 191606, 191722, 191723,
    };

    // The 12 sealed Cell spawns (world DB, map 608). No objective anchor may land
    // inside one: that was the whole defect in the derived roster.
    struct Cell { float x, y, z; };
    constexpr Cell kVhCells[] =
    {
        { 1908.06f, 844.89f, 41.14f },  // 191556 Xevozz
        { 1921.51f, 797.13f, 41.58f },  // 191557 empty
        { 1954.73f, 822.58f, 57.17f },  // 191558 empty
        { 1957.50f, 803.28f, 57.17f },  // 191559 empty
        { 1954.31f, 784.77f, 57.17f },  // 191560 empty
        { 1854.57f, 860.96f, 47.64f },  // 191562 Erekem Guard 2
        { 1892.01f, 871.24f, 47.64f },  // 191563 Erekem Guard 1
        { 1872.45f, 869.00f, 47.64f },  // 191564 Erekem
        { 1931.87f, 859.01f, 54.92f },  // 191565 Zuramat
        { 1847.81f, 752.48f, 49.30f },  // 191566 Lavanthor
        { 1895.07f, 733.72f, 57.67f },  // 191606 Moragg
        { 1938.43f, 754.70f, 28.78f },  // 191722 Ichoron
    };

    // The last TWO waypoints of all six violet_hold.h trash paths — the funnel the
    // camp has to straddle. (The final points are the two convergence points; the
    // penultimate ones are where each path turns onto the door landing.)
    struct Pt { float x, y; };
    constexpr Pt kVhFunnel[] =
    {
        { 1858.953735f, 810.048950f },  // FirstPortalTrashWPs[4]
        { 1843.707153f, 805.807739f },  // FirstPortalTrashWPs[5]        convergence A
        { 1860.843384f, 806.645020f },  // SecondPortalTrashWPs2[6]
        { 1857.811890f, 796.765564f },  // FourthPortalTrashWPs[7]
        { 1845.577759f, 800.681152f },  // FourthPortalTrashWPs[8]       convergence B
        { 1861.541504f, 804.149780f },  // SixthPoralTrashWPs[2]
        { 1843.567017f, 804.288208f },  // SixthPoralTrashWPs[3]
    };

    BossRosterPatch const* VhPatch()
    {
        for (BossRosterPatch const& p : BossRosterRegistry::AllPatches())
            if (p.mapId == VH_MAP)
                return &p;
        return nullptr;
    }

    bool Contains(std::vector<uint32> const& v, uint32 e)
    {
        return std::find(v.begin(), v.end(), e) != v.end();
    }

    float Dist2d(float ax, float ay, float bx, float by)
    {
        float const dx = ax - bx, dy = ay - by;
        return std::sqrt(dx * dx + dy * dy);
    }
}

// --- the wave driver's event, pinned to its exact shape -------------------
// Every one of these properties is a live failure the Black Morass already paid
// for, so an edit that drops one should fail with intent rather than as a silent
// behaviour change.
TEST(DungeonEventVioletHoldTest, WaveEventIsARepeatableOptionalCombatDriver)
{
    DungeonEvent const* ev = DungeonEventRegistry::Find(VH_MAP, /*eventId*/ 5);
    ASSERT_NE(ev, nullptr) << "Violet Hold (608) event 5 (repel the wave) is missing";

    EXPECT_EQ(ev->activation, EventActivation::Conditional);
    EXPECT_TRUE(static_cast<bool>(ev->condition)) << "the wave gate predicate must be bound";

    // 18 waves: the condition going false is the only "done".
    EXPECT_TRUE(ev->repeatable) << "wave event must be Repeatable (18 waves)";
    // A wipe / corpse-run must never hard-stall the run for a human — and this
    // instance is fully recoverable, so a stall would be a stall over nothing.
    EXPECT_FALSE(ev->required) << "wave event must be Optional (a timeout re-fires it fresh)";
    // A keeper portal pumps 3-4 adds every 20s forever; the party never leaves
    // combat, so a non-combat-only driver never runs once it falls behind.
    EXPECT_TRUE(ev->drivesInCombat) << "wave event must DrivesInCombat (continuous siege)";
    // Portals sit 52-86yd from the camp: the driver issues its own long-haul
    // splines and the per-tick hold would cancel each one.
    EXPECT_TRUE(ev->stepsOwnMovement) << "wave event must StepsOwnMovement (long-haul splines)";

    // ONE step, and it must be the driver hook. The priority this encounter needs
    // — always prefer a released boss, then the seal, then the keeper — is a
    // standing preference re-evaluated per tick, not a sequence.
    ASSERT_EQ(ev->steps.size(), 1u)
        << "the wave event must be exactly one Custom step (the driver hook); a step"
           " list cannot express the boss-then-seal-then-keeper priority";
    EXPECT_EQ(ev->steps[0].kind, EventStepKind::Custom);
    EXPECT_EQ(ev->steps[0].hookId, DcVioletHold::HOOK_DRIVE_WAVE);
    EXPECT_TRUE(ObjectiveHookRegistry::Has(DcVioletHold::HOOK_DRIVE_WAVE))
        << "hook " << DcVioletHold::HOOK_DRIVE_WAVE << " (VhDriveWave) must be registered";
}

// --- the four objectives ---------------------------------------------------
TEST(DungeonEventVioletHoldTest, ObjectiveEventsArePersistentHookDriversWithDistinctHooks)
{
    struct Row { uint32 eventId; uint32 hookId; };
    Row const rows[] = {
        { 1, DcVioletHold::HOOK_START },
        { 2, DcVioletHold::HOOK_DEFEND_1ST },
        { 3, DcVioletHold::HOOK_DEFEND_2ND },
        { 4, DcVioletHold::HOOK_DEFEND_CYANIGOSA },
    };

    std::set<uint32> hooks;
    for (Row const& r : rows)
    {
        DungeonEvent const* ev = DungeonEventRegistry::Find(VH_MAP, r.eventId);
        ASSERT_NE(ev, nullptr) << "Violet Hold (608) event " << r.eventId << " is missing";
        EXPECT_EQ(ev->activation, EventActivation::Anchored);
        // Every wave fight is a >1s combat gap that would rewind a non-persistent
        // step list back to the arrival MoveTo.
        EXPECT_TRUE(ev->persistent) << "event " << r.eventId << " must be Persistent";
        EXPECT_TRUE(ev->stepsOwnMovement) << "event " << r.eventId << " must StepsOwnMovement";

        // Arrival bump, then the hook. The MoveTo is what makes the persistent
        // event go sticky at the anchor; the Custom step is the whole behaviour.
        ASSERT_EQ(ev->steps.size(), 2u) << "event " << r.eventId;
        EXPECT_EQ(ev->steps[0].kind, EventStepKind::MoveTo);
        EXPECT_EQ(ev->steps[1].kind, EventStepKind::Custom);
        EXPECT_EQ(ev->steps[1].hookId, r.hookId);
        EXPECT_TRUE(ObjectiveHookRegistry::Has(r.hookId))
            << "hook " << r.hookId << " must be registered";

        // THE reason there are three defend hooks and not one: a Custom step is
        // handed a DEFAULT-CONSTRUCTED DungeonBossInfo (DungeonEventExecutor::
        // RunStep), so encounterIndex reads 0 for all three and a shared hook
        // could not tell which boss-state slot to watch. It would complete all
        // three objectives on the first prisoner's death.
        EXPECT_TRUE(hooks.insert(r.hookId).second)
            << "objective events 1-4 must use DISTINCT hook ids; " << r.hookId
            << " is reused, which makes the later objectives complete on the"
               " earlier one's boss-state slot";
    }

    // The three defend objectives share the STAGING anchor; the start objective
    // does not (it is at the entrance, by Sinclari).
    //
    // Staging, not the door camp, and the two must not drift apart: the anchor is
    // where the ordinary boss-navigation pipeline delivers the party, and
    // VhDriveDefend's own garrison is where the driver keeps it between waves. A
    // portal dying opens the next one three seconds later, so the party crosses
    // that boundary eighteen times a run; if the anchor and the garrison named
    // different points the party would be tugged between them on every crossing.
    for (uint32 id : {2u, 3u, 4u})
    {
        DungeonEvent const* ev = DungeonEventRegistry::Find(VH_MAP, id);
        ASSERT_NE(ev, nullptr);
        EXPECT_NEAR(ev->steps[0].x, DcVioletHold::STAGE_X, 0.01f);
        EXPECT_NEAR(ev->steps[0].y, DcVioletHold::STAGE_Y, 0.01f);
        EXPECT_NEAR(ev->steps[0].z, DcVioletHold::STAGE_Z, 0.01f);
    }
}

// --- the roster patch ------------------------------------------------------
// The `remove` half is load-bearing: instance_encounters names Erekem (29315) and
// Moragg (29316) as the First/Second Prisoner credits, both are real world spawns,
// and BossSpawnIndex therefore emits anchors AT THEIR CELL COORDINATES — inside
// sealed geometry no bot can path into. They are also simply the wrong bosses: the
// released pair is rolled per instance.
TEST(DungeonEventVioletHoldTest, RosterDropsTheSealedCellAnchorsAndAddsFourObjectives)
{
    BossRosterPatch const* p = VhPatch();
    ASSERT_NE(p, nullptr) << "map 608 has no BossRosterPatch — the derived roster would"
                             " anchor the clear inside two sealed cells";

    EXPECT_TRUE(Contains(p->remove, 29315u)) << "Erekem's cell anchor must be removed";
    EXPECT_TRUE(Contains(p->remove, 29316u)) << "Moragg's cell anchor must be removed";

    ASSERT_EQ(p->add.size(), 4u) << "four objectives: enter, then one per boss-state slot";
    for (uint32 i = 0; i < 4; ++i)
    {
        DungeonBossInfo const& o = p->add[i];
        EXPECT_EQ(o.kind, DungeonAnchorKind::Objective);
        EXPECT_EQ(o.mapId, VH_MAP);
        EXPECT_EQ(o.entry, BossRosterRegistry::ObjectiveEntry(i + 1));
        EXPECT_EQ(o.eventId, i + 1) << "objective " << i + 1 << " must drive its own event";
        EXPECT_EQ(o.orderOverride, static_cast<int32>(i + 1));
        // The objective's own on-arrive hook stays 0: the behaviour lives in the
        // event's Custom step, which is what carries the timeout and the
        // persistence.
        EXPECT_EQ(o.onArriveHook, 0u);
        // Persistent anchored events still obey arriveRadius on step 1, so these
        // have to stay lenient.
        EXPECT_GE(o.arriveRadius, 8.0f) << "objective " << i + 1 << " arriveRadius is tight"
                                           " enough to strand a persistent event at step 1";

        // NO ANCHOR INSIDE A CELL. This is the defect the patch exists for; a
        // future edit that re-derives an anchor from a spawn would land back
        // inside one.
        for (Cell const& c : kVhCells)
            EXPECT_GT(Dist2d(o.x, o.y, c.x, c.y), 15.0f)
                << "objective " << i + 1 << " (" << o.name << ") is within 15yd of a sealed"
                   " Cell gameobject — that is the failure the roster patch removes";
    }
}

// --- the entry anchor -------------------------------------------------------
// The whole start sequence is one gossip on an NPC standing 4yd inside the door,
// and the only thing that has to reach her is the tank. Live runs
// tr-20260826-224756-1 and -224909-2 both stalled 5+ minutes at this objective
// having never moved a yard, with the tank parked exactly on a 7.3yd anchor. The
// hook bug that caused it is fixed; this lint removes the dependency, by pinning
// the anchor close enough that ARRIVING is already enough.
TEST(DungeonEventVioletHoldTest, EntryAnchorLandsInSinclarisInteractRange)
{
    // Lieutenant Sinclari 30658, world-DB spawn on map 608.
    constexpr float SINCLARI_X = 1830.95f, SINCLARI_Y = 799.46f;

    BossRosterPatch const* p = VhPatch();
    ASSERT_NE(p, nullptr);
    ASSERT_FALSE(p->add.empty());
    DungeonBossInfo const& entry = p->add[0];

    // Player::GetNPCIfCanInteractWith gates on IsWithinDistInMap(
    // INTERACTION_DISTANCE = 5.5yd), which is bounding-radius aware, so
    // centre-to-centre it is 5.5 plus both combat reaches. 5.0 is inside that with
    // room for a tick of drift.
    EXPECT_LT(Dist2d(entry.x, entry.y, SINCLARI_X, SINCLARI_Y), 5.0f)
        << "the 'Enter the Violet Hold' anchor is far enough from Sinclari that the"
           " run depends on the hook's walk-in for a gap the ordinary boss-navigation"
           " arrival could have closed for free";

    // Still inside the Prison Seal (x 1822.59) — the anchor is where the party
    // stands when the door shuts behind them.
    EXPECT_GT(entry.x, 1822.59f);

    // And the event's own arrival MoveTo must agree with the anchor, or the
    // persistent event's step 0 sends the tank somewhere the objective did not.
    DungeonEvent const* ev = DungeonEventRegistry::Find(VH_MAP, /*eventId*/ 1);
    ASSERT_NE(ev, nullptr);
    ASSERT_FALSE(ev->steps.empty());
    EXPECT_NEAR(ev->steps[0].x, entry.x, 0.01f);
    EXPECT_NEAR(ev->steps[0].y, entry.y, 0.01f);
    EXPECT_NEAR(ev->steps[0].z, entry.z, 0.01f);
}

// --- the door camp ----------------------------------------------------------
// CAMP is the EMERGENCY position — rule 2 of VhDriveWave, where the party goes
// when the Prison Door Seal is actually being drained. Everything asserted here is
// about that job: it has to be on the funnel every add walks in on, inside the
// seal, on the landing rather than the ramp, and close enough to the convergence
// point that a mob standing there is inside the driver's force-pull. If it drifts
// off the funnel the driver still "works" and the run still loses, because the
// adds finish their walk to the seal beside a party that never engaged them.
TEST(DungeonEventVioletHoldTest, CampStraddlesTheTrashFunnelInsideTheSeal)
{
    using namespace DcVioletHold;

    // INSIDE the Prison Seal (191723 at x 1822.59): the door shuts behind the
    // party 15s after the gossip, and a camp on the wrong side of it would leave
    // the party locked out of their own encounter.
    EXPECT_GT(CAMP_X, 1822.59f + 10.0f)
        << "the camp must be well inside the Prison Seal at x 1822.59";

    // On the flat landing at the top of the entrance ramp, not on the ramp: the
    // ramp runs z 38.6 at x~1869.8 up to z 44.0 at x~1861.5, and the landing sits
    // at z ~44.1 (column-probed against the live 608 mmtile: exactly one walkable
    // surface at z 44.23 under this point).
    EXPECT_NEAR(CAMP_Z, 44.1f, 0.5f);
    EXPECT_LT(CAMP_X, 1861.5f) << "the camp must be past the top of the ramp, on the landing";

    // BETWEEN the adds and the door, not on top of either. Close enough to the
    // convergence midpoint that a mob arriving there is inside the driver's 30yd
    // force-pull from the camp; far enough that the party is not standing in the
    // Prison Door Seal's own footprint.
    float const toSeal = Dist2d(CAMP_X, CAMP_Y, SEAL_X, SEAL_Y);
    EXPECT_GT(toSeal, 5.0f);
    EXPECT_LT(toSeal, 20.0f) << "the camp is too far from the convergence point for the"
                                " force-pull radius to reach a mob that has arrived there";

    // ON THE FUNNEL. Every one of the six paths' last two waypoints must pass
    // within a short walk of the camp, or the party is not actually intercepting.
    for (Pt const& w : kVhFunnel)
        EXPECT_LT(Dist2d(CAMP_X, CAMP_Y, w.x, w.y), 15.0f)
            << "trash waypoint (" << w.x << ", " << w.y << ") is more than 15yd from the"
               " camp — the wave can reach the seal without passing the party";

    // The arena centroid used by the wave event's proximity gate must actually be
    // in the arena and reachable from the camp within the gate's range, or the
    // driver would never be due where it is standing.
    EXPECT_LT(Dist2d(CAMP_X, CAMP_Y, ARENA_X, ARENA_Y), EVENT_DUE_RANGE);
}

// --- the staging point ------------------------------------------------------
// STAGE is where the party waits when no portal is open, and it is the anchor the
// three defend objectives deliver to. Its whole reason to exist is that it is
// CLOSER TO THE NEXT PORTAL than the door is — the party stations on the live
// portal now, and every yard between the waiting spot and the rim is time a keeper
// portal spends pumping unopposed. This test is what stops it silently drifting
// back toward the door on some later edit.
TEST(DungeonEventVioletHoldTest, StagingPointIsCloserToEveryPortalThanTheDoorCamp)
{
    using namespace DcVioletHold;

    // The six rim portal spots (violet_hold.h PortalLocations).
    struct Pt3 { float x, y; };
    constexpr Pt3 kPortals[] =
    {
        { 1877.51f, 850.104f },
        { 1918.37f, 853.437f },
        { 1936.07f, 803.198f },
        { 1927.61f, 758.436f },
        { 1890.64f, 753.471f },
        { 1908.31f, 809.657f },
    };

    float stageWorst = 0.0f, campWorst = 0.0f;
    for (Pt3 const& p : kPortals)
    {
        float const fromStage = Dist2d(STAGE_X, STAGE_Y, p.x, p.y);
        float const fromCamp  = Dist2d(CAMP_X, CAMP_Y, p.x, p.y);
        EXPECT_LT(fromStage, fromCamp)
            << "portal (" << p.x << ", " << p.y << ") is no closer to the staging point"
               " than to the door camp — staging in the middle of the room buys nothing";
        stageWorst = std::max(stageWorst, fromStage);
        campWorst  = std::max(campWorst, fromCamp);
    }
    EXPECT_LT(stageWorst, campWorst);

    // On the arena floor at the foot of the ramp, not up on the landing: the
    // landing is z ~44.1 and the middle-room floor is z ~38.4 (column-probed
    // against the live 608 mmtile: exactly one walkable surface, z 38.89).
    EXPECT_NEAR(STAGE_Z, 38.44f, 1.0f);
    EXPECT_GT(STAGE_X, 1861.5f) << "the staging point must be down the ramp, on the"
                                   " arena floor, not on the door landing";

    // Inside the arena the wave event's proximity gate is measured against, and
    // well inside the driver's grid-scan radius of every rim portal, or the driver
    // would be standing somewhere it cannot see the encounter from.
    EXPECT_LT(Dist2d(STAGE_X, STAGE_Y, ARENA_X, ARENA_Y), EVENT_DUE_RANGE);
    for (Pt3 const& p : kPortals)
        EXPECT_LT(Dist2d(STAGE_X, STAGE_Y, p.x, p.y), ARENA_SCAN);

    // Still on the trash route. Every add walks from a rim portal to the door, so
    // a party waiting in the middle of the room is not merely closer to the next
    // portal — it is astride the path the current wave's leftovers are taking.
    // Asserted as "between the portals and the door": the staging point must sit
    // on the door side of the arena centroid and the door side of every portal.
    EXPECT_LT(Dist2d(STAGE_X, STAGE_Y, CAMP_X, CAMP_Y),
              Dist2d(ARENA_X, ARENA_Y, CAMP_X, CAMP_Y) + 10.0f);
}

// --- the three entry lists --------------------------------------------------
// Complete, exclusive and mutually disjoint in the ways the driver depends on.
TEST(DungeonEventVioletHoldTest, WaveEntryListsAreCompleteExclusiveAndDisjoint)
{
    std::vector<uint32> const& wave = VioletHoldWaveEntries();
    std::vector<uint32> const& keepers = VioletHoldKeeperEntries();
    std::vector<uint32> const& prisoners = VioletHoldPrisonerEntries();

    ASSERT_FALSE(wave.empty());
    ASSERT_FALSE(keepers.empty());
    ASSERT_FALSE(prisoners.empty());

    // COMPLETE — every entry npc_vh_teleportation_portal can field, from
    // violet_hold.cpp's two RAND() lists, plus the boss summons the driver has to
    // see. A miss here reads as "the arena is quiet" and hands the tick back to
    // the garrison while that mob walks into the seal.
    uint32 const mustBeInWave[] = {
        // EVENT_SUMMON_KEEPER_TRASH's eight-way RAND, both id variants of each.
        30661, 30961,  // Azure Invader
        30662, 30962,  // Azure Spellbreaker
        30663, 30918,  // Azure Binder
        30664, 30963,  // Azure Mage Slayer
        // EVENT_SUMMON_ELITES' four-way RAND.
        30666, 30667, 30668, 32191,  // Captain / Sorceror / Raider / Stalker
        // EVENT_SUMMON_KEEPER_OR_GUARDIAN's three-way RAND — the off-switch.
        30660, 30695, 30893,         // Portal Guardian / Portal Keeper x2
        // Boss summons the driver has to see.
        29321,                       // Ichor Globule   (Ichoron)
        29271, 32582,                // Ethereal Sphere (Xevozz)
        31134,                       // Cyanigosa
    };
    for (uint32 e : mustBeInWave)
        EXPECT_TRUE(Contains(wave, e)) << "wave entry " << e << " is missing from"
                                          " VioletHoldWaveEntries()";
    EXPECT_EQ(wave.size(), sizeof(mustBeInWave) / sizeof(mustBeInWave[0]))
        << "VioletHoldWaveEntries() has grown or shrunk — update this lint with the reason";

    // EXCLUSIVE — nothing here may be probed by mere aliveness, each for its own
    // reason. Admitting a WORLD SPAWN is the dangerous half: the predicate would
    // read true from map load and hand the wave driver the tick before the party
    // had even walked in.
    uint32 const mustNotBeInWave[] = {
        14881,  // Spider          — ambient fauna, faction 190
        2110,   // Black Rat       — ambient critter
        4075,   // Rat             — ambient critter
        31079,  // Azure Saboteur  — IMMUNE_TO_PC, unkillable (never-target row)
        31011,  // Teleportation Portal — probed separately by the predicate
        30896,  // Prison Door Seal     — the thing being drained
        30857,  // Defense Dummy Target — NOT_SELECTABLE | immune
        30837,  // Defense System       — the crystals' emitter
        29364, 29365,  // Void Sentry   — SetPhaseMask(16), invisible to the party
        31118,  // Azure Raider (faction-35 SCENERY — a different entry from 30668)
        29276, 29425, 30883,  // summon / controller triggers
        30658, 32204, 30659,  // Sinclari and the Violet Hold Guards
    };
    for (uint32 e : mustNotBeInWave)
        EXPECT_FALSE(Contains(wave, e)) << "entry " << e << " must NOT be in"
                                           " VioletHoldWaveEntries()";

    // The six caged prisoners and Erekem's guards are world spawns present from
    // map load, so they are probed for RELEASE, never for aliveness.
    for (uint32 e : { 29266u, 29312u, 29313u, 29314u, 29315u, 29316u, 29395u })
    {
        EXPECT_TRUE(Contains(prisoners, e)) << "prisoner entry " << e << " is missing";
        EXPECT_FALSE(Contains(wave, e))
            << "prisoner entry " << e << " is a WORLD SPAWN and must not be probed by"
               " aliveness — the wave event would be due from map load";
    }

    // Keepers are the off-switch and are a strict subset of the wave list (the
    // driver both selects them and counts them as "the arena is busy").
    for (uint32 k : keepers)
    {
        EXPECT_TRUE(Contains(wave, k)) << "keeper " << k << " must also be a wave entry";
        EXPECT_FALSE(Contains(prisoners, k));
    }
    EXPECT_EQ(keepers.size(), 3u) << "exactly three: Portal Guardian 30660 and Portal"
                                     " Keeper 30695 / 30893";

    // Cyanigosa is in the wave list ON PURPOSE (a TempSummon, so aliveness is a
    // sound probe, and the driver should own the tick before her 12.5s
    // NON_ATTACKABLE window ends) but she is a BOSS, so she must not be reachable
    // through the prisoner list as well — that would double-count her.
    EXPECT_TRUE(Contains(wave, 31134u));
    EXPECT_FALSE(Contains(prisoners, 31134u));
}

// --- safety rails -----------------------------------------------------------
TEST(DungeonEventVioletHoldTest, SaboteurIsNeverAClearTargetButWaveMobsAre)
{
    // 31079 carries unit_flags 768 = IMMUNE_TO_PC | IMMUNE_TO_NPC and the script
    // never clears them, so it cannot be killed at all — and it walks the entire
    // room on waves 6 and 12, which is exactly when the driver is otherwise idle.
    EXPECT_TRUE(DcNeverTargetRegistry::IsNeverTarget(608, 31079));

    // Scalpel, not a species ban: the wave mobs the party DOES have to kill must
    // all still be targets, and the row must not leak onto another map.
    for (uint32 e : { 30661u, 30662u, 30663u, 30664u, 30660u, 30695u, 30893u,
                      29321u, 31134u, 29315u, 29316u })
        EXPECT_FALSE(DcNeverTargetRegistry::IsNeverTarget(608, e)) << "entry " << e;
    EXPECT_FALSE(DcNeverTargetRegistry::IsNeverTarget(600, 31079));
}

TEST(DungeonEventVioletHoldTest, CellsAndPrisonSealAreScriptOnly)
{
    // All thirteen are GAMEOBJECT_TYPE_DOOR with lockId 0, so
    // BotCanOpenDoorLikePlayer reads them as freely clickable. Opening a cell does
    // NOT release its boss (the release rides StartBossEncounter, which clears the
    // unit flags), so a bot click only exposes an inert NON_ATTACKABLE creature
    // the clear would then try to route to; toggling the Prison Seal breaks the
    // very seal the encounter is about.
    for (uint32 go : kVhDoors)
        EXPECT_TRUE(DcEventDoorRegistry::IsScriptOnly(go))
            << "Violet Hold door " << go << " must be script-only";

    // Per-ENTRY, never per-lock — lock 0 is shared with doors bots SHOULD open
    // all over the game (the Shadowfang Keep lesson).
    EXPECT_FALSE(DcEventDoorRegistry::IsScriptOnly(191561));  // not a VH spawn
    EXPECT_FALSE(DcEventDoorRegistry::IsScriptOnly(0));
}

TEST(DungeonEventVioletHoldTest, ActivationCrystalsAreNavigationInvisible)
{
    // Wall controls whose template is GAMEOBJECT_TYPE_DOOR, permanently in
    // GO_STATE_READY around the arena rim: the closed-door predicate reads each as
    // a shut gate standing in an open room. They block nothing and are opened by
    // spell 57804's SEND_EVENT, not by a door click.
    EXPECT_TRUE(DcEventDoorRegistry::IsNavigationIgnored(193611));
    EXPECT_TRUE(DcEventDoorRegistry::IsNavigationIgnored(193615));

    // NOT script-only: clicking one is a legitimate (if unimplemented) panic
    // valve, and marking it script-only would be the wrong reason to leave it
    // alone.
    EXPECT_FALSE(DcEventDoorRegistry::IsScriptOnly(193611));
    EXPECT_FALSE(DcEventDoorRegistry::IsScriptOnly(193615));

    // The seal and the cells are BOTH, and the pairing is the point: script-only
    // alone would be the harmful half on its own, because a flagged door the bot
    // is not entitled to open falls straight through to the auto-pause in
    // DcEngageActions' parkAndStall. The cells sit 3-9yd from the fight positions
    // their bosses are released to, so travelling to any released boss parks the
    // party beside two or three permanently-shut ones; the Prison Seal is shut for
    // most of the run and cannot be opened by a bot OR a human from outside.
    for (uint32 go : kVhDoors)
    {
        EXPECT_TRUE(DcEventDoorRegistry::IsNavigationIgnored(go))
            << "Violet Hold door " << go << " must be navigation-invisible too, or a"
               " script-only listing auto-pauses the run at it";
    }
}

TEST(DungeonEventVioletHoldTest, CyanigosaBlizzardIsAGroundHazardSizedAgainstItsAura)
{
    // Spell.dbc 58693: Effect[0] = 27 PERSISTENT_AREA_AURA, aura 3 periodic
    // damage, EffectRadiusIndex 13 = 10.0yd, amplitude 2000ms, ~1500 frost/2s,
    // 8s duration; Effect[1] is a -40% snare on the same footprint.
    DcGroundHazard const* row = DcHazardRegistry::FindGround(608, 58693);
    ASSERT_NE(row, nullptr) << "Cyanigosa's Blizzard (58693) has no DcGroundHazard row";

    // vacateRadius is the RAW aura, the rule every row here follows.
    EXPECT_FLOAT_EQ(row->vacateRadius, 10.0f);
    // The retreat aims at vacate + retreatSlack; the placement keep-out has to
    // stay BELOW that or the retreat can never find a spot PointIsHot accepts.
    EXPECT_LT(row->radius, row->vacateRadius + row->retreatSlack)
        << "the keep-out is at or past the retreat's own aim point";
    EXPECT_GT(row->retreatSlack, row->holdBand);

    // Lavanthor's Cauterizing Flames (59466) is DELIBERATELY absent: Spell.dbc
    // gives it effect 2 (school damage) + effect 6 (aura 87) at implicit target
    // 22, i.e. a one-shot AoE plus a debuff. No PERSISTENT_AREA_AURA leg means no
    // DynamicObject, and this table has nothing to key on.
    EXPECT_EQ(DcHazardRegistry::FindGround(608, 59466), nullptr)
        << "59466 is not a persistent area aura — it spawns no DynamicObject, so there"
           " is nothing for this table to key on";

    // And the row must not leak onto a neighbouring map.
    EXPECT_EQ(DcHazardRegistry::FindGround(600, 58693), nullptr);
}

// --- the test-run entry point ----------------------------------------------
TEST(DungeonEventVioletHoldTest, TestEntryPointIsOutsideThePrisonSeal)
{
    DcTestDungeonRegistry::Row const* row = DcTestDungeonRegistry::Find("vh");
    ASSERT_NE(row, nullptr) << "the 'vh' test-run row is missing";
    EXPECT_EQ(row->mapId, VH_MAP);

    // The Prison Seal (191723) is at x 1822.59 and SHUTS 15s after Sinclari's
    // gossip. A test entry point inside it would drop a re-entering party into a
    // sealed room with the encounter already running — and, worse, would put them
    // there without having triggered the start.
    EXPECT_LT(row->x, 1822.59f)
        << "the 'vh' entry point must be OUTSIDE the Prison Seal";
}
