/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"
#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonRosterBuilders.h"

#include <vector>

#include "InstanceScript.h"
#include "Player.h"

#include "Ai/Dungeon/DungeonClear/Util/DcTargeting.h"

// --- The Culling of Stratholme (map 595) -----------------------------------
//
// The constants, and the reasoning behind every number in them, live in
// namespace DcCullingOfStratholme in DungeonEventTables.h. This file is the
// authored shape: nine events and two roster patches.
//
// THE SHAPE OF THE DUNGEON, in the order the party meets it:
//
//   1. Talk to Chromie at the entrance, then walk 330 yards of road using the
//      Arcane Disruptor on five plagued grain crates. The fifth sets the counter
//      to CRATES_FOUND and, twenty seconds later, summons a second Chromie 430
//      yards up the road.
//   2. Talk to her. That sets START_INTRO, which is what tells Arthas to begin.
//   3. Follow him through three minutes of Uther/Jaina dialogue to the city gate,
//      talk to him there, and follow the 85-second city intro to his stop at
//      waypoint 11. Twenty seconds after THAT, the waves begin.
//   4. Ten waves of four, at four clusters up to 264 yards apart, with Meathook
//      as wave 5 and Salramm as wave 10. Arthas does not move for any of it; the
//      mobs have no movement script. The party has to go to them.
//   5. Talk to Arthas at the Town Hall door (twice — the first click re-sends the
//      menu), fight three Infinite citizens and five time-rift sets, kill
//      Chrono-Lord Epoch.
//   6. Talk to him again; the bookcase opens; follow him down the secret passage
//      and through the Fire Street gauntlet to his stop at the edge of the Market.
//   7. (HEROIC) Detour 143 yards to the Infinite Corruptor, who has been standing
//      there on a 26-minute clock since wave 1, and kill him.
//   8. Talk to Arthas one last time and kill Mal'ganis.
//
// WHY THE ESCORT IS SPLIT INTO FOUR LEGS AND NOT ONE. Old Hillsbrad's Thrall is
// one EscortCreature step from his cell to Epoch Hunter's death, and that works
// because nothing interrupts it. Here two things do.
//
// THE WAVES force the first split. Arthas sits at waypoint 11 doing nothing for
// ten to fifteen minutes while the party fights somewhere else entirely, and an
// escort step across that window would spend the whole of it reporting that it
// cannot keep up with a creature that is not moving. So leg 1 ends at
// FINISHED_CITY_INTRO, the waves happen, and leg 2 picks him up again.
//
// THE HEROIC CORRUPTOR forces the last one. He stands in the Market district, 143
// yards from Arthas's waypoint-54 stop and 927 from Market Row on foot (the city
// does not connect the two) — so the only place the party can reach him without
// walking the dungeon twice is that stop, where Arthas waits indefinitely for a
// gossip. Ending a leg at BEFORE_MALGANIS puts an objective boundary exactly
// there, which the heroic detour then slots into. On a normal run the two legs
// simply run back to back: leg 3 completes at BEFORE_MALGANIS and leg 4 resumes
// the very gossip it stopped short of, with the party already standing on its
// anchor.
//
// The Epoch split in the middle is a free extra: it gives the panel a row, the
// test record an objective, and a rewind a clean boundary.
//
// WHY THERE IS NO KillCreature STEP FOR ANY OF THE FOUR BOSSES. All four attack
// Arthas or are attacked BY him — Epoch and Mal'ganis get an explicit AddThreat(me)
// plus SetInCombatWithZone, and the wave bosses spawn into a fight already in
// progress — so the escort driver's threat pick and the ordinary combat engine
// kill them. A KillCreature gate on top would add a second opinion about what the
// tank should be hitting during a scripted fight.

namespace
{
    using namespace DcCullingOfStratholme;

    // --- event 10's gate: the wave phase is ours --------------------------
    //
    // DUE for exactly the window between the end of the city intro and Salramm's
    // death: states 4 (FINISHED_CITY_INTRO, waves 1-5) and 5 (KILLED_MEATHOOK,
    // waves 6-10). It closes for good at 6, which is what hands the city back to
    // the ordinary clear so the Town Hall objective can take over.
    //
    // WHY A COUNTER TEST IS A STRONG ENOUGH NEAR-GATE to stand in
    // t/TestEventRegistry.cpp's hand-vetted list. The counter can only read 4
    // because the party performed, in order: Chromie's entrance gossip, five crate
    // uses, Chromie-middle's gossip, a three-minute escort to the city gate, a
    // gossip there, and an 85-second city intro that ends with Arthas parked at
    // waypoint 11 — which is ninety yards from the first wave cluster. There is no
    // path to that value that leaves the party anywhere but in the city, and none
    // of it can happen by accident or from a distance. (Contrast a creature-
    // presence probe, which is what the near-gate rule exists for: three of the
    // wave entries have static spawns on this map and would read true from the
    // instance entrance.)
    //
    // NOT GATED ON COMBAT, and on this map that is not a judgement call. There is
    // NO inter-wave timer at all: npc_arthasAI::SendNextWave summons the next four
    // mobs in the same call as the fourth death, so the party is in combat
    // essentially without a break from wave 1 to Salramm. A predicate that waited
    // for combat to drop would arm the driver for the 20 seconds before wave 1 and
    // then never again — and would stop arming altogether the moment the party
    // fell behind, which is exactly when it is needed.
    bool CosWavesDue(Player* bot, AiObjectContext* /*context*/)
    {
        if (!bot || bot->GetMapId() != MAP_ID)
            return false;

        InstanceScript* inst = DcTargeting::GetInstanceScript(bot);
        if (!inst)
            return false;

        uint32 const progress = inst->GetData(DATA_ARTHAS_EVENT);
        return progress == PROGRESS_FINISHED_CITY_INTRO ||
               progress == PROGRESS_KILLED_MEATHOOK;
    }
}

