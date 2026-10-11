/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "Ai/Dungeon/DungeonClear/Data/DungeonClearRouteRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"
#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonRosterBuilders.h"
#include "Ai/Dungeon/DungeonClear/Util/DcTargeting.h"

#include "InstanceScript.h"
#include "Player.h"
#include "SharedDefines.h"

// --- Halls of Reflection (map 668) -----------------------------------------
//
// The declarative half: the roster patch, five event rows and the three authored
// legs. The imperative half is Overrides/HallsOfReflectionDriver.cpp (hooks
// 31-35); the arithmetic is the two pure kernels Util/DcHorWaveDecision.h and
// Util/DcHorEscapeDecision.h; the numbers all of them agree on are namespace
// DcHallsOfReflection in DungeonEventTables.h.
//
// READ THE TWO CONDITIONAL DRIVERS FIRST. This dungeon is not a corridor with
// set pieces in it — it is a SEQUENCE OF SCRIPTED EVENTS with a corridor between
// two of them, and roughly three quarters of the run is spent inside one of the
// two drivers below. Everything else in this file exists to hand the party from
// one of them to the other.
//
// WHAT THE MAP IS, in the three sentences that determine every design choice:
//
//   * FROM THE FIRST GOSSIP TO MARWYN'S DEATH THERE IS NOTHING TO PULL. Falric,
//     Marwyn and all 34 wave mobs pre-spawn invisible, NOT_SELECTABLE and
//     SetImmuneToAll; the instance activates them a wave at a time and THEY
//     attack, on the FARTHEST player. DC's advance/pull pipeline has no work to
//     do for six to nine minutes, and left alone it does the one thing that loses
//     the run: it walks the tank onto Marwyn's invisible, immune spawn and stands
//     there. tr-20260907-100815-2, five minutes and thirty-eight seconds,
//     encounterMask 0x0.
//
//   * DURING THE WAVES THE MAP HAS A 70.5YD LEASH THAT WIPES THE EVENT AND
//     RESPAWNS EVERY DEAD MOB, and a bot that RELEASES is locked out of the
//     instance until the survivors wipe or kill Marwyn (Map::CannotEnter
//     special-cases 668). Camping the altar is not a preference, it is the
//     encounter.
//
//   * THE ESCAPE'S LICH KING (36954) HAS unit_flags 0 AND NO IMMUNITIES. Stock
//     playerbots assist triggers acquire him within about a second of the gossip,
//     he heals himself to 75% below 70% so the damage is wasted — and, far worse,
//     one bot holding him as a victim makes DcCombatFlag::IsEngaged true for the
//     party and freezes every MayDrive rung while a Lich King walks at them. See
//     the three registry rows in section 3.7 of the plan and
//     Data/DcTargetExclusionRegistry.cpp.
//
// WHAT IS AUTHORED HERE, and why each piece exists:
//
//   A  the roster patch — the escape (which cannot derive: it has no
//      instance_encounters row at all), the three travel objectives, and two
//      reorders to make integer room for them;
//   B  five event rows — two anchored gates, one anchored kill, and the two
//      conditional drivers;
//   C  three authored legs — Marwyn to the General, the General to the throne
//      room, and the whole escape corridor;
//   D  the hooks (HallsOfReflectionDriver.cpp).
//
// AND WHAT IS DELIBERATELY *NOT* AUTHORED, so nobody re-derives it:
//
//   * NO RoomAggroRegistry ROWS, ANYWHERE. Nothing on this map aggros by
//     proximity. Every wave activation ends in SetInCombatWithZone() plus an
//     AttackStart on the farthest player; the Frostsworn General is made
//     aggressive by the instance the moment Marwyn dies; the escape's summons
//     open on the player nearest the Lich King by explicit threat. A room-aggro
//     row models a danger this dungeon does not have.
//
//   * NO ScriptedPullRegistry / BossPullbackRegistry ROWS. There is no pull on
//     this map to script or to drag back. The one "do not move it" rule the map
//     does have — the General evades if dragged 30yd from home — is a
//     FightInPlaceRegistry box, which is a different registry answering a
//     different question.
//
//   * NO DcCombatPurgeRegistry ROW FOR THE PHANTOM HALLUCINATIONS (38567). A
//     Phantom Mage summons one at 40 seconds and it does NOT count toward the
//     wave clear, which is exactly the population that registry was built for —
//     but it despawns on its own, and a combat-blind global clock is the wrong
//     instrument for something that is already self-limiting.
//
//   * NO DcNavPenaltyRegistry ROWS. Every leg here is a corridor, and the one
//     stretch where the party must not stray — the escape path — is covered by
//     NO_STOP on its whole length plus a driver that owns the tank's movement
//     yard by yard.
//
// AZEROTHCORE FACTS ENCODED HERE, so nobody "fixes" them:
//
//   * THE LEADER'S INSTANCE GUID KEY IS 37223 ON BOTH FACTIONS (37554 for part
//     2). InstanceScript::OnCreatureCreate stores the object under its current
//     entry BEFORE instance_halls_of_reflection UpdateEntry()s it to the
//     Alliance entry, so a lookup on 37221 / 36955 resolves nothing at all.
//
//   * GetData(DATA_BATTERED_HILT) DOES NOT MOVE WHEN THE INTRO GOSSIP IS TAKEN.
//     OnGossipSelect calls SetData(DATA_BATTERED_HILT, 1), and that case only
//     StorePersistentData()s the PERSISTENT flag — `_batteredHiltStatus`, which
//     is what GetData returns, is untouched. The "somebody already gossiped"
//     probe is GetPersistentData(PERSISTENT_DATA_BATTERED_HILT).
//
//   * THE ESCAPE HAS NO ENCOUNTER BIT. DungeonEncounter.dbc has rows 843/844 for
//     "Escaped from Arthas" and instance_encounters has NOTHING, so there is no
//     credit entry and no mask bit. encounterMask 0x3 IS the expected final mask
//     on a fully cleared run; a run reporting a third bit has read something else.

