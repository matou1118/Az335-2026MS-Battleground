/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "Ai/Dungeon/DungeonClear/Data/DungeonClearRouteRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"
#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonRosterBuilders.h"
#include "Ai/Dungeon/DungeonClear/Util/DcTargeting.h"

#include "Creature.h"
#include "InstanceScript.h"
#include "Player.h"
#include "SharedDefines.h"

// --- Utgarde Pinnacle (map 575) -------------------------------------------
//
// The declarative half of this dungeon: the roster row without which nothing
// else here matters, five event rows, and the six authored legs. The imperative
// half is Overrides/UtgardePinnacleDriver.cpp (hooks 25-28); the numbers both
// halves agree on are namespace DcUtgardePinnacle in DungeonEventTables.h.
//
// READ THE ROSTER PATCH FIRST. Everything else on this map is downstream of it.
// The derived roster is three bosses for a four-boss dungeon, because
// instance_encounters credits Svala at an entry that exists only at runtime, and
// the observable consequence is not "the first boss is skipped" — it is "the
// party walks into a shut portcullis on the way to the second". A door-blind
// navmesh will happily route 696yd through two gates that open on the deaths of
// bosses three and four. See RegisterUtgardePinnacleRoster below.
//
// WHAT IS AUTHORED, and why each piece exists:
//
//   A  the roster patch — Svala, plus the three travel objectives the two
//      areatriggers and the Stasis Generator hang off;
//   B  five event rows (this file) — two areatrigger starts, one GO click, the
//      harpoon driver's activation row and the ritual hold;
//   C  six anchor routes (this file) — one per designed leg, so A* never
//      re-derives the door shortcut on a rebuild;
//   D  the leader-side hooks (UtgardePinnacleDriver.cpp).
//
// AND WHAT IS DELIBERATELY *NOT* AUTHORED, so nobody re-derives it:
//
//   * NO DcHazardRegistry rows. The candidates are real — Ymiron's boat flames
//     (aura 39199, 1800s, one per spent phase, parked on the four boat approach
//     points he keeps returning to) and the Bjorn phase's Spirit Fount (27339,
//     aura 48380, MoveFollow at speed 0.4, NOT_SELECTABLE) — but neither has been
//     measured on this map yet and a hazard row authored blind steers a party
//     away from ground that may be fine. The 75 Flame Breath Triggers (28351)
//     are explicitly NOT hazards: the breath is a SIDE-OF-THE-HALL decision
//     (y = -511) that Leg F already encodes, and 75 rows would be 75 wrong ones.
//   * NO DcNeverTargetRegistry rows for Palehoof's four frozen animals
//     (26683/26684/26685/26686). They spawn NOT_SELECTABLE | IMMUNE_TO_PC so
//     IsPossibleTarget almost certainly refuses them already, and the question
//     that registry answers is a different one ("is killing this progress") to
//     which the honest answer once they wake is YES. Measure before authoring.
//   * NO RoomAggroRegistry rows. Nothing on this map aggros a room on entry
//     except the two areatriggers, and those are events, not aggro.
//   * NO DcFactionEntrySwapRegistry rows — t/TestFactionEntrySwap already
//     asserts HasRules(575) == false, and that stays true.
//   * NO DcCombatPurgeRegistry row for the gauntlet. Skadi's add pump is exactly
//     the population DcCombatPurge was built for, but it is a combat-BLIND global
//     clock with no reachability guard ([[dc-unreachable-combat-purge]]), and
//     adding this map without one risks purging live adds mid-fight. The harpoon
//     driver ends the pump in under two minutes by killing the thing that feeds
//     it, which is the better answer to the same problem.
//   * NO NAV-PENALTY ROWS to steer A* off the door shortcuts. The anchor route
//     IS the answer; a penalty heavy enough to make the shortcut expensive would
//     also make the legitimate Leg G expensive, and Leg G is the only way to
//     Ymiron.
//
// AZEROTHCORE DEVIATIONS FROM RETAIL, encoded here so nobody "fixes" them:
//
//   * Palehoof releases TWO of four animals on normal, all four on heroic
//     (`if (Counter > (IsHeroic() ? 3 : 1))` jumps straight to EVENT_PALEHOOF_START).
//     The clear must not wait for four kills on a normal run.
//   * Svala's Call Flames is INERT. EVENT_SORROWGRAVE_FLAMES2 resolves its source
//     with GetCreaturesWithEntryInRange(100, 27273 Flame Brazier), and 27273 has
//     zero spawn rows anywhere in the DB. Do not author a hazard for it.
//   * Svala's ritual fires ONCE PER ATTEMPT, not on a repeating timer.
//   * The Skadi harpoon lock (LOCK_KEY_ITEM 37372) is NOT enforced server-side
//     for GOOBER use.
//   * SKADI_HITS (30) and SKADI_IN_RANGE (31) are dead instance fields — read by
//     GetData, written by nobody. Never gate anything on them.

namespace
{
    using namespace DcUtgardePinnacle;

