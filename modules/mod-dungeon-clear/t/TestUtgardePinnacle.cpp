/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "gtest/gtest.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

#include "InstanceScript.h"  // GOState (via GameObjectData) — GO_STATE_ACTIVE 0 is OPEN, READY 1 is shut

#include "Ai/Dungeon/DungeonClear/Data/DcEventDoorRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/DcNeverTargetRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonClearRouteRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonEventRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"
#include "Ai/Dungeon/DungeonClear/Data/RoomAggroRegistry.h"
#include "Ai/Dungeon/DungeonClear/Overrides/BossRosterRegistry.h"
#include "Ai/Dungeon/DungeonClear/Overrides/ObjectiveHookRegistry.h"
#include "TestRun/DcTestDungeonRegistry.h"

// Utgarde Pinnacle (map 575) — the authored-data lints for the dungeon whose
// defect is a MISSING ROSTER ROW rather than any kind of geometry.
//
// Suite name deliberately begins DungeonEvent so it is picked up by the
// `DungeonEvent*` filter that t/run_tests.sh and .github/workflows/tests.yml both
// use; a suite named UtgardePinnacle* would build, pass locally, and never run in
// CI.
//
// Every number checked here is either read out of the core
// (src/server/scripts/Northrend/UtgardeKeep/UtgardePinnacle/*), read out of the
// live world DB, or derived from the real map-575 mmtiles by
// t/TestUtgardePinnacleRouteProbe — which is the suite that re-derives the
// coordinates after an mmaps regen. These tests exist so an edit that drops one
// of those properties fails at author time rather than silently costing a run,
// which on this map means a party paused against a portcullis that opens on the
// death of a boss it has not been told exists.

using namespace DcUtgardePinnacle;

namespace
{
    uint32 Obj(uint32 seq) { return BossRosterRegistry::ObjectiveEntry(seq); }

    std::vector<WaypointHint> const* Route(uint32 entry)
    {
        return DungeonClearRouteRegistry::Get(MAP_ID, DUNGEON_DIFFICULTY_NORMAL, entry);
    }