std::vector<uint32> const& CosWaveEntries()
{
    // Deliberately a function rather than a constexpr array: it is shared with
    // the driver's grid scan, which wants a std::vector<uint32> by reference.
    static std::vector<uint32> const kEntries = {
        NPC_RISEN_ZOMBIE,   NPC_DEVOURING_GHOUL,     NPC_DARK_NECROMANCER,
        NPC_TOMB_STALKER,   NPC_CRYPT_FIEND,         NPC_BILE_GOLEM,
        NPC_ENRAGING_GHOUL, NPC_PATCHWORK_CONSTRUCT,
        // The two wave bosses. They belong in the SAME list as the trash, because
        // to the driver they are simply wave 5 and wave 10: they spawn at a cluster
        // with no movement script, they have to be walked to, and their death is
        // what summons the next wave.
        NPC_MEATHOOK, NPC_SALRAMM,
    };
    return kEntries;
}

void RegisterCullingOfStratholmeEvents(std::vector<DungeonEvent>& out)
{
    // (1) CHROMIE AND THE PLAGUED GRAIN — anchored on OBJ(1).
    //
    // The gossip first, for fidelity and for the crate counter's world state, and
    // because on a server where it works it is also how the Arcane Disruptor is
    // meant to be acquired: menu 9586 -> 9594 -> 9595, where the select casts
    // 49591 and grants item 37888. SelectGossip's submenu drill-down walks that
    // chain on its own (every menu on it has exactly one option, and the only
    // second option anywhere — 9586's "skip us ahead" — is condition-gated on
    // quest 13151 REWARDED, which a bot never has, so it is not even in the menu).
    //
    // SkipIfTargetMissing because the whole event must not deadlock on her: the
    // crate steps grant the item themselves, so a dead or missing Chromie costs
    // nothing but her dialogue.
    //
    // THEN THE FIVE CRATES, in road order. Each step walks to its crate, waits out
    // the Disruptor's 10-second cooldown, uses it, and latches on the Plagued
    // Grain Crate the helper's SpellHit leaves behind. Order does not matter to
    // the instance — any five distinct helpers count — so this is simply the walk,
    // shortest-leg-first from Chromie.
    //
    // PERSISTENT: the event spans 330 yards and several minutes, and its steps
    // deliberately walk the tank far outside the objective's arrive radius. Both
    // are things a non-persistent event's rewind would undo, and a rewind here
    // re-walks crates that are already done (harmlessly — the receipt latch is
    // idempotent — but for nothing).
    //
    // AND A LEADING MoveTo, which is Old Hillsbrad's step 0 verbatim and for its
    // reason. A persistent anchored event only goes STICKY — letting the tank roam
    // far from its anchor while the event drives — once stepIndex >= 1 OR its FRONT
    // step is a MoveTo. The crate steps walk the tank 330 yards away, so the latch
    // has to be engaged before the first of them; with a Gossip in front it would
    // not engage until that gossip had landed, leaving one tick in which a tank
    // already drifting off the anchor could close the at-objective gate on itself.
    // The MoveTo costs nothing — the anchor is Chromie's own position, so the tank
    // is standing on it — and it removes the window.
    {
        EventBuilder b(MAP_ID, EVENT_CRATES, "Chromie and the plagued grain");
        b.Anchored(ORDER_CRATES)
            .Persistent()
            .MoveTo(CHROMIE_X, CHROMIE_Y, CHROMIE_Z, CHROMIE_ARRIVE)
            .Gossip(NPC_CHROMIE_START, /*option*/ 0, /*searchRadius*/ 20.0f)
                .SkipIfTargetMissing();
        for (CratePos const& c : CRATES)
            b.UseItemAt(ITEM_ARCANE_DISRUPTOR, SPELL_ARCANE_DISRUPTION, GO_PLAGUED_CRATE,
                        c.x, c.y, c.z)
                .Timeout(CRATE_TIMEOUT_MS);
        out.push_back(b.Build());
    }

    // (2) CHROMIE: START THE ROYAL ESCORT — anchored on OBJ(2).
    //
    // TWO STEPS, AND THE FIRST ONE IS THE CRATE COUNT'S OWN AUDIT. Chromie-middle
    // does not exist until the instance summons her twenty seconds after the FIFTH
    // crate, so waiting for her is a direct test of "did all five land" — the Old
    // Hillsbrad pattern, where waiting for Lieutenant Drake is what makes a
    // double-hit barrel fail loudly instead of silently. The anchor sits on her
    // summon position, so the 250yd scan cannot resolve anything else.
    //
    // Then the gossip: 9610 -> 9611 -> 9612, where the select sets START_INTRO and
    // the instance DoAction(ACTION_START_EVENT)s Arthas. WaitTargetStill because
    // she is summoned and could still be settling; her script refuses the select
    // outright while the counter is not CRATES_FOUND, so a too-early click is a
    // silent no-op rather than an error.
    out.push_back(
        EventBuilder(MAP_ID, EVENT_START_RP, "Chromie: start the Royal Escort")
            .Anchored(ORDER_START_RP)
            .Persistent()
            .WaitForSpawn(NPC_CHROMIE_MIDDLE, /*wantAlive*/ true, CHROMIE_MID_TIMEOUT_MS)
            .Gossip(NPC_CHROMIE_MIDDLE, /*option*/ 0, /*searchRadius*/ 10.0f)
                .WaitTargetStill()
            .Build());

    // (3) ARTHAS: THE BRIDGE AND THE CITY GATE — anchored on OBJ(3).
    //
    // ONE escort step behind a leading MoveTo, gated on the counter reaching
    // FINISHED_CITY_INTRO (4).
    //
    // THE LEADING MoveTo IS NOT DECORATION. A persistent anchored event only goes
    // STICKY — letting the tank roam far from its anchor while the event drives —
    // once it has started, and "started" is stepIndex >= 1 OR a leading MoveTo.
    // An escort step is neither: it never advances its index until the whole
    // escort is over. Without this step the at-objective trigger would go false
    // the moment the escort walked the tank past BRIDGE_ARRIVE, Advance would haul
    // him back to the anchor, and the pull system would never stand down.
    // (tp-20260907-140341-2 is the same deadlock one step further out, on Pit of
    // Saron's ledge.) Its radius matches the anchor's so arrival and step 0 agree.
    //
    // THE DRIVER'S RESUME BRANCH IS THE WHOLE MECHANISM. Arthas keeps faction 2076
    // throughout, so the START branch (faction 35) never fires; instead he raises
    // UNIT_NPC_FLAG_GOSSIP at waypoint 8 with the counter at FINISHED_INTRO, the
    // driver walks to 5 yards and selects option 0 ("Yes, my Prince. We are
    // ready."), and ACTION_START_CITY takes him on. Thrall's model verbatim.
    //
    // threatR 20, the tightest of the three legs, for the COSMETIC Mal'ganis: the
    // city intro summons one 27 yards from Arthas's stop and he is visible for a
    // minute. He is IMMUNE_TO_PC|IMMUNE_TO_NPC and REACT_PASSIVE by template, so
    // the threat pick rejects him anyway — this is belt and braces on an
    // encounter-shaped creature standing next to a party with nothing else to do.
    //
    // THERE IS NO COMBAT ON THIS LEG AT ALL, which is worth stating because it
    // looks like a dead-air risk: three minutes of Uther/Jaina dialogue and then
    // 85 seconds of city intro, with the party parked in Arthas's follow slot. The
    // escort watchdog is DISTANCE-based (it only fires when the tank cannot keep
    // up), so standing still next to a standing escortee never trips it.
    out.push_back(
        EventBuilder(MAP_ID, EVENT_CITY_GATE, "Arthas: the bridge and the city gate")
            .Anchored(ORDER_CITY_GATE)
            .Persistent()
            .MoveTo(BRIDGE_X, BRIDGE_Y, BRIDGE_Z, BRIDGE_ARRIVE)
            .EscortCreature(NPC_ARTHAS, /*startGossipOption*/ 0,
                            /*doneEntry*/ 0, /*doneBit*/ -1,
                            ESCORT_STANDOFF, ESCORT_THREAT_CITY, ESCORT_Z_BAND,
                            ESCORT_SEARCH,
                            /*doneDataId*/ static_cast<int32>(DATA_ARTHAS_EVENT),
                            /*doneDataMin*/ PROGRESS_FINISHED_CITY_INTRO)
            .Build());

    // (4) and (5) WAVES 1-5 / MEATHOOK and WAVES 6-10 / SALRAMM — OBJ(4), OBJ(5).
    //
    // BOOKKEEPING, NOT NAVIGATION. Event 10 does the fighting; these two exist so
    // the panel has a row per wave boss and the test record has an objective per
    // wave boss, at zero mechanical cost.
    //
    // They are zero-cost because of WHERE they are anchored. Market Row is where
    // both bosses spawn, so it is where the party is standing when each of them
    // dies — and while event 10 is due it owns every tick, so these holds only get
    // to run once the counter has already passed them. Each then latches on its
    // first tick without moving anybody.
    //
    // The gate is the COUNTER and not a boss's death, because the party is in
    // continuous combat for the whole phase: a "boss alive" probe would have to
    // catch a window the event engine is asleep for, while a monotonic counter can
    // be read late and still be right.
    out.push_back(
        EventBuilder(MAP_ID, EVENT_MEATHOOK, "Waves 1-5 and Meathook")
            .Anchored(ORDER_MEATHOOK)
            .Persistent()
            .MoveToHoldUntilInstanceData(MARKET_ROW_X, MARKET_ROW_Y, MARKET_ROW_Z,
                                         /*radius*/ 8.0f,
                                         DATA_ARTHAS_EVENT, PROGRESS_KILLED_MEATHOOK)
                .Timeout(WAVE_HOLD_TIMEOUT_MS)
            .Build());

    out.push_back(
        EventBuilder(MAP_ID, EVENT_SALRAMM, "Waves 6-10 and Salramm the Fleshcrafter")
            .Anchored(ORDER_SALRAMM)
            .Persistent()
            .MoveToHoldUntilInstanceData(MARKET_ROW_X, MARKET_ROW_Y, MARKET_ROW_Z,
                                         /*radius*/ 8.0f,
                                         DATA_ARTHAS_EVENT, PROGRESS_KILLED_SALRAMM)
                .Timeout(WAVE_HOLD_TIMEOUT_MS)
            .Build());

    // (6) ARTHAS: THE TOWN HALL AND CHRONO-LORD EPOCH — anchored on OBJ(6).
    //
    // MEET HIM ON THE ROAD. HE DOES NOT WAIT, AND HE DOES NOT SET OFF WITHOUT YOU.
    //
    // Both halves of that sentence are load-bearing, and each was learned from a
    // 0/10 plan. See the TOWN_HALL_MEET_* note in DungeonEventTables.h for the full
    // account; the short version is:
    //
    //   - He is a DB spawn, so he only ticks while a player is within the 170yd
    //     instance grid activation range. Garrisoning at the Town Hall door left
    //     him asleep 265yd away and the counter never moved (tp-20260909-224741-1).
    //   - He resumes on a 10s timer after Salramm, not on being collected. Anchoring
    //     at WP12, his wave stop, meant the party's approach woke him and he ran
    //     east past them while they walked west to where he had been
    //     (tp-20260910-073710-1).
    //
    // WP16 is the meeting point: near enough to WP11 (137yd) that arriving there
    // wakes him, and near enough to the Town Hall door (156yd) that his entire
    // WP12..WP20 run fits inside ESCORT_SEARCH_TOWN_HALL. Whatever head start the
    // party's loot and rest stops handed him, the escort step finds him and walks
    // the rest of the way at his shoulder — which is also what keeps his cell
    // marked for the remainder of the run east.
    //
    // THE LEADING MoveTo IS NOT DECORATION, here or on any of the other three legs.
    // A Persistent anchored event only goes sticky — which is what lets the escort
    // walk the tank far from the anchor without Advance hauling him back — once its
    // front step is a MoveTo. It costs nothing: the objective's own arrive gate has
    // already walked the party to this point before the event is allowed to start,
    // so the step latches on its first tick.
    //
    // NO FLAT TIMEOUT ON THE ESCORT, by design: EscortCreature is watchdogOwned in
    // the executor, so DriveEscortCreature's distance-based dead-air clock owns
    // liveness instead. That matters on this leg because Epoch's 32-second immunity
    // and the time-rift fights are long stretches where nothing looks like progress.
    //
    // THE RESUME CLICK HERE IS TWO CLICKS, and the driver handles it only since
    // the drill-down fix. At REACHED_TOWN_HALL Arthas offers menu 13125; selecting
    // its one option runs GOSSIP_ACTION_INFO_DEF+2, which does NOT clear his gossip
    // flag and instead re-sends menu 13126 — and only THAT menu's option fires
    // ACTION_START_TOWN_HALL. Both his menus are built in C++, so they share a
    // GossipMenu id, and SelectGossip's old menu-id-only "did a submenu open?" test
    // bailed after click 1 — the flag survived and the resume branch re-clicked
    // menu 13125 for the whole run (tp-20260910-083410-1, 10/10). It now compares
    // the option's action too, so both clicks go out in the same call. Everything
    // before WP20 is a pure follow:
    // the driver's resume branch is gated on HasNpcFlag(UNIT_NPC_FLAG_GOSSIP) and
    // he does not raise it until he stops there, so no gossip is attempted mid-walk
    // — including on the stretch he covers before the party reaches WP16.
    //
    // threatR 40, the widest of the three legs, because this is the leg with
    // something to reach. Five time-rift sets are summoned at three of his
    // waypoints, every mob SetInCombatWithZone'd and given threat on him, and
    // Chrono-Lord Epoch is summoned 40 yards out and walks to about 30. His script
    // holds each waypoint until he is out of combat, so the party's pace is
    // self-synchronising; the radius just has to reach what is attacking him.
    //
    // Epoch's 32-second immunity needs no special handling: IsValidAttackTarget is
    // false for an IMMUNE_TO_PC creature, so the threat pick passes over him and
    // the tank holds the slot until he flips.
    out.push_back(
        EventBuilder(MAP_ID, EVENT_TOWN_HALL, "Arthas: the Town Hall and Chrono-Lord Epoch")
            .Anchored(ORDER_TOWN_HALL)
            .Persistent()
            .MoveTo(TOWN_HALL_MEET_X, TOWN_HALL_MEET_Y, TOWN_HALL_MEET_Z,
                    TOWN_HALL_MEET_ARRIVE)
            .EscortCreature(NPC_ARTHAS, /*startGossipOption*/ 0,
                            /*doneEntry*/ 0, /*doneBit*/ static_cast<int32>(BIT_EPOCH),
                            ESCORT_STANDOFF, ESCORT_THREAT_TOWN_HALL, ESCORT_Z_BAND,
                            ESCORT_SEARCH_TOWN_HALL,
                            /*doneDataId*/ static_cast<int32>(DATA_ARTHAS_EVENT),
                            /*doneDataMin*/ PROGRESS_KILLED_EPOCH)
            .Build());

    // (7) ARTHAS: THE SECRET PASSAGE AND FIRE STREET — anchored on OBJ(7).
    //
    // ONE escort step behind a leading MoveTo, covering TWO resume clicks and
    // everything between them: "I'm ready." at waypoint 31 (which walks him to the
    // bookcase and opens it) and "For Lordaeron!" at waypoint 45 at the bottom of
    // the passage. It ends where he stops at the edge of the Market with his gossip
    // up, which is PROGRESS_BEFORE_MALGANIS.
    //
    // WHY IT ENDS THERE RATHER THAN RUNNING ON INTO MAL'GANIS. The heroic Infinite
    // Corruptor is 143 yards from that stop and 927 from Market Row on foot, so
    // that stop is the ONLY place in the run he can be reached from — and Arthas
    // waits at it indefinitely, which makes a detour free. Ending a leg here puts
    // an objective boundary exactly where the detour has to go. On a normal run the
    // boundary costs nothing: leg 8 is absent, and leg 9's leading MoveTo is
    // already satisfied by the party standing where this leg left them.
    //
    // FIRE STREET IS THE PRESSURE ON THIS LEG and it is static, not summoned:
    // roughly 62 Risen Zombies, 7 Enraging Ghouls, 2 Crypt Fiends, a Patchwork
    // Construct, a Bile Golem and 3 Acolytes line the run from waypoint 46 to 53,
    // and ARTHAS is what they aggro. His escort AI will not advance a waypoint
    // while he has a victim, so he fights his way down at the party's pace rather
    // than outrunning them — which is why threatR 30 (wider than the city leg,
    // tighter than the Town Hall's) is enough: what matters is reaching his
    // attackers, and they are on him.
    //
    // His own survivability is the reason this is not a wipe: HealthModifier 3.5
    // (5.0 on heroic) and a self-heal below 40%, plus the healer, who gets him for
    // free — DcLeaderSignal::GetLeaderEscortee folds the leader's live escortee
    // into "party member to heal" whenever the active step is an EscortCreature,
    // which is true for this leg and every other escort leg here.
    //
    // If he dies on Fire Street the rewind is cheap: his script respawns him at the
    // LAST_CITY checkpoint with the gossip re-armed and the counter untouched, and
    // the driver's resume branch clicks him again.
    out.push_back(
        EventBuilder(MAP_ID, EVENT_FIRE_STREET, "Arthas: the secret passage and Fire Street")
            .Anchored(ORDER_FIRE_STREET)
            .Persistent()
            .MoveTo(PASSAGE_X, PASSAGE_Y, PASSAGE_Z, PASSAGE_ARRIVE)
            .EscortCreature(NPC_ARTHAS, /*startGossipOption*/ 0,
                            /*doneEntry*/ 0, /*doneBit*/ -1,
                            ESCORT_STANDOFF, ESCORT_THREAT_LAST_CITY, ESCORT_Z_BAND,
                            ESCORT_SEARCH,
                            /*doneDataId*/ static_cast<int32>(DATA_ARTHAS_EVENT),
                            /*doneDataMin*/ PROGRESS_BEFORE_MALGANIS)
            .Build());

    // (8) THE INFINITE CORRUPTOR — anchored on OBJ(8), HEROIC ONLY.
    //
    // The bonus boss, and the one piece of this dungeon that is on a CLOCK. He is
    // summoned the instant DATA_START_WAVES fires on a heroic run — the same tick
    // wave 1 spawns — with a 26-minute timer; when it expires the instance
    // DoAction(ACTION_RUN_OUT_OF_TIME)s him and he despawns, and the bonus
    // (achievement "The Culling of Time") is gone for that run.
    //
    // WHY HE IS SEQUENCED HERE and nowhere else, and the straight-line distance is
    // the trap that makes it look otherwise. He is 81 yards from Market Row, where
    // the party stands when Salramm dies — and 927 yards from it ON FOOT. The
    // Market district is a separate component of this city's street graph and the
    // only way into it is the one the dungeon intends: the Town Hall, the secret
    // passage, Fire Street. Measured on the real mmtiles, the walk to him is 1223
    // yards from where the waves start, 1139 from King's Square, 1255 from Elders'
    // Square, 927 from Market Row, 379 from Fire Street — and 143 from Arthas's
    // waypoint-54 stop, which is where the objective before this one ends.
    //
    // AND NOTHING IS WAITING ON THE PARTY WHILE THEY DO IT. At
    // PROGRESS_BEFORE_MALGANIS Arthas stands at waypoint 54 with his gossip up and
    // waits INDEFINITELY for someone to talk to him, so the 286-yard round trip
    // costs the run nothing but the fight.
    //
    // THE CLOCK IS THE HONEST LIMITATION, stated here so nobody triages a skipped
    // bonus as a bug. Reaching waypoint 54 takes 21-28 minutes of the 26: 10-15 of
    // waves, about one to the Town Hall, 5-7 for the Town Hall and Epoch, and about
    // five for the passage and Fire Street. So this is a time attack — winnable on a
    // fast run, gone on a slow one — which is what the encounter is in the first
    // place.
    //
    // TWO STEPS, AND THE WAIT IS THE CLOCK HANDLER. The anchor IS his summon
    // position, so when he is there he is found on the first tick; the 20-second
    // budget is only ever spent in the other case, where the timer expired and the
    // instance despawned him. Because the event is OPTIONAL that timeout skips the
    // objective and the clear carries on to Mal'ganis — which is the correct outcome
    // for a bonus boss and the only thing that stops a missed clock from stalling a
    // run.
    //
    // He needs no never-target rows and no arming wait: faction 1720, unit_flags
    // 0x40 and nothing else — attackable from the moment he is summoned, unlike
    // Epoch (32 seconds of immunity) and Mal'ganis (7). ENGAGE rather than a bare
    // gate, because he does not move and a held party would wait for him for ever.
    out.push_back(
        EventBuilder(MAP_ID, EVENT_CORRUPTOR, "The Infinite Corruptor")
            .Anchored(ORDER_CORRUPTOR)
            .HeroicOnly()
            .Optional()
            .Persistent()
            .WaitForSpawn(NPC_INFINITE_CORRUPTOR, /*wantAlive*/ true,
                          CORRUPTOR_SPAWN_TIMEOUT_MS)
            .KillCreatureEngage(NPC_INFINITE_CORRUPTOR, /*count*/ 1, /*searchRadius*/ 120.0f)
                .Timeout(CORRUPTOR_KILL_TIMEOUT_MS)
            .Build());

    // (9) ARTHAS: MAL'GANIS — anchored on OBJ(9).
    //
    // The last resume click ("I'm ready to battle the dreadlord, sire." at waypoint
    // 54) and the fight it starts. On a normal run this leg begins the instant the
    // Fire Street leg ends, with the party already standing on its anchor, so the
    // split is invisible; on heroic it begins after the Corruptor detour.
    //
    // COMPLETION IS OR'd AND BOTH HALVES ARE REAL. Mal'ganis never dies: at lethal
    // damage his DamageTaken zeroes the damage, makes him immune and passive, casts
    // 58630 on himself — which IS the encounter credit, because instance_encounters
    // row 296/300 is a CAST_SPELL credit on that spell and it exists as a spell_dbc
    // row, so the bit really does flip — summons the chest, and evades. Arthas then
    // talks for about 27 seconds before the counter reaches FINISHED. So the BIT is
    // the early, precise signal and the COUNTER is the backstop; either completes
    // the step.
    //
    // threatR 30, the same as Fire Street: the gossip summons Mal'ganis immune and
    // NON_ATTACKABLE at the Market's north end, and seven seconds after Arthas
    // reaches waypoint 55 the script clears both flags, SetInCombatWithZone's him
    // and has Arthas open on him — so what the threat pick needs to reach is
    // whatever is on Arthas, and that is Mal'ganis himself.
    //
    // The chest (190663 normal / 193597 heroic) is summoned at (2288.35, 1498.73)
    // and is loot-only; nothing here has to click it.
    out.push_back(
        EventBuilder(MAP_ID, EVENT_MALGANIS, "Arthas: Mal'ganis")
            .Anchored(ORDER_MALGANIS)
            .Persistent()
            .MoveTo(MARKET_X, MARKET_Y, MARKET_Z, MARKET_ARRIVE)
            .EscortCreature(NPC_ARTHAS, /*startGossipOption*/ 0,
                            /*doneEntry*/ 0, /*doneBit*/ static_cast<int32>(BIT_MALGANIS),
                            ESCORT_STANDOFF, ESCORT_THREAT_LAST_CITY, ESCORT_Z_BAND,
                            ESCORT_SEARCH,
                            /*doneDataId*/ static_cast<int32>(DATA_ARTHAS_EVENT),
                            /*doneDataMin*/ PROGRESS_FINISHED)
            .Build());

    // (10) THE TEN WAVES — the conditional controller, and the event that makes the
    // middle third of this dungeon possible.
    //
    // ONE Custom step, for the Black Morass / Halls of Reflection reason: what this
    // phase needs is a standing PREFERENCE re-decided every tick — fight, travel,
    // pull, rest, hold — not a sequence. A step list can only say "do these in
    // order and block on each", and every one of those states can be interrupted
    // at any moment by four mobs appearing 260 yards away.
    //
    // DRIVES IN COMBAT. There is NO inter-wave timer on this map — the next wave is
    // summoned in the same call as the previous wave's fourth death — so the party
    // is in combat from wave 1 to Salramm with gaps measured in the seconds it
    // takes to walk to the next cluster. A rung that only ran out of combat would
    // get the twenty seconds before wave 1 and essentially nothing after.
    //
    // STEPS OWN MOVEMENT. The driver walks the tank up to 264 yards on its own
    // long-range spline, and the per-tick position hold runs BEFORE Drive — so
    // without this flag last tick's spline is cancelled before the hook can see
    // it and the tank creeps one tick of movement per cycle. It is also what makes
    // a Done return YIELD the tick, which on this map is the half that decides
    // fights: the driver has nothing to steer for the whole of every wave, and
    // every tick it claimed then would be a tick the tank did not swing.
    //
    // OWNS THE PULL. The advanced pull's Idle branch answers unplanned aggro by
    // walking a fresh camp BACK along the route until it finds ground clear of
    // hostiles, and this phase is one long unplanned aggro across four clusters —
    // so left armed it would drag the party away from the wave it is supposed to
    // be killing. Dropping the scout-lag with it is the other half: a tank fifteen
    // yards ahead of its party is the wrong shape for walking into a 4-pack.
    //
    // REPEATABLE. The phase is not a thing that completes once — the condition
    // going false at Salramm's death is what ends it, not a latch — and
    // repeatability is also what makes the step timeout a harmless RE-ARM instead
    // of a skip (see WAVES_TIMEOUT_MS).
    //
    // PERSISTENT so a combat gap cannot rewind the step list.
    //
    // PanelAfterBoss is deliberately NOT used, and neither is PanelBeforeBoss:
    // objectives 4 and 5 already give the waves two panel rows of their own, and
    // PanelBeforeBoss additionally keys DcTargeting::HasPendingSummonEvent, which
    // reads an unlatched gating event as "this boss must still be summoned" and
    // suppresses the dynamic pull near it — permanently, for an event that is
    // Repeatable and therefore never latched.
    out.push_back(
        EventBuilder(MAP_ID, EVENT_WAVES, "The ten waves of Stratholme")
            .Conditional(&CosWavesDue)
            .Repeatable()
            .Persistent()
            .Optional()
            .OwnsThePull()
            .DrivesInCombat()
            .StepsOwnMovement()
            .Custom(HOOK_COS_WAVES)
                .Timeout(WAVES_TIMEOUT_MS)
            .Build());
}

