/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "gtest/gtest.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "Ai/Dungeon/DungeonClear/Data/DungeonBossInfo.h"
#include "Ai/Dungeon/DungeonClear/Data/DcEventDoorRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonEventRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonClearRouteRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonWingRegistry.h"
#include "Ai/Dungeon/DungeonClear/Overrides/BossRosterRegistry.h"
#include "Ai/Dungeon/DungeonClear/Overrides/ObjectiveHookRegistry.h"

// Registry cross-reference + persistence lint over the authored event data
// (events-system review F1/F2/F3, plus the F9 anchored-event wiring check). These
// registries reference each other by bare integers, and every dangling reference
// fails SILENTLY at runtime (an objective latches Done instantly, a required event
// never fires, an objective vanishes from a wing). These tests turn each of those
// silent-failure classes into a red test at build time.

namespace
{
    // A step kind whose rewind-on-gap is dangerous: teleport/drop/jump are one-way,
    // escort/engage/clear span combat gaps, a data-gated MoveTo garrisons a
    // gate. A multi-step anchored event containing one of these MUST be Persistent
    // or a mid-fight combat gap rewinds it (the module's most-repeated bug class —
    // see the dc-multihop-teleport-persistent memory). A KillCreatureEngage is a
    // KillCreature step with engage set.
    bool IsRewindHazardStep(EventStep const& s)
    {
        switch (s.kind)
        {
            case EventStepKind::TeleportParty:
            case EventStepKind::DropInHole:
            case EventStepKind::Jump:
            case EventStepKind::EscortCreature:
            case EventStepKind::ClearRadius:
                return true;
            case EventStepKind::KillCreature:
                return s.engage;  // KillCreatureEngage seeks + pulls across gaps
            case EventStepKind::MoveTo:
                // a garrison gate of either flavour holds across the whole fight
                return s.instanceDataId >= 0 || s.persistentDataId >= 0;
            default:
                return false;
        }
    }

    bool HasRewindHazard(DungeonEvent const& ev)
    {
        for (EventStep const& s : ev.steps)
            if (IsRewindHazardStep(s))
                return true;
        return false;
    }

    // Intentionally non-persistent multi-step anchored events (each carries an
    // in-file justification). Whitelisted so the F1 lint stays green while still
    // catching the next author who forgets .Persistent(). Keyed by {mapId, eventId}.
    //   - Stratholme (329) event 5 "Dathrohan -> Balnazzar": two idempotent
    //     KillCreatureEngage kill-gates, one continuous fight at a fixed spot, no
    //     WaitForSpawn to false-complete across a gap — a restart-from-0 re-evaluates
    //     correctly (StratholmeEvents.cpp:29-35).
    bool IsNonPersistentWhitelisted(uint32 mapId, uint32 eventId)
    {
        return mapId == 329 && eventId == 5;
    }

    // An "arrival" step forces the tank to a specific spot before it can report
    // Done: MoveTo/Jump HopTo until within radius, UseGameObject/Gossip approach
    // their target, and the escort/drop/teleport primitives all pin the leader to
    // a checkpoint. A conditional event that contains ONE of these can never
    // complete while the tank is far from the anchor. Everything else (ClearRadius,
    // KillCreature, WaitForSpawn/GOState, Wait, CastSpell, UseItem, Custom) can
    // report Done from wherever the tank stands the instant the condition turns
    // true — so an event built ONLY from those latches "done" from afar.
    bool HasArrivalStep(DungeonEvent const& ev)
    {
        for (EventStep const& s : ev.steps)
            switch (s.kind)
            {
                case EventStepKind::MoveTo:
                case EventStepKind::Jump:
                case EventStepKind::UseGameObject:
                case EventStepKind::Gossip:
                case EventStepKind::EscortCreature:
                case EventStepKind::DropInHole:
                case EventStepKind::TeleportParty:
                    return true;
                default:
                    break;
            }
        return false;
    }

    // Conditional events that legitimately complete WITHOUT any arrival step —
    // their single/all-gate step list is safe ONLY because their activation
    // predicate can't read true while the tank is far from the anchor. That
    // near-only guarantee lives in the opaque condition function (a test can't
    // inspect it), so each such event is vetted by hand and listed here with the
    // mechanism that makes it near-only. Keyed by {mapId, eventId}. See the
    // ConditionalEventsWithoutArrivalStepAreProximityVetted tripwire.
    bool IsNearGatedConditionalWhitelisted(uint32 mapId, uint32 eventId)
    {
        struct Row { uint32 mapId; uint32 eventId; };
        static constexpr Row kRows[] = {
            // Stratholme Timmy pre-clear: StrTimmyGated returns false until the
            // tank is within 60yd of Timmy's spawn (the #5 fix — a creature-
            // presence condition read true from map load and false-latched at the
            // instance entrance without this gate).
            {329, 6},
            // Stratholme ziggurat acolyte clears: gated on monotonic instance data
            // (GetData(TYPE_ZIGGURATx) == 1), which flips only when the ziggurat
            // boss dies right at the chamber — never true from afar.
            {329, 1},
            {329, 2},
            {329, 3},
            // ZulFarrak Zum'rah wake-up: ZfZumrahAsleep does a 60yd
            // FindNearestCreature for him, so it cannot read true until boss-nav
            // has parked the party at him — and the hook it drives only writes his
            // faction, which is meaningless from afar anyway.
            {209, 3},
            // Black Morass "Close the time rift": BmWaveHostilesActive requires the
            // leader within 250yd of the arena centroid AND (an open Time Rift or
            // a live wave hostile) within a 250yd grid scan OF THE BOT. Its lone
            // step (hook 12, BmDriveWave) OWNS the travel — it walks the tank to
            // the locked rift's keeper, or back to Medivh's ring to clear the
            // drainers — so there is nothing for an arrival step to add. It also
            // never reports Done while the encounter is live, so the far-tank
            // false-latch state is unreachable. Repeatable besides: a momentary
            // completion never latches, the next wave re-fires it.
            {269, 4},
            // Shattered Halls Nethekurse wake-up: ShNethekurseDormant does a 60yd
            // FindNearestCreature for him, so it cannot read true until boss-nav
            // has parked the party at him — and the hook it drives only fires his
            // intro DoAction, which is meaningless from afar anyway (the Zum'rah
            // pattern verbatim).
            {540, 5},
            // Drak'Tharon Keep "Novos: hold the camp": DtkNovosPhaseOne is a
            // 120yd FindNearestCreature FROM THE BOT for a live Novos carrying
            // UNIT_FLAG_NOT_SELECTABLE, so it cannot read true until the leader
            // is in or beside his chamber (the room is ~96 x 88yd). Its lone
            // step (hook 14, HoldNovosCamp) OWNS the travel — it walks the tank
            // back to the camp — so there is nothing an arrival step would add,
            // and Done is its "nothing to steer this tick" yield rather than a
            // completion. Repeatable besides: a momentary Done latches nothing.
            {600, 1},
            // Violet Hold "Repel the wave": VhWaveActive requires the leader on
            // map 608 AND within 200yd of the arena centroid, then probes a 200yd
            // grid scan OF THE BOT for an open portal, a live wave TempSummon or a
            // RELEASED prisoner. Its lone step (hook 19, VhDriveWave) OWNS the
            // travel — it walks the tank to the locked portal keeper, to a released
            // boss, or back to the door camp — so there is nothing an arrival step
            // would add. Done is its "nothing to steer this tick" yield, not a
            // completion, and it is Repeatable besides: a momentary Done latches
            // nothing and the next wave re-fires it.
            {608, 5},
            // Halls of Stone "Repel the Tribunal wave": HosTribunalWaveActive
            // requires the leader on map 599 AND within 150yd of the arena
            // centroid (930, 365) — which excludes Brann's DB spawn 183.5yd away,
            // where the escort still owns the party, and the Sjonnir door 403yd
            // east. It then probes a 120yd grid scan OF THE BOT for one of the
            // three Tribunal heads or one of the three wave adds, every one of
            // which is a TempSummon that exists only between InitializeEvent() and
            // EndTribunalFight(). Its lone step (hook 23, HosDriveWave) OWNS the
            // travel — it walks the tank to the add nearest Brann and back to the
            // intercept line — so there is nothing an arrival step would add. Done
            // is its "nothing to steer this tick" yield, not a completion, and it
            // is Repeatable besides: a momentary Done latches nothing and the next
            // wave re-fires it.
            {599, 4},
            // Blackwing Lair "Razorgore — orb and egg run": RazorgoreEggRunDue
            // requires the leader on map 469 AND within 200yd of the orb, then a
            // live Razorgore and at least one un-broken egg in a 150yd grid scan
            // OF THE BOT (the chamber is ~80 x 93yd, so that is "in the room").
            // Its lone step (hook 20, DriveRazorgoreOrb) owns everything it
            // steers — the POSSESSED BOSS's splines and the runner's election —
            // so an arrival step would only park the tank, which is the one thing
            // this encounter must not do: the tank is holding an add wave. Done
            // is its "nothing to steer this tick" yield, not a completion, and it
            // is Repeatable besides, so a momentary Done latches nothing.
            {469, 1},
            // Blackwing Lair "Cross the Suppression Rooms": SuppressionTransitDue
            // is gated on an AXIS-ALIGNED BOX (x -7720..-7570, y -1130..-905,
            // z 430..455) that is the two suppression rooms and their approach and
            // nothing else on the map — the leader is literally standing in the
            // corridor or the event is not due. It is then gated on the leader NOT
            // yet being within 10yd of the Broodlord standoff, which is the
            // crossing's own completion: the far-tank false-latch state is not
            // merely unreachable here, it is the state the predicate exists to
            // exclude. Its lone step (hook 21, DriveSuppressionTransit) OWNS the
            // travel — it walks the leader anchor to anchor down the authored
            // route — so there is nothing an arrival step would add, and Done is
            // its "nothing to steer this tick" yield rather than a completion.
            // Repeatable besides: a momentary Done latches nothing.
            {469, 3},
            // Halls of Lightning "Cross the Slag Furnace": the same shape one tier
            // down. SlagFurnaceTransitDue is gated on TWO axis-aligned boxes
            // (x 1298..1358, y -205..-44, z 18..44 for the descent and the pit
            // floor; x 1330..1346, y -240..-205 for the climb out) that between
            // them are the Slag Furnace and nothing else on map 602 — not
            // Volkhan's gallery 29yd directly overhead, and not the gallery ramp
            // 7.5yd east of the pit's own, which is why there are two of them. It
            // is then gated on the leader NOT yet being within 10yd of the mid
            // ledge, which is the crossing's own completion, so the far-tank
            // false-latch state is the state the predicate exists to exclude. Its
            // lone step (hook 24, DriveSlagFurnaceTransit) OWNS the travel, and
            // it is Repeatable, so a momentary Done latches nothing.
            {602, 1},
            // Underbog "Send Ghaz'an up to his platform": deliberately map-wide,
            // and the one case where near-gating would be WRONG. Its hook fires
            // the same DoAction areatrigger 4302 fires, and path 1383921 opens by
            // swimming AWAY into deeper water — so he has to be sent up BEFORE he
            // is the party's target, not once they have walked to him. It is also
            // Repeatable and its condition re-reads his live position, so a
            // completion with the tank far away latches nothing.
            {546, 2},
            // Gundrak "Drakkari Colossus: pull a Living Mojo": GdColossusFrozen
            // does a 40yd FindNearestCreature FROM THE BOT for a live Colossus
            // still carrying UNIT_FLAG_NON_ATTACKABLE, so it cannot read true
            // until the leader is standing in his arena. The flag is the
            // encounter's own phase bit in both directions — Reset() sets it on a
            // fresh spawn together with the five mojo summons, the fight clears
            // it, and after an EVADE Reset() takes its IsInEvadeMode() branch and
            // REMOVES it with no mojos summoned at all, so a post-wipe retry never
            // re-fires (which is right: the boss is directly pullable then). Its
            // lone step is a KillCreatureEngage, which DcRunEventAction walks the
            // leader into through the engage pipeline, so the "far tank latches
            // Done" state the tripwire guards against needs a boss that is both
            // within 40yd and frozen — i.e. the tank is already there.
            {604, 2},
            // Utgarde Pinnacle "Bring down Grauf": UpGraufFlying is gated on the
            // instance's own state word — GetData(DATA_SKADI) == IN_PROGRESS —
            // which is a STRICTLY STRONGER near-gate than a distance check,
            // because the only two things that set it are areatrigger 4991 (a
            // 32x22yd box the party has to be standing in) and
            // boss_skadiAI::JustEngagedWith. The encounter cannot be in progress
            // with the party elsewhere. It is then gated on Grauf being alive,
            // which is phase 1's own completion in the other direction. Its lone
            // step (hook 27, DriveGraufHarpoon) OWNS the travel — it walks the
            // leader the 161yd east to the harpoon pocket itself — so an arrival
            // step would add nothing, and Done is its "nothing to steer this tick"
            // yield rather than a completion. Repeatable besides: a momentary Done
            // latches nothing.
            {575, 4},
            // Utgarde Pinnacle "Svala's ritual: kill the channelers": the same
            // instance-state gate (GetData(DATA_SVALA) == IN_PROGRESS, set by
            // areatrigger 5140 and by her JustEngagedWith) plus a live position
            // read — the boss must be more than 12yd above her own authored floor,
            // which is only true for the 25 seconds the Ritual of the Sword has
            // her rooted at z 110. Its lone step (hook 28) issues NO movement and
            // no completion of any kind; it retargets the leader onto a channeler
            // and returns Done — the yield — on every tick it does not. Repeatable
            // and Optional, so there is no latch to false-set and nothing to stall.
            {575, 5},
            // Pit of Saron "Run the Ymirjar gauntlet": PosGauntletDue is gated on
            // the instance's own progress counter —
            // GetData(DATA_INSTANCE_PROGRESS) in [FINISHED_KRICK_SCENE,
            // AFTER_WARN_2] — which is a STRICTLY STRONGER near-gate than a
            // distance check. That counter is MONOTONIC and is raised by exactly
            // two things: the death of Ick, and a player standing inside one of
            // two areatrigger SPHERES 34.5 and 38.1yd across. It cannot read 3 or
            // 4 with the party anywhere else on the map, and the window closes the
            // instant the tunnel warn lands. It is additionally gated on both
            // earlier bosses being DONE (the orchestrator's own first refusal) and
            // on Tyrannus not being down. Its lone step (hook 29,
            // PosDriveGauntlet) OWNS the travel — it walks the leader from the
            // Krick arena to each of the three gates in turn — so an arrival step
            // would add nothing, and Done is its "nothing to steer this tick"
            // yield rather than a completion. Repeatable besides: a momentary Done
            // latches nothing.
            {658, 1},
            // Halls of Reflection "Hold the altar": HorWavesDue is gated on
            // GetPersistentData(PERSISTENT_DATA_INTRO), which is a STRICTLY
            // STRONGER near-gate than a distance check. That flag is raised by
            // exactly one thing — the intro script running to its last step — and
            // the intro script is started by exactly one thing, the gossip on
            // Jaina/Sylvanas that event 1 takes sixty yards inside the front door.
            // It cannot read true with the party outside the altar chamber,
            // because the instance SHUTS the front door three seconds before it is
            // set and again for every wave. The window closes for ever when Marwyn
            // dies. Its lone step (hook 32, HorDriveWaves) OWNS the travel — it
            // walks the leader back to the camp — so an arrival step would add
            // nothing, and Done is its "nothing to steer this tick" yield rather
            // than a completion. Repeatable besides: a momentary Done latches
            // nothing, and after a leash wipe the driver must re-arm.
            {668, 2},
            // Halls of Reflection "Escape the Lich King": HorEscapeDue is gated on
            // GetBossState(DATA_LICH_KING) == IN_PROGRESS, which is the tightest
            // near-gate in this list. That state is raised by exactly one line of
            // the core — npc_hor_leader_secondAI's ACTION_START_LK_FIGHT_REAL —
            // reached only through a gossip on a leader standing at
            // LeaderEscapePos, which is only offered after the freeze cutscene,
            // which is only reachable once the Frostsworn General is dead. The
            // party is standing in the throne room when it flips, by construction.
            // Its lone step (hook 35, HorDriveEscape) OWNS the travel — it walks
            // the leader stand point to stand point down 679yd of escape path — so
            // an arrival step would add nothing, and Done is its yield. Repeatable
            // because the instance only resets the escape when the LAST player
            // leaves the map, so a driver that came back must re-arm from whatever
            // state it finds.
            {668, 5},
            // The Culling of Stratholme "The ten waves of Stratholme": CosWavesDue is
            // gated on GetData(DATA_ARTHAS_EVENT) being exactly 4 or 5, and on this map
            // a counter test is a STRONGER near-gate than any distance check. The
            // counter can only read 4 because the party performed, in this order,
            // Chromie's entrance gossip, five Arcane Disruptor uses along 330yd of road,
            // Chromie-middle's gossip 430yd further on, a three-minute escort to the
            // city gate, a gossip there, and an 85-second city intro that ENDS with
            // Arthas parked at waypoint 11 — ninety yards from the first wave cluster.
            // There is no path to that value that leaves the party anywhere but in the
            // city, and none of it can happen from a distance or by accident. It closes
            // for ever at 6 (Salramm dead).
            //
            // Its lone step (hook 36, CosDriveWaves) OWNS the travel — it walks the
            // leader to the live wave, which is the entire reason the event exists,
            // because nothing on this map comes to the party — so an arrival step would
            // add nothing, and Done is its "nothing to steer this tick" yield rather
            // than a completion. Repeatable besides: a momentary Done latches nothing.
            {595, 10},
            // Trial of the Champion "The Champion's Arena": TocDriverDue is true for
            // the WHOLE run (progress < 9), from the entrance — and that is sound
            // here because the map IS the arena. The party lands 57yd from the
            // centre of one open bowl ~110yd across, every actor the driver touches
            // (the announcer, the soldiers, the boss, the Knight) stands inside it,
            // and there is no second room anywhere to fire from a distance into.
            // Its lone step (hook 37, TocDriveArena) owns its own movement — it
            // rides the tank's horse to the announcer and back to the joust post —
            // so an arrival step would add nothing, and Done is its yield.
            // Repeatable: it never completes; the predicate going false ends it.
            {650, 4},
            // The Oculus "The Oculus ascent": OculusDriverDue is gated on
            // GetData(DATA_DRAKOS) == DONE, and on this map that is a STRONGER
            // near-gate than any distance. Drakos stands on an island reachable only
            // through the Nexus Portal teleport, and nothing else on the map can set
            // the slot — so the party is on his ring, beside the drake-givers who
            // only walk out of their cages on that very death, when it flips. It
            // stays due until Eregos dies and nobody is left in a saddle. Its lone
            // step (hook 38, OcDriveAscent) owns the travel — the party flies island
            // to island, which no arrival step can express — so Done is its yield.
            // Repeatable: it never completes; the predicate going false ends it.
            {578, 9},
        };
        for (Row const& r : kRows)
            if (r.mapId == mapId && r.eventId == eventId)
                return true;
        return false;
    }
}

