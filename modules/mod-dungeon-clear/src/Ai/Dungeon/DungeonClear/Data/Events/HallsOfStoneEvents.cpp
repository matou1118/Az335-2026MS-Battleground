/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"
#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonRosterBuilders.h"

#include "Creature.h"
#include "InstanceScript.h"  // EncounterState (DONE) — the escort step's data gate
#include "Player.h"
#include "Playerbots.h"
#include "SharedDefines.h"

// --- Halls of Stone (map 599) ---------------------------------------------
//
// This dungeon does not fail because the party is bad at it. Five runs of
// tp-20260831-205458-3 finished 2/3 bosses with ZERO deaths, zero wipes, and
// every one of them ended the same way: `paused for over 60s: a closed door is
// blocking the path`, with the tank standing ~17yd from GO 191296 and Sjonnir
// 104yd away behind it. The party walked the whole dungeon, arrived at the door,
// and waited for a gossip option nobody was ever going to click.
//
// --- WHY IT IS LOCKED OUT --------------------------------------------------
//
// The derived roster for map 599 reads `0, 1, 3`. Index 2 is missing.
//
// instance_encounters, map 599 (both difficulties, verified live):
//
//     563/564  bit 0  Krystallus             creditType 0  creditEntry 27977
//     565/566  bit 1  Maiden of Grief        creditType 0  creditEntry 27975
//     567/568  bit 2  Tribunal of Ages       creditType 1  creditEntry 59046  <-- SPELL
//     569/570  bit 3  Sjonnir the Ironshaper creditType 0  creditEntry 27978
//
// BossSpawnIndex::Build skips every encounter that is not
// ENCOUNTER_CREDIT_KILL_CREATURE, because a cast-spell credit's `creditEntry` is
// a SPELL id and there is no creature to look a spawn position up by. That
// filter is right and stays. Drak'Tharon Keep hit the identical defect with The
// Prophet Tharon'ja and named this dungeon as the next instance of it
// (DrakTharonKeepEvents.cpp: "Halls of Stone (599, Tribunal of Ages, 59046)").
// This file is that patch.
//
// But on THIS map the missing encounter is not merely a missing scoreboard row —
// it is the ONLY thing that opens the way to the last boss:
//
//   * GO_SJONNIR_DOOR 191296 at (1206.56, 666.98, 197.74) spawns CLOSED
//     (gameobject.state = 1, template Data0 = 0, lockId 0).
//   * The only code path that opens it is instance_halls_of_stone's
//     SetData(BRANN_DOOR, DONE).
//   * The only caller of that is brann_bronzebeard.cpp, 3.2s after Brann
//     physically arrives at POINT_SJONNIR_DOOR.
//   * Brann only walks there after FOUR gossip selections, the second of which
//     starts a 300-second defend event.
//
// So the fix is not a roster row. It is the four clicks and the five minutes
// between two of them.
//
// --- DEVIATION FROM MakeBossWithBit, STATED DELIBERATELY -------------------
//
// DcRoster::MakeBossWithBit exists for exactly this defect and its doc comment
// names Halls of Stone as an intended user. It is NOT used here, and the comment
// there has been amended to say so.
//
// The reason is specific and does not generalise: MakeBossWithBit wants an
// `entry` to anchor and target, and for CoT Stratholme's Mal'ganis and Trial of
// the Champion's four that is exactly right — those are real creature spawns
// hidden only by the credit-type filter. THE TRIBUNAL OF AGES HAS NO CREATURE AT
// ALL. Brann is friendly; the three faces (30898/30897/30899) are
// NOT_SELECTABLE, faction 114, NullCreatureAI emitters that cannot be targeted,
// damaged or interrupted. There is nothing to put in `entry` and nothing to
// kill. So the Tribunal is authored as an OBJECTIVE whose event owns completion —
// the shape the Violet Hold uses for its three prisoner encounters.
//
// --- THE ENCOUNTER, in the terms the events reason about -------------------
//
//   PHASE A — THE ESCORT (not the encounter). Gossip 9669 at Brann's DB spawn
//     starts waypoint path 280701: 15 points, 170yd, all run, no delays. He walks
//     it as REACT_AGGRESSIVE and ALONE, with SetRegeneratingHealth(false) and no
//     immunity, past seven constructs. If he dies the escort restarts from
//     scratch at his DB spawn. At the path end PathEndReached sets
//     BossState(BRANN_BRONZEBEARD, DONE), REACT_PASSIVE, and re-offers gossip —
//     menu 9670.
//
//   PHASE B — THE TRIBUNAL. Gossip 9670 fires InitializeEvent() IMMEDIATELY (not
//     on arrival): boss state 2 -> IN_PROGRESS, the three faces are summoned, and
//     Brann walks to the console at (897.18, 331.77, 203.71). Then a FIXED
//     300-second survival timer in three 100s phases whose abilities STACK —
//     Glare of the Tribunal every 1.5s from +47s, Dark Matter from +113s, Searing
//     Gaze from +216s — plus three waves of adds, all of which arrive via
//     SetInCombatWithZone() and are Taunt-wired to Brann. NOTHING THE PARTY DOES
//     SHORTENS IT. At 300s EndTribunalFight() runs; at ~317s it despawns the
//     summons, sets boss state 2 DONE, casts 59046 (the bit-2 credit), and walks
//     Brann to the lore stop where he offers menu 10206.
//
//     The ONLY fail condition is Brann's death. He is REACT_PASSIVE with
//     regeneration off for the whole 300s.
//
//   PHASE C — THE DOOR. Gossip 10206 skips 256s of lore: Brann stealths, walks
//     off and despawns, then RESPAWNS (different GUID, same entry — never cache a
//     GUID here) at (1199.685, 667.155, 196.324) offering menu 10012. Gossip
//     10012 walks him to the door; +3.2s it opens.
//
// --- THE HIGHEST-RISK QUESTION, ANSWERED FROM THE CORE RATHER THAN LIVE ----
//
// The plan this file implements flagged one unknown as sitting directly on the
// critical path: between the Tribunal and the door Brann carries SPELL_STEALTH
// 58506, so if gossip target resolution respects stealth, phase C deadlocks at
// the exact door we are here to open. It does not, for two independent reasons,
// both read out of the core rather than guessed:
//
//   1. Spell.dbc 58506 effect 0 is APPLY_AURA with EffectApplyAuraName = 4,
//      which is SPELL_AURA_DUMMY — not SPELL_AURA_MOD_STEALTH (16). The
//      "stealth" is a visual. Nothing in the server's visibility system is
//      touched, so FindNearestCreature and CanSeeOrDetect are unaffected.
//   2. Player::GetNPCIfCanInteractWith — which is where CMSG_GOSSIP_HELLO lands —
//      tests exist / alive / npcflag / not-charmed / reaction > UNFRIENDLY /
//      within INTERACTION_DISTANCE. There is NO visibility test and NO immunity
//      test, so his SetImmuneToAll(true) is irrelevant too.
//
// And he keeps the flag: Reset() calls SetNpcFlag(GOSSIP | QUESTGIVER) at the
// top and the "Tribunal DONE" branch that teleports him to the door does not
// remove it. No fallback hook is needed and none is authored.

