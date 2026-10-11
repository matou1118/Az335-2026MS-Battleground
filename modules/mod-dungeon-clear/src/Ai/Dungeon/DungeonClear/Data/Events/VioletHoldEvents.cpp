/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"
#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonRosterBuilders.h"

#include "Creature.h"
#include "Player.h"
#include "Playerbots.h"

// --- The Violet Hold (map 608) ---------------------------------------------
//
// A FULLY event-driven dungeon, the Black Morass shape one size down (core:
// instance_violet_hold.cpp + violet_hold.cpp + boss_*.cpp). There is no walkable
// clear path and no static boss roster: the party seals itself into one round
// arena, Lieutenant Sinclari starts an 18-wave siege, and TWO OF SIX caged
// prisoners plus a final dragon are released BY THE ENCOUNTER, at random.
//
// What the stock pipeline does with that today, and why every part of it is
// wrong:
//
//  1. THE DERIVED ROSTER IS WRONG AND UNREACHABLE. instance_encounters gives map
//     608 exactly three credit rows — First Prisoner -> Erekem (29315), Second
//     Prisoner -> Moragg (29316), Cyanigosa (31134). Erekem and Moragg are real
//     world spawns, so BossSpawnIndex emits anchors at their CELL coordinates
//     (1871.46, 871.04, 43.42) / (1893.90, 728.13, 47.75) — behind sealed `Cell`
//     gameobjects, inside geometry no bot can path into. Cyanigosa is a
//     TempSummon and is invisible to the spawn store, so she never appears at
//     all. Hence the roster patch below REMOVES both cell anchors.
//  2. THE CREDIT ROWS LIE ABOUT WHICH BOSS. The first/second prisoner are rolled
//     per instance (StorePersistentData(PERSISTENT_DATA_FIRST_BOSS, <a random
//     pick over the BOSS_MORAGG .. BOSS_ZURAMAT range>)), so killing Zuramat as the first
//     prisoner sets NO DungeonEncounter bit that names Zuramat. Completion
//     therefore rides GetBossState(DATA_1ST_BOSS / DATA_2ND_BOSS /
//     DATA_CYANIGOSA) inside the objective hooks and NEVER the completed-encounter
//     mask. (See boss-state-index-spaces, ac-encounter-credit-is-per-credit-entry.)
//  3. NOTHING STARTS THE INSTANCE. The whole dungeon is inert until a player
//     picks Sinclari's gossip — event 1's job.
//  4. A CAMP-ONLY CLEAR CANNOT FINISH IT. Half the waves are gated behind a
//     Portal Guardian/Keeper that NEVER leaves its portal and never aggros a
//     party at the door. Sit still and that portal pumps 3-4 adds every 20s
//     FOREVER while the door seal drains to 0 and the instance resets. Killing
//     the keeper is the sole off-switch — event 5's whole reason to exist.
//
// THE ENCOUNTER, in the terms the driver reasons about:
//
//   * Sinclari (30658) gossip 9997 -> 9998 fires DoAction(ACTION_START_INSTANCE).
//     15s later the Prison Seal (191723) shuts behind the party and wave 1 opens.
//   * Waves 1-5, 7-11, 13-17 open a Teleportation Portal (31011) at one of six
//     RIM positions and roll one of two kinds:
//       - KEEPER: one Portal Guardian 30660 / Portal Keeper 30695 / 30893 at the
//         portal, then 3 (+1 from wave 12) trash EVERY 20 SECONDS, FOREVER. The
//         portal only dies when it has no live keeper left to channel on, so the
//         KEEPER IS THE ONLY OFF-SWITCH, and keepers have combat-only SmartAI:
//         they stand at the portal and never come to you.
//       - ELITES: 2 (+1) Azure Captain/Sorceror/Raider/Stalker, then the portal
//         goes invisible and dies when they do. They walk to you; nothing to do.
//   * Waves 6 and 12 open a portal in the MIDDLE of the room that spawns the
//     Azure Saboteur (31079), who escorts to the rolled prisoner's cell and
//     releases it. The wave only advances when that prisoner DIES.
//   * Wave 18 spawns Cyanigosa (31134) at (1930.28, 804.41, 52.41) and MoveJumps
//     her to the middle of the room; she is NON_ATTACKABLE for 12.5s.
//   * EVERY wave add is a violet_hold_trashAI (npc_escortAI) that walks a fixed
//     per-portal waypoint list to the door and calls CreatureStartAttackDoor():
//     drop escort state, SetImmuneToNPC(true), DoCastAOE(58040) on the Prison
//     Door Seal (30896). Each periodic tick of that aura is one
//     ACTION_DECREASE_DOOR_HEALTH; at 0 the instance wipes the field and resets.
//
// ALL SIX WAYPOINT PATHS CONVERGE ON THE SAME TWO POINTS — (1843.71, 805.81,
// 44.14) for paths 0/1/1-alt/2/5 and (1845.58, 800.68, 44.10) for paths 3/4 — so
// this dungeon does have a genuine chokepoint at the door, and CAMP below sits
// 10.4yd inside that midpoint straddling the funnel every path's last two
// waypoints run through.
//
// STANDING THERE IS STILL THE WRONG DEFAULT, and the first cut of this file said
// otherwise. Intercepting every add for free is worth little when the thing
// producing the adds is 52-86yd away at the rim, never comes, and never stops: a
// keeper portal pumps 3-4 trash every 20 seconds for as long as its keeper lives,
// so a party that waits at the door is fighting the pump's output on the pump's
// schedule and is losing gate points to every mob that slips the net. Nobody
// plays the Violet Hold that way. The party STATIONS ON THE LIVE PORTAL — adds
// are summoned within 2yd of it and the keeper spawns inside melee range, so the
// whole wave is met at the spawn instead of at the door — and STAGE, the middle
// of the arena floor, is where it waits in between (23.5yd closer to the average
// rim portal than the door, and still astride every trash path). CAMP survives as
// the emergency position for the runs where something did get through: rule 2 of
// VhDriveWave, armed by reading the seal's own aura list.