    float Dist3(float ax, float ay, float az, float bx, float by, float bz)
    {
        float const dx = ax - bx, dy = ay - by, dz = az - bz;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    // The whole authored roster for map 575 at normal difficulty, with an EMPTY
    // base list. That is deliberate: the derived half comes from DBC + the live
    // `creature` table, which a unit test has neither of, so what this exercises
    // is exactly the patch — the rows the module ADDS, which on this map is the
    // entire content of the fix.
    std::vector<DungeonBossInfo> PatchedFromEmpty(bool heroic = false)
    {
        DcDiffKey const key = DcDiffKey::Dungeon(heroic ? DUNGEON_DIFFICULTY_HEROIC
                                                        : DUNGEON_DIFFICULTY_NORMAL);
        return BossRosterRegistry::Apply(MAP_ID, key, {});
    }

    DungeonBossInfo const* FindAnchor(std::vector<DungeonBossInfo> const& list, uint32 entry)
    {
        for (DungeonBossInfo const& b : list)
            if (b.entry == entry)
                return &b;
        return nullptr;
    }
}

// --- the roster: the one row this whole file is downstream of ---------------

// THE REGRESSION THIS PINS. instance_encounters credits Svala at entry 26668,
// which has no `creature` spawn row anywhere in the DB — boss_svala.cpp does
// me->UpdateEntry(NPC_SVALA_SORROWGRAVE) ~34s into the intro, so the entry exists
// only at runtime. BossSpawnIndex::Build joins creditEntry -> spawn row, matches
// nothing, and drops the FIRST boss silently: no LOG_ERROR, no warning, a 3-row
// roster for a 4-boss dungeon.
//
// The observable cost is not a missing scoreboard line. With Svala gone the
// clear's first target is Palehoof, and the door-blind navmesh offers a 696yd
// corridor to him that threads BOTH portcullises — which is how
// tr-20260902-083808-1 spent its whole 314s paused against gates that open on
// the deaths of bosses three and four.
TEST(DungeonEventUtgardePinnacleTest, SvalaIsAddedWithHerRealKillBitAndHerFloorAnchor)
{
    ASSERT_TRUE(BossRosterRegistry::HasPatch(MAP_ID))
        << "map 575 has no roster patch — the first boss is invisible to the roster "
           "builder and the run walks into a shut portcullis instead";

    std::vector<DungeonBossInfo> const roster = PatchedFromEmpty();
    DungeonBossInfo const* svala = FindAnchor(roster, NPC_SVALA);
    ASSERT_NE(svala, nullptr) << "Svala Sorrowgrave (26668) is not in the patched roster";

    EXPECT_EQ(svala->kind, DungeonAnchorKind::Boss);

    // The kill-bit, which is what makes completion work at all. It rides
    // GetCompletedEncounterMask exactly like every other boss on the map:
    // KillRewarder reads _victim->GetEntry() AT DEATH TIME, by which point the
    // transform happened minutes ago, so the victim's entry IS 26668 and DBC bit 0
    // is set normally — even though this instance script is a legacy Encounters[4]
    // one with no SetBossState anywhere.
    EXPECT_EQ(svala->encounterIndex, BIT_SVALA)
        << "Svala must carry DBC bit 0 — the bit the kill path actually sets";

    // NOT the inherit path. inheritCompletionFrom resolves against the DERIVED
    // list, and the whole problem is that she is not in it.
    EXPECT_EQ(svala->inheritCompletionFrom, 0u)
        << "there is nothing to inherit from — she is missing from the base list";

    // And NOT the boss-state path either: that is for a boss with no DBC row at
    // all, and hers exists (DungeonEncounter.dbc rows 577 and 578).
    EXPECT_LT(svala->doneBossStateIndex, 0)
        << "a real DungeonEncounter bit exists; only the DERIVATION failed";

    // THE ANCHOR IS THE PLATFORM FLOOR. Three positions are in play during her
    // encounter — the spawn floor, a ~6yd hover, and the Ritual of the Sword's
    // NearTeleportTo(..., 110.0) twenty yards up — and NavmeshSnap's vertical
    // extent is a FIXED 10 regardless of snap radius, so an anchor authored at the
    // ritual height would not snap at all and this row would be dropped at load
    // with one LOG_ERROR. [[dc-boss-anchor-snap-vertical-extent]].
    EXPECT_NEAR(svala->z, SVALA_Z, 0.01f);
    EXPECT_LT(svala->z, 110.0f - 10.0f)
        << "an anchor at the ritual height cannot snap — NavmeshSnap's vertical "
           "extent is a fixed 10";
    EXPECT_LT(Dist3(svala->x, svala->y, svala->z, 296.632f, -346.075f, 90.6307f), 1.0f)
        << "the anchor must sit on the probed floor under the SPAWNED npc's row";
}

// One Any-gated patch serves both difficulties, because Svala is DBC bit 0 on
// normal AND heroic (rows 577/578) with no difficulty bit-shift — unlike Gundrak
// and the Nexus, whose heroic additions each needed a second gated patch. If a
// heroic run ever loses her, this is the test that says so.
TEST(DungeonEventUtgardePinnacleTest, TheRosterIsIdenticalOnHeroic)
{
    std::vector<DungeonBossInfo> const normal = PatchedFromEmpty(/*heroic*/ false);
    std::vector<DungeonBossInfo> const heroic = PatchedFromEmpty(/*heroic*/ true);

    ASSERT_EQ(normal.size(), heroic.size())
        << "map 575's patch must be difficulty-agnostic — Svala is bit 0 on both";
    for (std::size_t i = 0; i < normal.size(); ++i)
    {
        EXPECT_EQ(normal[i].entry, heroic[i].entry);
        EXPECT_EQ(normal[i].encounterIndex, heroic[i].encounterIndex);
        EXPECT_EQ(normal[i].orderOverride, heroic[i].orderOverride);
    }
}

// The three travel objectives, and the ONE contiguous order scale that puts them
// between the four bosses. Every one of the three anchors something that cannot
// be reached by walking to a boss: two areatrigger volumes with no creature in
// them, and a GameObject 83yd on the far side of its own boss.
TEST(DungeonEventUtgardePinnacleTest, ThreeObjectivesSitBetweenTheFourBossesInOneScale)
{
    std::vector<DungeonBossInfo> const roster = PatchedFromEmpty();

    struct Row { uint32 entry; int32 order; uint32 eventId; char const* what; };
    Row const kObjectives[] = {
        { Obj(1), ORDER_WAKE_SVALA,     EVENT_WAKE_SVALA,     "areatrigger 5140"   },
        { Obj(2), ORDER_STASIS,         EVENT_START_PALEHOOF, "Stasis Generator"   },
        { Obj(3), ORDER_ENTER_GAUNTLET, EVENT_ENTER_GAUNTLET, "areatrigger 4991"   },
    };

    for (Row const& r : kObjectives)
    {
        DungeonBossInfo const* o = FindAnchor(roster, r.entry);
        ASSERT_NE(o, nullptr) << "objective for " << r.what << " is missing";
        EXPECT_EQ(o->kind, DungeonAnchorKind::Objective);
        EXPECT_EQ(o->orderOverride, r.order);
        EXPECT_EQ(o->eventId, r.eventId)
            << r.what << "'s objective must reference the event that does the work";
        // An objective carries no kill-bit and NextDungeonBossValue never tests
        // the completion mask for one, so its encounterIndex is an ordering hint
        // and must stay 0 — a non-zero value here would read as a real DBC bit.
        EXPECT_EQ(o->encounterIndex, 0u);
        EXPECT_GT(o->arriveRadius, 0.0f)
            << r.what << " must have an arrive radius or the objective never fires";
    }

    // THE SCALE IS CONTIGUOUS AND IT IS THE TRAVEL ORDER. Ymiron last is not a
    // preference: boss_ymironAI::Reset() ADDS UNIT_FLAG_NOT_SELECTABLE and only
    // SetData(DATA_SKADI, DONE) removes it, so he cannot be attacked out of turn
    // and a mis-ordered roster fails loudly at him rather than silently.
    EXPECT_EQ(ORDER_WAKE_SVALA, 1);
    EXPECT_EQ(ORDER_SVALA, 2);
    EXPECT_EQ(ORDER_STASIS, 3);
    EXPECT_EQ(ORDER_PALEHOOF, 4);
    EXPECT_EQ(ORDER_ENTER_GAUNTLET, 5);
    EXPECT_EQ(ORDER_SKADI, 6);
    EXPECT_EQ(ORDER_YMIRON, 7);

    // Every objective entry must be distinct, because they flow through the same
    // entry-keyed machinery (skip / sticky / cleared-anchor latch / panel) as a
    // real spawn — a duplicate would silently merge two anchors.
    EXPECT_NE(Obj(1), Obj(2));
    EXPECT_NE(Obj(2), Obj(3));
    EXPECT_NE(Obj(1), Obj(3));
}

// The three derived bosses are REORDERED, never removed and re-added. Their DBC
// kill-bits are untouched; the reorder exists purely so the objectives have
// integer slots between them.
TEST(DungeonEventUtgardePinnacleTest, TheDerivedBossesAreReorderedAndNeverRemoved)
{
    for (BossRosterPatch const& p : BossRosterRegistry::AllPatches())
    {
        if (p.mapId != MAP_ID)
            continue;

        EXPECT_TRUE(p.remove.empty())
            << "map 575 removes nothing — all three derived bosses are correct, they "
               "just need slots made for the objectives";
        EXPECT_TRUE(p.skipByDesign.empty())
            << "nothing on map 575 may be skipped: Ymiron is NOT_SELECTABLE until "
               "Skadi is DONE, so a skip on Skadi costs TWO bosses, not one";
        EXPECT_EQ(p.gate, DcDifficultyGate::Any)
            << "Svala is DBC bit 0 on both difficulties — one Any patch serves both";

        bool sawPalehoof = false, sawSkadi = false, sawYmiron = false;
        for (auto const& [entry, order] : p.reorder)
        {
            if (entry == NPC_PALEHOOF) { sawPalehoof = true; EXPECT_EQ(order, ORDER_PALEHOOF); }
            if (entry == NPC_SKADI)    { sawSkadi    = true; EXPECT_EQ(order, ORDER_SKADI);    }
            if (entry == NPC_YMIRON)   { sawYmiron   = true; EXPECT_EQ(order, ORDER_YMIRON);   }
        }
        EXPECT_TRUE(sawPalehoof && sawSkadi && sawYmiron)
            << "all three derived bosses must be moved onto the 1..7 scale, or they "
               "sort by raw DBC bit and land before the objectives that unlock them";
    }
}

// --- the events -------------------------------------------------------------

TEST(DungeonEventUtgardePinnacleTest, WakeSvalaIsAnchoredPersistentAndWaitsOutTheIntro)
{
    DungeonEvent const* ev = DungeonEventRegistry::Find(MAP_ID, EVENT_WAKE_SVALA);
    ASSERT_NE(ev, nullptr) << "map 575 event 1 (wake Svala) is missing";

    EXPECT_EQ(ev->activation, EventActivation::Anchored);
    EXPECT_EQ(ev->orderIndex, static_cast<uint32>(ORDER_WAKE_SVALA));

    // The hold spans a 72s cutscene during which ten Dragonflayer Spectators run
    // across the arena approach and despawn — any of which can flick a bot in and
    // out of combat and rewind a non-persistent list to step 0, re-walking the
    // party to a trigger box it is already standing in.
    EXPECT_TRUE(ev->persistent);
    EXPECT_TRUE(ev->required);

    ASSERT_EQ(ev->steps.size(), 3u);
    EXPECT_EQ(ev->steps[0].kind, EventStepKind::MoveTo);
    EXPECT_EQ(ev->steps[1].kind, EventStepKind::Custom);
    EXPECT_EQ(ev->steps[1].hookId, HOOK_SVALA_AREATRIGGER);

    // WaitForSpawn(26668) is not waiting for a spawn — nothing spawns. It waits
    // for the TRANSFORM: at t+34s boss_svala UpdateEntry()s a creature that has
    // been standing there since instance init, and from that tick
    // FindNearestCreature(26668) starts matching it. That is also the right
    // SCHEDULE — 34s of intro, then a ~25s walk down Leg B, arriving around t+60s
    // for a boss who becomes attackable at t+72s.
    EXPECT_EQ(ev->steps[2].kind, EventStepKind::WaitForSpawn);
    EXPECT_EQ(ev->steps[2].creatureEntry, NPC_SVALA)
        << "the wait must be on the CREDIT entry (the UpdateEntry target), not on "
           "29281 — the spawned entry is the same creature and is never 'gone'";
    EXPECT_TRUE(ev->steps[2].wantAlive);
    EXPECT_GE(ev->steps[2].timeoutMs, 72000u)
        << "the intro is a measured 72 seconds; a shorter wait stalls a healthy run";
}

TEST(DungeonEventUtgardePinnacleTest, StartPalehoofClicksTheGeneratorAndVerifiesTheClick)
{
    DungeonEvent const* ev = DungeonEventRegistry::Find(MAP_ID, EVENT_START_PALEHOOF);
    ASSERT_NE(ev, nullptr) << "map 575 event 2 (start Palehoof) is missing";

    EXPECT_EQ(ev->activation, EventActivation::Anchored);
    EXPECT_TRUE(ev->persistent);

    ASSERT_EQ(ev->steps.size(), 3u);
    EXPECT_EQ(ev->steps[0].kind, EventStepKind::MoveTo);

    EXPECT_EQ(ev->steps[1].kind, EventStepKind::UseGameObject);
    EXPECT_EQ(ev->steps[1].goEntry, GO_STASIS_GENERATOR);
    // A PLAIN Use(), not a report-use. GameObject::Use() calls
    // sScriptMgr->OnGossipHello BEFORE the type switch and returns early when it
    // returns true, so go_palehoof_sphere's handler IS the whole mechanism and the
    // type-18 summoning-ritual machinery never runs. A report-use would take a
    // different path into the same script for no reason.
    EXPECT_FALSE(ev->steps[1].reportUse)
        << "the generator is started by OnGossipHello inside Use(), not by report-use";

    // The receipt. Use() returns void, so nothing else can tell a click that ran
    // the script from one that was swallowed — and a swallowed click sends the
    // party 83yd east to fight a boss that is still frozen. The handler's own
    // first act is SetGoState(GO_STATE_ACTIVE).
    EXPECT_EQ(ev->steps[2].kind, EventStepKind::WaitForGameObjectState);
    EXPECT_EQ(ev->steps[2].goEntry, GO_STASIS_GENERATOR);
    EXPECT_EQ(ev->steps[2].wantState, static_cast<uint32>(GO_STATE_ACTIVE));
}

// The gauntlet is a ONE-WAY DOOR: tripping AT 4991 arms a summon pump with no end
// condition, and leaving the hall for more than ~7s evades Skadi and resets
// everything. This event exists to make entering it deliberate.
TEST(DungeonEventUtgardePinnacleTest, EnterGauntletIsAnchoredAndNeverClaimsOwnsThePull)
{
    DungeonEvent const* ev = DungeonEventRegistry::Find(MAP_ID, EVENT_ENTER_GAUNTLET);
    ASSERT_NE(ev, nullptr) << "map 575 event 3 (enter Skadi's gauntlet) is missing";

    EXPECT_EQ(ev->activation, EventActivation::Anchored);
    EXPECT_TRUE(ev->persistent);
    EXPECT_TRUE(ev->required)
        << "a party that cannot form up must stall on the threshold, not walk in";

    // OwnsThePull is CONDITIONAL-ONLY. The anchored path infers the stand-down
    // from `persistent` alone (IsPersistentAnchoredEventActive) and
    // FindDueConditionalEvent — the only reader of ownsThePull — never sees an
    // anchored row, so the flag here would be a silent no-op. Event 4 carries it
    // for the leg that needs it.
    EXPECT_FALSE(ev->ownsThePull)
        << "OwnsThePull on an anchored event is inert — Persistent() already stands "
           "the pull down";

    ASSERT_EQ(ev->steps.size(), 2u);
    EXPECT_EQ(ev->steps[0].kind, EventStepKind::MoveTo);
    EXPECT_EQ(ev->steps[1].kind, EventStepKind::Custom);
    EXPECT_EQ(ev->steps[1].hookId, HOOK_SKADI_AREATRIGGER);
}

TEST(DungeonEventUtgardePinnacleTest, TheHarpoonDriverIsARepeatableCombatDriverThatOwnsThePull)
{
    DungeonEvent const* ev = DungeonEventRegistry::Find(MAP_ID, EVENT_BRING_DOWN_GRAUF);
    ASSERT_NE(ev, nullptr) << "map 575 event 4 (bring down Grauf) is missing";

    EXPECT_EQ(ev->activation, EventActivation::Conditional);
    EXPECT_TRUE(static_cast<bool>(ev->condition)) << "the phase-1 predicate must be bound";

    // The hall is DoZoneInCombat'd from the moment the trigger trips and the pump
    // never stops, so a rung that stands down on IsInCombat never runs once.
    EXPECT_TRUE(ev->drivesInCombat);
    // 161yd east on the driver's own long-haul spline; the per-tick hold would
    // cancel it, and the yield semantics are what let the party fight the adds.
    EXPECT_TRUE(ev->stepsOwnMovement);
    // A camp dragged west past the Flame Breath Trigger carpet resets the gauntlet.
    EXPECT_TRUE(ev->ownsThePull);
    // Grauf dying is the only "done"; a latch would stop it after one shot.
    EXPECT_TRUE(ev->repeatable);
    EXPECT_TRUE(ev->persistent);
    // Skadi cannot be skipped — Ymiron is NOT_SELECTABLE until she is DONE — so a
    // quiet skip here costs the party two bosses.
    EXPECT_TRUE(ev->required);

    ASSERT_EQ(ev->steps.size(), 1u)
        << "phase 1 is a per-tick walk-or-hold-or-fire preference; a step list cannot "
           "express a window that opens and closes on a flight lap";
    EXPECT_EQ(ev->steps[0].kind, EventStepKind::Custom);
    EXPECT_EQ(ev->steps[0].hookId, HOOK_GRAUF_HARPOON);
    EXPECT_EQ(ev->steps[0].timeoutMs, HARPOON_TIMEOUT_MS);
    EXPECT_TRUE(ObjectiveHookRegistry::Has(HOOK_GRAUF_HARPOON));

    // PanelBeforeBoss is NOT cosmetic: panelGatesBossEntry also keys
    // DcTargeting::HasPendingSummonEvent, which reads an unlatched gating event as
    // "this boss must still be SUMMONED" and suppresses the dynamic pull within
    // 80yd of him. A Repeatable event is never latched, so the suppression would be
    // permanent and the party would arrive at Skadi and never pull her.
    // [[dc-panelbeforeboss-repeatable-permanent-hold]].
    EXPECT_EQ(ev->panelGatesBossEntry, 0u)
        << "PanelBeforeBoss on a Repeatable event is a PERMANENT pull hold at that boss";
    EXPECT_EQ(ev->panelSortAfterBossEntry, NPC_PALEHOOF);
}

TEST(DungeonEventUtgardePinnacleTest, TheRitualHoldIsOptionalAndYieldsTheTick)
{
    DungeonEvent const* ev = DungeonEventRegistry::Find(MAP_ID, EVENT_SVALA_RITUAL);
    ASSERT_NE(ev, nullptr) << "map 575 event 5 (Svala's ritual) is missing";

    EXPECT_EQ(ev->activation, EventActivation::Conditional);
    EXPECT_TRUE(static_cast<bool>(ev->condition));
    EXPECT_TRUE(ev->drivesInCombat) << "the whole 25 seconds is a fight";
    EXPECT_TRUE(ev->stepsOwnMovement) << "taken for the YIELD, not for movement";
    EXPECT_TRUE(ev->repeatable);

    // The worst case if this never fires is a slower fight, not a lost one: the
    // channelers have small health pools and stock targeting reaches them
    // eventually. A stall here would be strictly worse than the bug it fixes.
    EXPECT_FALSE(ev->required) << "the ritual hold must be Optional";

    // It must NOT own the pull. The party is standing on the boss's own platform
    // in the middle of a boss fight; there is no leg to cross and nothing for a
    // pull stand-down to buy.
    EXPECT_FALSE(ev->ownsThePull);

    ASSERT_EQ(ev->steps.size(), 1u);
    EXPECT_EQ(ev->steps[0].kind, EventStepKind::Custom);
    EXPECT_EQ(ev->steps[0].hookId, HOOK_SVALA_RITUAL_HOLD);
    EXPECT_TRUE(ObjectiveHookRegistry::Has(HOOK_SVALA_RITUAL_HOLD));
}

TEST(DungeonEventUtgardePinnacleTest, EveryHookIdIsRegisteredAndUniqueAcrossEveryDungeon)
{
    EXPECT_EQ(HOOK_SVALA_AREATRIGGER, 25u);
    EXPECT_EQ(HOOK_SKADI_AREATRIGGER, 26u);
    EXPECT_EQ(HOOK_GRAUF_HARPOON,     27u);
    EXPECT_EQ(HOOK_SVALA_RITUAL_HOLD, 28u);

    for (uint32 id : { HOOK_SVALA_AREATRIGGER, HOOK_SKADI_AREATRIGGER,
                       HOOK_GRAUF_HARPOON, HOOK_SVALA_RITUAL_HOLD })
        EXPECT_TRUE(ObjectiveHookRegistry::Has(id)) << "hook " << id << " is not registered";

    // Ids are ONE FLAT SPACE across every dungeon; AddHook LOG_ERRORs a collision
    // and keeps the FIRST row, so a copy-pasted id silently disables the loser's
    // objective. Spot-check the neighbours these were numbered against.
    for (uint32 taken : { DcVioletHold::HOOK_DRIVE_WAVE,
                          DcBlackwingLair::HOOK_RAZORGORE_ORB,
                          DcBlackwingLair::HOOK_SUPPRESSION_TRANSIT,
                          DcHallsOfStone::HOOK_TRIBUNAL,
                          DcHallsOfStone::HOOK_WAVE,
                          DcHallsOfLightning::HOOK_SLAG_FURNACE_TRANSIT })
        for (uint32 mine : { HOOK_SVALA_AREATRIGGER, HOOK_SKADI_AREATRIGGER,
                             HOOK_GRAUF_HARPOON, HOOK_SVALA_RITUAL_HOLD })
            EXPECT_NE(mine, taken);
}

// --- the six authored legs -------------------------------------------------

// A leg longer than this is a leg the party cannot stay together across, and a
// leg steeper than this is a wall rather than a ramp.
//
// THE STEEPNESS TEST IS A SLOPE, NOT A VERTICAL STEP. These routes are decimated
// to ~16-20yd anchors from a corridor sampled every 4yd, so an anchor pair spans
// several corridor points and its rise is several steps' worth: Leg C's climb out
// of the lower ring rises 10.5yd between two anchors while the corridor's largest
// single step on the whole leg is 3.3yd. A flat dz cap would read that ramp as a
// ledge. The mmap generator's own walkable limit is 60 degrees (slope 1.73); 1.2
// sits under it and over this dungeon's steepest authored leg, so it catches a
// regen that has stitched a wall into a corridor without arguing with the ramps
// that are really there. The per-STEP ledge test belongs to the probe suite,
// which has the corridor itself.
TEST(DungeonEventUtgardePinnacleTest, EveryLegIsRegisteredContinuousAndARampNotAWall)
{
    struct Leg { uint32 entry; char const* name; std::size_t minAnchors; };
    Leg const kLegs[] = {
        { Obj(1),       "A: entrance -> areatrigger 5140", 10 },
        { NPC_SVALA,    "B: areatrigger 5140 -> Svala",     4 },
        { Obj(2),       "C: Svala -> Stasis Generator",    10 },
        { NPC_PALEHOOF, "D: Stasis Generator -> Palehoof",  4 },
        { Obj(3),       "E: Palehoof -> areatrigger 4991",  4 },
        { NPC_YMIRON,   "G: Skadi's landing -> Ymiron",    15 },
    };

    constexpr float MAX_LEG_3D = 26.0f;
    constexpr float MAX_SLOPE = 1.2f;

    for (Leg const& leg : kLegs)
    {
        std::vector<WaypointHint> const* route = Route(leg.entry);
        ASSERT_NE(route, nullptr) << "no authored route for leg " << leg.name;
        ASSERT_GE(route->size(), leg.minAnchors) << "leg " << leg.name << " is too short";

        for (std::size_t i = 1; i < route->size(); ++i)
        {
            WaypointHint const& a = (*route)[i - 1];
            WaypointHint const& b = (*route)[i];
            float const flat = std::sqrt((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
            float const rise = std::fabs(b.z - a.z);

            EXPECT_LT(Dist3(a.x, a.y, a.z, b.x, b.y, b.z), MAX_LEG_3D)
                << "leg " << leg.name << " anchor " << i << " is too far from its predecessor";
            EXPECT_GT(flat, 0.5f)
                << "leg " << leg.name << " anchor " << i << " is a duplicate of the one before it";
            EXPECT_LT(rise / flat, MAX_SLOPE)
                << "leg " << leg.name << " anchor " << i << " climbs " << rise << "yd over "
                << flat << "yd — that is a wall, not a ramp";
        }
    }
}

// EACH ROW MUST START WHERE THE PARTY WILL BE STANDING when that leg begins.
// DungeonPathFollower::SeedCursor projects the bot onto the row from its own
// position; a row that starts somewhere else snaps the cursor to the far end and
// the party walks the leg backwards
// ([[dc-anchor-route-must-cover-where-the-party-stands]]). That is why there are
// six rows and not one, and it is the single easiest property to break by
// "tidying" the table.
TEST(DungeonEventUtgardePinnacleTest, EachLegStartsWhereThePreviousOneEnded)
{
    struct Join { uint32 from; uint32 to; char const* what; };
    Join const kJoins[] = {
        { Obj(1),       NPC_SVALA,    "areatrigger 5140"  },
        { NPC_SVALA,    Obj(2),       "Svala's platform"  },
        { Obj(2),       NPC_PALEHOOF, "the Stasis Generator" },
        { NPC_PALEHOOF, Obj(3),       "Palehoof"          },
    };

    for (Join const& j : kJoins)
    {
        std::vector<WaypointHint> const* from = Route(j.from);
        std::vector<WaypointHint> const* to = Route(j.to);
        ASSERT_NE(from, nullptr);
        ASSERT_NE(to, nullptr);
        ASSERT_FALSE(from->empty());
        ASSERT_FALSE(to->empty());

        WaypointHint const& end = from->back();
        WaypointHint const& start = to->front();
        EXPECT_LT(Dist3(end.x, end.y, end.z, start.x, start.y, start.z), 1.0f)
            << "the leg after " << j.what << " does not start where the one before it ended";
    }

    // Leg A starts at the instance entrance the harness teleports a walked-in
    // party to, and Leg G at Skadi's phase-2 landing (spell 61790's
    // spell_target_position) — which is where the party is standing when she
    // dies, 134yd from her spawn. Neither has a predecessor row to join to.
    std::vector<WaypointHint> const* legA = Route(Obj(1));
    ASSERT_NE(legA, nullptr);
    EXPECT_LT(Dist3(legA->front().x, legA->front().y, legA->front().z,
                    584.117f, -327.974f, 110.138f), 1.0f)
        << "Leg A must start at the areatrigger_teleport target a walked-in party lands on";

    std::vector<WaypointHint> const* legG = Route(NPC_YMIRON);
    ASSERT_NE(legG, nullptr);
    EXPECT_LT(Dist3(legG->front().x, legG->front().y, legG->front().z,
                    476.799f, -511.167f, 104.723f), 1.5f)
        << "Leg G must start at Skadi's phase-2 landing, not at her spawn 134yd west";
}

// Each leg must actually END at the thing it is a route to, or the anchor
// fast-path delivers the party somewhere near and the goal segment does the rest
// blind — which on this map means walking the last stretch through A*'s
// door-blind preference.
TEST(DungeonEventUtgardePinnacleTest, EveryLegEndsAtItsOwnDestination)
{
    struct Row { uint32 entry; float x, y, z; char const* what; };
    Row const kRows[] = {
        { Obj(1),       AT_SVALA_X, AT_SVALA_Y, AT_SVALA_Z, "areatrigger 5140's centre" },
        { NPC_SVALA,    SVALA_X,    SVALA_Y,    SVALA_Z,    "Svala's platform"          },
        { Obj(2),       STASIS_X,   STASIS_Y,   STASIS_Z,   "the Stasis Generator"      },
        { NPC_PALEHOOF, 320.791f, -453.145f, 104.806f,      "Gortok Palehoof"           },
        { Obj(3),       AT_SKADI_X, AT_SKADI_Y, AT_SKADI_Z, "areatrigger 4991's centre" },
        { NPC_YMIRON,   392.835f, -286.809f, 109.284f,      "King Ymiron"               },
    };

    for (Row const& r : kRows)
    {
        std::vector<WaypointHint> const* route = Route(r.entry);
        ASSERT_NE(route, nullptr) << "no route to " << r.what;
        WaypointHint const& last = route->back();
        EXPECT_LT(Dist3(last.x, last.y, last.z, r.x, r.y, r.z), 6.0f)
            << "the route to " << r.what << " ends " << Dist3(last.x, last.y, last.z, r.x, r.y, r.z)
            << "yd short of it";
    }
}

// An areatrigger objective's arrive radius is a CONTAINMENT radius, not a
// tolerance, and this is the test that says so.
//
// Nothing server-side notices a unit entering an areatrigger. Both ways one can
// be fired for a bot — UtgardePinnacleDriver's ForgeAreaTrigger and the harness
// relay in TestRun/DcTestAreaTriggers.cpp — deliberately hand a CMSG_AREATRIGGER
// to WorldSession::HandleAreaTriggerOpcode rather than calling the script
// directly, and that handler re-tests Player::IsInAreaTriggerRadius(atEntry, 0.f)
// — IsWithinBox against the DBC half-extents with ZERO delta.
//
// Meanwhile both things that can stop the tank at an objective anchor — the
// arrival trigger in DungeonClearTriggers.cpp and the leading MoveTo step in
// DungeonEventExecutor::RunStep — latch on GetExactDist(anchor) <= arriveRadius.
// So the set of positions the tank may come to rest in is a sphere of that radius
// about the anchor, and if any of that sphere lies outside the box, some fraction
// of runs park in the gap and the encounter can never be started at all.
//
// That is not hypothetical. tp-20260902-121652-1 lost 3 of 20 Utgarde Pinnacle
// runs to it with AT_SVALA_ARRIVE at 8.0 against a 6.895yd short half-extent:
// tank at (313.8, -283.6), 0.68yd north of the y -284.278 face, `areatrigger
// relay: 0 packet(s) over 2 volume(s)`, the Custom step failing forever, 0/7
// bosses. The 17 that worked all fired the trigger at y -284.3 — every single
// one of them scraping the outermost yard of the box, which is the shape of a
// coin flip, not of a margin.
//
// Checked per axis against the anchor's own offset from the DBC centre, because
// an anchor is authored to the walkable surface and need not sit exactly on the
// box's midpoint.
TEST(DungeonEventUtgardePinnacleTest, AreatriggerArriveRadiiFitInsideTheirBoxes)
{
    struct Row
    {
        char const* what;
        uint32 trigger;
        float ax, ay, az;        // the authored anchor
        float cx, cy, cz;        // AreaTrigger.dbc position
        float halfL, halfW, halfH;
        float arrive;
    };

    // dbc positions and box sizes are the AreaTrigger.dbc rows verbatim.
    Row const kRows[] = {
        { "Svala (5140)", AREATRIGGER_SVALA,
          AT_SVALA_X, AT_SVALA_Y, AT_SVALA_Z,
          312.646f, -291.173f, 104.702f,
          AT_SVALA_HALF_L, AT_SVALA_HALF_W, AT_SVALA_HALF_H, AT_SVALA_ARRIVE },
        { "Skadi's gauntlet (4991)", AREATRIGGER_SKADI,
          AT_SKADI_X, AT_SKADI_Y, AT_SKADI_Z,
          330.903f, -508.430f, 104.272f,
          AT_SKADI_HALF_L, AT_SKADI_HALF_W, AT_SKADI_HALF_H, AT_SKADI_ARRIVE },
    };

    for (Row const& r : kRows)
    {
        struct Axis { char const* name; float offset; float half; };
        Axis const axes[] = {
            { "x", std::fabs(r.ax - r.cx), r.halfL },
            { "y", std::fabs(r.ay - r.cy), r.halfW },
            { "z", std::fabs(r.az - r.cz), r.halfH },
        };

        for (Axis const& a : axes)
        {
            EXPECT_LT(a.offset + r.arrive, a.half)
                << r.what << ": an arrive radius of " << r.arrive << " about an anchor "
                << a.offset << "yd off the box centre on " << a.name
                << " reaches " << (a.offset + r.arrive - a.half)
                << "yd past the " << a.half << "yd half-extent. A tank that stops out there "
                   "satisfies arrival, stops walking, and can never fire the trigger — "
                   "HandleAreaTriggerOpcode re-tests IsInAreaTriggerRadius with zero delta.";
        }

        // And a margin, not merely containment: the tank's resting position is
        // quantised by the movement tick, and combat can nudge it after arrival.
        float const tightest =
            std::min({ axes[0].half - axes[0].offset, axes[1].half - axes[1].offset,
                       axes[2].half - axes[2].offset });
        EXPECT_GE(tightest - r.arrive, 1.0f)
            << r.what << ": only " << (tightest - r.arrive)
            << "yd of slack between the arrive sphere and the nearest box face";
    }
}

// THE WHOLE POINT OF THE TABLE. The navmesh is baked DOOR-BLIND, so a corridor is
// happy to thread a shut portcullis; the authored legs are what keep the party
// off those corridors. Probe-measured, the designed legs clear 192174 by 43yd at
// worst and 192173 by 32.9yd at worst — and the ONE exception, Leg G passing
// 192173 at 3.1yd, is walked only after Skadi's death has opened it permanently.
TEST(DungeonEventUtgardePinnacleTest, NoLegBeforeSkadiGoesNearEitherPortcullis)
{
    constexpr float DOOR_SKADI_X = 477.496f, DOOR_SKADI_Y = -477.183f, DOOR_SKADI_Z = 103.064f;
    constexpr float DOOR_YMIRON_X = 445.062f, DOOR_YMIRON_Y = -325.520f, DOOR_YMIRON_Z = 100.953f;

    // Everything up to and including the gauntlet entry. Leg G is excluded on
    // purpose: by the time it is walked, 192173 is open.
    for (uint32 entry : { Obj(1), NPC_SVALA, Obj(2), NPC_PALEHOOF, Obj(3) })
    {
        std::vector<WaypointHint> const* route = Route(entry);
        ASSERT_NE(route, nullptr);

        float nearestSkadi = 1e9f;
        float nearestYmiron = 1e9f;
        for (WaypointHint const& h : *route)
        {
            nearestSkadi = std::min(
                nearestSkadi, Dist3(h.x, h.y, h.z, DOOR_SKADI_X, DOOR_SKADI_Y, DOOR_SKADI_Z));
            nearestYmiron = std::min(
                nearestYmiron, Dist3(h.x, h.y, h.z, DOOR_YMIRON_X, DOOR_YMIRON_Y, DOOR_YMIRON_Z));
        }

        // 25yd is the auto-pause band a phantom door prop is flagged within
        // ([[dc-scriptonly-is-not-navigation-ignored]]), so an anchor inside it is
        // a run that pauses on a gate it was never meant to reach.
        EXPECT_GT(nearestSkadi, 25.0f)
            << "an anchor on a pre-Skadi leg passes " << nearestSkadi
            << "yd from GO 192173 — that gate does not open until Skadi dies";
        EXPECT_GT(nearestYmiron, 25.0f)
            << "an anchor on a pre-Skadi leg passes " << nearestYmiron
            << "yd from GO 192174 — that gate is the LAST boss's exit and never opens "
               "during the run";
    }
}

// No NO_STOP anywhere, and that is a decision rather than an omission. Every
// trash spawn on map 575 is spawntimesecs 3600, so a kill is progress that stays
// bought and the ordinary pull is the right owner of all six legs — the opposite
// of Blackwing Lair's 30s whelps and Halls of Lightning's 20s Slags, where
// clearing was negative progress. The one stretch that IS a transit (AT 4991 to
// the harpoon launchers) is not in this table at all.
TEST(DungeonEventUtgardePinnacleTest, NoAuthoredLegSuppressesThePull)
{
    for (uint32 entry : { Obj(1), NPC_SVALA, Obj(2), NPC_PALEHOOF, Obj(3), NPC_YMIRON })
    {
        std::vector<WaypointHint> const* route = Route(entry);
        ASSERT_NE(route, nullptr);
        for (std::size_t i = 0; i < route->size(); ++i)
        {
            EXPECT_FALSE(HasFlag((*route)[i].flags, AnchorFlag::NO_STOP))
                << "anchor " << i << " on route " << entry
                << " suppresses the pull; on this map clearing is progress that stays "
                   "bought (every trash respawn is 3600s)";
            EXPECT_FALSE(HasFlag((*route)[i].flags, AnchorFlag::JUMP_DOWN));
            EXPECT_FALSE(HasFlag((*route)[i].flags, AnchorFlag::JUMP_GAP))
                << "no designed leg on map 575 needs a jump — all six route with "
                   "maxStepZ under 3.3";
        }
    }
}

// Heroic shares the geometry, so every row is registered under NORMAL and the
// registry's own difficulty fallback serves heroic. If a heroic-specific row is
// ever added, this is what says the fallback stopped being the whole story.
TEST(DungeonEventUtgardePinnacleTest, HeroicFallsBackToTheNormalRoutes)
{
    for (uint32 entry : { Obj(1), NPC_SVALA, Obj(2), NPC_PALEHOOF, Obj(3), NPC_YMIRON })
    {
        std::vector<WaypointHint> const* normal = Route(entry);
        ASSERT_NE(normal, nullptr);
        EXPECT_EQ(DungeonClearRouteRegistry::Get(MAP_ID, DUNGEON_DIFFICULTY_HEROIC, entry),
                  normal);
    }
}

// --- the doors --------------------------------------------------------------

TEST(DungeonEventUtgardePinnacleTest, BothPortcullisesAreScriptOnlyAndStillNavigationVisible)
{
    for (uint32 door : { GO_SKADI_DOOR, GO_YMIRON_DOOR })
    {
        EXPECT_TRUE(DcEventDoorRegistry::IsScriptOnly(door))
            << "GO " << door << " is a lock-free GAMEOBJECT_TYPE_DOOR, so a bot would "
               "happily open it — and force-opening either fights the instance script "
               "for the gate's state";

        // Deliberately NOT navigation-invisible, unlike the Molten Core props and
        // the Halls of Stone Sky Room Floor: these two ARE doors, the party really
        // is stopped by them, and the at-boss stand-down needs to see them. Hiding
        // them would mask the next roster regression instead of preventing it.
        EXPECT_FALSE(DcEventDoorRegistry::IsNavigationIgnored(door))
            << "GO " << door << " must stay visible to navigation — a pause on it is "
               "the tripwire that says the route reverted to A*'s door-blind shortcut";
    }
}

// The Svala mirror is a GAMEOBJECT_TYPE_DOOR that spawns OPEN and is driven to
// GO_STATE_READY — shut, by the collision-truth test — by boss_svala for the
// whole 72-second intro, 11yd from where the party fights. It is FX: lock 0,
// autoCloseTime 0, leading nowhere, and the arena has one entrance which is the
// ramp to the north.
//
// THE ROW IS HERE BECAUSE OF THE NUMBER BELOW, not because of the resemblance.
// DungeonClearBlockingDoorValue flags a door only when a route leg TRANSITS its
// footprint, or is GameObject-LOS-blocked within 12yd of it ON THE SAME FLOOR —
// so the whole question was how close Leg B actually passes. It passes at
// 7.35yd, inside that band, which is enough to auto-pause the run on the boss's
// own doorstep.
//
// The distance is pinned rather than merely bounded: if a re-authored leg moves
// it, the decision gets re-taken with the new facts instead of quietly leaning
// on a row whose justification has expired.
TEST(DungeonEventUtgardePinnacleTest, TheSvalaMirrorIsNavigationInvisibleBecauseLegBRunsPastIt)
{
    EXPECT_TRUE(DcEventDoorRegistry::IsNavigationIgnored(GO_SVALA_MIRROR))
        << "GO 191745 is an FX slab 7yd off Leg B and 11yd from Svala's platform; "
           "left visible it auto-pauses the run on the boss's own doorstep";

    // IsScriptOnly would be the WRONG tool, for the reason the Utgarde Keep forge
    // walls spell out: it only refuses the CLICK, and the auto-pause is what kills
    // the run. Navigation-invisible is the whole answer here.
    EXPECT_FALSE(DcEventDoorRegistry::IsScriptOnly(GO_SVALA_MIRROR));

    constexpr float MIRROR_X = 296.355f, MIRROR_Y = -356.967f, MIRROR_Z = 91.465f;

    std::vector<WaypointHint> const* legB = Route(NPC_SVALA);
    ASSERT_NE(legB, nullptr);
    float nearest = 1e9f;
    for (WaypointHint const& h : *legB)
        nearest = std::min(nearest, Dist3(h.x, h.y, h.z, MIRROR_X, MIRROR_Y, MIRROR_Z));

    EXPECT_NEAR(nearest, 7.35f, 1.0f)
        << "Leg B's closest approach to the Svala mirror is now " << nearest
        << "yd, not the 7.35 this row was authored from — re-take the decision "
           "rather than just moving this bound";
}

// --- rows this map deliberately does not have ------------------------------

TEST(DungeonEventUtgardePinnacleTest, PalehoofsFrozenAnimalsAreNotNeverTargetRows)
{
    // They spawn NOT_SELECTABLE | IMMUNE_TO_PC, so IsPossibleTarget almost
    // certainly refuses them already — and the question DcNeverTargetRegistry
    // answers is a different one ("is killing this progress"), to which the honest
    // answer once they wake is YES. Adding these rows is a design change that
    // needs a measured run behind it, not a tuning change.
    for (uint32 animal : { 26683u, 26684u, 26685u, 26686u })
        EXPECT_FALSE(DcNeverTargetRegistry::IsNeverTarget(MAP_ID, animal))
            << "creature " << animal
            << " is one of Palehoof's four stasis animals; they ARE the encounter "
               "once unfrozen. Measure before authoring this row";
}

TEST(DungeonEventUtgardePinnacleTest, NothingOnThisMapAggrosARoomOnEntry)
{
    // The two areatriggers are events, not aggro, and they are the only things on
    // map 575 that fire on entry. A RoomAggroRegistry row on any of the four would
    // double-drive whatever event 1 or event 3 is already doing.
    for (uint32 boss : { NPC_SVALA, NPC_PALEHOOF, NPC_SKADI, NPC_YMIRON })
        EXPECT_EQ(RoomAggroRegistry::Find(MAP_ID, boss), nullptr)
            << "boss " << boss
            << " has a room-aggro row; map 575's only on-entry mechanisms are "
               "areatriggers 5140 and 4991, and both are events";
}

// --- the driver's own numbers ----------------------------------------------

// The pocket, the launcher and the two breach hold points are the only geometry
// the harpoon driver has, and three separate constraints meet at the pocket. If
// any of the relationships below stops holding, phase 1 becomes unwinnable in a
// way no log line names — the party simply stands in a hallway until the step
// times out.
TEST(DungeonEventUtgardePinnacleTest, TheHarpoonPocketCanActuallyReachTheLauncher)
{
    // Harpoon Launcher 192175's live `gameobject` row.
    constexpr float LAUNCHER_X = 491.494f, LAUNCHER_Y = -508.188f, LAUNCHER_Z = 105.877f;
    constexpr float GO_USE_RANGE = 5.0f;  // DC_EVENT_GO_USE_RANGE

    float const pocketToLauncher =
        Dist3(POCKET_X, POCKET_Y, POCKET_Z, LAUNCHER_X, LAUNCHER_Y, LAUNCHER_Z);
    EXPECT_LT(pocketToLauncher, GO_USE_RANGE)
        << "the pocket is " << pocketToLauncher << "yd from launcher 192175 — the bot "
           "cannot press it from where the driver parks it";

    // ...AND STILL IN REACH FROM THE EDGE OF THE LEASH. The leash is a re-walk
    // trigger, not a guarantee, so the worst case is a bot sitting a full leash
    // from the pocket on the tick the shot window opens.
    EXPECT_LT(pocketToLauncher + POCKET_LEASH, GO_USE_RANGE)
        << "a bot at the edge of the pocket leash is out of interact range of the "
           "launcher — it would burn shot windows waiting to be re-walked";

    // The search radius has to find it from inside the pocket.
    EXPECT_LT(pocketToLauncher, HARPOON_SEARCH);
}

// The breach is where the shot has to be taken FROM the launcher, and both hold
// points must be inside the cone that fires it. The spell chain is: the bot
// Use()s the launcher -> 48641 FORCE_CASTs the World Trigger 19871 standing at it
// -> the TRIGGER casts 48642, a 60yd TARGET_UNIT_CONE_ENTRY already aimed at the
// breach. So the constraint is trigger-to-Grauf, not bot-to-Grauf.
TEST(DungeonEventUtgardePinnacleTest, BothBreachHoldPointsAreInsideTheHarpoonCone)
{
    // World Trigger 19871 at launcher 192175 (its live `creature` row).
    constexpr float TRIGGER_X = 490.516f, TRIGGER_Y = -508.443f, TRIGGER_Z = 107.042f;
    // 48642's EffectRadius index 48.
    constexpr float CONE_RADIUS = 60.0f;

    float const toA = Dist3(TRIGGER_X, TRIGGER_Y, TRIGGER_Z, BREACH_A_X, BREACH_A_Y, BREACH_A_Z);
    float const toB = Dist3(TRIGGER_X, TRIGGER_Y, TRIGGER_Z, BREACH_B_X, BREACH_B_Y, BREACH_B_Z);

    EXPECT_LT(toA, CONE_RADIUS)
        << "PATH_INITIAL's hold point is " << toA << "yd from the launcher's trigger — "
           "outside the 60yd cone, so the FIRST lap can never be shot";
    EXPECT_LT(toB, CONE_RADIUS)
        << "PATH_LEFT/RIGHT's hold point is " << toB << "yd from the launcher's trigger — "
           "outside the 60yd cone, so no lap after the first can be shot";

    // The two hold points are 8.6yd apart and the driver tests the NEARER of them
    // against one radius, so that radius has to cover the gap between them or a
    // lap would be missed depending on which path Grauf flew.
    float const gap = Dist3(BREACH_A_X, BREACH_A_Y, BREACH_A_Z,
                            BREACH_B_X, BREACH_B_Y, BREACH_B_Z);
    EXPECT_LT(gap, BREACH_RADIUS)
        << "the two breach hold points are " << gap << "yd apart, wider than the "
           "window radius — the driver would see one lap and not the other";

    // ...and the window must be far tighter than the cone, or the driver fires
    // mid-lap into a shot that simply misses.
    EXPECT_LT(BREACH_RADIUS, CONE_RADIUS * 0.5f);
}

// The pocket must be on the north side of the breath divider and inside the reach
// of the Flame Breath Trigger carpet.
//
// THE SECOND ONE IS THE ONE THAT LOSES RUNS. spell_area keeps 47546 on every
// player in area 1196 (the whole instance); it fires 47547 every 5s, restricted
// by `conditions` to creature 28351 within 40yd, with a 7s aura; and every 6s the
// reset trigger at (397.0, -511.5) counts 28351s still carrying it and, on ZERO,
// calls EnterEvadeMode on Skadi. More than ~7 seconds with nobody within 40yd of
// the carpet and the whole gauntlet resets.
TEST(DungeonEventUtgardePinnacleTest, ThePocketKeepsTheGauntletAlive)
{
    // The easternmost Flame Breath Trigger's live `creature` row.
    constexpr float NEAREST_TRIGGER_X = 483.221f, NEAREST_TRIGGER_Y = -507.151f;
    constexpr float RESET_REACH = 40.0f;  // 47547's EffectRadius index 23

    float const dx = POCKET_X - NEAREST_TRIGGER_X;
    float const dy = POCKET_Y - NEAREST_TRIGGER_Y;
    EXPECT_LT(std::sqrt(dx * dx + dy * dy), RESET_REACH)
        << "the pocket is outside 40yd of the nearest Flame Breath Trigger — the 6s "
           "reset check counts zero and Skadi evades";

    // The breath divider. spell_freezing_cloud_area_right strips targets with
    // y > -511 and _left strips y < -511, and the side is RAND per lap — so this
    // is half a defence, not a whole one, and it is taken because the launcher is
    // on that side anyway.
    EXPECT_GT(POCKET_Y, -511.0f)
        << "the pocket is on the south side of the breath divider AND away from the "
           "launcher — there is no reason to be there";
}

// --- the harness ------------------------------------------------------------

TEST(DungeonEventUtgardePinnacleTest, TheTestDungeonRowPointsAtMap575)
{
    DcTestDungeonRegistry::Row const* row = DcTestDungeonRegistry::Find("up");
    ASSERT_NE(row, nullptr) << "the .dc test registry has no 'up' row";
    EXPECT_EQ(row->mapId, MAP_ID);

    // Leg A starts at this row's own coordinates, so the two cannot drift apart
    // without the first leg starting somewhere the party never stands.
    std::vector<WaypointHint> const* legA = Route(Obj(1));
    ASSERT_NE(legA, nullptr);
    EXPECT_LT(Dist3(legA->front().x, legA->front().y, legA->front().z,
                    row->x, row->y, row->z), 1.0f)
        << "the harness entrance and Leg A's first anchor have drifted apart";
}