using namespace DcHallsOfStone;

namespace
{
    // NORMAL ENTRIES ONLY, on both difficulties. Creature::InitEntry swaps the
    // difficulty TEMPLATE and then does `SetEntry(Entry); // normal entry
    // always`, and every summon in brann_bronzebeard.cpp passes the bare normal
    // constant, so GetEntry() is 27983/27984/27985 on heroic too. See the
    // difficulty-twin note in namespace DcHallsOfStone.
    constexpr uint32 kWaveEntries[] =
    {
        NPC_DARK_RUNE_PROTECTOR,
        NPC_DARK_RUNE_STORMCALLER,
        NPC_IRON_GOLEM_CUSTODIAN,
    };

    constexpr uint32 kHeadEntries[] = { NPC_KADDRAK, NPC_MARNAK, NPC_ABEDNEUM };

    bool HosTribunalWaveActive(Player* bot, AiObjectContext* context);
}

std::vector<uint32> const& HallsOfStoneWaveEntries()
{
    static std::vector<uint32> const entries(std::begin(kWaveEntries), std::end(kWaveEntries));
    return entries;
}

std::vector<uint32> const& HallsOfStoneHeadEntries()
{
    static std::vector<uint32> const entries(std::begin(kHeadEntries), std::end(kHeadEntries));
    return entries;
}