// --- the roster: eight objectives, no boss rows, plus the heroic ninth -----
//
// See the declaration in DungeonEventTables.h for why there are no MakeBoss /
// MakeBossWithBit rows on a map whose four DBC bits all exist and all get set.
//
// An objective's encounterIndex is an ORDERING HINT ONLY — objectives carry no
// kill bit and NextDungeonBossValue never tests the mask for one — so every row
// here leaves it at 0 and the clear orders purely by orderOverride. That matters
// more than usual on this map: if these were given the bosses' real bit indices,
// the completed-mask check would read an objective as already done the moment the
// corresponding boss died, which for objectives 4 and 5 is the same instant their
// own gate clears.
void RegisterCullingOfStratholmeRoster(std::vector<BossRosterPatch>& t)
{
    using namespace DcRoster;

    // The Any patch: the eight objectives both difficulties share. There is
    // nothing to `remove` (the derived list is empty) and nothing to `reorder`
    // (there are no derived rows to reorder).
    BossRosterPatch p;
    p.mapId = MAP_ID;
    p.add = {
        MakeObjective(OBJ(EVENT_CRATES), /*encounterIndex*/ 0, MAP_ID,
                      "Chromie and the plagued grain",
                      CHROMIE_X, CHROMIE_Y, CHROMIE_Z, CHROMIE_ARRIVE,
                      /*gateEntry*/ 0, /*hook*/ 0, /*eventId*/ EVENT_CRATES,
                      /*orderOverride*/ ORDER_CRATES),

        MakeObjective(OBJ(EVENT_START_RP), /*encounterIndex*/ 0, MAP_ID,
                      "Chromie: start the Royal Escort",
                      CHROMIE_MID_X, CHROMIE_MID_Y, CHROMIE_MID_Z, CHROMIE_MID_ARRIVE,
                      /*gateEntry*/ 0, /*hook*/ 0, /*eventId*/ EVENT_START_RP,
                      /*orderOverride*/ ORDER_START_RP),

        MakeObjective(OBJ(EVENT_CITY_GATE), /*encounterIndex*/ 0, MAP_ID,
                      "Arthas: the bridge and the city gate",
                      BRIDGE_X, BRIDGE_Y, BRIDGE_Z, BRIDGE_ARRIVE,
                      /*gateEntry*/ 0, /*hook*/ 0, /*eventId*/ EVENT_CITY_GATE,
                      /*orderOverride*/ ORDER_CITY_GATE),

        // Both wave objectives anchor on Market Row, where both bosses spawn and
        // where the party therefore already is when each gate clears.
        MakeObjective(OBJ(EVENT_MEATHOOK), /*encounterIndex*/ 0, MAP_ID,
                      "Waves 1-5 and Meathook",
                      MARKET_ROW_X, MARKET_ROW_Y, MARKET_ROW_Z, MARKET_ROW_ARRIVE,
                      /*gateEntry*/ 0, /*hook*/ 0, /*eventId*/ EVENT_MEATHOOK,
                      /*orderOverride*/ ORDER_MEATHOOK),

        MakeObjective(OBJ(EVENT_SALRAMM), /*encounterIndex*/ 0, MAP_ID,
                      "Waves 6-10 and Salramm the Fleshcrafter",
                      MARKET_ROW_X, MARKET_ROW_Y, MARKET_ROW_Z, MARKET_ROW_ARRIVE,
                      /*gateEntry*/ 0, /*hook*/ 0, /*eventId*/ EVENT_SALRAMM,
                      /*orderOverride*/ ORDER_SALRAMM),

        // Anchored at Arthas's WP16 — a point ON his run to the Town Hall, at
        // neither end of it. He is a DB spawn and stops being updated once every
        // player is more than 170yd away, so anchoring on the door froze him at
        // WP11 forever; and he resumes on a timer rather than on being collected,
        // so anchoring on WP12 sent the party to a spot he had already left.
        MakeObjective(OBJ(EVENT_TOWN_HALL), /*encounterIndex*/ 0, MAP_ID,
                      "Arthas: the Town Hall and Chrono-Lord Epoch",
                      TOWN_HALL_MEET_X, TOWN_HALL_MEET_Y, TOWN_HALL_MEET_Z,
                      TOWN_HALL_MEET_ARRIVE,
                      /*gateEntry*/ 0, /*hook*/ 0, /*eventId*/ EVENT_TOWN_HALL,
                      /*orderOverride*/ ORDER_TOWN_HALL),

        MakeObjective(OBJ(EVENT_FIRE_STREET), /*encounterIndex*/ 0, MAP_ID,
                      "Arthas: the secret passage and Fire Street",
                      PASSAGE_X, PASSAGE_Y, PASSAGE_Z, PASSAGE_ARRIVE,
                      /*gateEntry*/ 0, /*hook*/ 0, /*eventId*/ EVENT_FIRE_STREET,
                      /*orderOverride*/ ORDER_FIRE_STREET),

        // Anchored at Arthas's waypoint 54, where he stops with his gossip up at
        // BEFORE_MALGANIS — which is also where the previous leg leaves the party,
        // so on a normal run this objective is satisfied on arrival without anybody
        // moving. On heroic the Corruptor detour happens between the two.
        MakeObjective(OBJ(EVENT_MALGANIS), /*encounterIndex*/ 0, MAP_ID,
                      "Arthas: Mal'ganis",
                      MARKET_X, MARKET_Y, MARKET_Z, MARKET_ARRIVE,
                      /*gateEntry*/ 0, /*hook*/ 0, /*eventId*/ EVENT_MALGANIS,
                      /*orderOverride*/ ORDER_MALGANIS),
    };
    t.push_back(std::move(p));

    // The HeroicOnly patch: the Infinite Corruptor. A SECOND patch rather than a
    // gated row, because that is the shape BossRosterRegistry::Apply supports —
    // every patch whose gate matches the run's difficulty key is applied in
    // registration order — and it is what keeps his anchor off a normal roster
    // entirely. On normal he does not exist, so a row for him would be an
    // objective the party can never satisfy.
    //
    // orderOverride 8 slots him between the Fire Street leg (7) and Mal'ganis (9) —
    // the only boundary in the run he is reachable from (143yd, against 927 from
    // Market Row). On a normal run that key is simply absent, which is fine because
    // orderOverride is a sort key and nothing requires the sequence to be dense.
    BossRosterPatch heroic;
    heroic.mapId = MAP_ID;
    heroic.gate = DcDifficultyGate::HeroicOnly;
    heroic.add = {
        MakeObjective(OBJ(EVENT_CORRUPTOR), /*encounterIndex*/ 0, MAP_ID,
                      "The Infinite Corruptor",
                      CORRUPTOR_X, CORRUPTOR_Y, CORRUPTOR_Z, CORRUPTOR_ARRIVE,
                      /*gateEntry*/ 0, /*hook*/ 0, /*eventId*/ EVENT_CORRUPTOR,
                      /*orderOverride*/ ORDER_CORRUPTOR),
    };
    t.push_back(std::move(heroic));
}