using namespace DcVioletHold;

namespace
{

    // --- creature entries (violet_hold.h) ---------------------------------
    //
    // THE WAVE ROSTER, probed by ALIVENESS ALONE. Every entry here is a
    // TempSummon that does not exist until the encounter creates it, which is
    // what makes a bare FindNearestCreature a sound "the arena is busy" probe.
    // The six caged prisoners and the two Erekem Guards are deliberately NOT
    // here — they are WORLD SPAWNS present from map load, so an aliveness probe
    // on them would read true before the party has even entered and would leave
    // the wave driver in charge of the start objective. They live in
    // kVhPrisonerEntries below and are probed for ATTACKABILITY instead.
    //
    // This list must stay COMPLETE: an entry missing here reads as "the arena is
    // quiet" and hands the tick back to the plain garrison while that mob walks
    // into the seal.
    //
    // It must also stay EXCLUSIVE. Map 608 carries ambient fauna (Spider 14881,
    // Black Rat 2110, Rat 4075) and a shelf of script furniture; admitting any of
    // them would leave the driver due — and the tank steering — for a critter.
    // Explicitly absent, each for its own reason:
    //
    //   31079 Azure Saboteur       unit_flags 768 = IMMUNE_TO_PC | IMMUNE_TO_NPC.
    //                              Unkillable bait that walks the whole room on
    //                              waves 6 and 12. AttackersValue::IsPossibleTarget
    //                              already rejects IMMUNE_TO_PC, but it is red-name
    //                              and selectable, so it also carries a
    //                              DcNeverTargetRegistry row.
    //   31011 Teleportation Portal NOT_SELECTABLE furniture; probed SEPARATELY by
    //                              the activation predicate so the party moves out
    //                              during the quiet 10s before a keeper spawns.
    //   30896 Prison Door Seal     the thing being drained, not a hostile.
    //   30857 Defense Dummy Target NOT_SELECTABLE | IMMUNE_TO_PC | IMMUNE_TO_NPC.
    //   30837 Defense System       the crystals' NOT_SELECTABLE emitter.
    //   29364/29365 Void Sentry    Zuramat's summons get SetPhaseMask(16), so a
    //                              phase-1 bot cannot see or target them at all —
    //                              which is also the achievement-correct behaviour.
    //   31118 Azure Raider         faction 35 scenery at (1966.8, 780.9, 52.4).
    //                              NOTE this is a DIFFERENT ENTRY from the wave
    //                              mob 30668 of the same name.
    //   29276 / 29425 / 30883      NOT_SELECTABLE summon/controller triggers.
    constexpr uint32 kVhWaveEntries[] =
    {
        30661, 30961,  // Azure Invader
        30662, 30962,  // Azure Spellbreaker
        30663, 30918,  // Azure Binder
        30664, 30963,  // Azure Mage Slayer
        30666,         // Azure Captain
        30667,         // Azure Sorceror
        30668,         // Azure Raider   (NOT the faction-35 scenery 31118)
        32191,         // Azure Stalker
        30660,         // Portal Guardian
        30695, 30893,  // Portal Keeper
        29321,         // Ichor Globule  (Ichoron's shattered-bubble adds)
        29271, 32582,  // Ethereal Sphere (Xevozz's Arcane Power carriers)
        31134,         // Cyanigosa — a TempSummon, so aliveness is a sound probe.
                       // Deliberately probed ALIVE and not attackable: she is
                       // NON_ATTACKABLE for 12.5s after her MoveJump, and the
                       // driver should already own the tick when that clears.
    };