void RegisterHallsOfStoneEvents(std::vector<DungeonEvent>& out)
{
    // (1) ESCORT BRANN BRONZEBEARD.
    //
    // Expressed entirely in typed primitives — no custom hook — because
    // EscortCreature already does the hard part. Its contract: "start its
    // scripted escort via gossip, then each tick FOLLOW it and actively ENGAGE
    // whatever attacks it (mod-playerbots gives a bot no threat event when only a
    // non-party escortee is hit, so aggro propagation alone never puts the tank
    // into the fight)." That is precisely Brann's failure mode: he walks 170yd
    // alone, REACT_AGGRESSIVE, with SetRegeneratingHealth(false), past seven
    // constructs, and his death restarts the whole escort.
    //
    // THE doneDataId ARGUMENT IS LOAD-BEARING AND IT IS NOT ONLY A COMPLETION
    // GATE. This is the part that did not survive contact with
    // DriveEscortCreature, so it is written out in full rather than left as a
    // number.
    //
    // The primitive has TWO gossip branches and, with a plain boss-entry gate,
    // NEITHER of them fires for Brann:
    //
    //   * The START branch is gated on `escortee->GetFaction() ==
    //     DC_ESCORT_IDLE_FACTION`, which is 35. That is the Wailing Caverns
    //     model, where an idle escortee sits at faction 35 until talked to.
    //     Brann is faction 1665 the whole way, so this branch is dead for him.
    //   * The RESUME branch — walk up to a paused, gossip-offering escortee and
    //     re-select the option — is gated on `step.instanceDataId >= 0`, i.e. it
    //     arms ONLY for an escort carrying an instance-data completion gate.
    //     With a boss-entry gate alone it is dead too.
    //
    // So a plain `.EscortCreature(NPC_BRANN, 0, NPC_KADDRAK, -1)` would find
    // Brann, follow him and defend him — and never click a single gossip. The
    // escort would never start, and the run would fail at the same closed door it
    // fails at today, having done more work to get there.
    //
    // GetData(BRANN_DOOR) is the fix, and it is an honest gate rather than a flag
    // smuggled in through an unused parameter. It is the one clean instance-data
    // reading on this map: brann_bronzebeard.cpp writes BOTH stores when the door
    // opens (SetBossState AND SetData), unlike BRANN_BRONZEBEARD which writes only
    // the boss state and so reads 0 forever. As a completion gate it says
    // something TRUE — "the escort is over once the door is already open" — which
    // is the right answer for a restart that finds the run further along than this
    // step is, and it can never fire early because the door is the LAST thing that
    // happens in the whole Brann sequence. Arming it then turns on exactly the
    // behaviour Brann needs, because his shape IS the resume model:
    //
    //     at his DB spawn      idle, GOSSIP flag up, menu 9669, standing still
    //                          -> resume branch selects option 0
    //                          -> ACTION_START_ESCORT_EVENT, path 280701 runs
    //     mid-walk             no gossip flag -> follow / threat-engage
    //     at the path end      PathEndReached re-raises the GOSSIP flag, sets
    //                          menu 9670, REACT_PASSIVE, standing still
    //                          -> resume branch selects option 0 again
    //                          -> ACTION_START_TRIBUNAL, InitializeEvent()
    //     if he dies mid-walk  despawn 5-10s, respawn at his DB spawn with menu
    //                          9669 -> back to the first line, and the event is
    //                          Persistent so the step list never rewinds
    //
    // One step, both clicks, and the mid-escort death case recovers for free.
    //
    // Both menus have exactly ONE option, so the POSITIONAL 0 is right for 9669 and
    // 9670 alike. Their raw OptionIDs are 37476 and 36142, and that distinction is
    // not cosmetic: GossipMenu keys its items by OptionID, so a positional argument
    // only works because SelectGossip translates it (see ResolveGossipListId). An
    // earlier revision of this comment claimed GetItem indexed by position and
    // passed 0 straight through to the protocol — every Brann gossip silently
    // no-opped and the dungeon could never start the escort.
    //
    // THE COMPLETION MARKER IS KADDRAK (30898), not "reached the end". The
    // primitive explicitly refuses to complete on arrival (the DM-West / RFD
    // premature-completion class of bug), and there is no usable data gate for the
    // escort itself — GetData(BRANN_BRONZEBEARD) is permanently 0 — so it takes a
    // live-creature marker. Kaddrak is summoned by InitializeEvent(), i.e. the
    // instant gossip 9670 lands. He is NOT_SELECTABLE and hovers ~14yd above the
    // arena floor, but FindNearestCreature is an entry+alive grid scan that tests
    // neither selectability nor Z, and its radius is 250yd against a 47yd gap. He
    // cannot read true early: nothing summons a head before gossip 9670.
    //
    // NO TIMEOUT on the escort step, deliberately: EscortCreature is
    // watchdog-owned (RunStep never escalates it on elapsed time; its own dead-air
    // watchdog owns liveness), so a number here would do nothing but mislead.
    out.push_back(
        EventBuilder(MAP, 1, "Escort Brann Bronzebeard")
            .Anchored(ORDER_ESCORT)
            .Persistent()        // spans several combat gaps; never rewind to step 1
            .StepsOwnMovement()  // the escort follow is the step's own glide
            .MoveTo(MEET_X, MEET_Y, MEET_Z, /*radius*/ 10.0f)
            // Brann's first forty yards, swept before he sets off. Entry-filtered
            // to the two construct types actually standing there so the sweep
            // cannot wander into the rest of the dungeon.
            .ClearRadius(PRECLEAR_X, PRECLEAR_Y, PRECLEAR_Z, PRECLEAR_R)
                .OnlyEntries({ NPC_RAGING_CONSTRUCT, NPC_UNRELENTING_CONSTRUCT })
                .Timeout(PRECLEAR_TIMEOUT_MS)
            .EscortCreature(NPC_BRANN, /*startGossipOption*/ 0,
                            /*doneEntry*/ NPC_KADDRAK, /*doneBit*/ -1,
                            ESCORT_STANDOFF, ESCORT_THREAT_R, ESCORT_THREAT_Z,
                            ESCORT_SEARCH_R,
                            /*doneDataId*/ static_cast<int32>(BRANN_DOOR),
                            /*doneDataMin*/ DONE)
            .Build());

    // (2) THE TRIBUNAL OF AGES — the missing encounter.
    //
    // The party garrisons the hold point for the fixed 300 seconds while the wave
    // event (4) does the fighting, then takes gossip 10206 to skip the 256s of
    // post-fight lore.
    //
    // PERSISTENT: from t ~ 52s the party is in continuous combat to t = 300s, and
    // every one of those is a >1s combat gap that would otherwise rewind a
    // non-persistent step list (dc-persistent-sticky-arms-at-step-1). A persistent
    // anchored event still obeys arriveRadius on step 1, hence the lenient 10yd.
    //
    // STEPS OWN MOVEMENT: hook 22 garrisons the hold point itself, and the
    // at-objective hold would cancel its glide on the very next tick (the Old
    // Hillsbrad barrel trap).
    //
    // WaitTargetStill() on the gossip is not decoration: at t ~ 317s Brann is
    // WALKING from the console to the lore stop and only gains the gossip flag on
    // arrival (POINT_TRIBUNAL_LORE). Talking to a moving escortee is at best a
    // no-op. The party is already standing 3.7yd away when he gets there — the
    // hold point and the lore stop sit on the same line — so this costs no travel.
    out.push_back(
        EventBuilder(MAP, 2, "The Tribunal of Ages")
            .Anchored(ORDER_TRIBUNAL)
            .Persistent()
            .StepsOwnMovement()
            .MoveTo(HOLD_X, HOLD_Y, HOLD_Z, /*radius*/ 10.0f)
            .Custom(HOOK_TRIBUNAL)
                .Timeout(TRIBUNAL_TIMEOUT_MS)
            .Gossip(NPC_BRANN, /*option*/ 0)   // menu 10206 — skip the lore
                .WaitTargetStill()
                .Timeout(LORE_SKIP_TIMEOUT_MS)
            .Build());

    // (3) OPEN THE WAY TO SJONNIR.
    //
    // Walk to the door stage, take gossip 10012, then VERIFY the door actually
    // opened. The WaitForGOState is the house rule that a Gossip/UseGO is always
    // followed by a verification step, and here it also absorbs the scripted 3.2s
    // between Brann arriving at POINT_SJONNIR_DOOR and SetData(BRANN_DOOR, DONE)
    // firing. GO_STATE_ACTIVE (0) is OPEN; the door spawns GO_STATE_READY (1).
    //
    // The step order matters and is not interchangeable. Brann despawns for ~5s
    // between the lore stop and the door, and RESPAWNS WITH A DIFFERENT GUID (the
    // script says so: "Sniff reveals different GUID, same entry"), so nothing here
    // may cache a GUID — the Gossip step re-resolves by entry every tick, which is
    // exactly right. Gating the gossip behind the MoveTo also means the party is
    // 300yd east before it looks for him, long after the old Brann is gone, so it
    // can never talk to the copy that is still walking off to despawn.
    out.push_back(
        EventBuilder(MAP, 3, "Open the way to Sjonnir")
            .Anchored(ORDER_DOOR)
            .Persistent()
            .StepsOwnMovement()
            .MoveTo(DOOR_STAGE_X, DOOR_STAGE_Y, DOOR_STAGE_Z, /*radius*/ 10.0f)
            .Gossip(NPC_BRANN, /*option*/ 0)   // menu 10012 — "We're with you Brann! Open it!"
                .WaitTargetStill()
                .Timeout(DOOR_GOSSIP_TIMEOUT_MS)
            .WaitForGOState(GO_SJONNIR_DOOR, GO_STATE_ACTIVE, DOOR_STATE_TIMEOUT_MS,
                            /*searchRadius*/ 60.0f)
            .Build());

    // (4) REPEL THE TRIBUNAL WAVE — the wave driver.
    //
    // ONE Custom step, not a step list. A list can only say "do these in order and
    // block on each"; this encounter needs a standing PREFERENCE re-evaluated
    // every tick as adds arrive from three spawn points, hazards land under the
    // party, and Brann's health falls. See HosDriveWave (HallsOfStoneDriver.cpp)
    // for the rungs.
    //
    // DRIVES IN COMBAT — the flag this encounter lives or dies on. JustSummoned
    // calls SetInCombatWithZone() on EVERY add, so from t ~ 52s the party is in
    // continuous combat to t = 300s with no gaps at all. The ordinary conditional
    // rung stands down on bot->IsInCombat(); without this flag the driver would
    // run only in gaps that do not exist, and the tank would hold wherever its
    // last fight ended — off the intercept line, while the next wave walked past
    // it to Brann. Same lesson as the Black Morass ({269,4}) and Drak'Tharon
    // ({600,1}).
    //
    // STEPS OWN MOVEMENT — the driver issues its own splines back to the hold
    // point and out to adds; without the flag ResolveEscortConflict / StopBot
    // cancels each one on the tick after it is issued.
    //
    // REPEATABLE (roughly nine waves over the 300s; the condition going false is
    // the only "done") + OPTIONAL (a timed-out step SKIPS and the repeat re-fires
    // fresh, so a wipe or a corpse run never hard-stalls the run for a human).
    out.push_back(
        EventBuilder(MAP, 4, "Repel the Tribunal wave")
            .Conditional(&HosTribunalWaveActive)
            .Repeatable()
            .Optional()
            .DrivesInCombat()
            .StepsOwnMovement()
            .Custom(HOOK_WAVE)
                .Timeout(WAVE_TIMEOUT_MS)
            .Build());
}