namespace
{
    using namespace DcHallsOfReflection;

    // --- event 2's gate: the altar is ours ---------------------------------
    //
    // DUE for exactly the window between the end of the intro and Marwyn's death
    // — ten waves, two boss fights, the 60-second rest between them, and any
    // number of automatic leash-wipe restarts. Three probes, cheapest first,
    // because this runs on every COMBAT tick of the DC leader on map 668 and the
    // party is in essentially unbroken combat for most of that window.
    //
    //   1. the map;
    //   2. PERSISTENT_DATA_INTRO. Set by the intro script's last step, which is
    //      the SAME TICK StartNextWave() runs — so this is precisely "wave 1 has
    //      begun". It is persistent, so it survives a map reload and a re-entered
    //      instance arms the driver correctly with no history of its own;
    //   3. Marwyn not yet down. False for ever once he is, which is what ends the
    //      driver and hands the corridor back to the ordinary clear.
    //
    // THIS IS A STRONGER NEAR-GATE THAN A DISTANCE CHECK, which is what
    // t/TestEventRegistry.cpp's IsNearGatedConditionalWhitelisted row records.
    // The intro flag is raised by exactly one thing — the intro script running to
    // the end — and that script is started by exactly one thing, the leader
    // gossip that event 1 performs sixty yards inside the front door. The flag
    // cannot read true with the party anywhere but in the altar chamber, because
    // the front door is SHUT for the last three seconds before it is set and for
    // every wave after.
    //
    // NOT GATED ON COMBAT, for the Pit of Saron / Halls of Lightning reason: the
    // gaps between waves 1-2-3-4 are FIVE SECONDS long (the instance cuts
    // _nextWaveTimer to 5000 when the last mob of a trash wave dies), and a
    // predicate that waited for combat to drop would get its ticks in those five
    // seconds and nowhere else.
    bool HorWavesDue(Player* bot, AiObjectContext* /*context*/)
    {
        if (!bot || bot->GetMapId() != MAP_ID)
            return false;

        InstanceScript* inst = DcTargeting::GetInstanceScript(bot);
        if (!inst)
            return false;

        if (!inst->GetPersistentData(PERSISTENT_DATA_INTRO))
            return false;

        return inst->GetBossState(DATA_MARWYN) != DONE;
    }

    // --- event 5's gate: the escape is running -----------------------------
    //
    // ONE PROBE, and it is the tightest near-gate in the module. The escape's
    // boss state is raised to IN_PROGRESS by exactly one line of the core —
    // npc_hor_leader_secondAI's ACTION_START_LK_FIGHT_REAL — reached only through
    // the gossip on a leader standing at LeaderEscapePos, which is itself only
    // offered after the freeze cutscene, which is only reached after the
    // Frostsworn General is dead. The party cannot be anywhere else on the map
    // when this reads true, and it cannot read true before the party put it there.
    //
    // It goes false again on DONE (the Lich King reaches the last waypoint with
    // four walls down) and on FAIL (the full instance reset that happens when the
    // last player leaves the map), so a wipe-and-retry re-arms cleanly.
    bool HorEscapeDue(Player* bot, AiObjectContext* /*context*/)
    {
        if (!bot || bot->GetMapId() != MAP_ID)
            return false;

        InstanceScript* inst = DcTargeting::GetInstanceScript(bot);
        if (!inst)
            return false;

        return inst->GetBossState(DATA_LICH_KING) == IN_PROGRESS;
    }
}