    // --- event 4's gate: Grauf is up and phase 1 is running -----------------
    //
    // DUE while Skadi's encounter is IN_PROGRESS and Grauf is still alive. Three
    // probes, cheapest first, because this runs on every COMBAT tick of the DC
    // leader on map 575 — and from the moment AT 4991 trips, the hall's own
    // Combat Trigger (38667 at the add-spawn corner) DoZoneInCombat()s the party
    // and it never leaves combat again until the drake is down.
    //
    //   1. the map;
    //   2. the encounter's own state word. NOT_STARTED means the gauntlet has not
    //      been entered (event 3's business); DONE means Skadi is dead and there
    //      is nothing left to steer. Only IN_PROGRESS is phase 1 OR phase 2;
    //   3. Grauf. He is SUMMONED BY boss_skadiAI::Reset(), not spawned from the
    //      `creature` table, so he exists from instance init and mere aliveness
    //      would read true on an inert dungeon — which is exactly why probe 2
    //      comes first. His DEATH is what ends phase 1 (JustDied ->
    //      skadi->ExitVehicle() + ACTION_PHASE2), so "IN_PROGRESS and Grauf
    //      alive" is precisely phase 1 and nothing else.
    //
    // READ THROUGH THE ObjectData STORE, not a grid scan. Grauf flies a lap that
    // takes him 200yd west of the harpoon pocket and 30yd up; any scan radius
    // honest enough to be called a room scan reads "not there" for most of every
    // lap, and the driver would disarm mid-shot. instance->GetCreature(DATA_GRAUF)
    // answers from anywhere, for free. (GetGuidData(DATA_GRAUF) works too — but
    // its Svala and Skadi siblings do NOT, so the store is the habit to keep.)
    //
    // NOT GATED ON COMBAT, for the Halls of Lightning reason: the transit east
    // down the gauntlet hall starts the moment the trigger trips, and a predicate
    // that waited for combat would arm the driver only after the party had
    // already been dragged into the add stream.
    bool UpGraufFlying(Player* bot, AiObjectContext* /*context*/)
    {
        if (!bot || bot->GetMapId() != MAP_ID)
            return false;

        InstanceScript* inst = DcTargeting::GetInstanceScript(bot);
        if (!inst || inst->GetData(DATA_SKADI) != IN_PROGRESS)
            return false;

        Creature* grauf = inst->GetCreature(DATA_GRAUF);
        return grauf && grauf->IsAlive();
    }

    // --- event 5's gate: Svala is 20 yards in the air ----------------------
    //
    // DUE while the Ritual of the Sword has her rooted above her own platform.
    //
    // THE TEST IS HEIGHT, not a timer and not an aura, and that is deliberate.
    // EVENT_SORROWGRAVE_RITUAL is scheduled once in JustEngagedWith and never
    // rescheduled, so there is no repeating signal to latch on to; she is not
    // flagged non-attackable while she is up there (she stays targetable and
    // simply cannot be reached), so no flag changes; and the 25s duration is a
    // script-side EventMap entry nothing outside boss_svala can read. Her Z
    // relative to her own authored floor is the one thing that is unambiguous
    // from here — the ordinary hover is ~6yd (UNIT_FIELD_HOVERHEIGHT), the ritual
    // is 20, and RITUAL_LIFT_Z sits between them.
    //
    // GetLiveBoss rather than a spawn lookup: she is the live creature whose
    // entry became 26668 at runtime, and her position is the whole question.
    bool UpSvalaRitualUp(Player* bot, AiObjectContext* context)
    {
        if (!bot || bot->GetMapId() != MAP_ID)
            return false;

        InstanceScript* inst = DcTargeting::GetInstanceScript(bot);
        if (!inst || inst->GetData(DATA_SVALA) != IN_PROGRESS)
            return false;

        Creature* svala = DcTargeting::GetLiveBoss(bot, context, NPC_SVALA);
        return svala && svala->IsAlive() && svala->GetPositionZ() - SVALA_Z >= RITUAL_LIFT_Z;
    }
}