    // THE OFF-SWITCH. Killing one of these despawns its portal and stops that
    // portal's 20-second add pump — npc_vh_teleportation_portal's UpdateAI kills
    // ITSELF the moment it has no live keeper left to channel 58012 on. Nothing
    // else in the encounter closes a keeper portal, which is why the wave driver
    // selects and travels by this list rather than by "nearest hostile".
    //
    // Disjoint from the trash on purpose: these three never walk to the door and
    // never drain the seal, so they are never sweep targets — and no trash mob
    // closes a portal, so trash are never selection targets.
    constexpr uint32 kVhKeeperEntries[] = { 30660, 30695, 30893 };

    // THE CAGED PRISONERS + their guards, probed for ATTACKABILITY, never for
    // mere aliveness. All eight are world spawns that exist from map load behind
    // sealed Cell gameobjects carrying UNIT_FLAG_NON_ATTACKABLE (the bosses,
    // unit_flags 514 / 33282) or IMMUNE_TO_PC (the guards, 33536). The instance's
    // StartBossEncounter clears those flags at the moment of release, so
    // "attackable" is exactly "released", and it is the only probe that does not
    // read true on an inert dungeon.
    //
    // Erekem's two guards (29395) are in the list because they are released WITH
    // him, fight normally, and stand 60yd from the camp: while they live the
    // driver must stay in charge rather than hand the tick to a garrison that
    // would walk the tank home and leave two elites loose.
    constexpr uint32 kVhPrisonerEntries[] =
    {
        29266,  // Xevozz
        29312,  // Lavanthor
        29313,  // Ichoron
        29314,  // Zuramat the Obliterator
        29315,  // Erekem
        29316,  // Moragg
        29395,  // Erekem Guard (x2, released with him)
    };

    // --- geometry ---------------------------------------------------------
    // Objective 1's anchor: inside the open Prison Seal (x 1822.59), on the flat
    // entrance floor (navmesh column probe: one walkable surface at z 44.49) and
    // **3.97yd from Lieutenant Sinclari's spawn**, so simply ARRIVING puts the tank
    // inside her interact range and the hook's walk-in is a no-op on the happy
    // path.
    //
    // It used to be (1837.0, 803.5), 7.3yd out — which is where the first two live
    // runs stalled. The proximate cause was in the hook (see VH_GOSSIP_REACH in
    // VioletHoldDriver.cpp), but a 7.3yd anchor made the run depend on the hook's
    // travel for a gap the arrival could have closed for free. It should not: the
    // objective anchor is the one position the ordinary boss-navigation pipeline
    // is guaranteed to deliver.
    constexpr float VH_ENTRY_X = 1834.0f, VH_ENTRY_Y = 802.0f, VH_ENTRY_Z = 44.49f;

    // Per-step budgets. The 18-wave siege is paced by kill speed, not a clock —
    // 3s between portals, 35s after a released boss dies — and measures 20-35min
    // wall-clock, so the three defend objectives get an hour each: they are
    // Running-until-their-boss-slot-is-DONE by construction and a timeout on one
    // is a bug report, not a control-flow mechanism. (The harness's own
    // DungeonClear.TestRun.OverallTimeoutS is 7200s, so an hour per objective is
    // not the binding constraint on a batch.)
    //
    // The start step is different: it is a walk-in plus one gossip, and if
    // Sinclari cannot be reached the run has nothing to do. Three minutes bounds
    // the loss instead of burning an hour at the instance door.
    constexpr uint32 VH_START_TIMEOUT_MS = 180000;
    constexpr uint32 VH_EVENT_TIMEOUT_MS = 3600000;

    bool VhWaveActive(Player* bot, AiObjectContext* context);
}

