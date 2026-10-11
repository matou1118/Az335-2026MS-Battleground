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

#include "GossipDef.h"       // GossipMenu — the option-id translation under test
#include "InstanceScript.h"  // EncounterState — DONE is 3 on AC (FAIL takes 2)

#include "Ai/Dungeon/DungeonClear/Data/DcEventDoorRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/DcHazardRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/DcNeverTargetRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonBossInfo.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonEventRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"
#include "Ai/Dungeon/DungeonClear/Data/FightInPlaceRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/SealedEncounterRegistry.h"
#include "Ai/Dungeon/DungeonClear/Overrides/BossRosterRegistry.h"
#include "Ai/Dungeon/DungeonClear/Overrides/ObjectiveHookRegistry.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonEventExecutor.h"
#include "TestRun/DcTestDungeonRegistry.h"

// Halls of Stone (map 599) — the authored-data lints for the dungeon whose
// bit-2 encounter has no creature at all.
//
// Suite name deliberately begins DungeonEvent so it is picked up by the
// `DungeonEvent*` filter that t/run_tests.sh and .github/workflows/tests.yml both
// use; a suite named HallsOfStone* would build, pass locally, and never run in CI.
//
// Every number checked here is either read out of the core
// (src/server/scripts/Northrend/Ulduar/HallsOfStone/*), read out of the live world
// DB, measured from Spell.dbc, or column-probed against the live 599 mmtiles. The
// reasoning lives in Data/Events/HallsOfStoneEvents.cpp and
// Overrides/HallsOfStoneDriver.cpp; these tests exist so an edit that drops one of
// those properties fails loudly at author time instead of silently costing a run —
// which on this map means a party that walks the whole dungeon and then stands in
// front of a closed door for the rest of the clear.

using namespace DcHallsOfStone;

namespace
{
    // The three add spawn points, verbatim from creature_summon_groups
    // (summonerId 28070, groups 0/1/2).
    struct Pt { float x, y; };
    constexpr Pt kAddSpawns[] = {
        { SPAWN_PROTECTOR_X,   SPAWN_PROTECTOR_Y   },
        { SPAWN_STORMCALLER_X, SPAWN_STORMCALLER_Y },
        { SPAWN_GOLEM_X,       SPAWN_GOLEM_Y       },
    };