void RegisterUtgardePinnacleEvents(std::vector<DungeonEvent>& out)
{
    using namespace DcUtgardePinnacle;

    // (1) WAKE SVALA.
    //
    // Walk into areatrigger 5140, fire it, and then WAIT OUT THE INTRO. There is
    // no other way in: smart_scripts source_type 2 entry 5140 is the single
    // SET_DATA that opens boss_svala's gate, and the boss cannot be pulled — she
    // is SetImmuneToAll(true) for 72 seconds and then pulls HERSELF
    // (SelectTargetFromPlayerList(100) -> AttackStart).
    //
    // THE THIRD STEP IS THE INTERESTING ONE. WaitForSpawn(26668) is not waiting
    // for a spawn at all — nothing spawns. It is waiting for the TRANSFORM: at
    // t+34s boss_svala.cpp does me->UpdateEntry(NPC_SVALA_SORROWGRAVE) on a
    // creature that has been standing there since instance init, and from that
    // tick FindNearestCreature(26668) starts matching it. That is the earliest
    // moment the roster's own anchor becomes findable, and holding the party at
    // the trigger until then is also the right SCHEDULE: 34s of intro, then a
    // ~25s walk down Leg B, arriving at about t+60s for a boss who becomes
    // attackable at t+72s.
    //
    // Deliberately NOT waiting for 29281 to disappear. Same creature, same GUID;
    // "gone" and "arrived" are the same event read two ways, and the positive
    // test is the one that stays true afterwards.
    //
    // PERSISTENT because the hold spans a minute of scripted cutscene during
    // which ten Dragonflayer Spectators run across the arena approach and despawn
    // — any of which can put a bot in and out of combat and rewind a
    // non-persistent step list to step 0, which would re-walk the party to the
    // trigger box it is already standing in.
    //
    // AND THE RETRY AFTER A WIPE NEEDS NO SECOND VISIT. `Started` is a member of
    // boss_svalaAI that Reset() does not clear, so a party that wipes and comes
    // back simply walks to her and attacks. The Custom hook no-ops in that case
    // (it gates on GetData(DATA_SVALA) == NOT_STARTED) and the WaitForSpawn is
    // already satisfied, so the whole event falls through in one tick.
    out.push_back(
        EventBuilder(MAP_ID, EVENT_WAKE_SVALA, "Wake Svala Sorrowgrave")
            .Anchored(ORDER_WAKE_SVALA)
            .Persistent()
            .MoveTo(AT_SVALA_X, AT_SVALA_Y, AT_SVALA_Z, AT_SVALA_ARRIVE)
            .Custom(HOOK_SVALA_AREATRIGGER)
                .Timeout(AREATRIGGER_TIMEOUT_MS)
            .WaitForSpawn(NPC_SVALA, /*wantAlive*/ true, SVALA_INTRO_TIMEOUT_MS)
            .Build());

    // (2) START GORTOK PALEHOOF.
    //
    // Click the Stasis Generator, then PROVE the click landed.
    //
    // The UseGO step already refuses to report success on a click that would be
    // swallowed (it holds on GO_FLAG_NOT_SELECTABLE rather than latching Done),
    // but it cannot see whether go_palehoof_sphere's OnGossipHello actually ran —
    // Use() returns void. The handler's own first act is
    // SetGoState(GO_STATE_ACTIVE), so a WaitForGOState on ACTIVE is a free,
    // script-side receipt: if the generator is still READY the click did nothing
    // and the step stalls visibly instead of sending the party east to fight a
    // boss that is still frozen. Halls of Stone's door gossip is verified the
    // same way and for the same reason.
    //
    // NO HOLD FOR THE ANIMALS, and that is a decision. The unfreeze chain has
    // ~20s of hard scripted dead time on normal (6s between an animal's unfreeze
    // animation and it becoming selectable, 3s between one death and the next
    // unfreeze, 3s + 6s before Palehoof himself wakes), which a stall detector
    // could mistake for a stuck run — but the party spends most of it walking the
    // 83yd of Leg D back east, and an authored hold here would be a guess at a
    // timing nobody has measured on this map. If a run does stall in that window,
    // the fix is the plan's candidate event 6, authored against a real trace.
    //
    // THE GEOMETRY IS BACKWARDS ON PURPOSE. The generator is 83yd WEST of the
    // boss, at the far end of his arena, so Leg C ends here and Leg D returns.
    // Do not "optimise" that into arriving at Palehoof first: there is nothing to
    // do at Palehoof until this GO has been clicked.
    out.push_back(
        EventBuilder(MAP_ID, EVENT_START_PALEHOOF, "Start Gortok Palehoof")
            .Anchored(ORDER_STASIS)
            .Persistent()
            .MoveTo(STASIS_X, STASIS_Y, STASIS_Z, STASIS_ARRIVE)
            .UseGO(GO_STASIS_GENERATOR, STASIS_SEARCH)
                .Timeout(STASIS_USE_TIMEOUT_MS)
            .WaitForGOState(GO_STASIS_GENERATOR, GO_STATE_ACTIVE,
                            STASIS_STATE_TIMEOUT_MS, STASIS_SEARCH)
            .Build());

    // (3) ENTER SKADI'S GAUNTLET.
    //
    // A ONE-WAY DOOR, and the only event on this map whose job is to make an
    // irreversible act DELIBERATE rather than accidental.
    //
    // Tripping AT 4991 mounts Skadi on Grauf, spawns a 13-mob first wave and arms
    // a World Trigger carrying 59275 Summon Gauntlet Mobs Periodic — two summons
    // a tick, forever, with no end condition but the drake's death. There is no
    // way to un-enter it and no way to leave: spell_area autocasts 47546 on every
    // player in area 1196 (the whole instance), which fires 47547 every 5s, which
    // is conditions-restricted to the Flame Breath Triggers within 40yd and lasts
    // 7s; every 6s the reset trigger 23472 counts triggers still carrying it and,
    // on ZERO, EnterEvadeMode()s Skadi and resets the entire gauntlet.
    //
    // SO THE HOOK GATHERS BEFORE IT FIRES. The box is large — x [315.09, 346.71],
    // y [-519.35, -497.51] — and the aggro fallback in
    // boss_skadiAI::JustEngagedWith means simply walking onto the platform starts
    // the encounter anyway. What this event buys is not the trigger (the party
    // would trip it regardless) but the ORDER of events: the whole party present
    // and rested on the near side before the pump starts, rather than a tank
    // arriving 40yd ahead of four followers still eating.
    //
    // NO OwnsThePull() HERE, and it is worth saying why rather than leaving the
    // absence to look like an oversight. The flag is CONDITIONAL-ONLY: the
    // anchored path infers the stand-down from Persistent() alone
    // (IsPersistentAnchoredEventActive), and FindDueConditionalEvent — the only
    // thing that reads ownsThePull — never sees an anchored row. Setting it here
    // would be a silent no-op, which is exactly what
    // TestEventRegistry's PullOwningEventsAreVetted turns red. Event 4 carries it
    // for the leg that actually needs it.
    out.push_back(
        EventBuilder(MAP_ID, EVENT_ENTER_GAUNTLET, "Enter Skadi's gauntlet")
            .Anchored(ORDER_ENTER_GAUNTLET)
            .Persistent()
            .MoveTo(AT_SKADI_X, AT_SKADI_Y, AT_SKADI_Z, AT_SKADI_ARRIVE)
            .Custom(HOOK_SKADI_AREATRIGGER)
                .Timeout(GAUNTLET_ENTRY_TIMEOUT_MS)
            .Build());

    // (4) BRING DOWN GRAUF — the one genuinely new mechanism this dungeon needs.
    //
    // ONE Custom step, for the Violet Hold / Blackwing Lair reason: what phase 1
    // needs is a standing PREFERENCE re-decided every tick — hold the pocket, or
    // press the launcher, or get out of the way — not a sequence. A step list can
    // only say "do these in order and block on each", and the shot window opens
    // and closes on a flight lap the party does not control.
    //
    // DRIVES IN COMBAT — the load-bearing flag. The hall's Combat Trigger
    // DoZoneInCombat()s the party the moment the gauntlet starts and the add pump
    // never lets it go, so the ordinary conditional rung (which stands down on
    // IsInCombat) is a rung that would never run once in the whole encounter.
    //
    // STEPS OWN MOVEMENT — the driver delivers the leader 160yd east down the
    // gauntlet hall on its own long-range spline, and the at-objective hold runs
    // BEFORE Drive: without this, last tick's glide is cancelled before the hook
    // can see it and the party creeps one tick at a time while every log line
    // reports a healthy spline issue. It is also what makes a Done return YIELD
    // THE TICK, which is what lets four bots fight the add stream through every
    // hold — and on this leg, unlike a transit, the adds are not optional: the
    // party is in combat regardless and a bot that never swings is a bot dying.
    //
    // OWNS THE PULL — zero advanced pulls across the gauntlet. The pull's Idle
    // branch answers unplanned aggro by walking a fresh camp BACK along the route
    // until it finds ground clear of hostiles, which against a pump with no end
    // condition is never anywhere, so it runs out to maxDrag and hauls the tank
    // west — and west is the one direction that loses the encounter: past the
    // Flame Breath Trigger carpet's 40yd reach the reset check counts zero and
    // Skadi evades.
    //
    // REPEATABLE — phase 1 is not a thing that completes once. Grauf dying is the
    // only "done", and expressing completion as "the condition went false" rather
    // than as a latch is what makes a wipe-and-retry re-arm cleanly with no stale
    // flag to clear.
    //
    // PanelAfterBoss(Palehoof), and NEVER PanelBeforeBoss(Skadi), which is what it
    // visually wants and must not have: panelGatesBossEntry also keys
    // DcTargeting::HasPendingSummonEvent, which reads an unlatched gating event as
    // "this boss must still be SUMMONED" and suppresses the dynamic pull within
    // 80yd of him — and a REPEATABLE event is never latched, so the suppression
    // would be permanent. Palehoof is the anchor immediately before this leg, so
    // the row lands in the same place on the panel with none of that meaning.
    //
    // NOT Optional. Skadi cannot be skipped: boss_ymironAI::Reset() adds
    // UNIT_FLAG_NOT_SELECTABLE and only SetData(DATA_SKADI, DONE) removes it, so
    // a skip here costs the party TWO bosses, not one. If the driver gives up,
    // stalling names the problem for the human, who can `dc skip` if they
    // disagree.
    out.push_back(
        EventBuilder(MAP_ID, EVENT_BRING_DOWN_GRAUF, "Bring down Grauf")
            .Conditional(&UpGraufFlying)
            .Repeatable()
            .Persistent()
            .OwnsThePull()
            .DrivesInCombat()
            .StepsOwnMovement()
            .PanelAfterBoss(NPC_PALEHOOF)
            .Custom(HOOK_GRAUF_HARPOON)
                .Timeout(HARPOON_TIMEOUT_MS)
            .Build());

    // (5) SVALA'S RITUAL — HOLD.
    //
    // Twenty-five seconds, once per attempt, during which the boss is twenty
    // yards straight up and rooted and the three Ritual Channelers around her
    // altar are the entire fight.
    //
    // The failure this prevents is a TARGETING one, not a movement one. She is
    // not flagged non-attackable up there — she stays selectable and simply
    // cannot be reached — so melee bots hold their target, walk under her and
    // stand there swinging at nothing, while the channelers (NullCreatureAI: they
    // never move, never melee, and add ten million threat) keep Paralyze 48278 on
    // the sacrificed member. That stun has INFINITE duration and ends only when
    // the channeler casting it dies, so a party that does not switch never gets
    // its fifth member back.
    //
    // So the hook does exactly one thing: point the leader at the nearest living
    // channeler, once, and let the assist ladder bring the rest. It claims the
    // tick only on the tick it actually retargets.
    //
    // DRIVES IN COMBAT because the whole 25s is a fight; STEPS OWN MOVEMENT for
    // the yield semantics, not for movement — the hook issues none. Without the
    // yield, a rung sitting above the stock combat movers that returns true every
    // tick would starve the rotation it exists to redirect.
    //
    // OPTIONAL, unlike event 4, and Repeatable so it re-arms on a retry. The
    // worst case if it never fires is a slower fight, not a lost one: the
    // channelers have small health pools and stock targeting reaches them
    // eventually. A stall here would be strictly worse than the bug.
    out.push_back(
        EventBuilder(MAP_ID, EVENT_SVALA_RITUAL, "Svala's ritual: kill the channelers")
            .Conditional(&UpSvalaRitualUp)
            .Repeatable()
            .Optional()
            .DrivesInCombat()
            .StepsOwnMovement()
            .PanelBeforeBoss(NPC_SVALA)
            .Custom(HOOK_SVALA_RITUAL_HOLD)
                .Timeout(RITUAL_TIMEOUT_MS)
            .Build());
}