// A caged prisoner is RELEASED when the instance has cleared the flags its cell
// kept it behind. instance_violet_hold's StartBossEncounter does exactly three
// things to the boss it rolls — RemoveUnitFlag(UNIT_FLAG_NON_ATTACKABLE),
// SetImmuneToNPC(false), SetReactState(REACT_AGGRESSIVE) — and the same to
// Erekem's two guards via SetImmuneToAll(false). So the flags ARE the latch, and
// testing them is the only probe that distinguishes a released boss from the
// five still sitting inert behind sealed cells 45-88yd away.
//
// NOT_SELECTABLE is in the test for a second reason: Ichoron sets it on himself
// (plus displayId 11686) for the 15 seconds after his Protective Bubble shatters,
// while ten Ichor Globules spawn at the room's rim. Reading him as "not the boss
// to fight" during that window is what keeps the driver from parking the tank in
// drop-target limbo on an untargetable unit (pb-drop-target-limbo-loop) and lets
// the globules — which ARE in kVhWaveEntries — take the tick instead.
bool DcVioletHold::IsReleased(Creature const* c)
{
    if (!c || !c->IsAlive())
        return false;
    return !c->HasUnitFlag(UNIT_FLAG_NON_ATTACKABLE) &&
           !c->HasUnitFlag(UNIT_FLAG_NOT_SELECTABLE) &&
           !c->HasUnitFlag(UNIT_FLAG_IMMUNE_TO_PC);
}

// --- entry lists shared with the wave driver (VioletHoldDriver.cpp) --------
// See the header declarations in DungeonEventTables.h for what each one is for
// and why the three are kept apart.

std::vector<uint32> const& VioletHoldWaveEntries()
{
    static std::vector<uint32> const entries(std::begin(kVhWaveEntries),
                                             std::end(kVhWaveEntries));
    return entries;
}

std::vector<uint32> const& VioletHoldKeeperEntries()
{
    static std::vector<uint32> const entries(std::begin(kVhKeeperEntries),
                                             std::end(kVhKeeperEntries));
    return entries;
}

std::vector<uint32> const& VioletHoldPrisonerEntries()
{
    static std::vector<uint32> const entries(std::begin(kVhPrisonerEntries),
                                             std::end(kVhPrisonerEntries));
    return entries;
}