// --- sanity ---------------------------------------------------------------

TEST(DungeonEventIntegrityTest, EventTableIsNonEmpty)
{
    EXPECT_FALSE(DungeonEventRegistry::AllEvents().empty());
}

// --- F2: dangling cross-references ----------------------------------------

TEST(DungeonEventIntegrityTest, ConditionalEventsHaveBoundCondition)
{
    // A Conditional event with no bound predicate never fires, so a Required one is
    // a silent wall (the run stalls at a shut door, never naming the event). With
    // the id space replaced by a function pointer (.Conditional(&Predicate)), a
    // wrong name is now a compile error; this lint still catches the one remaining
    // authoring slip — a Conditional() call that was never given a predicate.
    for (DungeonEvent const& ev : DungeonEventRegistry::AllEvents())
    {
        if (ev.activation != EventActivation::Conditional)
            continue;
        EXPECT_TRUE(static_cast<bool>(ev.condition))
            << "conditional event map " << ev.mapId << " id " << ev.id
            << " (" << ev.name << ") has no bound condition — it can never fire";
    }
}

// Tripwire for the Stratholme #5 bug class: a Conditional event whose steps can
// ALL report Done from wherever the tank stands (no arrival step) latches
// "complete" the instant its condition turns true — even with the tank at the
// far end of the map. The executor evaluates its first ClearRadius/KillCreature
// gate from that far position, finds nothing engage-reachable, returns Done, and
// the event is marked done with nothing accomplished (Timmy's room "already
// cleared" at the instance entrance). Such an event is sound ONLY if its
// condition can't read true while the tank is far — a property that lives in the
// opaque predicate a test can't inspect. So force a conscious opt-in: any such
// event must be either a room-aggro pre-clear (condition proximity-gated inside
// RoomTrashRemaining) or hand-vetted on IsNearGatedConditionalWhitelisted. A NEW
// no-arrival conditional event trips this until its author does one of those —
// or, better, gives it an arrival step / proximity-gated condition.
TEST(DungeonEventIntegrityTest, ConditionalEventsWithoutArrivalStepAreProximityVetted)
{
    for (DungeonEvent const& ev : DungeonEventRegistry::AllEvents())
    {
        if (ev.activation != EventActivation::Conditional || ev.steps.empty())
            continue;
        if (HasArrivalStep(ev))
            continue;  // an arrival step forces the tank on-site before completion
        if (DungeonEventRegistry::IsRoomAggroPreClear(ev))
            continue;  // condition is proximity-gated inside RoomTrashRemaining

        EXPECT_TRUE(IsNearGatedConditionalWhitelisted(ev.mapId, ev.id))
            << "conditional event map " << ev.mapId << " id " << ev.id << " ("
            << ev.name << ") has no arrival step (MoveTo/UseGO/Gossip/...), so every "
            << "step can report Done from afar and it false-latches 'complete' the "
            << "instant its condition turns true with the tank far from the anchor "
            << "(the Stratholme #5 'Timmy room already cleared at the entrance' bug). "
            << "Fix: give it a proximity-gated condition (like StrTimmyGated's 60yd "
            << "check) or an arrival step; then, if verified near-only, add it to "
            << "IsNearGatedConditionalWhitelisted with the gating mechanism noted.";
    }
}

TEST(DungeonEventIntegrityTest, CustomStepsHaveRegisteredHook)
{
    // A Custom step with an unregistered hookId now Blocks (was: silently Done) —
    // catch the typo at author time before it stalls a live run.
    for (DungeonEvent const& ev : DungeonEventRegistry::AllEvents())
    {
        for (EventStep const& s : ev.steps)
        {
            if (s.kind != EventStepKind::Custom)
                continue;
            EXPECT_NE(s.hookId, 0u)
                << "Custom step in event map " << ev.mapId << " id " << ev.id
                << " (" << ev.name << ") has hookId 0";
            EXPECT_TRUE(ObjectiveHookRegistry::Has(s.hookId))
                << "Custom step in event map " << ev.mapId << " id " << ev.id
                << " (" << ev.name << ") references unregistered hookId " << s.hookId;
        }
    }
}

TEST(DungeonEventIntegrityTest, RosterObjectiveEventIdsResolveToAnchoredEvents)
{
    // Every objective's eventId must resolve; a typo falls into the legacy hook
    // path (onArriveHook 0 -> Done) and the objective latches instantly on arrival,
    // silently skipping the gate it guards.
    for (BossRosterPatch const& patch : BossRosterRegistry::AllPatches())
    {
        for (DungeonBossInfo const& e : patch.add)
        {
            if (e.kind != DungeonAnchorKind::Objective || e.eventId == 0)
                continue;
            DungeonEvent const* ev = DungeonEventRegistry::Find(patch.mapId, e.eventId);
            ASSERT_NE(ev, nullptr)
                << "roster objective entry " << e.entry << " on map " << patch.mapId
                << " (" << e.name << ") references non-existent eventId " << e.eventId;
            EXPECT_EQ(ev->activation, EventActivation::Anchored)
                << "objective-referenced event map " << patch.mapId << " id " << e.eventId
                << " must be Anchored, not Conditional";
        }
    }
}

TEST(DungeonEventIntegrityTest, DifficultyGatesNeverContradictAcrossAnchorAndEvent)
{
    // An anchored event is gated primarily through the roster patch that wires
    // its objective anchor (BossRosterPatch::gate); the event's own gate is a
    // belt-and-suspenders Drive stop. The two must never CONTRADICT — a
    // HeroicOnly event wired by a NormalOnly patch (or vice versa) can never
    // run: the anchor surfaces on one difficulty, the event refuses to drive
    // there, and the run stalls (Required) or silently skips (Optional).
    auto const contradicts = [](DcDifficultyGate a, DcDifficultyGate b)
    {
        return (a == DcDifficultyGate::NormalOnly && b == DcDifficultyGate::HeroicOnly) ||
               (a == DcDifficultyGate::HeroicOnly && b == DcDifficultyGate::NormalOnly);
    };

    for (BossRosterPatch const& patch : BossRosterRegistry::AllPatches())
    {
        for (DungeonBossInfo const& e : patch.add)
        {
            if (e.kind != DungeonAnchorKind::Objective || e.eventId == 0)
                continue;
            if (DungeonEvent const* ev = DungeonEventRegistry::Find(patch.mapId, e.eventId))
                EXPECT_FALSE(contradicts(patch.gate, ev->gate))
                    << "map " << patch.mapId << " objective " << e.name
                    << " wires event id " << e.eventId
                    << " with a contradicting difficulty gate";
        }
    }
}

TEST(DungeonEventIntegrityTest, RosterObjectiveHooksAreRegistered)
{
    for (BossRosterPatch const& patch : BossRosterRegistry::AllPatches())
    {
        for (DungeonBossInfo const& e : patch.add)
        {
            if (e.kind != DungeonAnchorKind::Objective || e.onArriveHook == 0)
                continue;
            EXPECT_TRUE(ObjectiveHookRegistry::Has(e.onArriveHook))
                << "roster objective entry " << e.entry << " on map " << patch.mapId
                << " (" << e.name << ") references unregistered onArriveHook " << e.onArriveHook;
        }
    }
}

// --- F9: every anchored event is wired by exactly one objective -----------

TEST(DungeonEventIntegrityTest, AnchoredEventsAreWiredByExactlyOneObjective)
{
    // An anchored event enters the clear only through an objective anchor's eventId.
    // An anchored event no objective references is dead data (authored, never fired).
    // NOTE: DungeonEvent::orderIndex is doc-only and has drifted from the roster's
    // real order key in a few files (DireMaul pylons reuse the eventId, Uldaman uses
    // 7/8 vs roster 8/9) — so we assert the WIRING exists, not orderIndex equality.
    for (DungeonEvent const& ev : DungeonEventRegistry::AllEvents())
    {
        if (ev.activation != EventActivation::Anchored)
            continue;
        int refs = 0;
        for (BossRosterPatch const& patch : BossRosterRegistry::AllPatches())
        {
            if (patch.mapId != ev.mapId)
                continue;
            for (DungeonBossInfo const& e : patch.add)
                if (e.kind == DungeonAnchorKind::Objective && e.eventId == ev.id)
                    ++refs;
        }
        EXPECT_EQ(refs, 1)
            << "anchored event map " << ev.mapId << " id " << ev.id << " (" << ev.name
            << ") is wired by " << refs << " objectives (expected exactly 1)";
    }
}

// --- F1: persistence lint -------------------------------------------------

TEST(DungeonEventIntegrityTest, MultiStepRewindHazardEventsArePersistent)
{
    for (DungeonEvent const& ev : DungeonEventRegistry::AllEvents())
    {
        if (ev.activation != EventActivation::Anchored)
            continue;
        if (ev.steps.size() <= 1 || !HasRewindHazard(ev))
            continue;
        if (IsNonPersistentWhitelisted(ev.mapId, ev.id))
            continue;
        EXPECT_TRUE(ev.persistent)
            << "anchored event map " << ev.mapId << " id " << ev.id << " (" << ev.name
            << ") has >1 step and a rewind-hazard step but is not .Persistent() — a "
               "combat gap will rewind it to step 0. Add .Persistent() or whitelist it.";
    }
}

// --- F1b: an anchored event must be able to latch itself ACTIVE ------------

TEST(DungeonEventIntegrityTest, AnchoredStepsOwnMovementEventsCanLatchActive)
{
    // DungeonEventExecutor::IsPersistentAnchoredEventActive is what keeps a running
    // anchored event alive once its own steps walk the tank out of the objective's
    // arriveRadius: without that latch DungeonClearAtObjectiveTrigger falls back to
    // a plain distance test, the event stops being driven the moment the tank
    // leaves the radius, Advance hauls it back, and the two rungs fight until the
    // step times out. IsPullOwningEventDriving reads the same latch, so the pull
    // system does not stand down either.
    //
    // SCOPED TO stepsOwnMovement, which is the flag that makes the latch load-
    // bearing. It is the author's statement that this event's steps — not the
    // per-tick StopBot(Hold) — own where the tank stands, i.e. that they WILL walk
    // it off the anchor. Every other anchored event is content to be held at the
    // arriveRadius, and the `stepIndex >= 1` rule is what protects those from a
    // leading KillCreature/ClearRadius gate false-latching before the tank has
    // arrived. So this is not a lint about step counts; it is the one shape where
    // "my steps move the tank" and "the trigger drops me when I move" contradict.
    //
    // The latch is `stepIndex >= 1 || <the front step is one that owns the walk>`,
    // and the first disjunct cannot be leaned on: it is unreachable for a one-step
    // event (the index reaches 1 only on the tick the event completes), and for a
    // multi-step one it still leaves step 0 unprotected. So require the FRONT step
    // to latch.
    //
    // Every anchored stepsOwnMovement event in the registry already satisfies this
    // through a leading MoveTo — Black Morass' Medivh defence, Halls of Stone's
    // escort / Tribunal / door, both Violet Hold families. The one that did not was
    // Pit of Saron's Tyrannus's ledge (map 658 event 2): one bare Custom step, so
    // IsPersistentAnchoredEventActive was false for its whole life, its 34.4yd
    // walk-in left a 6.0yd arriveRadius, and 6 of 10 runs of tp-20260907-140341-2
    // stalled on the anchor while Advance and the hook fought over the tank.
    for (DungeonEvent const& ev : DungeonEventRegistry::AllEvents())
    {
        if (ev.activation != EventActivation::Anchored || !ev.stepsOwnMovement ||
            ev.steps.empty())
            continue;

        EventStep const& front = ev.steps.front();
        bool const latchable =
            front.kind == EventStepKind::MoveTo ||
            (front.kind == EventStepKind::Custom && ev.stepsOwnMovement);

        EXPECT_TRUE(latchable)
            << "anchored event map " << ev.mapId << " id " << ev.id << " (" << ev.name
            << ") sets .StepsOwnMovement() but its front step (kind "
            << static_cast<uint32>(front.kind)
            << ") cannot latch the event ACTIVE from step 0 — so"
               " IsPersistentAnchoredEventActive is false while those steps walk the"
               " tank off the anchor, DungeonClearAtObjectiveTrigger drops back to a"
               " plain arriveRadius test, and Advance fights the event for the tank"
               " until the step times out. Lead with a MoveTo, or make the front step"
               " a Custom driver hook.";
    }
}

// --- F3: wing/roster sync -------------------------------------------------

TEST(DungeonEventIntegrityTest, IsolatedWingObjectivesAppearInExactlyOneWing)
{
    // On a physically-isolated split map the boss list is filtered to the bot's
    // wing; a synthetic objective NOT listed in a wing is silently dropped and
    // never cleared. Assert every added objective on such a map is in exactly one
    // wing.
    for (BossRosterPatch const& patch : BossRosterRegistry::AllPatches())
    {
        DungeonWingLayout const* layout = DungeonWingRegistry::Get(patch.mapId);
        if (!layout || !layout->isolated)
            continue;  // single-wing, or label-only (Maraudon) — no filter applied
        for (DungeonBossInfo const& e : patch.add)
        {
            if (e.kind != DungeonAnchorKind::Objective)
                continue;
            int inWings = 0;
            for (DungeonWing const& w : layout->wings)
                for (uint32 entry : w.bossEntries)
                    if (entry == e.entry)
                        ++inWings;
            EXPECT_EQ(inWings, 1)
                << "objective entry " << e.entry << " (" << e.name << ") on isolated "
                << "wing-split map " << patch.mapId << " is listed in " << inWings
                << " wings (expected exactly 1) — it will be silently dropped from the clear";
        }
    }
}