    BossRosterPatch const* HosPatch()
    {
        for (BossRosterPatch const& p : BossRosterRegistry::AllPatches())
            if (p.mapId == MAP)
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

// --- the wave driver's event, pinned to its exact shape --------------------
// Every one of these properties is a live failure some other dungeon already paid
// for, so an edit that drops one should fail with intent rather than as a silent
// behaviour change.
TEST(DungeonEventHallsOfStoneTest, WaveEventIsARepeatableOptionalCombatDriver)
{
    DungeonEvent const* ev = DungeonEventRegistry::Find(MAP, /*eventId*/ 4);
    ASSERT_NE(ev, nullptr) << "Halls of Stone (599) event 4 (repel the wave) is missing";

    EXPECT_EQ(ev->activation, EventActivation::Conditional);
    EXPECT_TRUE(static_cast<bool>(ev->condition)) << "the wave gate predicate must be bound";

    // Roughly nine waves over the 300s: the condition going false is the only
    // "done".
    EXPECT_TRUE(ev->repeatable) << "wave event must be Repeatable (~9 waves)";
    // A wipe / corpse-run must never hard-stall the run for a human.
    EXPECT_FALSE(ev->required) << "wave event must be Optional (a timeout re-fires it fresh)";
    // JustSummoned SetInCombatWithZone()s every add, so the party is in unbroken
    // combat from t ~ 52s to t = 300s. A non-combat-only driver never runs at all.
    EXPECT_TRUE(ev->drivesInCombat) << "wave event must DrivesInCombat (continuous defend)";
    // The driver issues its own repositioning to the add nearest Brann and back to
    // the intercept line; the per-tick hold would cancel each spline.
    EXPECT_TRUE(ev->stepsOwnMovement) << "wave event must StepsOwnMovement";

    // ONE step, and it must be the driver hook. The priority this encounter needs
    // — always prefer the add closest to BRANN, whatever is closest to the tank —
    // is a standing preference re-evaluated per tick, not a sequence.
    ASSERT_EQ(ev->steps.size(), 1u)
        << "the wave event must be exactly one Custom step (the driver hook); a step"
           " list cannot express the nearest-to-Brann priority";
    EXPECT_EQ(ev->steps[0].kind, EventStepKind::Custom);
    EXPECT_EQ(ev->steps[0].hookId, HOOK_WAVE);
    EXPECT_TRUE(ObjectiveHookRegistry::Has(HOOK_WAVE))
        << "hook " << HOOK_WAVE << " (HosDriveWave) must be registered";
}

// --- the three Brann objectives -------------------------------------------
TEST(DungeonEventHallsOfStoneTest, BrannObjectivesArePersistentAndOwnTheirMovement)
{
    for (uint32 id : { 1u, 2u, 3u })
    {
        DungeonEvent const* ev = DungeonEventRegistry::Find(MAP, id);
        ASSERT_NE(ev, nullptr) << "Halls of Stone (599) event " << id << " is missing";

        EXPECT_EQ(ev->activation, EventActivation::Anchored)
            << "event " << id << " is reached by travelling to its objective anchor";

        // Every one of these spans combat: the escort walks 170yd through seven
        // constructs, the Tribunal is 300 seconds of unbroken fighting, and the
        // door leg crosses 300yd of cleared-but-repopulating dungeon. A
        // non-persistent step list rewinds to step 1 on every >1s combat gap.
        EXPECT_TRUE(ev->persistent)
            << "event " << id << " must be Persistent — a combat gap would otherwise"
               " rewind it to step 1 (dc-persistent-sticky-arms-at-step-1)";

        EXPECT_TRUE(ev->stepsOwnMovement)
            << "event " << id << " must set StepsOwnMovement — its steps/hook issue"
               " their own glides and the per-tick hold cancels them";

        ASSERT_FALSE(ev->steps.empty());
        EXPECT_EQ(ev->steps[0].kind, EventStepKind::MoveTo)
            << "event " << id << " must lead with the travel step";
    }
}

// --- THE ESCORT'S DATA GATE, and why it is not decoration ------------------
//
// This is the single most fragile authored fact in the dungeon and the one a
// future edit is most likely to "simplify" away, because at a glance
// `doneDataId = BRANN_DOOR` looks like a redundant second completion gate next to
// the Kaddrak marker.
//
// It is not. DriveEscortCreature has two gossip branches and BOTH are gated:
//   * the START branch on `escortee->GetFaction() == 35` (the Wailing Caverns
//     idle-faction model) — Brann is faction 1665 the whole way, so it is dead;
//   * the RESUME branch, which is the one Brann's shape needs, on
//     `step.instanceDataId >= 0`.
// Drop the data gate and neither branch fires: the party finds Brann, follows
// him, defends him, and never clicks a single gossip. The escort never starts and
// the run fails at the same closed door it fails at today.
TEST(DungeonEventHallsOfStoneTest, EscortStepCarriesTheDataGateThatArmsItsGossip)
{
    DungeonEvent const* ev = DungeonEventRegistry::Find(MAP, /*eventId*/ 1);
    ASSERT_NE(ev, nullptr);

    EventStep const* escort = nullptr;
    for (EventStep const& s : ev->steps)
        if (s.kind == EventStepKind::EscortCreature)
            escort = &s;
    ASSERT_NE(escort, nullptr) << "event 1 must contain the EscortCreature step";

    EXPECT_EQ(escort->creatureEntry, NPC_BRANN);

    // A POSITION, not an OptionID. Menus 9669 and 9670 each carry exactly one
    // option, so 0 is correct for both clicks this one step makes — but only
    // because SelectGossip translates the position into the menu's real key. Their
    // OptionIDs are 37476 and 36142; see GossipOptionIsAPositionNotTheDbOptionId.
    EXPECT_EQ(escort->gossipOption, 0)
        << "both 9669 and 9670 have exactly one option, so position 0 is the only"
           " correct value; SelectGossip maps it onto the real OptionID";

    // THE GATE. Losing this silently disarms the gossip — see the comment above.
    EXPECT_GE(escort->instanceDataId, 0)
        << "the EscortCreature step MUST carry an instance-data gate: it is what"
           " arms DriveEscortCreature's RESUME branch (gated on instanceDataId >= 0),"
           " and that branch is the ONLY one that fires for Brann because the START"
           " branch requires faction 35 and he is 1665. Without it the escort never"
           " starts.";
    EXPECT_EQ(static_cast<uint32>(escort->instanceDataId), BRANN_DOOR)
        << "BRANN_DOOR is the only index on this map whose GetData is truthful —"
           " brann_bronzebeard.cpp writes BOTH SetBossState and SetData for it,"
           " while BRANN_BRONZEBEARD writes only the boss state and reads 0 forever";
    // Against the SYMBOL, not a literal: AzerothCore's EncounterState puts FAIL
    // at 2, so DONE is 3 — a hand-written 2 here would silently assert "the door
    // event failed" as the escort's completion threshold.
    EXPECT_EQ(escort->instanceDataMin, static_cast<uint32>(DONE));

    // Kaddrak is the live-creature completion marker: he is summoned by
    // InitializeEvent(), i.e. the instant gossip 9670 lands. The primitive refuses
    // to complete on "reached the end" (the DM-West / RFD premature-completion
    // class), and nothing summons a head before that gossip, so it cannot fire
    // early either.
    EXPECT_EQ(escort->escortDoneEntry, NPC_KADDRAK);
    EXPECT_LT(escort->escortDoneBit, 0)
        << "no DungeonEncounter bit marks the escort; the marker is the head";

    // No timeout, deliberately: RunStep never escalates an EscortCreature on
    // elapsed time (it is watchdog-owned), so a number here would be inert and
    // would read as a budget somebody could tune.
    EXPECT_EQ(escort->timeoutMs, 0u)
        << "EscortCreature is watchdog-owned; a timeout here does nothing";
}

// --- the option is a POSITION, and something must translate it ---------------
//
// The bug this locks: GossipMenu keys its items by the DB's
// gossip_menu_option.OptionID (Player::PrepareGossipMenu -> AddMenuItem(OptionID,
// ...)), so GetItem(n) is a key lookup, not the n-th option. Every gossip option
// authored in the dungeon tables is a POSITION. Nearly all of those NPCs use
// OptionID 0, so passing the position straight through to the protocol was
// indistinguishable from correct — until Brann, whose four menus carry 37476 /
// 36142 / 36412 / 36236. Live (tr-20260831-225609-1) every Brann gossip silently
// no-opped: the party stood 7.9yd from him for eleven minutes with every watchdog
// reporting clear, and the run died to the 600s no-progress timer.
//
// Brann's real OptionIDs are asserted as literals on purpose. They are world-DB
// facts this dungeon depends on, and the whole failure was a claim about them
// that nobody had checked.
TEST(DungeonEventHallsOfStoneTest, GossipOptionIsAPositionNotTheDbOptionId)
{
    GossipMenu menu;

    // Menu 9669 as PrepareGossipMenu builds it: one option, keyed by its OptionID.
    menu.AddMenuItem(/*menuItemId*/ 37476, GOSSIP_ICON_CHAT,
                     "Brann, it would be our honor!", /*sender*/ 0, /*action*/ 0,
                     /*boxMessage*/ "", /*boxMoney*/ 0);

    ASSERT_EQ(menu.GetMenuItems().size(), 1u);
    EXPECT_EQ(menu.GetItem(0), nullptr)
        << "a positional 0 is NOT a key here — this is exactly what silently"
           " refused every Brann gossip";

    uint32 listId = 0;
    EXPECT_TRUE(DungeonEventExecutor::ResolveGossipListId(menu, /*option*/ 0, listId));
    EXPECT_EQ(listId, 37476u)
        << "position 0 must resolve to the menu's real OptionID, or the select"
           " packet is rejected by HandleGossipSelectOptionOpcode";

    // Out of range stays a clean false, so a caller keeps retrying rather than
    // sending a garbage id.
    uint32 unused = 0;
    EXPECT_FALSE(DungeonEventExecutor::ResolveGossipListId(menu, /*option*/ 1, unused));
    EXPECT_FALSE(DungeonEventExecutor::ResolveGossipListId(menu, /*option*/ -1, unused));

    GossipMenu empty;
    EXPECT_FALSE(DungeonEventExecutor::ResolveGossipListId(empty, /*option*/ 0, unused));
}

// The other half of the same guarantee: every OptionID-0 NPC the module talks to
// (Shadowfang, Wailing Caverns, Zul'Farrak, Black Morass, Old Hillsbrad, Dire
// Maul, Blackwing Lair) must keep resolving position 0 -> key 0, or this fix
// would trade one broken dungeon for eight.
TEST(DungeonEventHallsOfStoneTest, PositionZeroStillResolvesToKeyZeroForOrdinaryMenus)
{
    GossipMenu menu;
    menu.AddMenuItem(/*menuItemId*/ 0, GOSSIP_ICON_CHAT, "Please unlock the door.",
                     /*sender*/ 0, /*action*/ 0, /*boxMessage*/ "", /*boxMoney*/ 0);
    menu.AddMenuItem(/*menuItemId*/ 1, GOSSIP_ICON_CHAT, "second option",
                     /*sender*/ 0, /*action*/ 0, /*boxMessage*/ "", /*boxMoney*/ 0);

    uint32 listId = 99;
    ASSERT_TRUE(DungeonEventExecutor::ResolveGossipListId(menu, /*option*/ 0, listId));
    EXPECT_EQ(listId, 0u);
    ASSERT_TRUE(DungeonEventExecutor::ResolveGossipListId(menu, /*option*/ 1, listId));
    EXPECT_EQ(listId, 1u);
}

// --- the door leg VERIFIES the door -----------------------------------------
// The house rule is that a Gossip/UseGO is always followed by a verification
// step. Here it is load-bearing twice over: it absorbs the scripted 3.2s between
// Brann reaching POINT_SJONNIR_DOOR and SetData(BRANN_DOOR, DONE) firing, and it
// is the only thing that distinguishes "the gossip landed" from "the door opened".
TEST(DungeonEventHallsOfStoneTest, DoorEventGossipsThenVerifiesTheDoorState)
{
    DungeonEvent const* ev = DungeonEventRegistry::Find(MAP, /*eventId*/ 3);
    ASSERT_NE(ev, nullptr);
    ASSERT_EQ(ev->steps.size(), 3u) << "MoveTo -> Gossip -> WaitForGOState";

    EXPECT_EQ(ev->steps[1].kind, EventStepKind::Gossip);
    EXPECT_EQ(ev->steps[1].creatureEntry, NPC_BRANN);
    EXPECT_EQ(ev->steps[1].gossipOption, 0);
    // Brann is TELEPORTED to the door point by Reset() and stands still, but the
    // flag matters for the symmetrical case in event 2 and costs nothing here.
    EXPECT_TRUE(ev->steps[1].waitForStill);

    EXPECT_EQ(ev->steps[2].kind, EventStepKind::WaitForGameObjectState);
    EXPECT_EQ(ev->steps[2].goEntry, GO_SJONNIR_DOOR);
    // GO_STATE_ACTIVE (0) is OPEN. The door spawns GO_STATE_READY (1).
    EXPECT_EQ(ev->steps[2].wantState, 0u)
        << "the door must be verified OPEN (GO_STATE_ACTIVE), not merely present";
}

// --- the Tribunal objective waits for Brann to STOP walking -----------------
// At t ~ 317s EndTribunalFight sends Brann from the console to the lore stop, and
// he only gains UNIT_NPC_FLAG_GOSSIP on arrival (POINT_TRIBUNAL_LORE). Talking to
// a moving escortee is at best a no-op and at worst interrupts his path.
TEST(DungeonEventHallsOfStoneTest, TribunalObjectiveHoldsThenSkipsTheLore)
{
    DungeonEvent const* ev = DungeonEventRegistry::Find(MAP, /*eventId*/ 2);
    ASSERT_NE(ev, nullptr);
    ASSERT_EQ(ev->steps.size(), 3u) << "MoveTo -> Custom(hook 22) -> Gossip";

    EXPECT_EQ(ev->steps[1].kind, EventStepKind::Custom);
    EXPECT_EQ(ev->steps[1].hookId, HOOK_TRIBUNAL);
    EXPECT_TRUE(ObjectiveHookRegistry::Has(HOOK_TRIBUNAL))
        << "hook " << HOOK_TRIBUNAL << " (DriveHallsOfStoneTribunal) must be registered";

    // The Tribunal is a FIXED 300s timer plus a ~17s wrap-up; nothing the party
    // does shortens it. A budget under that would time the objective out on a
    // healthy run.
    EXPECT_GE(ev->steps[1].timeoutMs, 320000u)
        << "the Tribunal cannot finish sooner than ~317s — a smaller budget would"
           " fail a run that is winning";

    EXPECT_EQ(ev->steps[2].kind, EventStepKind::Gossip);
    EXPECT_EQ(ev->steps[2].creatureEntry, NPC_BRANN);
    EXPECT_TRUE(ev->steps[2].waitForStill)
        << "Brann is WALKING to the lore stop when this step first runs and only"
           " gains the gossip flag on arrival";
}

// --- the two hooks are distinct and both registered ------------------------
TEST(DungeonEventHallsOfStoneTest, HooksAreDistinctAndRegistered)
{
    EXPECT_NE(HOOK_TRIBUNAL, HOOK_WAVE);
    EXPECT_TRUE(ObjectiveHookRegistry::Has(HOOK_TRIBUNAL));
    EXPECT_TRUE(ObjectiveHookRegistry::Has(HOOK_WAVE));
}

// --- roster: three objectives added, NOTHING removed -----------------------
// Unlike the Violet Hold, all three derived bosses on this map are correct and
// reachable — they only need re-ordering onto one contiguous scale so the
// objectives have somewhere to sit. A `remove` row appearing here would mean
// somebody had decided a real, killable, correctly-placed boss should not be in
// the clear.
TEST(DungeonEventHallsOfStoneTest, RosterAddsThreeObjectivesAndRemovesNothing)
{
    BossRosterPatch const* p = HosPatch();
    ASSERT_NE(p, nullptr) << "map 599 must have a roster patch";

    EXPECT_TRUE(p->remove.empty())
        << "all three derived bosses are correct and reachable; nothing is removed";
    EXPECT_TRUE(p->skipByDesign.empty());
    EXPECT_EQ(p->gate, DcDifficultyGate::Any)
        << "heroic changes stat templates and three add cadences, not the roster shape";

    ASSERT_EQ(p->add.size(), 3u) << "escort / Tribunal / door";
    for (DungeonBossInfo const& b : p->add)
    {
        EXPECT_EQ(b.kind, DungeonAnchorKind::Objective)
            << "'" << b.name << "' must be an objective — the Tribunal has NO creature"
               " to anchor or target, which is why MakeBossWithBit is not used here";
        EXPECT_EQ(b.mapId, MAP);
        EXPECT_GT(b.eventId, 0u) << "'" << b.name << "' must name its event";
    }

    // The three derived bosses are reordered in place onto the SAME 1..6 scale, so
    // the objectives can be interleaved without colliding with a boss's key.
    ASSERT_EQ(p->reorder.size(), 3u);
    std::set<int32> keys;
    for (auto const& r : p->reorder)
        keys.insert(r.second);
    for (DungeonBossInfo const& b : p->add)
        keys.insert(b.orderOverride);
    EXPECT_EQ(keys.size(), 6u)
        << "all six order keys must be distinct — a collision silently reorders the"
           " clear";
    EXPECT_EQ(*keys.begin(), 1);
    EXPECT_EQ(*keys.rbegin(), 6);

    // The travel order is the dungeon's: both bosses before the Brann sequence,
    // Sjonnir after the door.
    EXPECT_LT(ORDER_KRYSTALLUS, ORDER_MAIDEN);
    EXPECT_LT(ORDER_MAIDEN, ORDER_ESCORT);
    EXPECT_LT(ORDER_ESCORT, ORDER_TRIBUNAL);
    EXPECT_LT(ORDER_TRIBUNAL, ORDER_DOOR);
    EXPECT_LT(ORDER_DOOR, ORDER_SJONNIR);
}

// --- the hold point is ON the intercept line, not merely near it -----------
// The encounter's whole positional problem is that every add walks from one of
// three spawn points to Brann at the console, ~83yd away, and the party has to be
// in between. This re-derives that property from the spawn coordinates rather
// than trusting the authored literal.
TEST(DungeonEventHallsOfStoneTest, HoldPointStraddlesEveryAddApproachLine)
{
    float const toBrann = Dist2d(HOLD_X, HOLD_Y, CONSOLE_X, CONSOLE_Y);

    // Close enough to peel anything that reaches him, far enough out to meet adds
    // before they arrive. Both bounds are behaviour, not taste: inside ~10yd the
    // party is standing on top of him and meets each add at his feet; past ~40yd
    // the three approach lines have fanned wider than one camp can straddle.
    EXPECT_GT(toBrann, 10.0f) << "hold point is on top of Brann — no interception";
    EXPECT_LT(toBrann, 40.0f) << "hold point is too far up the ramp to defend Brann";

    // Every spawn must be FURTHER from Brann than the party is — i.e. the party is
    // genuinely between them and him, for all three.
    for (Pt const& s : kAddSpawns)
    {
        float const spawnToBrann = Dist2d(s.x, s.y, CONSOLE_X, CONSOLE_Y);
        EXPECT_GT(spawnToBrann, toBrann)
            << "add spawn (" << s.x << ", " << s.y << ") is closer to Brann than the"
               " hold point — the party is not between them";
    }

    // And the hold point must sit within one camp's width of every spawn's line to
    // Brann. Perpendicular distance from the hold point to each spawn->Brann
    // segment; 12yd is about the radius a single tank position covers.
    for (Pt const& s : kAddSpawns)
    {
        float const vx = CONSOLE_X - s.x, vy = CONSOLE_Y - s.y;
        float const len = std::sqrt(vx * vx + vy * vy);
        ASSERT_GT(len, 1.0f);
        // 2D cross product / |v| = perpendicular distance to the infinite line.
        float const perp =
            std::fabs((HOLD_X - s.x) * vy - (HOLD_Y - s.y) * vx) / len;
        EXPECT_LT(perp, 12.0f)
            << "hold point is " << perp << "yd off the line from (" << s.x << ", "
            << s.y << ") to Brann — that add walks past the camp";
    }

    // The lore stop sits on the same line, which is why event 2's 10206 gossip
    // costs no travel. If a future edit moves the hold point, this is the property
    // that quietly stops being true.
    EXPECT_LT(Dist2d(HOLD_X, HOLD_Y, LORE_X, LORE_Y), 10.0f)
        << "the hold point should be within talking distance of Brann's lore stop";
}

// --- the wave/head entry lists are complete, exclusive and disjoint --------
TEST(DungeonEventHallsOfStoneTest, WaveAndHeadEntryListsAreCompleteAndDisjoint)
{
    std::vector<uint32> const& waves = HallsOfStoneWaveEntries();
    std::vector<uint32> const& heads = HallsOfStoneHeadEntries();

    // COMPLETE: every creature_summon_groups row for summonerId 28070.
    EXPECT_TRUE(Contains(waves, NPC_DARK_RUNE_PROTECTOR));
    EXPECT_TRUE(Contains(waves, NPC_DARK_RUNE_STORMCALLER));
    EXPECT_TRUE(Contains(waves, NPC_IRON_GOLEM_CUSTODIAN));
    EXPECT_EQ(waves.size(), 3u)
        << "exactly the three summon-group entries; a fourth would make the driver"
           " due for something the Tribunal does not spawn";

    EXPECT_EQ(heads.size(), 3u);
    EXPECT_TRUE(Contains(heads, NPC_KADDRAK));
    EXPECT_TRUE(Contains(heads, NPC_MARNAK));
    EXPECT_TRUE(Contains(heads, NPC_ABEDNEUM));

    // DISJOINT: the heads are probed as "the fight is running" and the adds as
    // "there is something to fight". Mixing them would make the driver due for the
    // whole 300s with nothing to steer for the first 52 of them.
    for (uint32 h : heads)
        EXPECT_FALSE(Contains(waves, h)) << "head " << h << " must not be a wave add";

    // EXCLUSIVE of things that are never Tribunal adds. Sjonnir's pipe adds are
    // ordinary trash on a different encounter entirely; the heroic difficulty
    // twins are stat templates that GetEntry() never returns (Creature::InitEntry
    // does `SetEntry(Entry); // normal entry always`), so listing them would be
    // dead weight that reads as a safety net.
    for (uint32 e : { 27979u, 27981u, 27982u, 28165u,     // Sjonnir's pipe adds
                      31876u, 31877u, 31380u,             // heroic stat twins
                      NPC_EARTHEN_DWARF, NPC_BRANN })
        EXPECT_FALSE(Contains(waves, e))
            << e << " must not be in the Tribunal wave list";
}

// --- never-target: the friendly dwarves, and NOTHING already filtered ------
TEST(DungeonEventHallsOfStoneTest, FriendlyEarthenDwarvesAreNeverAClearTarget)
{
    // Sjonnir's 25% adds take a random PLAYER's faction and attack HIM. Killing
    // one is negative progress; the row exists because SelectTargetFromPlayerList
    // can return nullptr (nobody within 100yd), leaving the dwarf on its hostile
    // template faction 1868 where the clear's pickers will happily select it.
    EXPECT_TRUE(DcNeverTargetRegistry::IsNeverTarget(MAP, NPC_EARTHEN_DWARF));

    // The table's own bar is "is killing it progress", and it is documented to be
    // kept SMALL. AttackersValue::IsPossibleTarget already rejects NOT_SELECTABLE
    // and IMMUNE_TO_PC, so every other candidate on this map is already filtered
    // and a row for it would be a dead row that reads as a safety net.
    for (uint32 e : { NPC_KADDRAK, NPC_MARNAK, NPC_ABEDNEUM,        // 33554436
                      NPC_SEARING_GAZE_TRIGGER, NPC_DARK_MATTER_TARGET,
                      NPC_DARK_MATTER_VISUAL,                        // 33554432
                      28824u,                                        // Brann Flying Machine
                      28149u })                                      // Earthen Protector, 768
        EXPECT_FALSE(DcNeverTargetRegistry::IsNeverTarget(MAP, e))
            << e << " is already rejected by IsPossibleTarget; a row here is dead weight";

    // Real, killable trash must stay targetable — the clear has to fight its way
    // to Brann and through Sjonnir's adds.
    for (uint32 e : { NPC_RAGING_CONSTRUCT, NPC_UNRELENTING_CONSTRUCT,
                      NPC_LIGHTNING_CONSTRUCT, NPC_DARK_RUNE_PROTECTOR,
                      NPC_DARK_RUNE_STORMCALLER, NPC_IRON_GOLEM_CUSTODIAN,
                      27979u, 27981u, 27982u })
        EXPECT_FALSE(DcNeverTargetRegistry::IsNeverTarget(MAP, e))
            << e << " is ordinary hostile trash and must remain a clear target";
}

// --- door policy: per ENTRY, and only the seven that are script-driven -----
TEST(DungeonEventHallsOfStoneTest, ScriptOwnedObjectsAreScriptOnlyAndOpenDoorsAreNot)
{
    // The door the whole feature exists to open, plus the six objects whose GO
    // state IS the Tribunal's lore state machine or Brann's own console work.
    for (uint32 go : { GO_SJONNIR_DOOR, 191527u, 191669u, 191670u, 191671u,
                       193906u, 193907u })
        EXPECT_TRUE(DcEventDoorRegistry::IsScriptOnly(go))
            << "GO " << go << " is driven only by the instance script; a bot Use()"
               " desyncs it and can skip the encounter";

    // The five Ulduar doodad doors spawn state = 0 (OPEN), carry template Data0 = 1
    // and are referenced by no C++ anywhere on this map. This is the SFK lesson:
    // the policy is per ENTRY, never per lock, and a table whose value is that
    // every row means something must not accumulate rows that mean nothing.
    for (uint32 go : { 191292u, 191293u, 191294u, 191295u, 191459u })
        EXPECT_FALSE(DcEventDoorRegistry::IsScriptOnly(go))
            << "GO " << go << " spawns OPEN and nothing ever closes it — listing it"
               " is noise";
}

// --- and the other half: script-only is NOT enough --------------------------
//
// tr-20260901-080117-8 (tp-20260901-080112-1, 5/6 bosses). Every object above is
// GAMEOBJECT_TYPE_DOOR with Data0 = 0 (startOpen) spawned GO_STATE_READY, so the
// blocking-door value reads it as a shut gate — and IsScriptOnly only refuses the
// CLICK. The prop is still flagged, still parked at, still auto-paused on. The
// Sky Room Floor is the FLOOR of the Tribunal room, ~13.6yd from Brann's console
// where the party waits out the door gossip, and Brann's post-fight lore shuts it
// deterministically. With the route to Sjonnir legitimately UNREACHABLE for the
// 400yd Brann walks to his door, the door-blocked action takes its no-corridor
// branch, 13.6yd is inside DC_DOOR_USE_RANGE, and the run auto-pauses on a floor.
// Eight of that plan's ten tanks flagged it; the one without a corridor died on it.
TEST(DungeonEventHallsOfStoneTest, PhantomTribunalDoorsAreNavigationIgnored)
{
    for (uint32 go : { 191527u, 191669u, 191670u, 191671u, 193906u })
    {
        EXPECT_TRUE(DcEventDoorRegistry::IsNavigationIgnored(go))
            << "GO " << go << " is a TYPE_DOOR prop that no one ever opens; left"
               " navigation-visible it is flagged as corridor-blocking and"
               " auto-pauses the run where it stands";
        EXPECT_TRUE(DcEventDoorRegistry::IsScriptOnly(go))
            << "GO " << go << " must also stay click-exempt — navigation-ignored"
               " stops the park, not the Use()";
    }

    // Sjonnir's Door is the one GENUINE gate on the map: the party really is
    // stopped by it and really does need Brann to open it, so it must stay
    // navigation-VISIBLE or the at-boss stand-down has nothing to read.
    EXPECT_FALSE(DcEventDoorRegistry::IsNavigationIgnored(GO_SJONNIR_DOOR))
        << "191296 is the real door — ignoring it blinds the approach";

    // 193907 is script-only but type 1 (BUTTON), not a door, so it can never
    // reach the closed-door predicate. Listing it would be a row that means
    // nothing — the same test the open doodads above are held to.
    EXPECT_FALSE(DcEventDoorRegistry::IsNavigationIgnored(193907u))
        << "193907 is a BUTTON, not a door; it is unreachable by the value";
}

// --- the two hazards are EMITTERS, not ground pools ------------------------
//
// The correction this test pins is a whole-table one rather than a tuning one.
// Both hazards were first sketched as DcGroundHazard rows keyed on the cast
// spell, and `DcGroundHazard::spellId` is what DynamicObject::GetSpellId()
// returns — it only ever matches a SPELL_EFFECT_PERSISTENT_AREA_AURA (27). From
// Spell.dbc neither 51136 nor 51012 has one: 51136 is APPLY_AURA(6) +
// PERIODIC_TRIGGER_SPELL every 500ms into 51125 (5.0yd), and 51012 is
// APPLY_AURA(6) debuffs plus a one-shot 5.0yd SCHOOL_DAMAGE. So both rows would
// have sat in the ground table matching nothing, forever, looking like coverage.
TEST(DungeonEventHallsOfStoneTest, SearingGazeAndDarkMatterAreCreatureEmitters)
{
    for (uint32 entry : { NPC_SEARING_GAZE_TRIGGER, NPC_DARK_MATTER_TARGET })
    {
        DcHazardEmitter const* e = DcHazardRegistry::Find(MAP, entry);
        ASSERT_NE(e, nullptr)
            << "creature " << entry << " must be a DcHazardEmitter — it CARRIES the"
               " aura; there is no DynamicObject to key a ground-pool row on";

        // Both are NOT_SELECTABLE, so there is nothing to fight and no reason to
        // stay: every row here is a threat-2 active-vacate emitter.
        EXPECT_GT(e->vacateRadius, 0.0f)
            << entry << " cannot be killed, so the party must be walked out of it";

        // The vacate radius is the MEASURED aura radius (5.0yd from Spell.dbc),
        // not a guess.
        EXPECT_FLOAT_EQ(e->vacateRadius, 5.0f)
            << "51125 / 51012 both splash 5.0yd — see Spell.dbc";

        // The keep-out must exceed the pulse (drift margin) but stay modest: both
        // land on top of the party during a 300-second defend the party may not
        // leave, and the whole intercept line is only ~10yd wide.
        EXPECT_GT(e->radius, e->vacateRadius);
        EXPECT_LE(e->radius, 12.0f)
            << "an over-wide keep-out sterilises the one piece of ground the"
               " encounter requires the party to stand on";

        // The shared invariant: the retreat aims vacateRadius + retreatSlack and
        // that point must fall outside the hold band, or the bot arrives still in
        // danger and re-flees forever.
        EXPECT_GT(e->retreatSlack, e->holdBand);
    }

    // DELIBERATELY ABSENT, and each for a reason that would be lost if a future
    // reader "completed" the table:
    //   50988 Glare of the Tribunal — a single-target beam with no radius at all.
    //   50840 / 59848 / 51849 Lightning Ring — a 10yd nova centred on a MELEE
    //     boss; a keep-out there would push the melee out of the fight.
    //   52341 / 59038 Electrical Overload — an instant on-death nova with no aura
    //     and no duration; there is no volume to avoid.
    for (uint32 spell : { 50988u, 50840u, 59848u, 51849u, 52341u, 59038u, 51136u, 51012u })
        EXPECT_EQ(DcHazardRegistry::FindGround(MAP, spell), nullptr)
            << "spell " << spell << " has no PERSISTENT_AREA_AURA effect, so a ground"
               " row for it would match nothing — see the note in DcHazardRegistry.cpp";
}

// --- Sjonnir's room is sealed, and must not be pulled out of ----------------
TEST(DungeonEventHallsOfStoneTest, SjonnirIsASealedFightInPlaceEncounter)
{
    SealedEncounterRow const* row = SealedEncounterRegistry::Find(MAP, NPC_SJONNIR);
    ASSERT_NE(row, nullptr)
        << "boss_sjonnirAI::JustEngagedWith shuts 191296 behind the party — without a"
           " row a straggler is locked out for the whole fight, and its closed-door"
           " report is indistinguishable from the nav failure this dungeon was fixed"
           " for";

    // Sjonnir stands at (1295.21, 667.16); his door is at x 1206.56. The gates must
    // arm WEST of the door or the party is already through when they fire.
    float const doorToBoss = 1295.21f - 1206.56f;
    EXPECT_GT(row->approachRadius, doorToBoss)
        << "approachRadius must reach back past the door (" << doorToBoss
        << "yd) or the muster is asking about a threshold everyone has crossed";

    // The volume must contain the boss and exclude the door corridor the party
    // stages in.
    EXPECT_LT(row->minX, 1295.21f);
    EXPECT_GT(row->maxX, 1295.21f);
    EXPECT_GT(row->minX, 1206.56f) << "the box must start INSIDE the door plane";
    EXPECT_LT(row->minY, 667.16f);
    EXPECT_GT(row->maxY, 667.16f);

    // The door-stage anchor is where objective 3 parks the party, and it must be
    // outside the sealed room but inside the approach radius — that is exactly the
    // spot where the clump gate should be tightening the party up.
    EXPECT_FALSE(FightInPlaceRegistry::IsNoPullZone(MAP, DOOR_STAGE_X, DOOR_STAGE_Y))
        << "the door stage must be OUTSIDE Sjonnir's box — it is west of the door";

    // Dragging Sjonnir to a camp west of the door takes him out of his own
    // RectangleBoundary(1206.56, 1341.4185, 579.9434, 753.9599) and he evades.
    EXPECT_TRUE(FightInPlaceRegistry::IsNoPullZone(MAP, 1295.21f, 667.16f))
        << "Sjonnir's room must forbid the pull-to-camp maneuver — his evade box's"
           " west edge IS the door plane";
}

// --- the test entry point is outside the door we are here to open ----------
TEST(DungeonEventHallsOfStoneTest, TestEntryPointIsOutsideSjonnirsDoor)
{
    DcTestDungeonRegistry::Row const* d = DcTestDungeonRegistry::Find("hos");
    ASSERT_NE(d, nullptr) << "the 'hos' test-dungeon row must exist";
    EXPECT_EQ(d->mapId, MAP);

    // The teleport-in is at the north entrance (1153.24, 806.16, 195.94); the door
    // is at x 1206.56. A party that spawned east of it would start the run past the
    // gate and never exercise any of this.
    EXPECT_LT(d->x, 1206.56f)
        << "the test entry point must be WEST of Sjonnir's door, or the run skips the"
           " entire Brann sequence";
}