// --- the wave gate (event 4, repeatable) ----------------------------------
//
// DUE while the Tribunal is running. Two probes, in this order:
//
//   1. THE THREE HEADS. They are summoned by InitializeEvent() and despawned by
//      EndTribunalFight()/ResetEvent(), so their mere existence IS "the fight is
//      on" — and it is the only probe that reads true during the QUIET FIRST 52
//      SECONDS, before the first Dark Rune Protector spawns. Arming the driver in
//      that window is what puts the party on the intercept line before the first
//      wave rather than after it. Same trick the Violet Hold predicate uses with
//      the Teleportation Portal.
//   2. THE ADDS, as a backstop for the tail: EndTribunalFight despawns the heads
//      and the summons together, but a stray survivor keeps the driver in charge
//      for the sweep rather than handing the tick to a garrison that would walk
//      the tank home with a Stormcaller still up.
//
// GRID SCANS, NOT THE SPAWN STORE: every one of these is a TempSummon with
// spawnId 0 (the Arcatraz Skyriss precedent). Early-exits on the first hit.
//
// The proximity gate keeps the event not-due for a leader outside the arena — a
// corpse run, or the party still back at Brann's DB spawn 183.5yd away where the
// escort still owns the tick. Required by
// ConditionalEventsWithoutArrivalStepAreProximityVetted.
namespace
{
    bool HosTribunalWaveActive(Player* bot, AiObjectContext* /*context*/)
    {
        if (!bot || bot->GetMapId() != MAP)
            return false;
        if (bot->GetExactDist2d(ARENA_X, ARENA_Y) > EVENT_DUE_RANGE)
            return false;

        for (uint32 head : HallsOfStoneHeadEntries())
            if (bot->FindNearestCreature(head, ARENA_SCAN, /*alive*/ true))
                return true;

        for (uint32 add : HallsOfStoneWaveEntries())
            if (bot->FindNearestCreature(add, ARENA_SCAN, /*alive*/ true))
                return true;

        return false;
    }
}