// --- Sethekk Halls (556): force-summon & kill Anzu ------------------------
// A focused regression over the specific wiring of the forced-Anzu bonus boss.
// The generic integrity tests above already cover it structurally; this pins the
// exact shape so a future edit that, say, drops .Persistent() or renumbers the
// hook fails loudly with intent rather than as an anonymous parametrized row.
TEST(DungeonEventIntegrityTest, SethekkAnzuEventIsWiredForForcedSummon)
{
    DungeonEvent const* ev = DungeonEventRegistry::Find(/*map*/ 556, /*eventId*/ 1);
    ASSERT_NE(ev, nullptr) << "Sethekk Halls (556) event 1 (force-summon Anzu) is missing";

    // Bonus boss: never stall the clear to Ikiss (Optional == !required), and it
    // spans the ~40s summon + the fight across combat gaps (Persistent).
    EXPECT_FALSE(ev->required) << "Anzu event must be Optional (bonus boss)";
    EXPECT_TRUE(ev->persistent) << "Anzu event must be Persistent (summon + fight span combat gaps)";
    EXPECT_EQ(ev->activation, EventActivation::Anchored);

    // Anzu is a HEROIC-ONLY bonus boss: the event must never fire on a normal run.
    EXPECT_EQ(ev->gate, DcDifficultyGate::HeroicOnly)
        << "Anzu event must be HeroicOnly (blizzlike heroic-only bonus boss)";

    // Step order matters: clear the room BEFORE poking the summon (Anzu's
    // SetInCombatWithZone would otherwise drag surviving trash into the fight).
    int clearStep = -1;
    int pokeStep = -1;
    int killStep = -1;
    for (size_t i = 0; i < ev->steps.size(); ++i)
    {
        EventStep const& s = ev->steps[i];
        if (s.kind == EventStepKind::ClearRadius)
            clearStep = static_cast<int>(i);
        if (s.kind == EventStepKind::Custom && s.hookId == 7)
            pokeStep = static_cast<int>(i);
        // KillCreatureEngage builds a KillCreature step with engage == true.
        if (s.kind == EventStepKind::KillCreature && s.engage && s.creatureEntry == 23035)
            killStep = static_cast<int>(i);
    }
    EXPECT_GE(clearStep, 0) << "Anzu event must pre-clear the room (a ClearRadius step)";
    EXPECT_GE(pokeStep, 0) << "Anzu event must poke via Custom hook 7 (DriveAnzuSummon)";
    EXPECT_GE(killStep, 0) << "Anzu event must KillCreatureEngage Anzu (23035)";
    EXPECT_TRUE(ObjectiveHookRegistry::Has(7)) << "hook 7 (DriveAnzuSummon) must be registered";
    EXPECT_LT(clearStep, pokeStep) << "room must be cleared BEFORE the Anzu summon poke";
    EXPECT_LT(pokeStep, killStep) << "the summon poke must precede the Anzu kill step";
}

TEST(DungeonEventIntegrityTest, ArcatrazSoulEaterRoomIsAnEntryFilteredSweep)
{
    DungeonEvent const* ev = DungeonEventRegistry::Find(/*map*/ 552, /*eventId*/ 2);
    ASSERT_NE(ev, nullptr) << "Arcatraz (552) event 2 (Eredar Soul-Eater room) is missing";

    // Trash, not an encounter: conditional (its own proximity + completion gate),
    // no roster objective. Persistent because the sweep is a fight and a mid-fight
    // Drive gap would otherwise rewind the step list and restart the walk-in.
    EXPECT_EQ(ev->activation, EventActivation::Conditional);
    EXPECT_TRUE(ev->persistent) << "the sweep spans combat gaps";
    // The 45yd aura is on the spawns in BOTH difficulties — never gate this heroic-only.
    EXPECT_EQ(ev->gate, DcDifficultyGate::Any);

    // Walk into the room, THEN sweep: the executor refuses to certify "clear" from
    // further out than DC_EVENT_CLEAR_JUDGE_RADIUS (12), so judging has to happen
    // from the middle of the room rather than the doorway.
    int moveStep = -1;
    int clearStep = -1;
    for (size_t i = 0; i < ev->steps.size(); ++i)
    {
        if (ev->steps[i].kind == EventStepKind::MoveTo)
            moveStep = static_cast<int>(i);
        if (ev->steps[i].kind == EventStepKind::ClearRadius)
            clearStep = static_cast<int>(i);
    }
    ASSERT_GE(moveStep, 0) << "must walk the tank into the room first";
    ASSERT_GE(clearStep, 0) << "must sweep the room (a ClearRadius step)";
    EXPECT_LT(moveStep, clearStep) << "arrive before judging the room clear";

    // BY-ENTRY BACKSTOP (the Shattered Halls assassin lesson). A ClearRadius gate
    // can only fight what AttackersValue::IsPossibleTarget (CanSeeOrDetect) and a
    // strict IsEngageReachable let it see; anything they reject reads as "room
    // clear" and latches the event done with a live 750-per-2s caster standing.
    // KillCreatureEngage resolves by entry via FindNearestCreature — no visibility
    // filter, looser reachability — so one per multispawn entry must follow the
    // sweep and catch whatever it could not see.
    std::vector<uint32> engageEntries;
    for (size_t i = 0; i < ev->steps.size(); ++i)
    {
        EventStep const& s = ev->steps[i];
        if (s.kind != EventStepKind::KillCreature || !s.engage)
            continue;
        EXPECT_GT(static_cast<int>(i), clearStep)
            << "the by-entry backstop must follow the sweep, not precede it";
        engageEntries.push_back(s.creatureEntry);
    }
    std::sort(engageEntries.begin(), engageEntries.end());
    EXPECT_EQ(engageEntries, (std::vector<uint32>{20879u, 20880u}))
        << "one KillCreatureEngage backstop per multispawn entry";

    EventStep const& sweep = ev->steps[clearStep];

    // The filter is the whole point: the sweep is the three Soul-Eaters and
    // nothing else. Unfiltered it would also pull the Arcatraz Sentinel on the
    // room's west edge (whose death summons the unkillable 21761 pulse) and offer
    // up the three corpse props as targets.
    //
    // BOTH multispawn entries, and both must be the NORMAL ones. Every one of the
    // three spawn points rolls 20879 or 20880 independently at spawn and again at
    // each respawn, so a room can contain three Deathbringers and no Soul-Eater —
    // a 20879-only filter would certify that room "clear" instantly. And
    // Creature::InitEntry keeps GetEntry() on the normal entry in every
    // difficulty, so the heroic templates (21595 / 21594) can never match.
    ASSERT_EQ(sweep.entryFilter.size(), 2u);
    EXPECT_NE(std::find(sweep.entryFilter.begin(), sweep.entryFilter.end(), 20879u),
              sweep.entryFilter.end()) << "Eredar Soul-Eater";
    EXPECT_NE(std::find(sweep.entryFilter.begin(), sweep.entryFilter.end(), 20880u),
              sweep.entryFilter.end()) << "Eredar Deathbringer (multispawn variant)";
    EXPECT_EQ(std::find(sweep.entryFilter.begin(), sweep.entryFilter.end(), 21595u),
              sweep.entryFilter.end()) << "heroic template can never match GetEntry()";
    EXPECT_EQ(std::find(sweep.entryFilter.begin(), sweep.entryFilter.end(), 21594u),
              sweep.entryFilter.end()) << "heroic template can never match GetEntry()";

    // Every spawn (305.7,148.1,24.9) / (285.5,146.2,22.3) / (301.8,127.4,22.3)
    // must sit inside the volume, or the gate certifies with one still alive.
    struct P { float x, y, z; };
    static constexpr P kSpawns[] = {
        { 305.7f, 148.1f, 24.9f }, { 285.5f, 146.2f, 22.3f }, { 301.8f, 127.4f, 22.3f },
    };
    for (P const& p : kSpawns)
    {
        float const dx = p.x - sweep.x;
        float const dy = p.y - sweep.y;
        EXPECT_LT(std::sqrt(dx * dx + dy * dy), sweep.radius) << "spawn outside the sweep radius";
        EXPECT_LT(std::fabs(p.z - sweep.z), sweep.zBand) << "spawn outside the sweep z-band";
    }

    // ...and the tank must be able to judge from the anchor: the executor only
    // trusts a "clear" verdict taken within 12yd of the centre, so the arrival
    // radius has to land it inside that.
    EXPECT_LE(ev->steps[moveStep].radius, 12.0f)
        << "arrival radius must land the tank inside DC_EVENT_CLEAR_JUDGE_RADIUS";

    // MUST-FINISH CONTRACT. The party cannot rest in this room (the aura holds it
    // flagged, and every rest path bails on the raw combat flag), so a half-driven
    // sweep leaves it standing in up to three 750-per-2s Unholy Auras with no way
    // to recover. Two authoring mistakes would break that, and both look harmless:
    //
    //   Optional() — a Failed step would Skip instead of Stall, advancing the
    //   clear with live Deathbringers behind the party.
    EXPECT_TRUE(ev->required)
        << "the Eredar room must not be skippable — a half-cleared room is the bug";
    //
    //   A tight timeout — required means Failed => Stalled, and DcRunEventAction
    //   answers Stalled by PARKING the tank. In this room parking is the wipe, so
    //   the timeout has to sit far past a real fight (3 elites, ~30-60s each for a
    //   5-bot party) rather than close to it.
    EXPECT_GE(sweep.timeoutMs, 300000u)
        << "sweep timeout must be far past a legitimate 3-elite fight; a Stall here parks "
           "the party inside the damage aura";
}

// --- drivesInCombat containment -------------------------------------------
// The flag takes ticks away from the stock combat engine: while it is set and the
// event's condition reads true, the leader's whole combat tick is spent on
// DcRunEventCombatAction (DcRel::EventDueCombat = 61, above every stock combat
// mover). That is exactly right for a continuous wave encounter whose event IS
// the fight, and exactly wrong anywhere else — an ordinary lever/gossip event
// with the flag set would hijack every fight on its map.
//
// So this is a CONTAINMENT tripwire, not a style check: the flag stays on the
// hand-vetted list below and nowhere else. Adding a map here is a deliberate act
// that says "this encounter cannot be steered between pulls".
TEST(DungeonEventIntegrityTest, DrivesInCombatIsConfinedToVettedWaveEncounters)
{
    struct Row { uint32 mapId; uint32 eventId; };
    static constexpr Row kVetted[] = {
        // The Black Morass "Close the time rift". 18 rifts, each pumping one add
        // every 15s until its keeper dies; the party is in combat from the first
        // pull to the last, so the non-combat rung ran only in the gaps between
        // waves and stopped running entirely once two rifts were open at once —
        // at which point nothing ever walked the tank to a portal and no rift ever
        // closed. See DungeonEvent::drivesInCombat.
        {269, 4},
        // Drak'Tharon Keep "Novos: hold the camp". boss_novos summons a Fetid
        // Troll Corpse every 3 SECONDS for the whole of phase 1 and each one
        // DoZoneInCombat()s itself, so the party is in combat continuously from
        // the pull until the 70s+ Crystal Handler gate opens. The non-combat
        // rung would never run once, and the tank would fight wherever the pull
        // left it — which is inside an 11yd / 1665-per-second Arcane Field.
        {600, 1},
        // The Violet Hold "Repel the wave". An 18-wave siege in which a keeper
        // portal summons 3 (+1 from wave 12) trash EVERY 20 SECONDS for as long
        // as its keeper lives, and the keeper stands at the portal and never
        // aggros. The party is in combat from wave 1 to wave 18, so the
        // non-combat rung would run only in the shrinking gaps between waves and
        // stop entirely once the party fell behind — at which point nothing ever
        // walks the tank to the one thing that turns the pump off, and the door
        // seal drains out from under a party that never leaves combat.
        {608, 5},
        // Blackwing Lair "Razorgore — orb and egg run". The instance spawns 2-5
        // adds every 15 seconds from eight floor positions for as long as an egg
        // is standing, and each one DoZoneInCombat()s itself — so from the first
        // egg to the thirtieth (a measured ~135s: a 262yd tour plus 30 three-
        // second casts) the raid never leaves combat. The non-combat rung would
        // get its ticks only before the first egg and never again, which is
        // precisely backwards: nothing would re-elect a runner when a 90s mind
        // control expires, and phase 1 would never end. Note this driver does not
        // steer the TANK at all — it yields every tick — so the cost the flag
        // usually carries (taking the combat tick off the stock movers) is not
        // paid here.
        {469, 1},
        // Blackwing Lair "Cross the Suppression Rooms". THE flag this event turns
        // on, and the reason the leg needs an event at all. Two rooms hold 160
        // Corrupted Whelps on a THIRTY-SECOND respawn, a hundred of them within
        // 20yd of the 375yd route line — so AnyPartyEngagement is true essentially
        // without a break, DcCombatFlag::MayDrive is therefore false, and Advance
        // (which lives only in the non-combat engine) never runs. The clear does
        // not cross this leg slowly; it does not cross it at all. Unlike the
        // Razorgore row above, this driver DOES steer the tank and so does pay the
        // flag's usual cost — which is why it yields the tick on every hold, and
        // why "advance" is the only branch that returns Running.
        {469, 3},
        // Halls of Lightning "Cross the Slag Furnace". Blackwing Lair's argument
        // verbatim, with smaller numbers that reach the same conclusion: fourteen
        // Slag (28585) line the pit walkway on a TWENTY-SECOND respawn with an 8yd
        // wander, and every one of them is inside aggro range of the route line —
        // so AnyPartyEngagement never drops for the no-engage grace window,
        // DcCombatFlag::MayDrive is false, and Advance (non-combat engine only)
        // never runs. The leg is not slow, it is stopped. This driver steers the
        // tank and so pays the flag's usual cost, which is why it yields the tick
        // on every hold and "advance" is the only branch that returns Running.
        {602, 1},
        // Halls of Stone "Repel the Tribunal wave". The Tribunal of Ages is a
        // FIXED 300-second survival timer, and brann_bronzebeardAI::JustSummoned
        // calls SetInCombatWithZone() on EVERY add it spawns — so from the first
        // Dark Rune Protector at t ~ 52s the party is in unbroken combat to
        // t = 300s with no gaps at all. The non-combat rung would not run once in
        // those four minutes, and the tank would fight wherever the last pull left
        // it instead of on the intercept line between the three spawn points and
        // Brann. That matters more here than on the other four rows: the adds are
        // Taunt-wired to Brann (51774 / 51775), he is REACT_PASSIVE with
        // SetRegeneratingHealth(false), and HIS DEATH IS THE ENCOUNTER'S ONLY FAIL
        // CONDITION — it resets boss state 2 to NOT_STARTED and sends him back to
        // a DB spawn 200yd away, costing a second full escort.
        {599, 4},
        // Utgarde Pinnacle "Bring down Grauf". The moment areatrigger 4991 trips,
        // a 38667 Combat Trigger at the add-spawn corner calls DoZoneInCombat() on
        // the whole hall and a World Trigger starts casting 59275 Summon Gauntlet
        // Mobs Periodic — a deque of eight summon spells, two per tick, WITH NO END
        // CONDITION but the drake's death. The party is therefore in unbroken
        // combat for the whole of phase 1, so the non-combat rung would never run
        // once — and phase 1 cannot be won by fighting at all: Grauf carries
        // IMMUNE_TO_PC and Skadi is NOT_SELECTABLE until he dies, so the ONLY
        // damage in the encounter is a GameObject click this driver has to be alive
        // to make. It yields on every tick it is not walking or firing, which is
        // most of them.
        {575, 4},
        // Utgarde Pinnacle "Svala's ritual: kill the channelers". The whole 25s is
        // a fight — three Ritual Channelers holding an INFINITE-duration Paralyze
        // (48278, ends only when its caster dies) on a teleported party member —
        // and the retarget this event exists to make is a COMBAT-tick act. An
        // out-of-combat-only rung would never fire once inside a boss encounter.
        {575, 5},
        // Pit of Saron "Run the Ymirjar gauntlet". From the tick areatrigger 5578
        // is accepted the party is fighting: ten Ymirjar arrive on splines at two
        // fixed homes and go REACT_AGGRESSIVE in place, and the moment they are
        // dead the driver has to walk the leader eighty yards to the NEXT gate —
        // which is only accepted while the counter still reads AFTER_WARN_1. Then
        // six (twelve heroic) Wrathbone, then the same again. The gaps between
        // those fights are measured in seconds, so the non-combat rung would get
        // its ticks before the first gate and essentially never again — and the
        // thing this driver exists to do happens entirely inside them. Note it
        // yields the tick on every wave tick it is not steering, which is most of
        // them, so the flag's usual cost (taking the combat tick off the stock
        // movers) is paid only while it is walking.
        {658, 1},
        // Halls of Reflection "Hold the altar". The gaps between waves 1-2-3-4 and
        // 6-7-8-9 are FIVE SECONDS — instance_halls_of_reflection cuts its 150s
        // wave timer to 5000 the moment the last trash mob of a wave dies — and
        // the two boss waves are continuous. An out-of-combat-only rung would get
        // five seconds in every ninety, and it would stop running entirely once
        // the party fell behind, which is exactly when the hold matters most: a
        // party that falls behind gets scattered, and scattering past 70.5yd from
        // the altar WIPES THE EVENT and respawns every dead mob. Note this driver
        // yields the tick on every wave tick (the mobs come to the party — there
        // is nothing to steer), so the flag's usual cost is paid only on the few
        // ticks it is actually walking the tank back to the camp.
        {668, 2},
        // Halls of Reflection "Escape the Lich King". The strongest case in this
        // list: npc_hor_lich_kingAI calls SetInCombatWithZone() on every player
        // ONCE A SECOND for the whole four-to-six-minute escape, so a
        // non-combat-only rung would get exactly zero ticks. The flag is not an
        // optimisation here, it is the difference between the event existing and
        // not. It yields for most of every wall fight.
        {668, 5},
        // The Culling of Stratholme "The ten waves of Stratholme". THERE IS NO
        // INTER-WAVE TIMER ON THIS MAP AT ALL: npc_arthasAI::SendNextWave summons
        // the next four mobs in the SAME CALL as the previous wave's fourth death,
        // so the party is in combat essentially without a break from wave 1 to
        // Salramm, and the only gaps are the seconds it takes to walk between
        // clusters. An out-of-combat-only rung would get the twenty seconds before
        // wave 1 and essentially nothing afterwards — and the thing this driver
        // exists to do happens entirely in those gaps, because the four spawn
        // clusters are up to 264yd apart and the mobs have NO movement script.
        // Note it yields the tick on every wave tick it is not steering, which is
        // most of them, so the flag's usual cost is paid only while it is walking.
        {595, 10},
        // Trial of the Champion "The Champion's Arena". The flag here is for the
        // OPPOSITE reason to every row above: the driver never steers under fire.
        // It takes combat ticks only to hand them straight back — every in-combat
        // verdict is a yield (the joust is mod-playerbots' `wotlk-toc`, every
        // other fight the stock engine's) — and it needs to SEE them because the
        // progress counter moves mid-fight and the telemetry line reports it.
        // Its combat rung (61) is below `toc mounted` (66) regardless.
        {650, 4},
        // The Oculus "The Oculus ascent". The ToC reason, not the wave reason: the
        // driver never steers under fire — it NEVER lifts off while anyone is
        // engaged, because a drake spell on a ground mob kills its rider — and every
        // in-combat verdict on the ground is a yield. It needs combat ticks to SEE a
        // Guardian picket hold riders mid-leg (a Hold, so the ladder stays off the
        // mounted tank) and the Eregos fight, which is in combat from start to end.
        {578, 9},
    };

    for (DungeonEvent const& ev : DungeonEventRegistry::AllEvents())
    {
        if (!ev.drivesInCombat)
            continue;

        bool vetted = false;
        for (Row const& r : kVetted)
            if (r.mapId == ev.mapId && r.eventId == ev.id)
                vetted = true;

        EXPECT_TRUE(vetted)
            << "event " << ev.mapId << "/" << ev.id << " '" << ev.name
            << "' sets DrivesInCombat(), which hands the leader's COMBAT tick to the"
               " event driver above every stock combat mover. If that is genuinely"
               " intended (a continuous wave encounter that cannot be steered"
               " between pulls), add it to kVetted here with the reason.";

        // The combat rung is a copy of the CONDITIONAL rung; an anchored event is
        // driven off its objective arrival and never reaches it, so the flag there
        // would be silently inert.
        EXPECT_EQ(ev.activation, EventActivation::Conditional)
            << "event " << ev.mapId << "/" << ev.id
            << " sets DrivesInCombat() but is Anchored — the flag only has an"
               " effect on Conditional events (DungeonClearEventDueCombatTrigger).";
    }
}