void RegisterHallsOfReflectionEvents(std::vector<DungeonEvent>& out)
{
    using namespace DcHallsOfReflection;

    // (1) START THE INTRO — anchored on OBJ(1), two steps.
    //
    // THE WHOLE DUNGEON HANGS OFF ONE GOSSIP. Nothing on this map is attackable,
    // reachable or even visible until the leader's intro has run; without this
    // event the party walks past her to a boss it cannot touch, which is exactly
    // what the only run that ever got inside did.
    //
    // TWO STEPS, AND THE SPLIT IS THE POINT. Step 0 is the gossip and nothing
    // else. Step 1 is a GARRISON: walk the party the sixty yards to the altar
    // camp and HOLD it there until GetData(DATA_WAVE_NUMBER) reaches 1.
    //
    // WHY THE HOLD IS A GATED MoveTo AND NOT A Custom. The intro is 224.5 seconds
    // long on Alliance (209 on Horde, 75.5 with the skip), and none of it is
    // proximity-gated — nobody needs to be anywhere. So this is nearly four
    // minutes in which the right thing for the party to do is stand at the camp
    // and RECOVER, and a gated MoveTo with no WhileHolding hook is the one hold
    // shape in the module that does not starve the tank's rest rung while it
    // holds. The party should reach wave 1 full, because after wave 1 there is no
    // rest until Falric is dead.
    //
    // WHY THE GATE IS DATA_WAVE_NUMBER >= 1 AND NOT THE INTRO FLAG. They are set
    // on the same tick, but the wave number is what event 2's driver keys on and
    // using the same fact for the hand-over means the two can never disagree
    // about which of them owns the tick. On the instant the gate clears,
    // HorWavesDue is already true and DcRel::EventDueCombat (61) takes the party.
    //
    // STEPS OWN MOVEMENT because the gossip hook delivers the tank to the leader
    // itself. Without the flag the per-tick position hold cancels that walk-in
    // the tick after it is issued.
    //
    // PERSISTENT because the hold spans four minutes of cutscene during which the
    // Arthas door opens, a Lich King walks in and thirty-four mobs become visible
    // — any of which can flicker a bot's combat flag and rewind a non-persistent
    // step list back to step 0, re-walking the party to a gossip it has spent.
    out.push_back(
        EventBuilder(MAP_ID, EVENT_INTRO, "Start the intro")
            .Anchored(ORDER_INTRO)
            .Persistent()
            .StepsOwnMovement()
            .Custom(HOOK_HOR_INTRO_GOSSIP)
                .Timeout(INTRO_GOSSIP_TIMEOUT_MS)
            .MoveToHoldUntilInstanceData(CAMP_X, CAMP_Y, CAMP_Z, CAMP_LEASH,
                                         DATA_WAVE_NUMBER, 1)
                .Timeout(INTRO_HOLD_TIMEOUT_MS)
            .Build());

    // (2) HOLD THE ALTAR — the conditional wave driver, and the event that makes
    // the first two thirds of this dungeon possible.
    //
    // ONE Custom step, for the Black Morass / Pit of Saron reason: what this leg
    // needs is a standing PREFERENCE re-decided every tick — hold, or hand the
    // tick to the rotation, or regather after a wipe — not a sequence. A step list
    // can only say "do these in order and block on each", and every one of the
    // five states can be interrupted at any point by a wave landing or by the
    // whole event wiping back to zero.
    //
    // DRIVES IN COMBAT — the load-bearing flag. The gaps between waves 1-2-3-4
    // and 6-7-8-9 are FIVE SECONDS (the instance cuts its 150s wave timer to 5000
    // the moment the last trash mob of a wave dies), and the two boss waves are
    // continuous. A rung that only ran out of combat would get five seconds in
    // every ninety and would stop running altogether exactly when the party fell
    // behind — which is when the hold matters most, because a party that falls
    // behind gets scattered and scattering is what trips the 70.5yd leash.
    //
    // STEPS OWN MOVEMENT — the driver walks the tank back onto the camp on its
    // own spline, and the at-objective hold runs BEFORE Drive: without the flag
    // last tick's glide is cancelled before the hook can see it. It is also what
    // makes a Done RETURN YIELD THE TICK, and on this map that is the half that
    // decides fights: the driver has NOTHING to steer for the whole of every wave
    // — the mobs come to the party — so every tick it claimed while a wave was up
    // would be a tick the tank did not swing.
    //
    // OWNS THE PULL — zero advanced pulls at the altar, and here that is not a
    // preference but a survival rule. The pull's Idle branch answers unplanned
    // aggro by walking a fresh camp BACK along the route until it finds ground
    // clear of hostiles; on this map "back" is toward the front door, which is
    // 65yd from CenterPos against a leash of 70.5. A single drag leg would wipe
    // the event and replay four waves with every dead mob respawned. Dropping the
    // scout-lag with it is the other half of the same win: wave activation opens
    // on the FARTHEST player, so a tight clump around the tank is the whole
    // defence, and a scouting tank fifteen yards ahead of its party is the
    // opposite of one.
    //
    // REPEATABLE — the wave phase is not a thing that completes once. A leash
    // wipe puts the counter back to 0 and the instance restarts it automatically
    // once every player is alive within 40yd of the altar, and expressing
    // completion as "the condition went false" rather than as a latch is what
    // lets the driver re-arm in whatever state the instance really is.
    //
    // PERSISTENT — the step list must not be rewound by a combat gap, and on this
    // leg a "gap" is five seconds long.
    //
    // NOT Optional. Skipping the waves is skipping Falric and Marwyn.
    //
    // PanelAfterBoss(Falric), and NEVER PanelBeforeBoss(Marwyn), which is what it
    // visually wants and must not have: panelGatesBossEntry also keys
    // DcTargeting::HasPendingSummonEvent, which reads an unlatched gating event as
    // "this boss must still be SUMMONED" and suppresses the dynamic pull within
    // 80yd of him — and a REPEATABLE event is never latched, so the suppression
    // would be permanent ([[dc-panelbeforeboss-repeatable-permanent-hold]]).
    out.push_back(
        EventBuilder(MAP_ID, EVENT_WAVES, "Hold the altar")
            .Conditional(&HorWavesDue)
            .Repeatable()
            .Persistent()
            .OwnsThePull()
            .DrivesInCombat()
            .StepsOwnMovement()
            .PanelAfterBoss(NPC_FALRIC)
            .Custom(HOOK_HOR_WAVES)
                .Timeout(WAVES_TIMEOUT_MS)
            .Build());

    // (3) THE FROSTSWORN GENERAL — anchored on OBJ(2), one engage-kill.
    //
    // THE ONE MANDATORY FIGHT THIS DUNGEON DOES NOT COUNT AS AN ENCOUNTER. He has
    // no DungeonEncounter row and no kill bit; his death sets
    // PERSISTENT_DATA_FROSTSWORN_GENERAL, and at_hor_shadow_throne — the trigger
    // that starts the freeze cutscene and therefore the entire second half —
    // refuses SILENTLY while that flag is clear. A run that walks past him stalls
    // in the throne room with nothing to name.
    //
    // A PLAIN KillCreatureEngage, and no completion hook behind it. The step is
    // Done once no LIVE creature of the entry remains, which is aliveness rather
    // than corpse presence — so a corpse that despawns before DC next ticks
    // completes the step rather than stranding it, and there is no window in
    // which a false "nothing found" can latch it early: 36723 has a real DB spawn
    // that exists from map load (invisible and immune until Marwyn dies), so the
    // scan can never read empty before the kill.
    //
    // NO OwnsThePull() and NO DrivesInCombat(), and both absences are structural
    // rather than choices — the flags are conditional-only. The anchored path
    // infers the pull stand-down from Persistent() alone, and there is no
    // anchored combat rung to set DrivesInCombat on (DcRel::AtObjective is
    // non-combat). Neither is wanted here anyway: this is one ordinary fight
    // against one boss and five adds, which is what the stock combat engine is
    // for. What it DOES need is the FightInPlaceRegistry box — he evades if
    // dragged 30yd from home — and that is authored in Data/FightInPlaceRegistry.
    out.push_back(
        EventBuilder(MAP_ID, EVENT_GENERAL, "The Frostsworn General")
            .Anchored(ORDER_GENERAL)
            .Persistent()
            .KillCreatureEngage(NPC_FROSTSWORN_GENERAL, 1, 80.0f)
                .Timeout(GENERAL_TIMEOUT_MS)
            .Build());

    // (4) THE THRONE ROOM — anchored on OBJ(3), three steps, and the last one is
    // irreversible.
    //
    // Step 0 (hook 33) gathers the party at the west door, walks the tank into
    // areatrigger 5605 and forges it, then waits out the ~21 seconds of freeze
    // cutscene until the part-2 leader carries a gossip flag.
    //
    // Step 1 walks the party PAST THE FROZEN LICH KING to her. He is 36yd
    // south-west of where she ends up and he is not yet hostile; the muster point
    // is three yards short of her on the approach side so the gather does not
    // stand on top of the NPC it is about to talk to.
    //
    // Step 2 (hook 34) is THE POINT OF NO RETURN and is the pickiest step in this
    // file. Its gossip has no quest requirement and no confirmation: the tick it
    // lands, the boss state goes IN_PROGRESS, the prison drops off the Lich King
    // and he starts walking. From then until the run ends there is no drinking,
    // no out-of-combat resurrect and no way back, because he SetInCombatWithZone()s
    // every player once a second. So the hook holds until the party is genuinely
    // ready — see hook 34 for the four conditions and why 5-of-5 alive is not the
    // usual 3-of-4.
    //
    // STEPS OWN MOVEMENT because hooks 33 and 34 both issue their own walks (the
    // forge point is 25yd past the anchor, and the muster is 25yd past that), and
    // the anchored per-tick hold would cancel each spline the tick after it is
    // issued.
    //
    // PERSISTENT because the sequence spans a 21-second cutscene in which the
    // leader is attacking the Lich King and the party's combat flags flicker —
    // a rewind there would re-walk the party out of the throne room to re-forge a
    // trigger that has already fired.
    out.push_back(
        EventBuilder(MAP_ID, EVENT_THRONE, "The throne room")
            .Anchored(ORDER_THRONE)
            .Persistent()
            .StepsOwnMovement()
            .Custom(HOOK_HOR_THRONE)
                .Timeout(THRONE_TIMEOUT_MS)
            .MoveTo(MUSTER_X, MUSTER_Y, MUSTER_Z, MUSTER_LEASH)
            .Custom(HOOK_HOR_ESCAPE_GO)
                .Timeout(ESCAPE_GO_TIMEOUT_MS)
            .Build());

    // (5) ESCAPE THE LICH KING — the conditional escape driver.
    //
    // Same shape as event 2 and for the same four reasons, plus one this
    // encounter has on its own: THE PARTY IS IN COMBAT FOR EVERY SINGLE TICK OF
    // IT. npc_hor_lich_kingAI calls SetInCombatWithZone() once a second for the
    // whole four to six minutes, so an event driven only out of combat would get
    // exactly zero ticks. DrivesInCombat is not an optimisation here, it is the
    // difference between the event existing and not.
    //
    // OWNS THE PULL for the same reason as event 2, one step harder: a camp
    // dragged BACKWARD on this leg is dragged toward a Lich King who is
    // permanently 10-30yd behind the party, and every yard back is closer to a
    // 7068-per-second frost ring and a 10 000-damage zap.
    //
    // STEPS OWN MOVEMENT — the driver moves the tank stand point to stand point
    // with its own long-range splines, and yields on Done so the party can
    // actually kill the summons. It yields for most of every wall fight.
    //
    // REPEATABLE because a wipe here does not reset anything while a corpse
    // remains in the map. Only when the LAST player leaves does the instance put
    // the leader back at LeaderEscapePos with her gossip and set the boss state to
    // FAIL; if the harness ever empties and refills the map mid-run, the driver
    // must re-arm from whatever state it finds.
    //
    // PanelAfterBoss(Marwyn) rather than PanelBeforeBoss for the same reason as
    // event 2 — and note that the Lich King HAS a roster row here, so folding the
    // event into it would also hide the one row whose completion is read from a
    // boss-state slot rather than from the encounter mask.
    out.push_back(
        EventBuilder(MAP_ID, EVENT_ESCAPE, "Escape the Lich King")
            .Conditional(&HorEscapeDue)
            .Repeatable()
            .Persistent()
            .OwnsThePull()
            .DrivesInCombat()
            .StepsOwnMovement()
            .PanelAfterBoss(NPC_MARWYN)
            .Custom(HOOK_HOR_ESCAPE)
                .Timeout(ESCAPE_TIMEOUT_MS)
            .Build());
}