void RegisterVioletHoldEvents(std::vector<DungeonEvent>& out)
{
    // (1) ENTER THE VIOLET HOLD — start the siege.
    //
    // Lieutenant Sinclari (30658) spawns at (1830.95, 799.46, 44.42), INSIDE the
    // seal, with gossip menu 9997. Option 0 opens submenu 9998, whose option 0
    // carries smart_scripts 30658 id 1 -> DoAction(ACTION_START_INSTANCE).
    // DungeonEventExecutor::SelectGossip already drills submenus (the Old
    // Hillsbrad Thrall path), so one select walks 9997 -> 9998 by itself.
    //
    // UNLIKE the Black Morass's Sa'at this gossip carries NO quest or item
    // condition — only conditions 15/9997/0 and 15/9997/1, which are instance-
    // STATE conditions (type 13, DATA_ENCOUNTER_STATUS). That pair is also what
    // makes the select safe to re-run: option 0 (start) is visible only while
    // NOT_STARTED and option 1 (LATE-JOIN TELEPORT) only while IN_PROGRESS, so
    // the two can never both be in the menu and index 0 is unambiguous. The hook
    // re-reads the instance state every tick and returns Done the instant it
    // leaves NOT_STARTED, so it can never reach the menu in which index 0 means
    // "teleport me".
    //
    // PERSISTENT + StepsOwnMovement for the usual reasons: the walk-in is a hook-
    // owned glide, and the at-objective hold would cancel it on the very next
    // tick (the Old Hillsbrad barrel trap).
    out.push_back(
        EventBuilder(608, 1, "Enter the Violet Hold")
            .Anchored(/*orderIndex*/ 1)
            .Persistent()
            .StepsOwnMovement()
            .MoveTo(VH_ENTRY_X, VH_ENTRY_Y, VH_ENTRY_Z, /*radius*/ 8.0f)
            .Custom(HOOK_START)
                .Timeout(VH_START_TIMEOUT_MS)
            .Build());

    // (2)(3)(4) DEFEND THE HOLD — the three checkpoints.
    //
    // Three objectives rather than one, because the instance itself checkpoints
    // in three: InstanceCleanup rolls _waveCount back to 0 / 6 / 12 depending on
    // which of DATA_1ST_BOSS / DATA_2ND_BOSS is DONE. Splitting the run the same
    // way gives the `dc bosses` panel three real milestones and gives each hook
    // one unambiguous Done-gate.
    //
    // All three share the staging anchor and differ ONLY in which boss-state slot
    // they watch, which is why they take three hook ids instead of one hook that
    // branches on the objective. A Custom step is handed a DEFAULT-CONSTRUCTED
    // DungeonBossInfo (see DungeonEventExecutor::RunStep) — encounterIndex reads
    // 0 for all three — so branching on the objective inside one shared hook is
    // not available, and pretending otherwise would silently make objectives 3
    // and 4 complete on the FIRST boss's death.
    //
    // Each hook garrisons the staging point between waves and, if it finds the
    // instance back at NOT_STARTED, re-runs the start gossip: that is the wipe / gate-
    // failure recovery path, and it is the direct analogue of the Black Morass's
    // "rift counter 0 + Medivh alive => re-nudge". The WAVE FIGHTS themselves are
    // event 5's job — its EventDue rung (31) preempts this garrison (30) exactly
    // while anything is up.
    //
    // PERSISTENT: every wave fight is a >1s combat gap that would otherwise
    // rewind a non-persistent step list (dc-persistent-sticky-arms-at-step-1 —
    // and note a persistent anchored event still obeys arriveRadius on step 1,
    // hence the lenient radius on the MoveTo and on the anchors below).
    struct DefendRow { uint32 id; char const* name; uint32 order; uint32 hook; };
    static constexpr DefendRow kDefend[] = {
        { 2, "Defend the Hold — First Prisoner",  2, HOOK_DEFEND_1ST },
        { 3, "Defend the Hold — Second Prisoner", 3, HOOK_DEFEND_2ND },
        { 4, "Defend the Hold — Cyanigosa",       4, HOOK_DEFEND_CYANIGOSA },
    };
    for (DefendRow const& r : kDefend)
        out.push_back(
            EventBuilder(608, r.id, r.name)
                .Anchored(r.order)
                .Persistent()
                .StepsOwnMovement()
                .MoveTo(STAGE_X, STAGE_Y, STAGE_Z, /*radius*/ 10.0f)
                .Custom(r.hook)
                    .Timeout(VH_EVENT_TIMEOUT_MS)
                .Build());

    // (5) REPEL THE WAVE — the wave driver.
    //
    // ONE Custom step, and that is the point. The priority this encounter needs
    // is a standing PREFERENCE re-evaluated every tick as portals open, bosses
    // release and adds arrive — not a sequence. A step list can only say "do
    // these in order and block on each", which is exactly how the Black Morass's
    // rift keeper ended up behind nine kill gates that a live rift re-blocked
    // every 15s. See VhDriveWave (VioletHoldDriver.cpp) for the five rungs.
    //
    // DRIVES IN COMBAT — the load-bearing flag. This is a continuous siege: the
    // party is in combat from wave 1 to wave 18, and the ordinary conditional
    // rung stands down on bot->IsInCombat(). Without the flag the driver runs
    // only in the shrinking gaps between waves and stops entirely once the party
    // falls behind — the exact spiral that killed Black Morass runs before
    // DcRel::EventDueCombat existed. And with an infinite 20-second add pump on
    // the other side of it, "falls behind" is not a hypothetical here.
    //
    // STEPS OWN MOVEMENT — the driver issues long-range splines to portals
    // 52-86yd away; without the flag ResolveEscortConflict / StopBot(Hold)
    // cancels each one on the next tick (the 151-attempts-0-arrivals trap).
    //
    // REPEATABLE (18 waves; the condition going false is the only "done") +
    // OPTIONAL (a timed-out step SKIPS and the repeat re-fires fresh, so a wipe
    // or a corpse-run never hard-stalls the run for a human).
    out.push_back(
        EventBuilder(608, 5, "Repel the wave")
            .Conditional(&VhWaveActive)
            .Repeatable()
            .Optional()
            .DrivesInCombat()
            .StepsOwnMovement()
            .Custom(HOOK_DRIVE_WAVE)
                .Timeout(VH_EVENT_TIMEOUT_MS)
            .Build());
}