// The Mechanar bridge gauntlet is a DEFENSIVE set-piece: three scripted waves
// DoZoneInCombat the party from up the bridge and run down to it, and Pathaleon
// only becomes attackable once the four wave-3 deaths have ticked the instance
// script's persistent counter to 4. Every property below encodes "hold the camp,
// let them come" — the previous shape walked the tank up the bridge into the
// oncoming wave and on toward the boss, which is what this pins shut.
TEST(DungeonEventIntegrityTest, MechanarBridgeIsHeldAsACampNotWalked)
{
    DungeonEvent const* ev = DungeonEventRegistry::Find(/*map*/ 554, /*eventId*/ 3);
    ASSERT_NE(ev, nullptr) << "The Mechanar (554) event 3 (bridge gauntlet) is missing";
    EXPECT_TRUE(ev->persistent)
        << "the camp spans the whole gauntlet's combat; a rewind restarts it at step 0";

    // No advance and no sweep. A ClearRadius drives EngageDirect into its volume,
    // and a second MoveTo to a point further up the deck IS the aggressive walk.
    // The waves come to the party; the combat engine does the killing.
    for (EventStep const& s : ev->steps)
        EXPECT_NE(s.kind, EventStepKind::ClearRadius)
            << "a ClearRadius here walks the party up the bridge into the next wave";

    // Exactly one garrison, gated on the PERSISTENT counter. instance_mechanar
    // stores DATA_BRIDGE_MOB_DEATH_COUNT in the persistent vector and never
    // overrides GetData, so an instanceDataId gate would read 0 forever.
    EventStep const* camp = nullptr;
    int garrisons = 0;
    for (EventStep const& s : ev->steps)
        if (s.kind == EventStepKind::MoveTo &&
            (s.persistentDataId >= 0 || s.instanceDataId >= 0 || s.creatureEntry != 0))
        {
            camp = &s;
            ++garrisons;
        }
    ASSERT_NE(camp, nullptr) << "the bridge must be held by a garrison step";
    EXPECT_EQ(garrisons, 1) << "one camp; a second garrison further up is an advance";
    EXPECT_EQ(camp->persistentDataId, 0)
        << "the gate must read DATA_BRIDGE_MOB_DEATH_COUNT (persistent index 0)";
    EXPECT_EQ(camp->instanceDataId, -1)
        << "instance_mechanar never overrides GetData — this gate would never clear";
    EXPECT_EQ(camp->persistentDataMin, 4u)
        << "only the four wave-3 deaths write the counter, so 4 means 'last wave down'";

    // The camp sits on the bridge deck's centre line (x130..146), PAST the wave-1
    // cluster (y37.3..41.2) and SHORT of wave 3 (y100..112). Past wave 1 is the
    // arrivability property: an anchored event drives only out of combat, and the
    // gauntlet leaves no out-of-combat gap once wave 1 is up, so an anchor short of
    // wave 1 is never reached and the event never starts (tr-20260816-105518-10).
    // Short of wave 3 is the "don't walk up the bridge to meet them" property.
    EXPECT_GT(camp->y, 41.2f) << "an anchor short of wave 1 is never arrived at";
    EXPECT_LT(camp->y, 100.0f) << "the camp is up among the wave-3 spawns";
    EXPECT_GT(camp->x, 130.0f);
    EXPECT_LT(camp->x, 146.0f);
    // A garrison radius is a leash, not a dead band (the Ring of Law lesson).
    EXPECT_LE(camp->radius, 8.0f) << "too wide to re-centre the tank between waves";

    // Every step before the camp must be an approach to the SAME spot: anything
    // else is a second position the party is walked to before the waves are down.
    for (EventStep const& s : ev->steps)
    {
        if (&s == camp)
            break;
        ASSERT_EQ(s.kind, EventStepKind::MoveTo)
            << "only a walk-in may precede the camp";
        EXPECT_FLOAT_EQ(s.x, camp->x);
        EXPECT_FLOAT_EQ(s.y, camp->y);
    }

    // The boss is taken only after the camp. The seek must reach him from the camp
    // (he is at (139.5, 149.3), ~105yd away) and must NOT reach much further: the
    // combat-side stealth-breaker arms off this step's entry+radius, and Pathaleon
    // is greater-invisible until the counter hits 4, so a wide radius makes him look
    // like a stuck stealthed sapper from anywhere on the floor and the tank sprints
    // at him mid-fight. Both guards below were the tp-20260816-105517-2 regression.
    ASSERT_FALSE(ev->steps.empty());
    EventStep const& last = ev->steps.back();
    EXPECT_EQ(last.kind, EventStepKind::KillCreature);
    EXPECT_TRUE(last.engage);
    EXPECT_EQ(last.creatureEntry, 19220u) << "Pathaleon the Calculator";
    float const dx = 139.5f - camp->x, dy = 149.3f - camp->y;
    float const campToBoss = std::sqrt(dx * dx + dy * dy);
    EXPECT_GE(last.radius, campToBoss)
        << "the seek radius must reach Pathaleon from the camp (" << campToBoss << "yd)";
    EXPECT_LE(last.radius, campToBoss + 40.0f)
        << "a seek radius this wide arms the combat stealth-breaker across the floor";
    EXPECT_TRUE(last.engageOnlyWhenActive)
        << "Pathaleon is invisible by script until the gauntlet ends — without this"
           " the combat-side stealth-breaker walks the tank at him mid-fight";
}

// The Shattered Halls flame gauntlet is the OPPOSITE call from the Mechanar
// bridge above, and the contrast is the point: there, nothing forces the party
// forward and holding is correct; here the fire is unavoidable in the corridor
// (an unbroken x~261..497 band once the 20 wandering Flame Arrow anchors' 12-17yd
// wander and 15yd trigger are added up) and only exists while the two archers
// live, so holding is strictly worse the longer it lasts. The answer is neither
// a camp nor a sprint: BOUNDS.
//
// Every property below encodes "fight, then push, ~40yd at a time, and turn the
// fire off before the big fight". The shape this replaced ran entry->ledge in one
// 90yd hop and cost three deaths with the party strung out (tr-20260816-144504-8).
TEST(DungeonEventIntegrityTest, ShatteredHallsGauntletIsFoughtInBounds)
{
    DungeonEvent const* ev = DungeonEventRegistry::Find(/*map*/ 540, /*eventId*/ 2);
    ASSERT_NE(ev, nullptr) << "The Shattered Halls (540) event 2 (flame gauntlet) is missing";
    EXPECT_TRUE(ev->persistent)
        << "the gauntlet spans minutes of wave combat; a rewind restarts it at step 0";

    ASSERT_GE(ev->steps.size(), 7u) << "entry + bounds + staging + archers + ledge";

    // Step 0 is the entry walk-in. It both arms the encounter (the scout at
    // (341.3, 314.9) triggers on any player within 50yd 2D at z > -3) and bumps
    // stepIndex so the persistence sticky-trigger latches.
    EXPECT_EQ(ev->steps[0].kind, EventStepKind::MoveTo);
    EXPECT_NEAR(ev->steps[0].x, 300.0f, 1.0f);
    float const scoutDist = 341.3f - ev->steps[0].x;
    EXPECT_LT(scoutDist, 50.0f)
        << "the entry anchor must be inside the scout's 50yd trigger, or the party"
           " stands there waiting for a gauntlet that never starts";

    // The bounds march monotonically EAST and none of them is a long hop. A leg
    // longer than ~45yd is where the party strings out and meets a wave with the
    // tank alone at the front.
    float prevX = ev->steps[0].x;
    int bounds = 0;
    for (std::size_t i = 1; i < ev->steps.size(); ++i)
    {
        EventStep const& s = ev->steps[i];
        if (s.kind != EventStepKind::ClearRadius && s.kind != EventStepKind::MoveTo)
            continue;
        EXPECT_GT(s.x, prevX)
            << "step " << i << " walks BACK down the corridor";
        EXPECT_LE(s.x - prevX, 45.0f)
            << "step " << i << " is a " << (s.x - prevX) << "yd hop — long enough for"
               " the party to string out across it";
        prevX = s.x;
        if (s.kind == EventStepKind::ClearRadius)
            ++bounds;
    }
    EXPECT_GE(bounds, 4) << "fewer bounds than this is a sprint with extra steps";

    // The archers are killed BY ENTRY and BEFORE the ledge is cleared: their
    // death is the off-switch for the fire (FireArrows() stops re-arming once no
    // 17427 is alive), so doing it first is what makes the last fight safe.
    std::size_t archerStep = ev->steps.size();
    std::size_t ledgeStep = ev->steps.size();
    for (std::size_t i = 0; i < ev->steps.size(); ++i)
    {
        EventStep const& s = ev->steps[i];
        if (s.kind == EventStepKind::KillCreature && s.creatureEntry == 17427u)
            archerStep = i;
        if (s.kind == EventStepKind::ClearRadius && s.x > 500.0f)
            ledgeStep = i;
    }
    ASSERT_LT(archerStep, ev->steps.size()) << "nothing kills the Shattered Hand Archers";
    ASSERT_LT(ledgeStep, ev->steps.size()) << "nothing clears the far ledge";
    EXPECT_LT(archerStep, ledgeStep)
        << "the fire must be switched off before the 12-zealot pack fight, not after";
    EXPECT_TRUE(ev->steps[archerStep].engage)
        << "the archers stand behind the pack — the step has to SEEK them";
    EXPECT_TRUE(ev->steps[archerStep].engageOnlyWhenActive)
        << "keep the combat-side stealth-breaker from arming off this step out of"
           " turn (the Mechanar/Pathaleon lesson)";

    // The staging step immediately before the archer kill sits on the scout's own
    // waypoint terminus: past every flame anchor's reach (the last two spawn at
    // x467.5/x468.7 with 13yd wander, so x481.7 worst case) and short of the
    // nearest far-pack zealot at x498.9. That is the only fire-free ground within
    // aggro reach of the pack, and Blizzard's own script marks it.
    ASSERT_GT(archerStep, 0u);
    EventStep const& stage = ev->steps[archerStep - 1];
    EXPECT_EQ(stage.kind, EventStepKind::MoveTo);
    EXPECT_GT(stage.x, 482.0f) << "the staging point is still inside the fire band";
    EXPECT_LT(stage.x, 498.9f) << "the staging point is inside the far pack";

    // The seek must reach the archers (514.5, 319.7) from there.
    float const adx = 514.5f - stage.x, ady = 319.7f - stage.y;
    EXPECT_GE(ev->steps[archerStep].radius, std::sqrt(adx * adx + ady * ady));

    // The ledge clear is the last step and its volume covers the whole far pack
    // (zealots x498.9..515.1, y292.4..340.4, plus the Blood Guard at 512.7/315.7
    // whose death cancels the wave scheduler on normal).
    EXPECT_EQ(ledgeStep, ev->steps.size() - 1);
    EventStep const& ledge = ev->steps[ledgeStep];
    EXPECT_GE(ledge.radius, 30.0f) << "too tight to cover the spread-out far pack";
    for (auto const& pack : { std::pair<float, float>{498.9f, 309.1f},
                              std::pair<float, float>{515.1f, 339.8f},
                              std::pair<float, float>{510.7f, 292.4f},
                              std::pair<float, float>{512.7f, 315.7f} })
    {
        float const dx = pack.first - ledge.x, dy = pack.second - ledge.y;
        EXPECT_LE(std::sqrt(dx * dx + dy * dy), ledge.radius)
            << "far-pack spawn (" << pack.first << "," << pack.second
            << ") falls outside the ledge clear";
    }
}

// The BRD Ring of Law, pinned to the two properties tr-20260808-150405-10 broke
// on. Both look like tuning and are not.
TEST(DungeonEventIntegrityTest, RingOfLawGarrisonsTheCentreAndCanRestartItself)
{
    DungeonEvent const* ev = DungeonEventRegistry::Find(/*map*/ 230, /*eventId*/ 1);
    ASSERT_NE(ev, nullptr) << "Blackrock Depths (230) event 1 (Ring of Law) is missing";

    EventStep const* hold = nullptr;
    for (EventStep const& s : ev->steps)
        if (s.kind == EventStepKind::MoveTo && s.instanceDataId >= 0)
            hold = &s;
    ASSERT_NE(hold, nullptr) << "the Ring of Law must hold on TYPE_RING_OF_LAW";

    // A garrison radius is a LEASH, not a dead band. At 10yd the tank simply kept
    // wherever the last wave died — 9.7yd out toward the mob gate, never
    // re-centring, for the rest of the run.
    EXPECT_LE(hold->radius, 5.0f)
        << "the arena garrison must actually re-centre the tank between waves;"
           " a radius this wide is a dead band the tank parks inside";

    // TYPE_RING_OF_LAW is not monotonic: npc_grimstone's 30s no-victim watchdog
    // SetData(FAIL)s it back to NOT_STARTED and despawns Grimstone and every
    // summon. Without a hook running inside the hold, nothing ever notices — the
    // Custom step that started it latched Done and the areatrigger relay is edge-
    // triggered on a volume the party is standing in.
    EXPECT_NE(hold->hookId, 0u)
        << "the hold must re-run the start hook (.WhileHolding) or a Grimstone-side"
           " reset stalls the party in an empty arena until the step times out";
    EXPECT_TRUE(ObjectiveHookRegistry::Has(hold->hookId))
        << "the Ring of Law hold references unregistered hook " << hold->hookId;
}