// --- the roster: the invisible first boss ---------------------------------
//
// ONE MakeBossWithBit row is the single highest-value change on this map, and it
// is worth understanding exactly what it repairs.
//
// BossSpawnIndex::Build is a two-step join. Step 1 walks DungeonEncounter.dbc x
// instance_encounters keeping ENCOUNTER_CREDIT_KILL_CREATURE rows and builds
// creditEntry -> {encounterIndex, name}. Step 2 walks the `creature` spawn table
// and matches each spawn's DB id against that map — and THE BOSS'S WORLD POSITION
// COMES FROM THE SPAWN ROW.
//
//     instance_encounters: (577, 0, 26668, 0, 'Svala Sorrowgrave')
//     creature:            (126115, 29281, ..., 575, 296.632, -346.075, 90.6307, ...)
//
// Entry 26668 has no spawn row on any map. What spawns is 29281, and
// boss_svala.cpp's EVENT_SVALA_TALK4 does me->UpdateEntry(NPC_SVALA_SORROWGRAVE)
// ~34 seconds into the intro. equal_range(29281) against a map keyed on 26668
// matches nothing, and the failure is SILENT — no LOG_ERROR, no warning, just a
// three-row roster.
//
// THIS IS ITS OWN FAILURE CLASS. It is not the Halls of Stone / Drak'Tharon
// shape ([[dc-cast-spell-credit-boss-invisible]]) — the credit here is a plain
// kill-creature row — and it is not the Gundrak shape (those bosses HAVE spawns,
// just under entries the roster cannot reach). It is "the credit entry is a
// runtime UpdateEntry target", and the distinguishing test is simply whether the
// `creature` table contains the credit entry at all. Utgarde KEEP's Ingvar has
// the identical transform and derives fine, because there the credit entry is
// the one that spawns. See [[dc-credit-entry-is-an-updateentry-target]].
//
// THE COMPLETION BIT IS SAFE, and it is worth writing down WHY, because the
// usual reasoning does not apply here. This instance script is a LEGACY one —
// Encounters[4] plus plain SetData, with no AddDoor, no DoorData[] and no
// SetBossState anywhere — so "SetBossState maintains completedEncounters" is not
// available as an argument. The mask is maintained by the kill path instead:
// KillRewarder calls Map::UpdateEncounterState(ENCOUNTER_CREDIT_KILL_CREATURE,
// _victim->GetEntry(), _victim), and _victim->GetEntry() is read AT DEATH TIME,
// by which point the transform happened minutes ago. The victim's entry IS 26668
// and DBC bit 0 is set normally.
//
// AND THE INSTANCE'S OWN GUID STORE IS NOT SAFE. OnCreatureCreate switches on
// entry 26668, which never spawns, so GetGuidData(DATA_SVALA_SORROWGRAVE)
// returns ObjectGuid::Empty forever; Skadi has no case in that switch at all.
// Nothing in this module may build a predicate on either. Use GetData for
// encounter state and DcTargeting::GetLiveBoss for the creature.
void RegisterUtgardePinnacleRoster(std::vector<BossRosterPatch>& t)
{
    using namespace DcRoster;
    using namespace DcUtgardePinnacle;

    // ONE Any-gated patch serves both difficulties. Svala is DBC bit 0 on normal
    // AND heroic (rows 577 and 578), with no difficulty bit-shift — unlike
    // Gundrak and the Nexus, whose heroic-only additions needed their own gated
    // patch. And the DBC order (Svala 0, Palehoof 1000, Skadi 2000, Ymiron 3000)
    // is already the travel order, so the reorder block below exists only to make
    // room for the three objectives between the bosses, never to move a boss
    // relative to another.
    BossRosterPatch p;
    p.mapId = MAP_ID;

    p.add = {
        // The boss the derivation drops. Anchored on the PLATFORM FLOOR under her
        // spawn — not her hover position and emphatically not the ritual's z 110,
        // which NavmeshSnap's fixed 10yd vertical extent could not reach and which
        // would therefore drop this row at load with a single LOG_ERROR.
        MakeBossWithBit(NPC_SVALA, MAP_ID, "Svala Sorrowgrave",
                        SVALA_X, SVALA_Y, SVALA_Z,
                        /*encounterIndex*/ BIT_SVALA,
                        /*orderOverride*/ ORDER_SVALA),

        // The three travel objectives. An objective's encounterIndex is an
        // ordering hint only — it carries no kill-bit and NextDungeonBossValue
        // never tests the completion mask for one — so it stays 0 and the clear
        // orders by orderOverride.
        //
        // Each one exists because the thing it anchors CANNOT be reached by
        // walking to a boss: two are areatrigger volumes with no creature in them,
        // and the third is a GameObject 83yd on the far side of its own boss.
        MakeObjective(OBJ(1), /*encounterIndex*/ 0, MAP_ID, "Wake Svala",
                      AT_SVALA_X, AT_SVALA_Y, AT_SVALA_Z,
                      AT_SVALA_ARRIVE, /*gateEntry*/ 0, /*hook*/ 0,
                      /*eventId*/ EVENT_WAKE_SVALA,
                      /*orderOverride*/ ORDER_WAKE_SVALA),
        MakeObjective(OBJ(2), /*encounterIndex*/ 0, MAP_ID, "Stasis Generator",
                      STASIS_X, STASIS_Y, STASIS_Z,
                      STASIS_ARRIVE, /*gateEntry*/ 0, /*hook*/ 0,
                      /*eventId*/ EVENT_START_PALEHOOF,
                      /*orderOverride*/ ORDER_STASIS),
        MakeObjective(OBJ(3), /*encounterIndex*/ 0, MAP_ID, "Enter Skadi's gauntlet",
                      AT_SKADI_X, AT_SKADI_Y, AT_SKADI_Z,
                      AT_SKADI_ARRIVE, /*gateEntry*/ 0, /*hook*/ 0,
                      /*eventId*/ EVENT_ENTER_GAUNTLET,
                      /*orderOverride*/ ORDER_ENTER_GAUNTLET),
    };

    // The three derived bosses onto the same 1..7 scale. Their relative order is
    // unchanged from what their DBC bits already gave them.
    p.reorder = {
        { NPC_PALEHOOF, ORDER_PALEHOOF },
        { NPC_SKADI,    ORDER_SKADI    },
        { NPC_YMIRON,   ORDER_YMIRON   },
    };

    t.push_back(std::move(p));
}