// --- the wave gate (event 5, repeatable) ----------------------------------
// DUE while a Teleportation Portal is OPEN — the portal itself is the first
// probe, so the party moves out during the quiet 10s before its keeper even
// spawns — or while any wave hostile is alive, which is what keeps the driver in
// charge for the post-wave sweep at the door, or while a RELEASED prisoner is
// up, which is what sends the party out to a boss that will never come to them
// (every boss_* in this dungeon overrides MoveInLineOfSight to {} — checked, all
// nine AI structs).
//
// Grid scans, NOT the spawn store: the portal, every wave mob and Cyanigosa are
// TempSummons with spawnId 0 (the Arcatraz Skyriss precedent). Early-exits on the
// first hit, so the full sweep only runs in the quiet between waves.
//
// The proximity gate keeps the event not-due for a leader outside engage-scan
// range (a corpse-run), where the driver would otherwise steer a bot that cannot
// see the arena — it walks back on the objective machinery first.
//
// Note this predicate is the ONLY thing that ends the driver's turn: while it
// reads true the wave event outranks the defend garrison (31 over 30) and, in
// combat, outranks the stock combat movers (DcRel::EventDueCombat).
namespace
{
    bool VhWaveActive(Player* bot, AiObjectContext* /*context*/)
    {
        if (bot->GetMapId() != 608)
            return false;
        if (bot->GetExactDist2d(ARENA_X, ARENA_Y) > EVENT_DUE_RANGE)
            return false;

        if (bot->FindNearestCreature(NPC_TELEPORTATION_PORTAL, ARENA_SCAN, /*alive*/ true))
            return true;

        for (uint32 entry : kVhWaveEntries)
            if (bot->FindNearestCreature(entry, ARENA_SCAN, /*alive*/ true))
                return true;

        // The caged prisoners are world spawns, so they are probed for RELEASE
        // (the instance clears NON_ATTACKABLE / IMMUNE_TO_PC in StartBossEncounter)
        // rather than for existence — see kVhPrisonerEntries.
        std::list<Creature*> prisoners;
        bot->GetCreatureListWithEntryInGrid(prisoners, VioletHoldPrisonerEntries(), ARENA_SCAN);
        for (Creature* c : prisoners)
            if (DcVioletHold::IsReleased(c))
                return true;

        return false;
    }
}

// --- roster patch: three checkpoints, no derived roster -------------------
// The two derived anchors are REMOVED, not corrected: they sit inside sealed
// cells (Erekem at (1871.46, 871.04, 43.42) behind GO 191564, Moragg at
// (1893.90, 728.13, 47.75) behind GO 191606), and they name the wrong bosses
// besides — the pair actually released is rolled per instance. Cyanigosa has no
// spawn to derive from at all. What replaces them is four travel objectives whose
// events ARE the clear, the Black Morass / Old Hillsbrad model.
void RegisterVioletHoldRoster(std::vector<BossRosterPatch>& t)
{
    using namespace DcRoster;

    BossRosterPatch p;
    p.mapId = 608;
    p.remove = { 29315, 29316 };  // Erekem / Moragg — sealed cells, wrong bosses
    p.add = {
        MakeObjective(OBJ(1), /*encounterIndex*/ 1, 608, "Enter the Violet Hold",
                      VH_ENTRY_X, VH_ENTRY_Y, VH_ENTRY_Z, /*arriveRadius*/ 8.0f,
                      /*gateEntry*/ 0, /*hook*/ 0, /*eventId*/ 1, /*orderOverride*/ 1),
        MakeObjective(OBJ(2), /*encounterIndex*/ 2, 608, "Defend the Hold — First Prisoner",
                      STAGE_X, STAGE_Y, STAGE_Z, /*arriveRadius*/ 10.0f,
                      /*gateEntry*/ 0, /*hook*/ 0, /*eventId*/ 2, /*orderOverride*/ 2),
        MakeObjective(OBJ(3), /*encounterIndex*/ 3, 608, "Defend the Hold — Second Prisoner",
                      STAGE_X, STAGE_Y, STAGE_Z, /*arriveRadius*/ 10.0f,
                      /*gateEntry*/ 0, /*hook*/ 0, /*eventId*/ 3, /*orderOverride*/ 3),
        MakeObjective(OBJ(4), /*encounterIndex*/ 4, 608, "Defend the Hold — Cyanigosa",
                      STAGE_X, STAGE_Y, STAGE_Z, /*arriveRadius*/ 10.0f,
                      /*gateEntry*/ 0, /*hook*/ 0, /*eventId*/ 4, /*orderOverride*/ 4),
    };
    t.push_back(std::move(p));
}