// The Black Morass wave driver, pinned to its exact shape. Every one of these
// properties was a live failure before it was set, so a future edit that drops
// one should fail loudly with intent rather than as a silent behaviour change.
TEST(DungeonEventIntegrityTest, BlackMorassWaveEventIsWiredForTheWaveDriver)
{
    DungeonEvent const* ev = DungeonEventRegistry::Find(/*map*/ 269, /*eventId*/ 4);
    ASSERT_NE(ev, nullptr) << "Black Morass (269) event 4 (close the time rift) is missing";

    EXPECT_EQ(ev->activation, EventActivation::Conditional);
    EXPECT_TRUE(static_cast<bool>(ev->condition)) << "the wave gate predicate must be bound";

    // 18 waves: the condition going false is the only "done".
    EXPECT_TRUE(ev->repeatable) << "wave event must be Repeatable (18 rifts)";
    // A wipe / corpse-run must never hard-stall the run for a human.
    EXPECT_FALSE(ev->required) << "wave event must be Optional (a timeout re-fires it fresh)";
    // THE fix for "they fall apart any time two portals are open": the party is in
    // combat essentially the whole encounter, so a non-combat-only driver never
    // runs once it falls behind.
    EXPECT_TRUE(ev->drivesInCombat) << "wave event must DriveInCombat (continuous wave fight)";

    // ONE step, and it must be the driver hook. The priority this encounter needs
    // ("always prefer the rift keeper, it is the only shutoff") is a standing
    // preference re-evaluated per tick, not a sequence — a step list can only
    // block in order, which is how the keeper ended up behind nine drainer gates
    // that a live rift re-blocked every 15s.
    ASSERT_EQ(ev->steps.size(), 1u)
        << "the wave event must be exactly one Custom step (the driver hook); a step"
           " list cannot express the keeper-first priority this encounter needs";
    EXPECT_EQ(ev->steps[0].kind, EventStepKind::Custom);
    EXPECT_EQ(ev->steps[0].hookId, 12u);
    EXPECT_TRUE(ObjectiveHookRegistry::Has(12)) << "hook 12 (BmDriveWave) must be registered";

    // The two entry lists the driver keys on must be non-empty and DISJOINT:
    // killing a keeper closes a rift and never drains the shield; killing a
    // drainer stops a drain and never closes a rift. Overlap would make the driver
    // chase adds as if they were the shutoff.
    ASSERT_FALSE(BlackMorassKeeperEntries().empty());
    ASSERT_FALSE(BlackMorassDrainEntries().empty());
    for (uint32 keeper : BlackMorassKeeperEntries())
        for (uint32 drainer : BlackMorassDrainEntries())
            EXPECT_NE(keeper, drainer)
                << "entry " << keeper << " is in BOTH the Black Morass keeper and"
                   " drain lists; they must stay disjoint";
}

// --- stepsOwnMovement containment ------------------------------------------
// The flag does two things, and both are dangerous on an event that does not own
// its own driving. It suppresses the per-tick position hold the driving actions
// apply (StopBot(Hold) anchored, ResolveEscortConflict conditional) — that hold is
// what stops a tank coasting past its objective on a stale glide. And it makes a
// completed repeatable event YIELD the engine tick rather than claim it, which is
// only safe when the step itself decides per tick whether it has work.
//
// Containment tripwire, same contract as the drivesInCombat one: the flag stays
// on the hand-vetted list and nowhere else.
TEST(DungeonEventIntegrityTest, StepsOwnMovementIsConfinedToVettedEvents)
{
    struct Row { uint32 mapId; uint32 eventId; };
    static constexpr Row kVetted[] = {
        // Black Morass "Defend Medivh": hook 8 walks the tank into Medivh's 20yd
        // start trigger and holds the Medivh-side hold point, both through the
        // long-haul spline funnel (the portals and the hold point are 80-102yd
        // apart, past what a bare MovePoint delivers).
        {269, 3},
        // Black Morass "Close the time rift": hook 12 walks the tank to the locked
        // rift's keeper and back to Medivh's ring, same funnel. With the hold in
        // place this glide was cancelled the tick after it was issued — 151 camp
        // attempts in one batch, none arriving, all logging a healthy spline.
        {269, 4},
        // Drak'Tharon Keep "Novos: hold the camp": hook 14 issues the garrison
        // MovePoint itself, so the per-tick hold would cancel it before the hook
        // ran. It is also what makes the driver YIELD on Done — this rung sits
        // above the stock combat movers and the tank must keep its rotation.
        {600, 1},
        // The Violet Hold, all five events. Hooks 15-19 issue every metre of
        // their own movement: hook 15 walks the tank into Sinclari's 5yd interact
        // range, hooks 16-18 garrison the door camp, and hook 19 runs the
        // long-haul funnel out to rim portals 52-86yd away. With the per-tick
        // hold in place each of those splines is cancelled the tick after it is
        // issued. On events 2-4 the flag is also what makes the garrison YIELD
        // rather than claim the tick, which the tank's rotation depends on.
        {608, 1},
        {608, 2},
        {608, 3},
        {608, 4},
        {608, 5},
        // Blackwing Lair "Razorgore — orb and egg run". The movement this event
        // issues is not the tank's at all: hook 20 walks the MIND-CONTROLLED BOSS
        // from egg to egg with its own splines, and the tank must be left alone to
        // hold the add wave. The per-tick hold would do exactly the wrong thing in
        // both directions — park the tank out of its fight, and (via
        // ResolveEscortConflict) cancel the orb runner's long-haul glide to the
        // ledge the tick after it is issued.
        {469, 1},
        // Blackwing Lair "Cross the Suppression Rooms": hook 21 issues every metre
        // of the crossing itself, one authored leg at a time through the long-haul
        // funnel (DcTransit::TravelTo). With the per-tick hold in place each of
        // those splines is cancelled the tick after it is issued and the raid
        // creeps a tick at a time down a 342yd route while every log line reports
        // a healthy spline — the Black Morass shape verbatim. The flag is also
        // what makes a Done RETURN YIELD, which is load-bearing here: the driver
        // holds for the ramp's six Taskmasters, and a raid that spends that fight
        // claiming the tick is a raid with no rotation.
        {469, 3},
        // Halls of Lightning "Cross the Slag Furnace": hook 24 issues every metre
        // of the crossing through the same long-haul funnel (DcTransit::TravelTo),
        // one authored leg at a time. With the per-tick hold in place each of
        // those splines is cancelled the tick after it is issued and the party
        // creeps down a 395yd route while every log line reports a healthy spline.
        // The flag is also what makes a Done RETURN YIELD, which is load-bearing:
        // the driver holds for the two Unbound Firestorms at the foot of the climb
        // out, and a party that spends that fight claiming the tick has no
        // rotation.
        {602, 1},
        // Halls of Stone, all four events.
        //
        // Events 1-3 are the Brann sequence and each is a hook- or
        // primitive-driven glide the per-tick hold would cancel the tick after it
        // is issued. Event 1's EscortCreature step follows Brann for 170yd and
        // breaks off to engage what attacks him; event 2's hook 22 garrisons the
        // hold point through the whole 300s defend AND owns the 200yd walk back to
        // his DB spawn when he dies; event 3 walks 300yd east to the door stage.
        // On events 1-3 the flag is also what makes the garrison YIELD rather than
        // claim the tick, which the tank's rotation depends on for five minutes of
        // continuous combat.
        //
        // Event 4 is the wave driver: hook 23 issues its own repositioning to the
        // add nearest Brann and back to the intercept line, and returns Done to
        // hand the tick to the rotation the moment it has nothing to steer.
        {599, 1},
        {599, 2},
        {599, 3},
        {599, 4},
        // Utgarde Pinnacle "Bring down Grauf": hook 27 walks the leader 161yd east
        // down the gauntlet hall to the harpoon pocket on its own long-haul spline
        // (DcTransit::TravelTo — a bare MovePoint truncates silently past ~150yd),
        // and the per-tick hold would cancel it the tick after it is issued. The
        // flag is also what makes a Done RETURN YIELD, and on this map that is the
        // load-bearing half: the driver is idle for most of every flight lap, and
        // those are precisely the ticks four bots and a tank need in order to fight
        // an add pump that never stops.
        {575, 4},
        // Utgarde Pinnacle "Svala's ritual: kill the channelers": the hook issues
        // NO movement at all and takes the flag purely for the yield semantics. It
        // sits above the stock combat movers to retarget the tank off a boss rooted
        // twenty yards in the air, and a rung there that claimed every tick would
        // starve the rotation it exists to redirect.
        {575, 5},
        // Pit of Saron "Run the Ymirjar gauntlet": hook 29 issues every metre of
        // the leg itself through the long-haul funnel (DcTransit::TravelTo) — the
        // hold-back at the Krick staging point, the walk into each of the three
        // gate spheres, and the close on a wave mob parked at a home position the
        // party would otherwise never reach. With the per-tick hold in place each
        // of those splines is cancelled the tick after it is issued and the party
        // creeps a tick at a time while every log line reports a healthy spline.
        // The flag is also what makes a Done RETURN YIELD, which is load-bearing
        // here: the driver is idle for most of both waves, and those are precisely
        // the ticks a tank and four bots need in order to kill sixteen mobs.
        {658, 1},
        // Pit of Saron "Tyrannus's ledge": hook 30 owns a gather hold on the ledge
        // anchor and then the ~30yd walk INTO areatrigger 5633's sphere — which is
        // the whole point of the event, because the anchor is deliberately 22yd
        // OUTSIDE that sphere so arriving cannot fire it. The per-tick hold would
        // cancel the walk-in the tick after it is issued and the encounter would
        // never start.
        {658, 2},
        // Halls of Reflection "Start the intro": hook 31 walks the tank the last
        // yards to Jaina/Sylvanas itself, through the long-haul funnel, before it
        // can send a gossip the whole dungeon is behind. The anchored per-tick
        // hold would cancel that walk-in the tick after it is issued.
        {668, 1},
        // Halls of Reflection "Hold the altar": hook 32 issues the walk back to
        // the camp itself — from wherever a wave ended, which can be sixty yards
        // across the chamber. The flag is also what makes a Done RETURN YIELD,
        // and on this map that is the half that decides fights: the driver has
        // NOTHING to steer for the whole of every wave, because the mobs walk to
        // the party by design, so every tick it claimed would be a tick the tank
        // did not swing across ten waves and two bosses.
        {668, 2},
        // Halls of Reflection "The throne room": hooks 33 and 34 own two walks the
        // anchor cannot express — 25yd from the west door INTO areatrigger 5605's
        // box (the anchor is deliberately outside it, so arriving must not fire
        // the cutscene), and then the hold on the muster point while the party
        // tops off before the point of no return.
        {668, 4},
        // Halls of Reflection "Escape the Lich King": hook 35 walks the tank stand
        // point to stand point down 679yd of escape path, on legs of 100-176yd,
        // against a Lich King who never pauses. It yields for most of every wall
        // fight — four summon batches totalling 831k-1.17M HP have to be killed by
        // a party whose leader is running a rung above the stock combat movers.
        {668, 5},
        // The Culling of Stratholme "The ten waves of Stratholme": hook 36 walks the
        // tank up to 264yd, cluster to cluster, on its own long-range spline — the
        // whole point of the event, because Arthas stops at waypoint 11 for the
        // entire phase and the wave mobs have no movement script, so nothing comes
        // to the party and a driver that only held would never fight. The per-tick
        // hold runs BEFORE Drive, so without the flag last tick's spline is
        // cancelled before the hook can see it. It is also what makes a Done RETURN
        // YIELD, and on this map that is the half that decides fights: the driver
        // has nothing to steer for the whole of every wave fight, and forty trash
        // mobs plus two bosses have to be killed by a party whose leader is running
        // a rung above the stock combat movers.
        {595, 10},
        // Trial of the Champion "The Champion's Arena": hook 37 RIDES A HORSE.
        // While the tank is mounted it steers the vehicle base itself (to the
        // announcer, back to the joust post), because nothing else in the module
        // can — the per-tick hold would cancel that before the hook saw it. And
        // Done has to yield: the Argent soldiers, the Argent champion and the
        // Black Knight are all stock-engine fights.
        {650, 4},
        // The Oculus "The Oculus ascent": hook 38 flies a drake — the vehicle base
        // is the only thing that can move the party between rings, and the per-tick
        // hold would pin the tank on the pad it has to take off from. (The moves
        // themselves are each rider's own rung; the hook holds the ladder off the
        // tank while they happen.) And Done has to yield: every island clear,
        // Varos and Urom are ground fights for the objectives and the stock engine.
        {578, 9},
    };

    for (DungeonEvent const& ev : DungeonEventRegistry::AllEvents())
    {
        if (!ev.stepsOwnMovement)
            continue;

        bool vetted = false;
        for (Row const& r : kVetted)
            if (r.mapId == ev.mapId && r.eventId == ev.id)
                vetted = true;

        EXPECT_TRUE(vetted)
            << "event " << ev.mapId << "/" << ev.id << " '" << ev.name
            << "' sets StepsOwnMovement(), which suppresses the per-tick hold that"
               " keeps a tank parked at its anchor. Only set it when the event's own"
               " steps issue the movement; then add it to kVetted here with the"
               " reason.";
    }
}

// Aeonus (17881) is a DRAINER, not a keeper — the single least obvious fact in
// the Black Morass wiring, and the party silently loses the run if it is
// mis-filed. boss_aeonus::IsSummonedBy does exactly what DoSummonAtRift does to
// the trash:
//
//     me->SetReactState(REACT_DEFENSIVE);
//     me->SetHomePosition(medivh + 14yd);
//     me->GetMotionMaster()->MoveTargetedHome();
//
// and JustReachedHome then DoCastAOE(37853) inside 20yd of Medivh, which
// spell_black_morass_corrupt_medivh drains at 2 per tick — DOUBLE the trash rate.
// So the final boss never aggros the party: it walks past them to Medivh and
// drains the shield out from under the run.
//
// It must NOT be in the keeper list, where it would fail both of that list's
// jobs: it is never at the rift to be selected on (it leaves immediately), and
// killing it closes nothing (npc_time_rift::JustSummoned sets _riftKeeperGUID
// only for the first NON-Aeonus summon, so SummonedCreatureDies never matches).
//
// Deja (17879) and Temporus (17880) are the contrast: plain BossAI with no
// IsSummonedBy override, so they stay aggressive and fight at their rift. They
// belong in the keeper list and nowhere near the drain list.
TEST(DungeonEventIntegrityTest, BlackMorassAeonusIsFiledAsADrainerNotAKeeper)
{
    constexpr uint32 kAeonus   = 17881;
    constexpr uint32 kDeja     = 17879;
    constexpr uint32 kTemporus = 17880;

    auto has = [](std::vector<uint32> const& v, uint32 e)
    { return std::find(v.begin(), v.end(), e) != v.end(); };

    EXPECT_TRUE(has(BlackMorassDrainEntries(), kAeonus))
        << "Aeonus must be in the DRAIN list: it spawns REACT_DEFENSIVE, homes to"
           " Medivh's 14yd ring and channels 37853 at double rate. Out of that list"
           " nothing force-pulls it and nothing counts it toward 'Medivh's ring is"
           " dirty', so the party never engages the final boss.";
    EXPECT_FALSE(has(BlackMorassKeeperEntries(), kAeonus))
        << "Aeonus must NOT be in the keeper list: it is never at the rift to be"
           " selected on, and killing it does not close its rift.";

    // The wave-6/12 bosses are the opposite case — keep them straight.
    EXPECT_TRUE(has(BlackMorassKeeperEntries(), kDeja));
    EXPECT_TRUE(has(BlackMorassKeeperEntries(), kTemporus));
    EXPECT_FALSE(has(BlackMorassDrainEntries(), kDeja));
    EXPECT_FALSE(has(BlackMorassDrainEntries(), kTemporus));
}

// --- objective-hook id space -------------------------------------------------
// Hook ids are ONE FLAT SPACE shared by every dungeon, and unlike event
// conditions (function pointers, so a typo is a compile error) nothing about an
// id is checked by the compiler. Now that controllers register from their own
// TUs (RegisterBlackMorassHooks) instead of one central initializer list, a
// copy-pasted id is a live hazard: emplace() keeps the first row and silently
// drops the second, so the losing dungeon's objective latches Done on arrival and
// its event never runs, with nothing in the log to say why. ObjectiveHookRegistry
// ::AddHook turns that into a LOG_ERROR; these cases turn it into a red test.