// --- the roster: the encounter with no encounter row -----------------------
//
// THIS MAP'S DERIVATION IS SOUND, unlike Utgarde Pinnacle's and Pit of Saron's.
// Falric (38112) and Marwyn (38113) each have an ENCOUNTER_CREDIT_KILL_CREATURE
// instance_encounters row (839/840 and 841/842) and a real `creature` spawn, so
// BossSpawnIndex::Build's two-step join emits both correctly with their DBC bits
// 0 and 1. Nothing here repairs them; the two reorder rows exist only to make
// integer room for the three objectives on one contiguous 1..6 scale.
//
// WHAT DOES NOT DERIVE IS THE THIRD ENCOUNTER, and it is a different failure from
// either of those maps. "Escaped from Arthas" has DungeonEncounter.dbc rows
// 843/844 — so the achievement side of the game knows about it — and NO
// instance_encounters row on either difficulty. Step 1 of the join walks
// instance_encounters, so there is no creditEntry to key on and, more
// importantly, NO KILL BIT ANYWHERE: nothing sets a mask bit when the escape
// completes, because the escape is not a kill. The Lich King is alive and at full
// health when the run ends.
//
// SO MakeBossWithBit IS THE WRONG ESCAPE HATCH HERE and MakeBoss with a
// doneBossStateIndex is the right one. That is the Nexus Frozen Commander shape
// (Data/Events/NexusEvents.cpp): completion is read from the instance script's
// own boss-state slot — npc_hor_lich_kingAI sets DATA_LICH_KING to DONE when he
// reaches the last waypoint with all four walls down — and MakeBoss parks
// encounterIndex at 64 so the completed-mask check, which is guarded by
// `encounterIndex < 32`, never consults a bit that does not exist.
//
// THE CONSEQUENCE FOR THE HARNESS, stated so nobody triages it as a bug:
// encounterMask 0x3 IS the expected final mask on a fully cleared Halls of
// Reflection run. There is no third bit. The roster is three bosses and three
// objectives; "bosses 3/3" comes from the roster and the boss-state slot, not
// from the mask.
//
// AND THE ANCHOR IS THE END OF THE ESCAPE PATH, NOT THE LICH KING'S SPAWN. His
// spawn (5552.77, 2262.57, 733.01) is thirty yards from where the party stands to
// take the escape gossip, and anchoring him there would mean that any tick the
// escape driver is not active — before the gossip, or after a wipe — the ordinary
// clear walks the party AT him. WP18 (5262.77, 1669.98, 784.30) is where he
// actually is when the encounter completes, it is 679yd down the path, and
// walking toward it is walking the right way.
void RegisterHallsOfReflectionRoster(std::vector<BossRosterPatch>& t)
{
    using namespace DcRoster;
    using namespace DcHallsOfReflection;

    // ONE Any-gated patch serves both difficulties. The two DBC bits are 0 and 1
    // on normal AND heroic with no shift, the escape has no bit on either, and
    // heroic changes only numbers (1.4x add health, a harsher Hopelessness, the
    // PermBindAllPlayers in the outro) and not the shape of the dungeon.
    BossRosterPatch p;
    p.mapId = MAP_ID;

    p.add = {
        // OBJ(1) — the gossip that starts the dungeon. An objective's
        // encounterIndex is an ordering hint only (objectives carry no kill bit
        // and NextDungeonBossValue never tests the mask for one), so it stays 0
        // and the clear orders by orderOverride.
        //
        // It exists because the thing it anchors cannot be reached by walking to
        // a boss: the leader is a friendly NPC 60yd short of the altar, and the
        // two bosses behind her are invisible and immune until her cutscene has
        // run. Without this row the clear's first act is to walk past her.
        MakeObjective(OBJ(1), /*encounterIndex*/ 0, MAP_ID, "Start the intro",
                      INTRO_X, INTRO_Y, INTRO_Z,
                      INTRO_ARRIVE, /*gateEntry*/ 0, /*hook*/ 0,
                      /*eventId*/ EVENT_INTRO,
                      /*orderOverride*/ ORDER_INTRO),

        // OBJ(2) — the Frostsworn General. He is a real creature with a real
        // spawn, so this could have been a boss row; it is an OBJECTIVE because
        // he is not an encounter — no DungeonEncounter row, no kill bit, nothing
        // for a completion mask to carry. An objective whose event owns
        // completion is the honest shape for that, and it is the same shape Halls
        // of Stone's Tribunal uses one map over.
        MakeObjective(OBJ(2), /*encounterIndex*/ 0, MAP_ID, "The Frostsworn General",
                      GENERAL_X, GENERAL_Y, GENERAL_Z,
                      GENERAL_ARRIVE, /*gateEntry*/ 0, /*hook*/ 0,
                      /*eventId*/ EVENT_GENERAL,
                      /*orderOverride*/ ORDER_GENERAL),

        // OBJ(3) — the throne room. Anchored at the WEST DOOR (197342), which is
        // deliberately OUTSIDE areatrigger 5605's box: arriving here must not be
        // the same act as starting the cutscene, because the step after the
        // cutscene is the point of no return and the party has to be formed up
        // and topped off before it.
        MakeObjective(OBJ(3), /*encounterIndex*/ 0, MAP_ID, "The throne room",
                      THRONE_X, THRONE_Y, THRONE_Z,
                      THRONE_ARRIVE, /*gateEntry*/ 0, /*hook*/ 0,
                      /*eventId*/ EVENT_THRONE,
                      /*orderOverride*/ ORDER_THRONE),

        // The escape. completionFrom is 0 — there is nothing to inherit a bit
        // from — and doneBossStateIndex is DATA_LICH_KING, the instance's own
        // slot 2. See the header comment for why this is the Nexus shape and not
        // the MakeBossWithBit one.
        MakeBoss(NPC_LICH_KING, MAP_ID, "The Lich King",
                 PATH_WAYPOINTS[18].x, PATH_WAYPOINTS[18].y, PATH_WAYPOINTS[18].z,
                 /*completionFrom*/ 0,
                 /*orderOverride*/ ORDER_LICH_KING,
                 /*doneBossStateIndex*/ DATA_LICH_KING),
    };

    // The two derived bosses onto the same 1..6 scale. Their relative order is
    // unchanged from what their DBC bits already gave them; the reorder exists
    // only to open integer slots for the objective before them and the two after.
    p.reorder = {
        { NPC_FALRIC, ORDER_FALRIC },
        { NPC_MARWYN, ORDER_MARWYN },
    };

    t.push_back(std::move(p));
}