// --- the six designed legs, in walking order ------------------------------
//
// DERIVED, NOT DRAWN. Every anchor below is a point on the corridor
// LongRangePathfinder itself returns for that leg against the live map-575
// mmtiles, decimated to ~16yd — the [[dc-navharness-prints-the-route]] method,
// and the reason t/TestUtgardePinnacleRouteProbe prints the polylines it routed:
// an mmaps regen that moves a corridor is re-authored the same way rather than
// by hand. All six route reachable, complete, with no step over 3.3yd of Z and
// no jump anywhere; total walked distance is about 1 383yd.
//
// WHY THESE EXIST AT ALL. Not because the mesh defeats the pathfinder — every
// leg here routes fine — but because the mesh is baked DOOR-BLIND, and the
// corridors A* prefers between the same endpoints are catastrophically wrong:
//
//     entrance -> Palehoof   695.9yd, 192174 at 5.2yd and 192173 at 1.9yd
//     entrance -> Skadi      611.8yd, both gates
//     entrance -> Ymiron     205.9yd, 192174 at 5.2yd
//
// That last one is the sharpest illustration in the dungeon: the SHORTEST path
// on the map, and permanently impassable, because 192174 is Ymiron's own exit
// and opens when he dies. Probe-measured, the six legs below clear 192174 by
// 43yd at worst and 192173 by 32.9yd at worst — except Leg G, which passes
// 3.1yd from 192173 and is walked only after Skadi has opened it.
//
// EACH ROW STARTS WHERE THE PARTY WILL BE STANDING when that leg begins, because
// DungeonPathFollower::SeedCursor projects the bot onto the row from its own
// position and a row that starts somewhere else snaps the cursor to the far end
// ([[dc-anchor-route-must-cover-where-the-party-stands]]). That is why there are
// six rows and not one, and why each is keyed on the entry of its DESTINATION.
//
// NO NO_STOP ANYWHERE, deliberately. Every trash spawn on this map is
// spawntimesecs 3600, so a kill is progress that stays bought and the ordinary
// pull is the right owner of all six legs — the opposite of Blackwing Lair's
// whelps and Halls of Lightning's Slags, where the respawn made clearing
// negative progress. The one stretch that IS a transit (Leg F, AT 4991 -> the
// harpoon launchers) is not in this table at all: it is walked by the harpoon
// driver, which carries OwnsThePull on its own event row.
void RegisterUtgardePinnacleRoute()
{
    using namespace DcRoster;
    using namespace DcUtgardePinnacle;

    // --- Leg A: the entrance to areatrigger 5140 --------------------------
    // 362.4yd, detour x1.28. West down the Mindless Servant hall, north up the
    // east wall, west along the Dragonflayer corridor at y -255..-238, then
    // south-west into the trigger box. Clears 192173 by 157.8yd and 192174 by
    // 43.4yd — the door-blind alternative to Palehoof passes the latter at 5.2.
    //
    // ANCHORS 9-11 ARE THE NORTH-WEST BEND AND MUST NOT BE COLLAPSED. The north
    // corridor (x 478-482) meets the west corridor (y ~-256) around a solid
    // inside corner: the whole quadrant x 466-474, y -259..-268 has no floor.
    // The escort spline walks authored anchors as STRAIGHT legs, and 8 used to
    // join straight to what is now 12 — one 18.2yd chord through that corner, with
    // 7 of 8 ground samples along it finding no navmesh at any dungeon z.
    //
    // What made that FATAL rather than merely ugly is the length. Past
    // DC_REANCHOR_DISTANCE (12yd) the tank re-anchors on arrival every tick, and
    // the only way out is DungeonPathFollower::Resnap — forward-only and gated on
    // BotCanWalk, a Detour raycast asking whether the straight leg to a forward
    // route point stays on the mesh. Around this corner nothing forward is
    // straight-line walkable, so the re-anchor could never clear: 881 consecutive
    // "Resnap failed, falling through" in tr-20260902-101652-2, the tank pinned at
    // (478.6, -265.5, 104.7) for 11 minutes, 0 of 7 pulls advanced, run lost.
    //
    // Every step across the bend is now UNDER the re-anchor distance, so the
    // corner never asks Resnap for anything and its geometry stops mattering.
    // TestUtgardePinnacleRouteProbe pins that; its comment carries the survey
    // showing why the same rule must NOT be applied route-wide.
    //
    // The three anchors are verbatim vertices 38-40 of the probe's own printed
    // corridor, and the corner is tight enough that nothing shorter survives:
    // measured against the live mmaps, dropping to two anchors still leaves
    // 2 off-mesh samples and either single-anchor variant leaves 3-11. Round it
    // the way Detour does or not at all — [[dc-bwl-anchor-16-18-bend-wall-clip]],
    // [[dc-wailing-caverns-shortcut-wall]], [[dc-navharness-prints-the-route]].
    DungeonClearRouteRegistry::Register(
        MAP_ID, DUNGEON_DIFFICULTY_NORMAL, OBJ(1),
        {
            { 584.12f, -327.97f, 110.15f },  //  0  the entrance (AT 4747's target)
            { 564.19f, -326.27f, 110.65f },  //  1
            { 544.26f, -324.57f, 110.52f },  //  2
            { 524.34f, -322.87f, 110.68f },  //  3
            { 508.39f, -321.51f, 110.62f },  //  4
            { 488.47f, -319.81f, 105.17f },  //  5  down into the north corridor
            { 484.50f, -304.31f, 105.43f },  //  6
            { 481.18f, -284.61f, 105.43f },  //  7
            { 478.99f, -268.76f, 105.43f },  //  8
            { 478.45f, -264.80f, 105.43f },  //  9  into the bend, still northbound
            { 476.53f, -261.28f, 105.23f },  // 10  the pivot
            { 472.86f, -259.70f, 105.17f },  // 11  out of the bend, now westbound
            { 465.51f, -256.54f, 105.56f },  // 12  the turn west
            { 445.84f, -255.03f, 111.02f },  // 13
            { 429.84f, -255.10f, 111.04f },  // 14
            { 413.84f, -255.17f, 112.87f },  // 15
            { 397.97f, -256.50f, 113.14f },  // 16
            { 380.93f, -247.59f, 106.33f },  // 17
            { 367.86f, -238.34f, 105.53f },  // 18
            { 354.55f, -237.97f, 105.76f },  // 19
            { 337.59f, -248.57f, 107.81f },  // 20
            { 320.40f, -258.79f, 105.26f },  // 21
            { 314.76f, -276.88f, 105.43f },  // 22
            { 312.65f, -291.17f, 104.90f },  // 23  dead centre of the trigger box
        });

    // --- Leg B: areatrigger 5140 down to Svala ----------------------------
    // 101.4yd, 7 anchors. West, then down the ramp from the main ring (z 105)
    // into the sunken arena (z 87). The lowest point is anchor 4 at z 87.01 and
    // the altar itself sits back up at 90.83.
    //
    // The Svala mirror (GO 191745) is 11yd SOUTH of the last anchor and this leg
    // approaches from the north, which is the measured basis for leaving it
    // unauthored — see the namespace block.
    DungeonClearRouteRegistry::Register(
        MAP_ID, DUNGEON_DIFFICULTY_NORMAL, NPC_SVALA,
        {
            { 312.65f, -291.17f, 104.90f },  // 0  the trigger box
            { 292.68f, -292.31f, 105.38f },  // 1
            { 276.91f, -297.22f, 105.03f },  // 2  the head of the ramp
            { 280.38f, -316.68f,  89.99f },  // 3
            { 287.53f, -335.35f,  87.01f },  // 4  the arena floor
            { 293.25f, -350.30f,  91.52f },  // 5
            { SVALA_X, SVALA_Y,   SVALA_Z },  // 6  the platform
        });

    // --- Leg C: Svala to the Stasis Generator -----------------------------
    // 237.7yd for a 129yd hop, detour x1.85 — the awkward one. It drops from the
    // arena to z 75, doubles back through a switchback at (267, -390), climbs to
    // z 105 and then runs due south down the Palehoof arena's west wall.
    //
    // DO NOT CUT THE SWITCHBACK. Anchors 7-9 are a genuine hairpin: 7 -> 8 runs
    // 19yd east and climbs 13yd, 8 -> 9 turns back west and climbs 5 more, and
    // 9 -> 10 turns west again onto the upper floor. The
    // [[dc-bwl-anchor-16-18-bend-wall-clip]] lesson applies exactly — the route
    // is clean, and shortening a hairpin authored off a real corridor is how a
    // party ends up inside geometry. If it ever needs shortening, re-derive it
    // from the probe.
    DungeonClearRouteRegistry::Register(
        MAP_ID, DUNGEON_DIFFICULTY_NORMAL, OBJ(2),
        {
            { SVALA_X, SVALA_Y,   SVALA_Z },  //  0  the platform
            { 281.02f, -348.02f,  88.08f },  //  1
            { 265.02f, -348.20f,  87.03f },  //  2
            { 245.16f, -350.07f,  81.63f },  //  3  the descent west
            { 240.36f, -362.63f,  76.66f },  //  4
            { 239.68f, -378.61f,  75.83f },  //  5  the lower ring
            { 249.83f, -395.44f,  76.62f },  //  6
            { 268.99f, -395.58f,  89.86f, /*doorGoEntry*/ 0,
              ToFlag(AnchorFlag::PIVOT_TIGHT) },  //  7  the switchback's east end
            { 266.74f, -385.72f,  95.08f },  //  8
            { 247.52f, -389.50f, 105.55f },  //  9  back onto the main ring
            { 240.02f, -403.41f, 105.43f },  // 10
            { 238.47f, -419.09f, 105.43f },  // 11
            { 238.49f, -435.09f, 105.43f },  // 12
            { 238.51f, -451.09f, 105.64f },  // 13
            { STASIS_X, STASIS_Y, STASIS_Z },  // 14  the generator
        });

    // --- Leg D: the Stasis Generator back east to Palehoof ----------------
    // 82.6yd, detour x1.00 — dead straight across the arena floor.
    //
    // AND STRAIGHT DOWN A PATROL LINE. Bloodthirsty Tundra Wolf 126086 runs
    // waypoint path 1260860, an eight-point sweep from (310.79, -451.59) west to
    // (236.44, -449.69) — the full width of this leg, right past the generator —
    // leading a creature_formations group of three on groupAI 514 (follow +
    // share aggro). Expect the click at the far end to be contested; that is the
    // ordinary pull's job and the reason this leg carries no NO_STOP.
    DungeonClearRouteRegistry::Register(
        MAP_ID, DUNGEON_DIFFICULTY_NORMAL, NPC_PALEHOOF,
        {
            { STASIS_X, STASIS_Y, STASIS_Z },  // 0  the generator
            { 254.45f, -459.33f, 105.54f },  // 1
            { 274.36f, -457.46f, 105.95f },  // 2
            { 294.27f, -455.59f, 105.69f },  // 3
            { 314.19f, -453.72f, 105.67f },  // 4
            { 320.80f, -453.10f, 105.16f },  // 5  Palehoof
        });

    // --- Leg E: Palehoof south to areatrigger 4991 ------------------------
    // 66.4yd, 5 anchors, and it STOPS AT THE TRIGGER BOX rather than at Skadi
    // 12yd beyond it. That is the whole point of the objective: the gauntlet is
    // a one-way door and the party arrives at its threshold on purpose.
    DungeonClearRouteRegistry::Register(
        MAP_ID, DUNGEON_DIFFICULTY_NORMAL, OBJ(3),
        {
            { 320.80f, -453.10f, 105.16f },  // 0  Palehoof
            { 312.51f, -471.24f, 105.43f },  // 1
            { 312.73f, -487.24f, 105.43f },  // 2
            { 319.30f, -499.96f, 105.42f },  // 3
            { AT_SKADI_X, AT_SKADI_Y, AT_SKADI_Z },  // 4  the trigger box
        });

    // --- Leg G: Skadi's landing to King Ymiron ----------------------------
    // 367.5yd for a 240yd hop, detour x1.53, three storey changes. North through
    // the now-open 192173, up and over into the lower ring, west along z 75, then
    // a long climb at x ~394 from z 77 back up to the throne room.
    //
    // ANCHOR 0 IS SKADI'S PHASE-2 LANDING (spell 61790's spell_target_position),
    // not her spawn 134yd west, because that is where the party will be standing:
    // Grauf dies over the harpoon pocket, she ExitVehicle()s and lands here, and
    // the fight that follows is an ordinary one fought on this spot.
    //
    // THE ONLY LEG IN THE DUNGEON THAT TOUCHES A GATE. Anchors 1-3 pass 192173 at
    // 3.1yd — and by the time this row is ever consulted Skadi is dead and
    // SetData(DATA_SKADI, DONE) has opened it permanently. It stays a real,
    // IsScriptOnly door precisely so that a run which somehow arrives here with
    // Skadi alive PAUSES and says so, instead of walking into a portcullis.
    //
    // ANCHORS 5-8 HAIRPIN AROUND (481, -422) and have the shape of
    // [[dc-bwl-anchor-16-18-bend-wall-clip]]: 367yd of route for a 240yd hop with
    // a bend that looks cuttable and is not. Do not shorten it by hand.
    DungeonClearRouteRegistry::Register(
        MAP_ID, DUNGEON_DIFFICULTY_NORMAL, NPC_YMIRON,
        {
            { 476.80f, -511.17f, 105.07f },  //  0  Skadi's phase-2 landing
            { 476.00f, -495.19f, 105.43f },  //  1
            { 475.20f, -479.21f, 105.43f },  //  2  <- 192173, open by now
            { 473.24f, -463.37f, 105.43f },  //  3
            { 469.33f, -443.76f, 104.68f },  //  4
            { 468.60f, -428.33f,  94.42f },  //  5  the drop into the lower ring
            { 481.28f, -422.20f,  92.21f, /*doorGoEntry*/ 0,
              ToFlag(AnchorFlag::PIVOT_TIGHT) },  //  6  the hairpin's apex
            { 490.90f, -434.98f,  82.61f },  //  7
            { 480.84f, -447.16f,  75.60f },  //  8  the lower ring floor
            { 465.96f, -453.05f,  75.68f },  //  9
            { 450.53f, -455.53f,  75.72f },  // 10
            { 431.67f, -451.83f,  75.83f },  // 11
            { 417.21f, -438.02f,  76.86f },  // 12
            { 405.64f, -426.96f,  75.85f },  // 13
            { 394.76f, -414.68f,  77.71f },  // 14  the foot of the long climb
            { 394.48f, -398.68f,  86.29f },  // 15
            { 394.20f, -382.68f,  94.91f },  // 16
            { 393.92f, -366.68f, 103.79f },  // 17
            { 393.57f, -346.69f, 104.82f },  // 18  back on the main ring
            { 393.29f, -330.69f, 104.81f },  // 19
            { 393.01f, -314.69f, 104.87f },  // 20
            { 392.73f, -298.69f, 104.77f },  // 21
            { 392.53f, -287.20f, 109.47f },  // 22  the throne
        });
}