TEST(DungeonEventIntegrityTest, AddHookRejectsDuplicateIdsAndKeepsTheFirst)
{
    auto const first  = [](Player*, AiObjectContext*, DungeonBossInfo const&)
    { return ObjectiveArriveResult::Done; };
    auto const second = [](Player*, AiObjectContext*, DungeonBossInfo const&)
    { return ObjectiveArriveResult::Blocked; };

    ObjectiveHookRegistry::HookTable t;
    ObjectiveHookRegistry::AddHook(t, 42, first);
    ObjectiveHookRegistry::AddHook(t, 42, second);

    ASSERT_EQ(t.size(), 1u) << "a duplicate id must not add a second row";
    EXPECT_EQ(t.at(42)(nullptr, nullptr, DungeonBossInfo{}), ObjectiveArriveResult::Done)
        << "on a collision the FIRST registration wins; if the second silently"
           " replaced it, whichever dungeon's TU happened to register last would"
           " hijack the other's objective.";
}

TEST(DungeonEventIntegrityTest, AddHookRejectsReservedIdZeroAndEmptyCallables)
{
    ObjectiveHookRegistry::HookTable t;

    ObjectiveHookRegistry::AddHook(t, 0, [](Player*, AiObjectContext*, DungeonBossInfo const&)
                                   { return ObjectiveArriveResult::Done; });
    EXPECT_TRUE(t.empty())
        << "id 0 means 'no hook' on DungeonBossInfo::onArriveHook and EventStep::hookId;"
           " registering it would make a hookless objective run someone's handler.";

    ObjectiveHookRegistry::AddHook(t, 43, ObjectiveHookRegistry::Hook{});
    EXPECT_TRUE(t.empty())
        << "an empty callable would satisfy Has() but crash on Run()";
}

// The live table, cross-checked against what the per-dungeon appenders are
// supposed to have contributed. This is what actually catches a controller TU
// being dropped by the linker (the static-lib hazard the explicit
// Register<Dungeon>Hooks calls exist to prevent) — its ids just stop resolving.
TEST(DungeonEventIntegrityTest, EveryAuthoredObjectiveHookIdIsRegistered)
{
    struct Expected { uint32 id; char const* what; };
    constexpr Expected kHooks[] = {
        { 1, "BRD Ring of Law — EnsureRingStarted" },
        { 2, "Deadmines — FireDefiasCannon" },
        { 3, "Old Hillsbrad — GrantIncendiaryBombs" },
        { 4, "The Mechanar — GrantCacheKeyAndLoot" },
        { 5, "ZulFarrak — WakeZumrah" },
        { 6, "Arcatraz — DriveMellicharWaves" },
        { 7, "Sethekk Halls — DriveAnzuSummon" },
        { 8, "Black Morass — DriveBlackMorassEvent (BlackMorassDriver.cpp)" },
        { 9, "Shattered Halls — StartNethekurseIntro" },
        { 10, "The Underbog — SendGhazanToPlatform" },
        { 12, "Black Morass — BmDriveWave (BlackMorassDriver.cpp)" },
        { 13, "Azjol-Nerub — HadronoxHasWebbedTheDoors" },
        { 38, "The Oculus — OcDriveAscent (OculusDriver.cpp)" },
        { 39, "The Oculus — OcEregosHold (OculusDriver.cpp)" },
    };

    for (Expected const& e : kHooks)
        EXPECT_TRUE(ObjectiveHookRegistry::Has(e.id))
            << "objective hook " << e.id << " (" << e.what << ") is not registered."
               " If this is one of the Black Morass ids, the most likely cause is"
               " BlackMorassDriver.cpp losing its RegisterBlackMorassHooks call from"
               " Hooks() — the module is a static lib, so a TU nothing references"
               " gets dropped along with its hooks.";

    EXPECT_FALSE(ObjectiveHookRegistry::Has(0))
        << "id 0 is the 'no hook' sentinel and must never resolve";

    // 11 (BmPullDrainers) is half of the old Black Morass wave pair, retired into
    // hook 12 and left unused rather than recycled so an old log line naming it
    // stays legible. Its partner, 10 (BmCampActivePortal), WAS recycled in S1593
    // for the Underbog — accepted then because the Black Morass rework predates
    // any log a reader still consults.
    EXPECT_FALSE(ObjectiveHookRegistry::Has(11))
        << "hook id 11 is RETIRED (old BmPullDrainers) and must stay unused";
}

// --- Utgarde Keep (574): the forge masters must be swept ONE AT A TIME -----
// The three Dragonflayer Forge Masters share entry 24079 and refuse to be fought
// out of order (npc_dragonflayer_forge_master::JustEngagedWith EnterEvadeMode()s
// unless the previous forge's instance bit is set). The ordering is bought by
// three separate position-anchored ClearRadius sweeps, one per forge, wired to
// three roster objectives in order. Two ways to break that silently, both pinned
// here: collapsing the sweeps onto one entry-keyed KillCreature step, and adding
// the usual by-entry backstop — either would seek the NEAREST 24079 and re-open
// the out-of-order engage.
TEST(DungeonEventIntegrityTest, UtgardeKeepForgesAreSweptOneAtATime)
{
    constexpr uint32 UK_FORGE_MASTER = 24079;
    struct Forge { uint32 eventId; float x; float y; };
    // West -> east -> north, the order the instance script enforces.
    Forge const kForges[] = {
        { 1, 349.6f, -39.3f },
        { 2, 385.8f, -16.2f },
        { 3, 347.6f,   4.6f },
    };

    for (Forge const& f : kForges)
    {
        DungeonEvent const* ev = DungeonEventRegistry::Find(/*map*/ 574, f.eventId);
        ASSERT_NE(ev, nullptr) << "Utgarde Keep (574) event " << f.eventId << " is missing";

        EXPECT_EQ(ev->activation, EventActivation::Anchored)
            << "the forge order is bought by the OBJECTIVE order, so each sweep must"
               " be anchored to its own objective, not fired by a predicate";
        EXPECT_EQ(ev->gate, DcDifficultyGate::Any)
            << "the ordering script runs in both difficulties";
        EXPECT_FALSE(ev->required)
            << "nothing gates on ForgeEventMask but the masters themselves, so a"
               " wedged forge must degrade rather than stall the run";

        ASSERT_EQ(ev->steps.size(), 1u)
            << "event " << f.eventId << " must be exactly one sweep — a second step"
               " would make it a rewind hazard needing .Persistent()";
        EventStep const& s = ev->steps[0];
        EXPECT_EQ(s.kind, EventStepKind::ClearRadius)
            << "must be POSITION-anchored: KillCreature resolves by ENTRY and all"
               " three masters share 24079, so it would seek the nearest one";
        ASSERT_EQ(s.entryFilter.size(), 1u) << "the sweep must be entry-filtered";
        EXPECT_EQ(s.entryFilter[0], UK_FORGE_MASTER);
        EXPECT_NEAR(s.x, f.x, 0.5f) << "sweep centred on its own forge master";
        EXPECT_NEAR(s.y, f.y, 0.5f);
        // 12yd names exactly one master: they are 41-44yd apart and the nearest
        // other spawn to any of them is 14.7yd. A wider volume would swallow a
        // neighbouring forge and the ordering with it.
        EXPECT_GT(s.radius, 0.0f);
        EXPECT_LE(s.radius, 14.0f)
            << "a sweep wider than the 14.7yd nearest neighbour stops naming one forge";
        EXPECT_GT(s.timeoutMs, 30000u)
            << "the 30s EventStepTimeout default is short of a walk-in plus an elite kill";

        // NO by-entry backstop, deliberately — see the file note.
        for (EventStep const& step : ev->steps)
            EXPECT_FALSE(step.kind == EventStepKind::KillCreature &&
                         step.creatureEntry == UK_FORGE_MASTER)
                << "a KillCreature(Engage) backstop on 24079 seeks the NEAREST master,"
                   " which past forge 1 is usually the wrong one — it undoes the"
                   " ordering these three objectives exist to buy";
    }

    // The three sweeps must be three DISTINCT places, not a copy-paste of one.
    EXPECT_NE(DungeonEventRegistry::Find(574, 1)->steps[0].x,
              DungeonEventRegistry::Find(574, 3)->steps[0].x);
    EXPECT_NE(DungeonEventRegistry::Find(574, 1)->steps[0].y,
              DungeonEventRegistry::Find(574, 2)->steps[0].y);
}

// --- The Nexus (576): three sphere clicks are what free Keristrasza --------
// Keristrasza spawns UNIT_FLAG_NON_ATTACKABLE inside a frozen prison, and
// boss_keristrasza::CanRemovePrison only lets go once DATA_TELESTRA_ORB,
// DATA_ANOMALUS_ORB and DATA_ORMOROK_ORB are all DONE. The only thing in the
// instance that sets any of them is a click on the matching Containment Sphere
// (each GO's smart_scripts SMART_EVENT_GOSSIP_HELLO -> SET_INST_DATA). Miss one
// and the run walks to an unattackable last boss and stalls, so all three clicks
// must be Required, distinct, and ordered after the orb bosses.
TEST(DungeonEventIntegrityTest, NexusSpheresAreThreeRequiredClicks)
{
    struct Sphere { uint32 eventId; uint32 goEntry; };
    Sphere const kSpheres[] = {
        { 1, 188526 },  // Telestra's
        { 2, 188528 },  // Ormorok's
        { 3, 188527 },  // Anomalus'
    };

    for (Sphere const& sp : kSpheres)
    {
        DungeonEvent const* ev = DungeonEventRegistry::Find(/*map*/ 576, sp.eventId);
        ASSERT_NE(ev, nullptr) << "The Nexus (576) event " << sp.eventId << " is missing";

        EXPECT_EQ(ev->activation, EventActivation::Anchored)
            << "each sphere gets its own objective anchor so boss-nav does the walk"
               " — the three are 40-57yd apart, past what an event step's own HopTo"
               " is meant to cover";
        EXPECT_EQ(ev->gate, DcDifficultyGate::Any)
            << "the prison gates Keristrasza on both difficulties";
        EXPECT_TRUE(ev->required)
            << "these gate the LAST BOSS — a sphere that will not click must surface"
               " as a stall, not be skipped past onto an unattackable Keristrasza";

        ASSERT_EQ(ev->steps.size(), 1u)
            << "one click; a second step would make it a rewind hazard needing"
               " .Persistent()";
        EventStep const& s = ev->steps[0];
        EXPECT_EQ(s.kind, EventStepKind::UseGameObject);
        EXPECT_EQ(s.goEntry, sp.goEntry);
        EXPECT_GT(s.radius, 8.0f)
            << "the GO search must cover the objective's 8yd arrive radius";
        EXPECT_GT(s.timeoutMs, 30000u)
            << "the step deliberately HOLDS on a still-NOT_SELECTABLE sphere, so the"
               " default 30s would read a boss-state race as a failure";
    }

    // Three DISTINCT spheres, not a copy-paste of one.
    EXPECT_NE(DungeonEventRegistry::Find(576, 1)->steps[0].goEntry,
              DungeonEventRegistry::Find(576, 2)->steps[0].goEntry);
    EXPECT_NE(DungeonEventRegistry::Find(576, 2)->steps[0].goEntry,
              DungeonEventRegistry::Find(576, 3)->steps[0].goEntry);
}

// --- Azjol-Nerub (601): the two structural events -------------------------
//
// 1. Hadronox's swarm is INFINITE until every Anub'ar Crusher (28922) is dead
//    AND she has walked up to the platform and cast Web Front Doors — and every
//    add she eats while it carries her Leech Poison heals her 10% of max HP, so
//    an un-webbed Hadronox is not killable. Two ways to break the fix silently,
//    both pinned here: dropping the crusher gate (releasing the party the moment
//    the platform looks clear), and dropping the web wait (handing her to boss
//    navigation while she is still 60yd below, mid-climb).
// 2. The way on is a hole with a ~360yd drop across a hard navmesh break. The
//    checkpoint must stay on the pit floor and the landing under the hole.
TEST(DungeonEventIntegrityTest, AzjolNerubHoldsThePlatformUntilTheDoorsAreWebbed)
{
    constexpr uint32 AN_ANUBAR_CRUSHER = 28922;

    DungeonEvent const* ev = DungeonEventRegistry::Find(/*map*/ 601, /*event*/ 1);
    ASSERT_NE(ev, nullptr) << "Azjol-Nerub (601) event 1 'Hadronox: web the doors' is missing";

    EXPECT_EQ(ev->activation, EventActivation::Anchored);
    EXPECT_EQ(ev->gate, DcDifficultyGate::Any)
        << "the swarm and its off-switch are identical on both difficulties";
    EXPECT_FALSE(ev->required)
        << "Optional on purpose: a wedged crusher pack must degrade into 'fight her"
           " wherever she is', not stall the run for the human";
    EXPECT_TRUE(ev->persistent)
        << "both steps span a continuous swarm fight — a combat gap must not rewind"
           " the crusher gate";

    ASSERT_EQ(ev->steps.size(), 2u);

    // Step 1 — GARRISON on the platform until no Anub'ar Crusher lives.
    EventStep const& hold = ev->steps[0];
    EXPECT_EQ(hold.kind, EventStepKind::MoveTo)
        << "a garrison, not a sweep: the crusher packs MovePoint themselves onto"
           " this deck, so seeking them only marches the tank into the add stream";
    EXPECT_EQ(hold.creatureEntry, AN_ANUBAR_CRUSHER)
        << "the gate must key on the Anub'ar Crusher — it is the ONLY entry"
           " boss_hadronox's _crushersLeft counts (ACTION_CRUSHER_DIED comes from"
           " npc_anub_ar_crusher::JustDied alone), and it is what gates MOVE3";
    EXPECT_FALSE(hold.wantAlive) << "hold until they are DEAD";
    EXPECT_GT(hold.timeoutMs, 30000u)
        << "the default 30s cannot cover pack 1 plus two packs walking ~65yd down"
           " from the ledges under a swarm that never stops";

    // Step 2 — wait for the web itself.
    EventStep const& web = ev->steps[1];
    EXPECT_EQ(web.kind, EventStepKind::Custom)
        << "killing the crushers only makes MOVE3 ELIGIBLE (it is scheduled at 70s"
           " and re-checked every 2s); the party must hold until the doors are"
           " actually webbed";
    EXPECT_EQ(web.hookId, 13u) << "ObjectiveHookRegistry HadronoxHasWebbedTheDoors";
    EXPECT_TRUE(ObjectiveHookRegistry::Has(web.hookId));
    EXPECT_GT(web.timeoutMs, 30000u);
}

TEST(DungeonEventIntegrityTest, AzjolNerubDropsPastTheLakeNotDownTheWall)
{
    DungeonEvent const* ev = DungeonEventRegistry::Find(/*map*/ 601, /*event*/ 2);
    ASSERT_NE(ev, nullptr) << "Azjol-Nerub (601) event 2 'Drop into the lower kingdom' is missing";

    EXPECT_EQ(ev->activation, EventActivation::Anchored);
    EXPECT_TRUE(ev->required)
        << "there is no other route into the lower kingdom — a skip strands the run";

    ASSERT_EQ(ev->steps.size(), 1u)
        << "one hop; a second step would make it a rewind hazard needing .Persistent()";
    EventStep const& s = ev->steps[0];
    EXPECT_EQ(s.kind, EventStepKind::TeleportParty)
        << "NOT DropInHole: the drop is ~360yd and nothing in the module makes a"
           " fall that long survivable";

    // The checkpoint is on the pit floor at the hole's rim (mesh probe at
    // (522,548): 648.87) — that is where the party musters and it has not moved.
    EXPECT_NEAR(s.x, 522.0f, 3.0f);
    EXPECT_NEAR(s.y, 548.0f, 3.0f);
    EXPECT_NEAR(s.z, 648.9f, 2.0f);

    // The landing is NOT under the hole. TeleportParty is explicitly a diagonal
    // relocation, and directly beneath the hole are the two traps this
    // coordinate exists to step past: the lake (NAV_WATER meshed at the liquid
    // surface, 145yd of it) and the x=533.3333 mmtile seam whose sliver fan
    // defeats the long-range smoothing walk. (544.18, 481.26, 288.98) is dry
    // ground past both — one NAV_GROUND surface in the column, nothing else
    // within 400yd. See AN_DROP_LANDING_X.
    EXPECT_NEAR(s.landX, 544.18f, 1.0f);
    EXPECT_NEAR(s.landY, 481.26f, 1.0f);
    EXPECT_NEAR(s.landZ, 288.98f, 1.0f);
    EXPECT_GT(s.landX - 533.3333f, 5.0f)
        << "the drop landing must stay clear of the x=533.3333 mmtile seam";
    EXPECT_LT(s.landY, 500.0f)
        << "the landing must be SOUTH of the lake's drop-chamber end, not in it";
    EXPECT_GT(s.z - s.landZ, 300.0f) << "this is the 360yd shaft, not a ledge hop";
}