// --- the three designed legs, in walking order ------------------------------
//
// DERIVED, NOT DRAWN. Every anchor below is a point on the corridor
// LongRangePathfinder itself returns for that leg against the live map-668
// mmtiles, decimated to ~16yd — the [[dc-navharness-prints-the-route]] method,
// and the reason t/TestHallsOfReflectionRouteProbe prints the polylines it
// routed: an mmaps regen that moves a corridor is re-authored the same way
// rather than by hand.
//
// THE FIRST TWO LEGS EXIST FOR THE PLAIN REASON — a fixed polyline through two
// script-only doors, so A* does not re-derive the corridor on every rebuild and
// so the doors are declared rather than discovered.
//
// THE ESCAPE ROW EXISTS FOR A REASON NO OTHER ROW IN THE MODULE HAS. The driver
// never reads it: hook 35 moves the tank from stand point to stand point with its
// own splines, and while the escape is running the row is dead weight. It is
// there for the ticks the driver is NOT running — before the gossip, and after a
// wipe — because on this leg the difference between "walk forward" and "walk
// back" is the difference between finishing and dying. Every anchor carries
// NO_STOP, so nothing plans a camp anywhere on the path, and the polyline runs
// strictly -x and -y from the throne room to WP18, so a party handed to
// DcRel::Advance moves AWAY from the Lich King by construction.
//
// Each row is keyed on the entry of its DESTINATION and starts where the party
// will be standing when that leg begins, because DungeonPathFollower::SeedCursor
// projects the bot onto the row from its own position and a row that starts
// somewhere else snaps the cursor to the far end
// ([[dc-anchor-route-must-cover-where-the-party-stands]]).
void RegisterHallsOfReflectionRoute()
{
    using namespace DcRoster;
    using namespace DcHallsOfReflection;

    // --- Leg A: the altar to the Frostsworn General ------------------------
    //
    // ~110yd, out of the altar chamber through the Arthas door and up the
    // corridor. Starts at CenterPos because that is the ground the party is
    // standing on when Marwyn dies — the camp is 20yd from it and the fight
    // itself ends wherever Marwyn was tanked, both of which project onto this row
    // near anchor 0.
    //
    // The Arthas door (197341) is flagged DOOR_AHEAD. It is opened by the
    // instance the moment Marwyn dies — i.e. as a precondition of this whole leg
    // — so the flag is belt-and-braces, and it is IsScriptOnly rather than
    // IsNavigationIgnored on purpose: a run that DOES pause here has regressed
    // somewhere the module should be told about.
    //
    // 123yd, ten anchors, and one straight line: the corridor A* returns for this
    // leg is dead straight in xy from the altar to the anchor and flat at z 708.3
    // for all but its first eleven yards, so the decimation is a plain every-16yd
    // sample of it with two exceptions.
    //
    // ANCHOR 1 IS A CREST ANCHOR. The escort spline between anchors is LINEAR, so
    // a chord over a convex rise puts the bot inside the floor; the step down off
    // the altar (710.4 -> 708.6 over eleven yards) bows 0.6yd above its chord at
    // the halfway point. One anchor on the mesh at the top of it is the fix, and
    // it costs nothing.
    //
    // ANCHOR 6 IS THE ARTHAS DOOR, kept as its own anchor rather than falling
    // where the decimation would have put it, so the DOOR_AHEAD flag names the
    // real GO position.
    DungeonClearRouteRegistry::Register(
        MAP_ID, DUNGEON_DIFFICULTY_NORMAL, OBJ(2),
        {
            { CENTER_X,  CENTER_Y,  CENTER_Z  },                       // 0  the altar
            { 5315.02f,  2012.23f,  710.09f   },                       // 1  crest
            { 5320.58f,  2017.98f,  708.55f   },                       // 2
            { 5331.70f,  2029.49f,  708.28f   },                       // 3
            { 5342.82f,  2041.00f,  708.28f   },                       // 4
            { 5353.93f,  2052.50f,  708.28f   },                       // 5
            { GO_ARTHAS_DOOR_X, GO_ARTHAS_DOOR_Y, GO_ARTHAS_DOOR_Z,
              GO_ARTHAS_DOOR, ToFlag(AnchorFlag::DOOR_AHEAD) },        // 6  the Arthas door
            { 5370.61f,  2069.76f,  708.28f   },                       // 7
            { 5381.73f,  2081.27f,  708.28f   },                       // 8
            { GENERAL_X, GENERAL_Y, GENERAL_Z },                       // 9  the anchor
        });

    // --- Leg B: the Frostsworn General to the throne room ------------------
    //
    // ~160yd and a 26yd climb, up the ramp from the corridor floor (z 707.7) to
    // the throne-room landing (z 733). Starts at the General's ANCHOR rather than
    // at his home, because the anchor is where the party formed up and the fight
    // is held in place around him (FightInPlaceRegistry) rather than dragged.
    //
    // The west throne door (197342) is spawned OPEN and never scripted; the flag
    // is there so the leg declares it rather than meeting it.
    //
    // 183yd, thirteen anchors, and again one straight line — but this one CLIMBS
    // 26 yards, and the climb is why the sampling stays at 16yd across it rather
    // than widening. The ramp runs from anchor 7 (z 708.5) to anchor 11 (z 733.6)
    // at a steady 1.7yd of rise per 4yd of ground; measured against the mesh, the
    // worst chord sag over any 16yd span of it is 1.35yd ABOVE the floor, which
    // is the safe direction (a chord over a CONCAVE run rides above the surface;
    // it is a chord over a CONVEX crest that drags a bot through rock).
    //
    // ANCHOR 2 IS THE GENERAL'S HOME, kept explicitly because the FightInPlace box
    // is centred on it — a leg that named a decimated point instead would drift
    // out of the box a mesh regen later and nobody would notice.
    DungeonClearRouteRegistry::Register(
        MAP_ID, DUNGEON_DIFFICULTY_NORMAL, OBJ(3),
        {
            { GENERAL_X, GENERAL_Y, GENERAL_Z },                       //  0  the anchor
            { 5405.91f,  2106.70f,  708.28f   },                       //  1
            { GENERAL_HOME_X, GENERAL_HOME_Y, GENERAL_HOME_Z },        //  2  his home
            { 5427.74f,  2130.10f,  708.28f   },                       //  3
            { 5438.66f,  2141.80f,  708.28f   },                       //  4
            { 5449.57f,  2153.50f,  708.28f   },                       //  5
            { 5460.48f,  2165.20f,  708.28f   },                       //  6
            { 5471.40f,  2176.90f,  708.46f   },                       //  7  the ramp foot
            { 5482.31f,  2188.60f,  714.10f   },                       //  8
            { 5493.23f,  2200.30f,  720.89f   },                       //  9
            { 5504.14f,  2212.00f,  727.66f   },                       // 10
            { 5515.05f,  2223.70f,  733.61f   },                       // 11  the landing
            { THRONE_X,  THRONE_Y,  THRONE_Z, GO_DOOR_BEFORE_THRONE,
              ToFlag(AnchorFlag::DOOR_AHEAD) },                        // 12  the west door
        });

    // --- Leg C: the escape corridor ----------------------------------------
    //
    // 692yd, twenty anchors, NO_STOP on every one of them, and the party should
    // never walk a single yard of it under this row's control — hook 35 owns the
    // movement from the gossip to WP18. See the header note for why it exists
    // anyway.
    //
    // It starts at the MUSTER POINT, which is where the party is standing when
    // the gossip lands, and then follows the Lich King's own PathWaypoints —
    // because that is the only line through this corridor the encounter has ever
    // been walked on, and a re-derived A* corridor could legitimately cut a
    // corner the wall geometry does not allow.
    DungeonClearRouteRegistry::Register(
        MAP_ID, DUNGEON_DIFFICULTY_NORMAL, NPC_LICH_KING,
        {
            { MUSTER_X, MUSTER_Y, MUSTER_Z, 0, ToFlag(AnchorFlag::NO_STOP) },
            { PATH_WAYPOINTS[0].x,  PATH_WAYPOINTS[0].y,  PATH_WAYPOINTS[0].z,  0, ToFlag(AnchorFlag::NO_STOP) },
            { PATH_WAYPOINTS[1].x,  PATH_WAYPOINTS[1].y,  PATH_WAYPOINTS[1].z,  0, ToFlag(AnchorFlag::NO_STOP) },
            { PATH_WAYPOINTS[2].x,  PATH_WAYPOINTS[2].y,  PATH_WAYPOINTS[2].z,  0, ToFlag(AnchorFlag::NO_STOP) },
            { PATH_WAYPOINTS[3].x,  PATH_WAYPOINTS[3].y,  PATH_WAYPOINTS[3].z,  0, ToFlag(AnchorFlag::NO_STOP) },
            { PATH_WAYPOINTS[4].x,  PATH_WAYPOINTS[4].y,  PATH_WAYPOINTS[4].z,  0, ToFlag(AnchorFlag::NO_STOP) },
            { PATH_WAYPOINTS[5].x,  PATH_WAYPOINTS[5].y,  PATH_WAYPOINTS[5].z,  0, ToFlag(AnchorFlag::NO_STOP) },
            { PATH_WAYPOINTS[6].x,  PATH_WAYPOINTS[6].y,  PATH_WAYPOINTS[6].z,  0, ToFlag(AnchorFlag::NO_STOP) },
            { PATH_WAYPOINTS[7].x,  PATH_WAYPOINTS[7].y,  PATH_WAYPOINTS[7].z,  0, ToFlag(AnchorFlag::NO_STOP) },
            { PATH_WAYPOINTS[8].x,  PATH_WAYPOINTS[8].y,  PATH_WAYPOINTS[8].z,  0, ToFlag(AnchorFlag::NO_STOP) },
            { PATH_WAYPOINTS[9].x,  PATH_WAYPOINTS[9].y,  PATH_WAYPOINTS[9].z,  0, ToFlag(AnchorFlag::NO_STOP) },
            { PATH_WAYPOINTS[10].x, PATH_WAYPOINTS[10].y, PATH_WAYPOINTS[10].z, 0, ToFlag(AnchorFlag::NO_STOP) },
            { PATH_WAYPOINTS[11].x, PATH_WAYPOINTS[11].y, PATH_WAYPOINTS[11].z, 0, ToFlag(AnchorFlag::NO_STOP) },
            { PATH_WAYPOINTS[12].x, PATH_WAYPOINTS[12].y, PATH_WAYPOINTS[12].z, 0, ToFlag(AnchorFlag::NO_STOP) },
            { PATH_WAYPOINTS[13].x, PATH_WAYPOINTS[13].y, PATH_WAYPOINTS[13].z, 0, ToFlag(AnchorFlag::NO_STOP) },
            { PATH_WAYPOINTS[14].x, PATH_WAYPOINTS[14].y, PATH_WAYPOINTS[14].z, 0, ToFlag(AnchorFlag::NO_STOP) },
            { PATH_WAYPOINTS[15].x, PATH_WAYPOINTS[15].y, PATH_WAYPOINTS[15].z, 0, ToFlag(AnchorFlag::NO_STOP) },
            { PATH_WAYPOINTS[16].x, PATH_WAYPOINTS[16].y, PATH_WAYPOINTS[16].z, 0, ToFlag(AnchorFlag::NO_STOP) },
            { PATH_WAYPOINTS[17].x, PATH_WAYPOINTS[17].y, PATH_WAYPOINTS[17].z, 0, ToFlag(AnchorFlag::NO_STOP) },
            { PATH_WAYPOINTS[18].x, PATH_WAYPOINTS[18].y, PATH_WAYPOINTS[18].z },
        });
}