// --- roster patch: three objectives, no removals --------------------------
//
// NOTHING IS REMOVED. Unlike the Violet Hold — whose two derived anchors sit
// inside sealed cells and name the wrong bosses — all three of map 599's derived
// bosses are correct, reachable and correctly positioned. They only need
// re-ordering onto one contiguous scale so the three new objectives have
// somewhere to sit between the Maiden and Sjonnir.
//
// The bosses' RELATIVE order is untouched: bits 0/1/3 already sort Krystallus ->
// Maiden -> Sjonnir along the travel path, and `reorder` moves the clear sequence
// only — every kill-bit is left exactly as the DBC has it.
//
// One patch, gate Any. Heroic changes stat templates and three add cadences and
// nothing else structural, so the roster shape is identical on both difficulties.
//
// Consequence, recorded so it is not read as a regression: the boss panel now
// shows SIX rows where it showed three, and `dc bosses` lists the Tribunal as an
// objective rather than a boss. That is correct — it is not a creature.
void RegisterHallsOfStoneRoster(std::vector<BossRosterPatch>& t)
{
    using namespace DcRoster;

    BossRosterPatch p;
    p.mapId = MAP;
    p.add = {
        MakeObjective(OBJ(1), /*encounterIndex*/ 1, MAP, "Escort Brann Bronzebeard",
                      MEET_X, MEET_Y, MEET_Z, /*arriveRadius*/ 10.0f,
                      /*gateEntry*/ 0, /*hook*/ 0, /*eventId*/ 1,
                      /*orderOverride*/ ORDER_ESCORT),
        MakeObjective(OBJ(2), /*encounterIndex*/ 2, MAP, "The Tribunal of Ages",
                      HOLD_X, HOLD_Y, HOLD_Z, /*arriveRadius*/ 10.0f,
                      /*gateEntry*/ 0, /*hook*/ 0, /*eventId*/ 2,
                      /*orderOverride*/ ORDER_TRIBUNAL),
        MakeObjective(OBJ(3), /*encounterIndex*/ 3, MAP, "Open the way to Sjonnir",
                      DOOR_STAGE_X, DOOR_STAGE_Y, DOOR_STAGE_Z, /*arriveRadius*/ 10.0f,
                      /*gateEntry*/ 0, /*hook*/ 0, /*eventId*/ 3,
                      /*orderOverride*/ ORDER_DOOR),
    };
    p.reorder = {
        { NPC_KRYSTALLUS,      ORDER_KRYSTALLUS },
        { NPC_MAIDEN_OF_GRIEF, ORDER_MAIDEN     },
        { NPC_SJONNIR,         ORDER_SJONNIR    },
    };
    t.push_back(std::move(p));
}