// The lower kingdom is hand-authored because the navmesh pathfinder cannot
// smooth its way out of the drop chamber reliably — see the comment block above
// RegisterAzjolNerubRoute. These anchors are walked in a STRAIGHT LINE (the
// anchor fast-path in StridedPathfinder::Build builds no corridor at all), so
// the two things that can silently break the route are a missing row and legs
// too long for the follower to re-anchor onto after a fight.
TEST(DungeonEventIntegrityTest, AzjolNerubAnchorsTheRouteToAnubarak)
{
    constexpr uint32 kAnubarak = 29120;
    std::vector<WaypointHint> const* route =
        DungeonClearRouteRegistry::Get(601, DUNGEON_DIFFICULTY_NORMAL, kAnubarak);
    ASSERT_NE(route, nullptr)
        << "Azjol-Nerub (601) has no authored route to Anub'arak; the long-range "
           "pathfinder would be asked to smooth across the x=533.3333 mmtile seam";
    ASSERT_GE(route->size(), 2u);

    // Heroic shares the geometry and must inherit the same row.
    EXPECT_EQ(DungeonClearRouteRegistry::Get(601, DUNGEON_DIFFICULTY_HEROIC, kAnubarak), route);

    // Leg length. DungeonPathFollower::RESNAP_RADIUS is 45yd and InstallLongPath
    // resets the follower cursor to segment 0 on every rebuild, so a leg longer
    // than the resnap radius means a party that rebuilds mid-route walks BACK to
    // an anchor it already cleared. Kept well under with margin for the leg from
    // the drop landing into the first anchor.
    constexpr float kMaxLeg = 40.0f;
    float const landX = 544.18f, landY = 481.26f;
    float prevX = landX, prevY = landY;
    for (size_t i = 0; i < route->size(); ++i)
    {
        WaypointHint const& h = (*route)[i];
        float const leg = std::hypot(h.x - prevX, h.y - prevY);
        EXPECT_LT(leg, kMaxLeg)
            << "leg " << i << " is " << leg << "yd — longer than the follower can resnap over";
        prevX = h.x;
        prevY = h.y;
    }

    // The route must end short of the boss: StridedPathfinder appends Anub'arak's
    // own spawn as the goal segment, so a final anchor sitting on top of him is a
    // duplicate hop, and one 40yd+ away leaves the goal leg unvalidated.
    float const tailLeg = std::hypot(prevX - 551.0f, prevY - 248.3f);
    EXPECT_GT(tailLeg, 5.0f) << "last anchor duplicates the appended goal segment";
    EXPECT_LT(tailLeg, kMaxLeg) << "the appended goal leg is longer than any authored leg";

    // Every anchor heads SOUTH — the route is a one-way descent from the landing
    // to the arena, and any anchor that doubles back north is either a stale
    // coordinate left over from an older landing or a corner cut across the lake
    // behind it. Anchor DRYNESS itself needs the mmaps and is asserted in
    // TestAzjolNerubRouteProbe; this is the cheap always-on half.
    EXPECT_LT((*route)[0].y, landY) << "anchor 1 is north of the drop landing";
    for (size_t i = 1; i < route->size(); ++i)
        EXPECT_LT((*route)[i].y, (*route)[i - 1].y)
            << "anchor " << (i + 1) << " doubles back north";

    // And they must stay out of the lake's y-band the landing was moved past.
    // The water sheet ends around y 404; every anchor south of the landing is
    // clear of the drop chamber by construction, so the one to watch is the
    // first: it sits 25yd past the southern shore.
    EXPECT_LT((*route)[0].y, 470.0f)
        << "anchor 1 is back in the drop chamber's half of the lower kingdom";
}

// --- Ahn'kahet: The Old Kingdom (map 619) ---------------------------------
//
// Both of this map's gates fail the same silent way: the boss stays immune and
// the run stalls at an anchor with nothing to fight, several minutes and several
// hundred yards after the step that actually went wrong. These pin the shape of
// each event so a later edit cannot quietly re-open that hole.

TEST(DungeonEventIntegrityTest, AhnkahetTaldaramDevicesAreTwoVerifiedClicks)
{
    // {eventId, GO entry, persistent-data index the GO's SmartAI row sets}.
    // 193093 carries SET_INST_DATA 0 (DATA_TELDRAM_SPHERE1), 193094 SET_INST_DATA 1.
    struct DeviceCase { uint32 eventId; uint32 goEntry; int32 dataIndex; };
    constexpr DeviceCase kDevices[] = {
        { 1, 193094, 1 },  // east, visited first
        { 2, 193093, 0 },  // west, visited second
    };

    for (DeviceCase const& d : kDevices)
    {
        DungeonEvent const* ev = DungeonEventRegistry::Find(/*map*/ 619, d.eventId);
        ASSERT_NE(ev, nullptr) << "Ahn'kahet (619) event " << d.eventId
                               << " (Ancient Nerubian Device) is missing";

        EXPECT_EQ(ev->activation, EventActivation::Anchored)
            << "the devices are 74.5yd apart — boss-nav must deliver the tank, not a HopTo";
        EXPECT_TRUE(ev->persistent)
            << "the verification hold is a data-gated MoveTo; a combat gap on this "
               "deck would rewind the click";
        EXPECT_TRUE(ev->required)
            << "an unclicked device leaves Prince Taldaram permanently immune — that "
               "must stall for the human, not be skipped";
        // Both spheres exist on both difficulties.
        EXPECT_EQ(ev->gate, DcDifficultyGate::Any);

        // Click, THEN prove the click landed. UseGameObject reports Done the
        // moment it calls Use() without checking that anything happened, so a
        // swallowed click would latch the objective complete and send the party
        // on to a boss that is still in his prison.
        ASSERT_EQ(ev->steps.size(), 2u)
            << "expected exactly UseGameObject + persistent-data verification";
        EXPECT_EQ(ev->steps[0].kind, EventStepKind::UseGameObject);
        EXPECT_EQ(ev->steps[0].goEntry, d.goEntry);
        EXPECT_EQ(ev->steps[1].kind, EventStepKind::MoveTo);
        EXPECT_EQ(ev->steps[1].persistentDataId, d.dataIndex)
            << "the hold must watch the index THIS device sets";
        EXPECT_EQ(ev->steps[1].persistentDataMin, 3u)
            << "EncounterState::DONE is 3";

        // The step scans for the GO from the BOT, so the objective has to put the
        // tank inside that scan or the step holds forever on a device it cannot see.
        float arrive = -1.0f;
        for (BossRosterPatch const& patch : BossRosterRegistry::AllPatches())
        {
            if (patch.mapId != 619)
                continue;
            for (DungeonBossInfo const& e : patch.add)
                if (e.kind == DungeonAnchorKind::Objective && e.eventId == d.eventId)
                    arrive = e.arriveRadius;
        }
        ASSERT_GT(arrive, 0.0f) << "event " << d.eventId << " has no objective anchor";
        EXPECT_LT(arrive, ev->steps[0].radius)
            << "arrive radius " << arrive << " is outside the " << ev->steps[0].radius
            << "yd GO search — the tank can be 'arrived' and still not see the device";
    }
}

TEST(DungeonEventIntegrityTest, AhnkahetInitiateSweepCoversEveryInitiateSpawn)
{
    constexpr uint32 kInitiate = 30114;

    DungeonEvent const* ev = DungeonEventRegistry::Find(/*map*/ 619, /*eventId*/ 3);
    ASSERT_NE(ev, nullptr) << "Ahn'kahet (619) event 3 (Twilight Initiates) is missing";

    EXPECT_EQ(ev->activation, EventActivation::Anchored);
    EXPECT_TRUE(ev->persistent) << "a ClearRadius is a fight and spans combat gaps";
    EXPECT_TRUE(ev->required)
        << "Jedoga never descends while one initiate lives — a half-cleared chamber "
           "must stall, not be skipped";
    EXPECT_EQ(ev->gate, DcDifficultyGate::Any);

    int clearStep = -1;
    int backstopStep = -1;
    for (size_t i = 0; i < ev->steps.size(); ++i)
    {
        EventStep const& s = ev->steps[i];
        if (s.kind == EventStepKind::ClearRadius)
            clearStep = static_cast<int>(i);
        if (s.kind == EventStepKind::KillCreature && s.engage && s.creatureEntry == kInitiate)
            backstopStep = static_cast<int>(i);
    }
    ASSERT_GE(clearStep, 0) << "must sweep the chamber (a ClearRadius step)";
    ASSERT_GE(backstopStep, 0)
        << "must carry a by-entry KillCreatureEngage backstop — a position sweep can "
           "only fight what IsPossibleTarget / IsEngageReachable let it see";
    EXPECT_GT(backstopStep, clearStep) << "the backstop follows the sweep";

    // NO leading MoveTo, deliberately: fifteen hostiles is not a room to walk into
    // the middle of. The objective anchor delivers the tank to the volume's edge
    // and the step's driving half EngageDirects the nearest initiate, re-picking
    // after each kill, so the pack is taken one at a time.
    for (EventStep const& s : ev->steps)
        EXPECT_NE(s.kind, EventStepKind::MoveTo)
            << "a MoveTo here marches the tank to the centroid and face-pulls all fifteen";

    EventStep const& sweep = ev->steps[clearStep];

    // Exactly the fifteen. Three other things stand inside this volume and none
    // may be swept: Jedoga hovering 13.5yd overhead, the two Jedoga Controllers
    // (30181) on the ledges 45yd up, and the ten Twilight Worshippers (30111)
    // that group 2 summons the moment she is engaged.
    ASSERT_EQ(sweep.entryFilter.size(), 1u);
    EXPECT_EQ(sweep.entryFilter[0], kInitiate);

    // creature_summon_groups, summonerId 29310 group 0 — the fifteen Twilight
    // Initiates Jedoga's Reset() places around the ritual floor. Every one has to
    // sit inside the sweep, or the gate certifies "clear" with a live initiate in
    // the room and she never comes down.
    struct Spawn { float x, y, z; };
    constexpr Spawn kInitiates[] = {
        { 379.204f, -716.697f, -16.0964f }, { 378.424f, -708.388f, -16.0964f },
        { 379.049f, -712.899f, -16.0964f }, { 382.583f, -711.713f, -16.0964f },
        { 375.400f, -711.434f, -16.0964f }, { 385.693f, -694.376f, -16.0964f },
        { 383.812f, -700.410f, -16.0964f }, { 387.224f, -698.006f, -16.0964f },
        { 392.276f, -695.895f, -16.0964f }, { 368.151f, -719.763f, -16.0964f },
        { 362.020f, -719.828f, -16.0964f }, { 364.937f, -716.110f, -16.0964f },
        { 368.781f, -713.932f, -16.0964f }, { 362.458f, -714.166f, -16.0964f },
        { 389.626f, -702.300f, -16.0964f },
    };
    ASSERT_EQ(std::size(kInitiates), 15u);
    for (Spawn const& p : kInitiates)
    {
        float const dx = p.x - sweep.x;
        float const dy = p.y - sweep.y;
        EXPECT_LT(std::sqrt(dx * dx + dy * dy), sweep.radius)
            << "initiate spawn outside the sweep radius";
        EXPECT_LT(std::fabs(p.z - sweep.z), sweep.zBand)
            << "initiate spawn outside the sweep z-band";
    }

    // The z-band must still exclude what is stacked in this column: Jedoga's
    // hover pose at -2.46 and the Amanitar cave floor the mmaps put at -97.37,
    // 81yd under the ritual chamber.
    EXPECT_LT(sweep.zBand, std::fabs(-2.46f - sweep.z))
        << "the band reaches Jedoga's hover position";

    // Dire Maul crystal lesson: while the sweep runs the tank must stay inside
    // arriveRadius, or the at-objective action stops owning the tick and
    // engage-trash/Advance start competing for it.
    float arrive = -1.0f;
    for (BossRosterPatch const& patch : BossRosterRegistry::AllPatches())
    {
        if (patch.mapId != 619)
            continue;
        for (DungeonBossInfo const& e : patch.add)
            if (e.kind == DungeonAnchorKind::Objective && e.eventId == 3)
                arrive = e.arriveRadius;
    }
    ASSERT_GT(arrive, 0.0f) << "event 3 has no objective anchor";
    EXPECT_GT(arrive, sweep.radius)
        << "arrive radius " << arrive << " is inside the " << sweep.radius
        << "yd sweep — the tank falls out of 'arrived' mid-clear";

    // Set far past any real fight: a timeout that fires while the party is still
    // winning turns a slow clear into a Failed step, and this step is required.
    EXPECT_GE(sweep.timeoutMs, 300000u);
}

TEST(DungeonEventIntegrityTest, AhnkahetGatedBossesAreReanchoredOffTheirMidAirSpawns)
{
    // BossSpawnIndex takes its coordinates from the `creature` row, and for these
    // two that row is the PRE-FIGHT pose. DungeonBossesValue::SnapAll cannot
    // rescue either: BOSS_SNAP_RADIUS is 40, but NavmeshSnap's query box uses a
    // fixed 10yd VERTICAL half-extent, and findNearestPoly only sees polys that
    // overlap it. Both floors are further down than that, so both snaps fail and
    // the raw mid-air coords survive into the at-boss trigger.
    struct Reanchor { uint32 entry; float spawnZ; float floorZ; char const* what; };
    constexpr Reanchor kBosses[] = {
        { 29308, 42.0351f,  11.43f,  "Prince Taldaram (hovering in his prison)" },
        { 29310, -0.624178f, -15.98f, "Jedoga Shadowseeker (hovering over the ritual floor)" },
    };

    for (Reanchor const& b : kBosses)
    {
        bool removed = false;
        DungeonBossInfo const* readded = nullptr;
        for (BossRosterPatch const& patch : BossRosterRegistry::AllPatches())
        {
            if (patch.mapId != 619)
                continue;
            for (uint32 e : patch.remove)
                if (e == b.entry)
                    removed = true;
            for (DungeonBossInfo const& e : patch.add)
                if (e.kind == DungeonAnchorKind::Boss && e.entry == b.entry)
                    readded = &e;
        }
        EXPECT_TRUE(removed) << b.what << ": derived row not removed";
        ASSERT_NE(readded, nullptr) << b.what << ": no re-added anchor";
        EXPECT_NEAR(readded->z, b.floorZ, 0.5f) << b.what << ": anchor is not on the floor";
        EXPECT_GT(std::fabs(b.spawnZ - b.floorZ), 10.0f)
            << b.what << ": the drop is now inside NavmeshSnap's vertical extent, so "
                         "the hand-authored anchor is no longer the only thing fixing it";
        // The re-add must keep the boss's own kill-bit rather than inventing one.
        EXPECT_EQ(readded->inheritCompletionFrom, b.entry)
            << b.what << ": completionFrom must be the boss's own entry";
    }
}

TEST(DungeonEventIntegrityTest, AhnkahetPrisonApparatusIsNavigationInvisible)
{
    // All four are lock-free GAMEOBJECT_TYPE_DOOR spawned GO_STATE_READY, which
    // the closed-door predicate reads as shut gates on the corridor.
    EXPECT_TRUE(DcEventDoorRegistry::IsNavigationIgnored(193564))
        << "Taldaram's prison FX sits ON his objective — leaving it flagged parks "
           "the run on its own destination and auto-pauses";
    EXPECT_TRUE(DcEventDoorRegistry::IsNavigationIgnored(193093))
        << "the device belongs to event 2, not to the door-blocked watchdog";
    EXPECT_TRUE(DcEventDoorRegistry::IsNavigationIgnored(193094))
        << "the device belongs to event 1, not to the door-blocked watchdog";

    // The one real door on the map. DOOR_TYPE_PASSAGE on DATA_PRINCE_TALDARAM, so
    // only his death opens it — but it is lock 0, so a bot would happily click it
    // open and unlock Jedoga / Amanitar / Volazj with him still immune behind it.
    EXPECT_TRUE(DcEventDoorRegistry::IsScriptOnly(192236))
        << "the Taldaram Door opens on his death and must never be force-opened";
    EXPECT_FALSE(DcEventDoorRegistry::IsNavigationIgnored(192236))
        << "it IS a real corridor door — the party must still see it as blocking";
}

// --- Drak'Tharon Keep (map 600) -------------------------------------------

// Novos' phase 1 is a >=70s gate the party can only open by killing four Crystal
// Handlers, fought under a Fetid Troll Corpse every three seconds and an 11yd /
// 1665-per-second Arcane Field on the boss. Every property below encodes "hold
// the camp under fire and keep your rotation" — the two halves that a driver
// event gets wrong in opposite directions.
TEST(DungeonEventIntegrityTest, DrakTharonNovosIsACampHeldUnderFire)
{
    DungeonEvent const* ev = DungeonEventRegistry::Find(/*map*/ 600, /*eventId*/ 1);
    ASSERT_NE(ev, nullptr) << "Drak'Tharon Keep (600) event 1 (Novos camp) is missing";

    // Conditional, not anchored: the party is in combat from the pull, and an
    // anchored event drives on the non-combat engine only.
    EXPECT_EQ(ev->activation, EventActivation::Conditional);
    ASSERT_TRUE(ev->condition) << "the phase-1 predicate is not bound";

    // The three flags that make a driver work, and the one that makes it stop.
    EXPECT_TRUE(ev->drivesInCombat)
        << "without this the driver never runs — there is no out-of-combat tick "
           "between the pull and the gate opening";
    EXPECT_TRUE(ev->stepsOwnMovement)
        << "the hook issues its own garrison, and this is also what makes the "
           "driver yield the tick so the tank keeps a rotation";
    EXPECT_TRUE(ev->repeatable)
        << "the hook reports Done on every in-position tick (that IS the yield), so "
           "a one-shot latch would end the hold ~1s into a 70s phase";
    EXPECT_FALSE(ev->required)
        << "a timed-out driver must skip and re-fire, not stall the run";

    // ONE Custom step. A second step would impose a sequence on an encounter
    // whose problem is a standing position preference.
    ASSERT_EQ(ev->steps.size(), 1u) << "the driver is one hook, not a step list";
    EXPECT_EQ(ev->steps[0].kind, EventStepKind::Custom);
    EXPECT_EQ(ev->steps[0].hookId, 14u);
    EXPECT_TRUE(ObjectiveHookRegistry::Has(14));

    // No sweep anywhere on this event, in any form. The Risen Shadowcasters and
    // Hulking Corpses of phase 1 are left standing, passive but hostile, at the
    // spawn trigger 45yd UP the staircase (56yd from the camp) — a ClearRadius or
    // a seeking KillCreature would march the party into them and off Novos' 80yd
    // leash. This is the Mechanar bridge lesson on different scenery.
    for (EventStep const& s : ev->steps)
    {
        EXPECT_NE(s.kind, EventStepKind::ClearRadius)
            << "a sweep here walks the party up the staircase into the decoy pile";
        EXPECT_FALSE(s.kind == EventStepKind::KillCreature && s.engage)
            << "nothing has to be sought — the handlers SetInCombatWithZone and the "
               "corpses walk into the camp";
    }

    // Folded into Novos' panel row: it is his encounter, not a standalone gate.
    EXPECT_EQ(ev->panelGatesBossEntry, 26631u);

    // Both difficulties: phase 1 and its gate are identical on normal and heroic
    // (the heroic-only extra is 59910, which fires in phase TWO).
    EXPECT_EQ(ev->gate, DcDifficultyGate::Any);
}

// King Dred's Raptor Call (59416) is scheduled inside `if (IsHeroic())` and picks
// a random Drakkari Scytheclaw / Gutripper within 100yd that is alive and NOT in
// combat. The pen sweep exists to empty that pool deterministically; every
// property below is about not doing collateral damage while it does.
TEST(DungeonEventIntegrityTest, DrakTharonRaptorPenIsAHeroicEntryFilteredSweep)
{
    DungeonEvent const* ev = DungeonEventRegistry::Find(/*map*/ 600, /*eventId*/ 2);
    ASSERT_NE(ev, nullptr) << "Drak'Tharon Keep (600) event 2 (raptor pen) is missing";

    EXPECT_EQ(ev->activation, EventActivation::Anchored);
    EXPECT_EQ(ev->gate, DcDifficultyGate::HeroicOnly)
        << "Raptor Call is never cast on normal — the sweep would be a pure detour";
    EXPECT_TRUE(ev->persistent) << "seven elites is several combat gaps";
    EXPECT_FALSE(ev->required)
        << "this is a tuning pre-clear, not a gate: Dred is reachable either way";

    ASSERT_EQ(ev->steps.size(), 1u);
    EventStep const& sweep = ev->steps[0];
    EXPECT_EQ(sweep.kind, EventStepKind::ClearRadius);

    // Entry-filtered, and this is load-bearing twice over: Dred himself spawns
    // 12.5yd from the anchor and Elder Kilias (30534, faction 35) stands 26yd
    // away in the same pen. An unfiltered volume would pull the boss early and go
    // looking at a friendly quest NPC.
    ASSERT_EQ(sweep.entryFilter.size(), 2u);
    EXPECT_NE(std::find(sweep.entryFilter.begin(), sweep.entryFilter.end(), 26628u),
              sweep.entryFilter.end()) << "Drakkari Scytheclaw";
    EXPECT_NE(std::find(sweep.entryFilter.begin(), sweep.entryFilter.end(), 26641u),
              sweep.entryFilter.end()) << "Drakkari Gutripper";

    // Reach. Measured from (-533, -692), the seven pen raptors sit at 5.7 .. 33.7
    // yards and the two HALL raptors at 62.0 / 65.9 — the latter stand on the
    // Novos -> Dred route through the handler hall and are met by the corridor
    // sweep long before the party turns into the pen.
    EXPECT_GE(sweep.radius, 34.0f) << "must reach the farthest raptor inside the pen";
    EXPECT_LT(sweep.radius, 62.0f)
        << "a radius this wide marches the party back east to re-clear the handler "
           "hall it already walked through";

    // Flat pen floor (z 30.1 .. 32.1); the zBand keeps the sweep on it.
    EXPECT_LE(sweep.zBand, 10.0f);
    EXPECT_GE(sweep.timeoutMs, 120000u) << "seven elites, one at a time";

    // The Dire Maul crystal lesson, restated by Ahn'kahet's initiate sweep: the
    // whole kill zone must be inside the objective's arriveRadius, or the tank
    // falls out of "arrived" mid-clear and engage-trash / Advance start competing
    // for the tick. .Persistent() does NOT cover this — the sticky latch in
    // IsPersistentAnchoredEventActive only arms at stepIndex >= 1, and the sweep
    // is step 0.
    float arrive = -1.0f;
    for (BossRosterPatch const& patch : BossRosterRegistry::AllPatches())
    {
        if (patch.mapId != 600)
            continue;
        for (DungeonBossInfo const& e : patch.add)
            if (e.kind == DungeonAnchorKind::Objective && e.eventId == 2)
                arrive = e.arriveRadius;
    }
    ASSERT_GT(arrive, 0.0f) << "the raptor-pen objective has no arrive radius";
    EXPECT_GT(arrive, sweep.radius)
        << "arrive radius " << arrive << " is inside the " << sweep.radius
        << "yd sweep — the tank falls out of 'arrived' mid-clear";
}

// The four Ritual Crystals are the Utgarde Keep forge-fire shape on a different
// map: inverted-state DOOR_TYPE_ROOMs lying across the route, opened by a kill
// rather than a click, and unopenable by a bot in any case (lock 1669 needs item
// 38555). Left flagged they end the run at the door to the encounter.
TEST(DungeonEventIntegrityTest, DrakTharonRitualCrystalsAreNavigationInvisible)
{
    // Four DOOR_TYPE_ROOM gameobjects in a 27x26yd square centred on Novos, all
    // startOpen (so their state is INVERTED) and all on lock 1669, a key item no
    // bot carries. The party walks THROUGH the square to reach him.
    for (uint32 crystal : { 189299u, 189300u, 189301u, 189302u })
    {
        EXPECT_TRUE(DcEventDoorRegistry::IsNavigationIgnored(crystal))
            << "Ritual Crystal " << crystal << " left flagged reads as a shut gate on "
               "the route into the Novos chamber and auto-pauses the run there";
        // NOT key-exempt: the crystals are not a gate the party solves, and a bot
        // clicking one would fight the instance script for the GO state.
        EXPECT_FALSE(DcEventDoorRegistry::IsKeyExempt(crystal))
            << "Ritual Crystal " << crystal << " must never be clicked by a bot";
    }
}

// A pull-owning event stands the WHOLE pull system down for as long as it drives
// (DungeonClearPullModeCurrentValue) and drops the follower scout-lag with it
// (DcLeaderSignal::IsLeaderDynamicScouting). That is the right call only for a leg
// the party must cross or hold rather than clear pack-by-pack, so — like
// DrivesInCombat above — every row that claims it is vetted here by hand.
TEST(DungeonEventIntegrityTest, PullOwningEventsAreVetted)
{
    struct Row { uint32 mapId; uint32 eventId; };
    static Row const kVetted[] = {
        // Blackwing Lair "Cross the Suppression Rooms". The leg is a TRANSIT, and
        // an advanced pull is the exact opposite of crossing: its Idle branch
        // answers unplanned aggro by walking a fresh camp BACK along the route
        // until it finds ground clear of hostiles, and among 160 whelps on a 30s
        // respawn there is no such ground short of maxDrag. Live on
        // tr-20260828-142623-4: nine drag legs in four minutes at 16-71yd (a
        // sibling run at 102yd), transit cursor falling 10/19 -> 6/19 and
        // re-walking the same four anchors — the tank ran the gauntlet backwards.
        {469, 3},
        // Halls of Lightning "Cross the Slag Furnace". Same reason, plus one this
        // map has on its own: the pull's Idle branch drags a camp BACKWARD looking
        // for ground clear of hostiles, which among fourteen Slags on a 20s
        // respawn is never nearby — and on this leg "backward" means UP THE
        // DESCENT and then down again, because Volkhan's leash
        // (GetDistance(1331.9, -106, 56) > 95 -> EnterEvadeMode) reaches 55yd into
        // the pit floor. A camp dragged onto the ramp therefore takes the boss
        // into the Slags over a path he cannot walk and evades him.
        {602, 1},
        // Utgarde Pinnacle "Bring down Grauf". The 161yd leg east to the harpoon
        // pocket is a TRANSIT across a summon pump with no end condition, so the
        // pull's Idle branch — which answers unplanned aggro by walking a fresh
        // camp BACK along the route until it finds ground clear of hostiles — can
        // never find such ground and runs out to maxDrag. And on this map
        // "backward" is not merely slow, it LOSES THE ENCOUNTER: spell_area keeps
        // 47546 on every player in area 1196 (the whole instance), which fires
        // 47547 every 5s restricted to the Flame Breath Triggers within 40yd, and
        // every 6s the reset trigger at (397.0, -511.5) counts triggers still
        // carrying it and evades Skadi on zero. A camp dragged west past that
        // carpet resets the entire gauntlet.
        {575, 4},
        // Pit of Saron "Run the Ymirjar gauntlet". The pull's Idle branch answers
        // unplanned aggro by stamping a fresh camp BEHIND the tank and dragging it
        // there until it finds ground clear of hostiles — and on this leg
        // "backward" costs the run twice over. It drags the party out of the
        // 80yd volume the driver measures wave 2 in, so "is the wave dead" starts
        // reading empty with mobs still up and the driver walks to a gate that
        // refuses for killsLeft != 0; and it drags them back through a gate sphere
        // whose SmartTrigger has already been spent, which is harmless for the
        // driver's own level-triggered forge but not for the test harness's
        // edge-triggered relay. There is also nothing to gain: the gauntlet
        // corridor has no static trash between Krick and the tunnel at all, so
        // every hostile on it is a scripted summon the driver already holds ground
        // for.
        {658, 1},
        // Halls of Reflection "Hold the altar", and here the flag is not a
        // preference but a survival rule. The pull's Idle branch answers unplanned
        // aggro by walking a fresh camp BACK along the route until it finds ground
        // clear of hostiles — and on this map "back" is toward the front door,
        // which is 65yd from CenterPos against a leash of 70.5. ONE drag leg wipes
        // the wave event and replays four waves with every dead mob respawned.
        // Dropping the scout-lag with it is the other half of the same win: a wave
        // activation opens on the FARTHEST player, so a tight clump around the
        // tank is the whole defence and a tank scouting fifteen yards ahead of its
        // party is the opposite of one. There is nothing to gain either — the
        // altar chamber has no trash at all; all 34 mobs in it are the waves.
        {668, 2},
        // Halls of Reflection "Escape the Lich King". The same reason one step
        // harder: a camp dragged backward here is dragged toward a Lich King who
        // is permanently 10-30yd behind the party, and every yard back is closer
        // to a 7068-per-second frost ring and a 10 000-damage zap whose knockback
        // throws the victim further back still. The corridor has no static trash
        // on it either — every hostile is a summon the encounter spawns at him.
        {668, 5},
        // The Culling of Stratholme "The ten waves of Stratholme". The pull's Idle
        // branch answers unplanned aggro by stamping a fresh camp BEHIND the tank
        // and dragging it back until it finds ground clear of hostiles — and this
        // phase is one long unplanned aggro across four clusters up to 264yd apart,
        // so "backward" is always away from the wave the party has to kill and the
        // wave counter only moves on deaths. Dropping the scout-lag with it is the
        // other half: the tank walks INTO a 4-pack by design here, and a tank
        // fifteen yards ahead of its party is the wrong shape for that. There is
        // nothing to gain either — every hostile in the city during this phase is a
        // wave summon (the static Risen Zombies are 250yd away in Fire Street,
        // which belongs to the LAST objective of the dungeon).
        {595, 10},
        // Trial of the Champion "The Champion's Arena". There is no trash on this
        // map at all — every hostile is an encounter summon — and the one pull the
        // party makes, the Argent side packs, the driver makes itself. The
        // advanced pull's answer to them (a fresh camp dragged back to clear
        // ground) would walk the party away from the packs the counter needs dead,
        // and a scout-lagged tank is the wrong shape for a five-rider joust.
        {650, 4},
        // The Oculus "The Oculus ascent". Every construct island is twenty to eighty
        // yards across with nothing but air past the rim: the advanced pull's
        // drag-back camp would be stamped off the edge or on another island, and a
        // scout-lagged tank is fifteen yards of rim nobody can stand on.
        {578, 9},
    };

    for (DungeonEvent const& ev : DungeonEventRegistry::AllEvents())
    {
        if (!ev.ownsThePull)
            continue;

        bool vetted = false;
        for (Row const& r : kVetted)
            if (r.mapId == ev.mapId && r.eventId == ev.id)
                vetted = true;

        EXPECT_TRUE(vetted)
            << "event " << ev.mapId << "/" << ev.id << " '" << ev.name
            << "' sets OwnsThePull(), which forces the effective pull mode Off and"
               " drops the scout-lag for the event's whole duration. If that is"
               " genuinely intended (a leg to cross or hold, not to clear), add it"
               " to kVetted here with the reason.";

        // The flag is read through DungeonEventExecutor::IsPullOwningEventDriving,
        // whose conditional half asks FindDueConditionalEvent. An ANCHORED event
        // would never reach that half — it takes the IsPersistentAnchoredEventActive
        // path, which infers the stand-down from `persistent` alone — so the flag
        // on an anchored row is a silent no-op, exactly the failure class this file
        // exists to turn red.
        EXPECT_TRUE(ev.condition != nullptr)
            << "event " << ev.mapId << "/" << ev.id << " '" << ev.name
            << "' sets OwnsThePull() but is not Conditional(). The flag is only"
               " consulted for conditional events; on an anchored row it does"
               " nothing at all.";
    }
}

// The Suppression Rooms crossing must carry the flag. This is the regression pin
// for the backward-running tank: without it the advanced pull owns every whelp
// aggro and drags the raid back down the gauntlet, and nothing else in the transit
// can stop it — the driver yields the tick on every Done/advancing verdict, and
// DcRel::PullManeuver (60) then outranks nothing it needs to.
TEST(DungeonEventIntegrityTest, SuppressionCrossingOwnsThePull)
{
    DungeonEvent const* ev = DungeonEventRegistry::Find(469, 3);
    ASSERT_NE(ev, nullptr) << "BWL 'Cross the Suppression Rooms' (469/3) is missing";
    EXPECT_STREQ(ev->name.c_str(), "Cross the Suppression Rooms");

    EXPECT_TRUE(ev->ownsThePull)
        << "the crossing must stand the pull system down: with it live, the pull's"
           " Idle branch stamps a fresh camp behind the tank on every unplanned"
           " whelp aggro and drags it there, backwards through the gauntlet";

    // Conditional, or IsPullOwningEventDriving's conditional half never sees it.
    EXPECT_NE(ev->condition, nullptr);
    // And persistent, so the crossing keeps its progress across the combat gaps
    // the whelps guarantee.
    EXPECT_TRUE(ev->persistent);
}
