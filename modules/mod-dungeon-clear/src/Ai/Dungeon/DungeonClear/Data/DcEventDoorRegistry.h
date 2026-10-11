/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCEVENTDOORREGISTRY_H
#define _PLAYERBOT_DCEVENTDOORREGISTRY_H

#include "Common.h"

// Per-ENTRY list of door gameobjects that are SCRIPT-ONLY: the live client
// refuses a direct player open and ONLY an in-game event opens them, even though
// their template (an empty lock-85, the same template as plenty of plainly
// clickable doors) reads as openable to BotCanOpenDoorLikePlayer / DcDoorPolicy.
// A bot generic-Use()ing one of these toggles the server GO state while the
// client still treats the door as shut — a desync — and it also skips the
// intended event (e.g. Shadowfang Keep's courtyard door, which only opens when a
// freed prisoner walks over and unlocks it).
//
// This is DELIBERATELY keyed by GO ENTRY, not by lock id: lock 85 is shared with
// many doors bots SHOULD open (Deadmines Factory/Foundry/Mast Room, etc.), so a
// lock-level rule would break them. Keep this list to doors verified to be
// script/event-opened only; the door-blocked action consults it before deciding
// it is "entitled" to open a door, and leaves a listed door for the events
// framework or the human instead.
namespace DcEventDoorRegistry
{
    inline bool IsScriptOnly(uint32 goEntry)
    {
        switch (goEntry)
        {
            case 18895:  // Shadowfang Keep — Courtyard Door (freed-prisoner event)
            // Shadowfang Keep's other two gates, both empty-lock-85 like the
            // Courtyard Door and both driven purely by SmartAI:
            //
            //   18972 Sorcerer's Gate (guid 33785) — the Fenrus room's east exit
            //     toward Nandos. It opens on 'Arugal's Voidwalker (4627) - On Just
            //     Died - Set GO State'. The intended sequence is: Fenrus (4274)
            //     dies -> his SmartAI sets data on Archmage Arugal (4275) -> that
            //     runs timed actionlist 427500, which summons the four voidwalkers
            //     6s later -> killing one opens the gate. Force-opening the gate
            //     skipped that whole mechanic: the party walked out ~6s before the
            //     adds existed, then the voidwalkers spawned BEHIND it at the room's
            //     west end and the run wedged between advancing to Nandos and
            //     turning back for them (live report 2026-08-01). The gate has
            //     door.autoCloseTime 0, so a bot click opened it permanently.
            //     The "Arugal's Voidwalkers" event (map 33 id 3) now drives the
            //     real sequence.
            //
            //   18971 Arugal's Lair (guid 33241) — opens on 'Wolf Master Nandos
            //     (3927) - On Just Died - Set GO State'. Nandos stands 2.6yd in
            //     FRONT of it, so the ordinary run kills him and the door opens
            //     itself; a bot that force-opened it could instead walk straight
            //     past him to Archmage Arugal and skip an encounter. The
            //     door-blocked watchdog in DcEngageActions already named this door
            //     as the reason it exists — this is the entry that fixes it.
            case 18971:  // Shadowfang Keep — Arugal's Lair (opens on Nandos' death)
            case 18972:  // Shadowfang Keep — Sorcerer's Gate (voidwalker event)
            // Ahn'kahet: The Old Kingdom — Taldaram Door (192236, guid 67337, at
            // (550.1,-865.1,11.6)). instance_ahnkahet registers it as a
            // DOOR_TYPE_PASSAGE on DATA_PRINCE_TALDARAM, so the script opens it
            // on his death and nothing else ever does — the Arugal's Lair shape.
            // It is lock 0 / startOpen 0 like the rest of this map's doors, so
            // BotCanOpenDoorLikePlayer reads it as freely clickable, and a bot
            // that force-opened it would unlock the whole lower half of the
            // dungeon (Jedoga, Amanitar, Volazj) with Taldaram still hovering
            // immune in his prison behind it.
            //
            // The roster's order keys already send the party to Taldaram before
            // anything past this door, so in the normal case the door opens
            // itself and is never the thing to solve. This entry is what keeps
            // that true when the order is perturbed — a `dc skip`, a wing
            // filter, a future reorder.
            case 192236:  // Ahn'kahet — Taldaram Door (opens on Taldaram's death)
            // The Violet Hold (map 608) — the Prison Seal and all twelve Cells.
            // Every one of them is GAMEOBJECT_TYPE_DOOR with lockId 0, so
            // BotCanOpenDoorLikePlayer reads the whole set as freely clickable,
            // and every one of them is driven ONLY by instance_violet_hold:
            //
            //   191723 PRISON SEAL — the main door, and the only one spawned
            //     startOpen. EVENT_START_ENCOUNTER shuts it 15s after Sinclari's
            //     gossip and InstanceCleanup reopens it. A bot Use() would toggle
            //     it under the encounter: opened mid-siege it breaks the seal the
            //     whole fight is about, and closed before the party is through it
            //     locks them out of their own dungeon.
            //
            //   191556 / 191562-191566 / 191606 / 191722 — the eight occupied
            //     cells. StartBossEncounter opens exactly the one whose prisoner
            //     the instance rolled, and the boss's own release (clearing
            //     UNIT_FLAG_NON_ATTACKABLE, SetImmuneToNPC(false),
            //     REACT_AGGRESSIVE, MovePoint to its fight position) rides that
            //     same call. Force-opening a cell therefore does NOT release the
            //     boss — it just exposes an inert, permanently NON_ATTACKABLE
            //     creature the clear would then try to route to. Worse, opening
            //     the WRONG one leaves the party staring at a boss the encounter
            //     will never release.
            //
            //   191557-191560 — the four empty cells. Present on the map, never
            //     opened by anything. Listed for completeness so a future
            //     door-blocked walk-in cannot single one of them out.
            case 191556:  // Violet Hold — Xevozz cell
            case 191557:  // Violet Hold — empty cell
            case 191558:  // Violet Hold — empty cell
            case 191559:  // Violet Hold — empty cell
            case 191560:  // Violet Hold — empty cell
            case 191562:  // Violet Hold — Erekem Guard 2 cell
            case 191563:  // Violet Hold — Erekem Guard 1 cell
            case 191564:  // Violet Hold — Erekem cell
            case 191565:  // Violet Hold — Zuramat cell
            case 191566:  // Violet Hold — Lavanthor cell
            case 191606:  // Violet Hold — Moragg cell
            case 191722:  // Violet Hold — Ichoron cell
            case 191723:  // Violet Hold — Prison Seal (the main door)
            // Halls of Stone (map 599) — the seven script-owned objects.
            //
            // 191296 SJONNIR DOOR is the whole reason map 599 needs automation and
            // the one object here a bot might plausibly try to click: it is
            // GAMEOBJECT_TYPE_DOOR with lockId 0 and template Data0 = 0, so
            // BotCanOpenDoorLikePlayer reads it as freely clickable, and it is the
            // ONLY closed door on the map — five runs of tp-20260831-205458-3 ended
            // parked in front of it. It is opened by exactly one code path
            // (instance_halls_of_stone's SetData(BRANN_DOOR, DONE), called 3.2s
            // after Brann arrives at POINT_SJONNIR_DOOR) and it SHUTS AGAIN behind
            // the party when Sjonnir is engaged, reopening on his death or a wipe.
            // A bot Use() would desync it against the client and, worse, could walk
            // the party through to Sjonnir with the Tribunal never done — skipping
            // the very encounter this dungeon's automation exists to complete.
            //
            // 191527 SKY ROOM FLOOR is toggled back and forth by the Tribunal's
            // post-fight lore state machine (SetData(BOSS_TRIBUNAL_OF_AGES,
            // SPECIAL/DONE) drives it against the three head GOs). It is a
            // TYPE_DOOR the party can stand on; a click mid-sequence drops it out
            // from under the cutscene.
            //
            // 191669/191670/191671 TRIBUNAL HEADS are TYPE_DOOR too, not scenery,
            // and their GO state IS the lore state machine's memory — SetData reads
            // GetGoState() on all three to decide which face speaks next. A click
            // does not merely desync a visual, it corrupts that read.
            //
            // 193906 SJONNIR CONSOLE and 193907 TRIBUNAL CONTROL CONSOLE are
            // Brann's, and are only ever set by SetGameObjectState from his own
            // MovementInform / PathEndReached. 193907 is the object he channels on
            // for the entire 300-second defend.
            //
            // NOT LISTED, deliberately: 191292 / 191293 / 191294 / 191295 / 191459.
            // All five are Doodad_UL_Ulduar_doors* that spawn state = 0 (OPEN),
            // carry template Data0 = 1 (startOpen), and are referenced by no C++
            // anywhere. Nothing ever closes them and nothing in Halls of Stone
            // traps the party behind one, so listing them would be noise in a table
            // whose whole value is that every row means something.
            case 191296:  // Halls of Stone — Sjonnir's Door (Brann opens it; shuts on engage)
            case 191527:  // Halls of Stone — Sky Room Floor (Tribunal lore state machine)
            case 191669:  // Halls of Stone — Tribunal Head, Center
            case 191670:  // Halls of Stone — Tribunal Head, Right
            case 191671:  // Halls of Stone — Tribunal Head, Left
            case 193906:  // Halls of Stone — Sjonnir Console (Brann's, post-kill)
            case 193907:  // Halls of Stone — Tribunal Control Console (the defend point)
                return true;
            // Halls of Lightning (map 602) — the two progression doors, and the
            // only two of the map's eleven GameObjects that gate anything.
            //
            // Both are GAMEOBJECT_TYPE_DOOR, both spawn `state 1` (shut) with
            // Data0 = 0 (startOpen), and both are lockId 0 — so
            // BotCanOpenDoorLikePlayer reads an empty lock as "any player can
            // click this" and a door-blocked bot would GameObject::Use() either
            // one. The ONLY thing that legitimately opens them is
            // instance_halls_of_lightning's DoorData firing on
            // SetBossState(..., DONE). The Ahn'kahet Taldaram Door shape, twice.
            //
            //   191325 VOLKHAN DOOR (1277.4, -164.7, 53.5) — DOOR_TYPE_PASSAGE
            //     on DATA_VOLKHAN. Force-opening it walks the party past a LIVE
            //     Volkhan into the Hall of the Watchers, which is 31 frozen
            //     Titanium statues and three ambush areatriggers.
            //
            //   191326 IONAR DOOR (1074.3, -232.2, 62.6) — DOOR_TYPE_PASSAGE on
            //     DATA_IONAR. Force-opening it unlocks the whole west corridor
            //     AND Loken with Ionar still alive.
            //
            // In the normal case neither is ever the thing to solve: the roster
            // order (DungeonEncounter.dbc — Bjarngrim, Volkhan, Ionar, Loken) is
            // also the walking order, so the party kills the boss each door is
            // keyed on before it ever reaches the door, and the instance opens
            // it. These rows are what keeps that true when the order is
            // perturbed — a `dc skip`, a wing filter, a wipe that leaves the
            // party re-routing.
            //
            // Deliberately NOT navigation-invisible, unlike the Molten Core
            // props and the Halls of Stone Sky Room Floor: these two ARE doors,
            // the party really is stopped by them, and the at-boss stand-down
            // needs to see them. Sjonnir's Door (191296) is on this list for the
            // same reason and for the same one line of code.
            case 191325:  // Halls of Lightning — Volkhan Door (opens on Volkhan's death)
            case 191326:  // Halls of Lightning — Ionar Door (opens on Ionar's death)
                return true;

            // UTGARDE PINNACLE (575) — the two portcullises, and the cleanest
            // example on record of a door that is not a door problem.
            //
            //   192173 Doodad_VR_Portcullis01 (477.5, -477.2, 103.1) — opens on
            //     SetData(DATA_SKADI_THE_RUTHLESS, DONE). It joins the room north
            //     of Skadi's hall to the hall itself.
            //   192174 Doodad_VR_Portculliswithchain01 (445.1, -325.5, 101.0) —
            //     opens on SetData(DATA_KING_YMIRON, DONE). It is the LAST BOSS'S
            //     EXIT, so it is shut for the entire run and nothing in a correct
            //     clear ever waits on it.
            //
            // Both spawn state 1 (shut) with lockId 0 and autoCloseTime 0, and the
            // instance C++ is their sole authority — smart_scripts source_type 1
            // has zero rows for map 575, and the script contains no AddDoor, no
            // DoorData[], no SetBossState and no DoUseDoorOrButton. HandleGameObject
            // is called exactly five times in the whole dungeon, twice of them in
            // OnGameObjectCreate to re-open on reload. So neither door is ever
            // CLOSED by a script: they spawn shut and are opened once, permanently,
            // and there is no IsSelfClearing case to consider.
            //
            // A bot must never click either. Both are lock-free
            // GAMEOBJECT_TYPE_DOORs, so BotCanOpenDoorLikePlayer would happily
            // open one — and force-opening 192173 hands the party Ymiron's room
            // with Skadi alive (he stays UNIT_FLAG_NOT_SELECTABLE regardless, so
            // the reward is a walk to a boss that cannot be attacked), while
            // force-opening 192174 opens nothing the run needs at all.
            //
            // DELIBERATELY NOT IsNavigationIgnored, and that is the interesting
            // half. Unlike the Molten Core props and the Halls of Stone Sky Room
            // Floor, these two ARE doors: the party really is stopped by them and
            // the at-boss stand-down needs to see them. With the roster patched
            // (see UtgardePinnacleEvents.cpp) the designed legs clear 192174 by
            // 43yd at worst and 192173 by 32.9yd at worst, except the post-Skadi
            // leg to Ymiron which passes 192173 at 3.1yd with it already open. A
            // run that DOES pause on one of these has regressed somewhere else —
            // 192174 means the route reverted to the entrance-hall shortcut,
            // 192173 means the gauntlet approach — and hiding them from navigation
            // would mask that instead of preventing it.
            case 192173:  // Utgarde Pinnacle — Skadi's Door (opens on Skadi's death)
            case 192174:  // Utgarde Pinnacle — Ymiron's Door (opens on Ymiron's death)
                return true;

            // PIT OF SARON (658) — the Ice Wall, and a row authored from a
            // MEASUREMENT rather than from a resemblance.
            //
            // 201885 Ice Wall (932.27, -80.67, 591.68) is a real
            // GAMEOBJECT_TYPE_DOOR with lockId 0 and autoCloseTime 0, spawned
            // state 1 (SHUT) and opened ONCE, permanently, by
            // instance_pit_of_saron the moment Garfrost and Ick are both DONE —
            // from SetData on either boss, and again from OnGameObjectCreate so a
            // reload re-opens it. There is no auto-close and therefore no
            // IsSelfClearing case.
            //
            // It is on the DESIGNED LEG, not beside it: the wave-2 ambush is
            // fought on the ground it stands in, ~9yd from the near spawn line, so
            // the authored route passes well inside DungeonClearBlockingDoorValue's
            // 12yd same-floor fallback band.
            //
            // A BOT MUST NEVER CLICK IT. Lock-free type-0 doors are exactly what
            // BotCanOpenDoorLikePlayer will happily open, and this one is the
            // instance script's sole property.
            //
            // DELIBERATELY NOT IsNavigationIgnored, for the Utgarde Pinnacle
            // reason. By the time anything in this module reaches it the wall is
            // already open — the gauntlet's own activation predicate requires both
            // bosses DONE — so the row is belt-and-braces, and a run that DOES
            // pause here has regressed somewhere the module should be told about.
            // Hiding the door from navigation would mask that instead of
            // preventing it.
            case 201885:  // Pit of Saron — Ice Wall (opens on Garfrost + Ick)
                return true;

            // --- Halls of Reflection (668) --------------------------------
            //
            // FOUR DOORS, all of them the instance's alone, and one of them is
            // the reason this map needs the rows at all.
            //
            // GO 201976, the FRONT DOOR, is a real DOOR_TYPE_ROOM the party
            // stands behind for nine minutes on purpose. instance_halls_of
            // _reflection shuts it at the end of the intro and again for EVERY
            // wave, and re-opens it when a wave wipes or when Marwyn dies. So it
            // is shut for most of the first two thirds of the run with the whole
            // party on the inside — and the one thing the module must never do
            // there is treat it as a corridor blocker and auto-pause the run in
            // front of it, or click it and fight the script for the state.
            //
            // DELIBERATELY NOT IsNavigationIgnored, for the Pit of Saron reason:
            // the party never needs to path THROUGH any of these, so a run that
            // does pause at one has regressed somewhere the module should be
            // told about, and hiding them from navigation would mask that rather
            // than prevent it.
            case 201976:  // front door (shuts for the intro's end and every wave)
            case 197341:  // the Arthas door (opens on Marwyn's death)
            case 197342:  // the door before the throne (spawned open, never scripted)
            case 201385:  // Ice Wall (SUMMONED at an Ice Wall Target; opens on WallCompleted)
            // The Culling of Stratholme (map 595) — the two objects the instance
            // owns. Both are GAMEOBJECT_TYPE_DOOR, both spawned shut, and neither
            // is ever clickable by anybody:
            //
            //   188686 the TOWN HALL BOOKCASE, the secret passage to the last
            //     third of the dungeon. Opened by exactly two lines of the core —
            //     npc_arthasAI's waypoint-36 handler, and ReorderInstance for a
            //     state at or past KILLED_EPOCH — and the intended flow is that
            //     the party follows Arthas to it and he opens it. A bot Use()
            //     would toggle the server state with the client still drawing it
            //     shut, AND would do so while Arthas is still walking, which is the
            //     one thing this event must not race. ALSO navigation-ignored
            //     below; see the row there for why one row is not enough.
            //
            //   191788 the CITY ENTRANCE GATE (the exit). Opened only when the
            //     counter reaches FINISHED, after Mal'ganis. Never on the critical
            //     path — the run is over by then — so this row exists purely so a
            //     stray door-blocked walk-in can never single it out.
            case 188686:  // CoS — Town Hall bookcase (opens at Arthas's waypoint 36)
            case 191788:  // CoS — City Entrance Gate (opens at PROGRESS_FINISHED)
            // Trial of the Champion (map 650) — the four arena gates. All four
            // are opened and shut by instance_trial_of_the_champion alone (the
            // champions' and soldiers' entrances, the Knight's exit, every
            // cleanup), and none is ever the party's to open. Also
            // navigation-ignored below.
            case 195647:  // ToC — Main Gate (north wall; the soldiers come through it)
            case 195648:  // ToC — East Portcullis
            case 195649:  // ToC — South Portcullis
            case 195650:  // ToC — North Portcullis (the entrance; shuts at progress 1)
            // Karazhan (map 532) — Gamesman's Hall Exit Door (184277, guid at
            // the Chess room's far side). Lock 0, faction 1375, flags 32
            // (NODESPAWN), and the only thing that ever opens it is
            // instance_karazhan's SetData(DATA_CHESS_EVENT, DONE) handler. It is
            // the only way to Prince Malchezaar, which is why the roster skips
            // Prince while Chess is skipped. Lock-free already refuses it; this
            // row keeps it refused if a future edit lists lock-free doors more
            // loosely, so nobody walks past Chess to Prince.
            case 184277:  // Karazhan — Gamesman's Hall Exit Door (opens on Chess DONE)
                return true;
            default:
                return false;
        }
    }

    // Doors NAVIGATION must ignore entirely: never flagged as a corridor
    // blocker, never opened, never a reason to park or auto-pause. These are
    // interact-THROUGH gates — the run's objective is completed from the
    // players' side of the shut door (a gossip through the bars), after which
    // the event script opens the door itself. Flagging one as blocking is
    // always wrong: the route intentionally ends beside it, and the pause
    // machinery would halt a run that needs nothing from the door at all.
    inline bool IsNavigationIgnored(uint32 goEntry)
    {
        switch (goEntry)
        {
            case 184393:  // Old Hillsbrad — Thrall's Prison Door (gossip through
                          // the gate; his script opens it via EVENT_OPEN_DOORS)
                return true;
            // Blackwing Lair — Chromaggus' cage Portcullis (179116, guid 75161
            // at (-7506.3,-1043.2,480.0)). The cage is opened from the RAID's
            // side by the Lever (179148, 65yd south), whose GossipHello does
            // three things in one go: SetGUID(GUID_LEVER_USER) on Chromaggus,
            // which is the ONLY thing in the core that clears his
            // UNIT_FLAG_IMMUNE_TO_PC; MoveWaypoint, which walks him out; and
            // HandleGameObject on this portcullis. Nothing else ever touches it
            // — it is in instance_blackwing_lair's objectData but NOT its
            // doorData, so no encounter state opens it either.
            //
            // Left unlisted it is a DEADLOCK, not merely a wrong park, because
            // the flag and the thing that clears it need each other:
            //
            //   flagged blocking -> DungeonClearAtBossTrigger stands down
            //     -> the engage rung never runs -> the raid muster never stages
            //     -> ChromaggusCageDue is false (it waits on muster Ready)
            //     -> the door-blocked trigger's due-event yield never fires
            //     -> lock-free and not IsLockFreeClickable, so
            //        BotCanOpenDoorLikePlayer refuses -> "can't open ... ->
            //        auto-pausing", and a paused run cannot drive the event
            //        that pulls the lever.
            //
            // This is what ended NINE BWL runs at the same spot, all parked
            // ~21yd from the portcullis at (-7486,-1046,476.6):
            // tr-20260828-183508-1/-2/-3, -195344-2/-4/-5, -215521-15,
            // -235310-4 and tr-20260829-195231-1 (8/9 bosses; flagged 4s after
            // the anchor flipped to Chromaggus, auto-paused 9s later, with no
            // "raid muster: staging at Chromaggus" line ever reached).
            //
            // Deliberately IsNavigationIgnored and NOT IsScriptOnly, for the
            // Thrall reason above it: IsScriptOnly only stops the CLICK, and
            // the auto-pause below it is what actually kills the run. The route
            // ends inside the cage on Chromaggus' spawn, the boss-engage rung
            // holds the tank on the approach while he reads `caged`, and the
            // cage event does the opening. The exit portcullis (179117) needs
            // nothing: it is doorData DOOR_TYPE_PASSAGE on DATA_CHROMAGGUS and
            // opens itself when he dies.
            case 179116:  // Blackwing Lair — Chromaggus cage Portcullis
                          // (opened by go_chromaggus_lever from the raid's side)
                return true;
            // The Steamvault — Main Chambers Access Panels. These are wall
            // CONTROLS, not doors, but their template is GAMEOBJECT_TYPE_DOOR
            // and they spawn (and permanently stay) in GO_STATE_READY, so the
            // closed-door predicate reads each one as a shut gate sitting on
            // the corridor. Clicking one runs go_main_chambers_access_panel's
            // OnGossipHello, which returns true BEFORE GameObject::Use reaches
            // UseDoorOrButton — so the panel's own GOState never flips, and the
            // door-blocked action concluded "clicked it, still closed, can't
            // open" and auto-paused the run 13.8yd from its objective (live run
            // 2026-07-20, tank Fedrel). The panel is opened by nothing and
            // blocks nothing; the Steamvault event (map 545 id 1) clicks it,
            // which is what opens the real Main Chambers Door (183049).
            case 184125:  // Hydromancer Thespia's panel
            case 184126:  // Mekgineer Steamrigger's panel
                return true;
            // Halls of Reflection (map 668) — the Frostmourne Altar. The
            // Steamvault shape again, and the most expensive version of it yet:
            // a decorative DAIS whose template is GAMEOBJECT_TYPE_DOOR
            // (displayId 9294, startOpen 0, lock 0), spawned state 1 and left
            // there forever. instance_halls_of_reflection carries it in
            // objectData purely so GetGameObject can find it — there is no
            // HandleGameObject, no SetGoState, no doorData row anywhere in the
            // core, and the only altar code path at all is the glow spell cast
            // on the altar bunny. So IsDoorClosed reads it shut on every tick
            // of every run, permanently, and nothing will ever open it.
            //
            // It blocks nothing — the party fights all three altar waves
            // standing around it — but Leg A of the authored route begins at
            // CenterPos, which is 0.13yd in xy from the altar's own origin, so
            // the corridor's FIRST leg transits its footprint the instant the
            // route to the Frostsworn General is seeded. Lock-free and not
            // IsLockFreeClickable, so BotCanOpenDoorLikePlayer refuses, and the
            // walk-in park lands "at door (0.0yd along path)" on tick one and
            // falls straight through to the auto-pause.
            //
            // That is unconditional, and it ended ALL TEN runs of plan
            // tp-20260907-212113-1 at 3/6 bosses, every one of them within a
            // second of Marwyn dying and none of them ever reaching the Arthas
            // door: tr-20260907-212118-1/-2 and -212119-3/-4/-5/-6/-7/-8/-9/-10,
            // each flagging the same GUID 0xf1100315fc000003 and auto-pausing
            // on "a closed door is blocking the path".
            //
            // IsNavigationIgnored and NOT IsScriptOnly, for the Chromaggus
            // reason above: IsScriptOnly suppresses only the CLICK, and it is
            // the auto-pause underneath it that actually kills the run.
            case 202236:  // Halls of Reflection — Frostmourne Altar
            // ...and its TWIN, which is why the altar row alone was not enough.
            // GO 202302 'Frostmourne' — the sword — is a SECOND
            // GAMEOBJECT_TYPE_DOOR spawned at (5309.36, 2006.55), 0.03yd from
            // the altar's own origin and on the same first leg. Whitelisting
            // only the altar just handed the flag to the sword: plan
            // tp-20260907-214408-1 repeated tp-20260907-212113-1 exactly — all
            // ten runs paused at 3/6 bosses seconds after Marwyn died, this time
            // flagging GUID 0xf11003163e000002 entry 202302.
            //
            // Unlike the altar the sword IS scripted — but it is scripted SHUT.
            // instance_halls_of_reflection's OnGameObjectCreate closes it, the
            // intro opens it for the cutscene, and EVENT_INTRO_LK_4_2/4_3 close
            // it again and SetPhaseMask(2) it out of the party's phase. That
            // chain runs on the SKIPPED intro too — it is what spawns Falric —
            // so from the end of the intro onward the sword is shut and phased
            // away forever, blocking nothing and opening never.
            //
            // The phase is the real tell, and the scan now skips out-of-phase
            // gameobjects for exactly that reason (see the InSamePhase guard in
            // DungeonClearBlockingDoorValue). This row stays anyway: it is the
            // cheap, map-specific statement of a fact the generic guard only
            // implies, and it holds even for the ticks BEFORE the intro phases
            // the sword out, when it is in phase and still unopenable.
            case 202302:  // Halls of Reflection — Frostmourne (the sword)
            // The Culling of Stratholme (map 595) — the TOWN HALL BOOKCASE, and
            // this is the Halls of Reflection Frostmourne lesson applied before it
            // cost a run rather than after.
            //
            // 188686 at (2473.27, 1121.36, 149.96) is an interact-THROUGH gate in
            // the fullest sense: the party's job at it is to have talked to Arthas
            // forty yards earlier, and ARTHAS is what opens it — his waypoint-36
            // handler, about six seconds after he walks up. So for those six
            // seconds a shut TYPE_DOOR is standing directly on the party's path
            // with the escort walking INTO it.
            //
            // IsScriptOnly alone does not cover that. It only stops the bot
            // CLICKING the door; the blocking-door value still flags it, and the
            // auto-pause underneath that is what ends the run — in front of a door
            // that is about to open on its own. A paused run then cannot drive the
            // event that would open it, which is the same deadlock shape nine
            // Blackwing Lair runs died of at Chromaggus's cage.
            //
            // So it needs BOTH rows, and the reason it needs both is the reason
            // Thrall's prison door and the Chromaggus portcullis do.
            case 188686:  // CoS — Town Hall bookcase (Arthas opens it at waypoint 36)
                return true;
            // Trial of the Champion (map 650) — the four arena gates, and here the
            // row is about WHERE THEY STAND, not about passing through them. The
            // run never leaves the bowl, so no route needs any of them open; but
            // the North Portcullis is 2.6yd from where the party lands and shuts
            // behind it at progress 1, and the Main Gate is 17yd behind the
            // soldier line. A shut TYPE_DOOR that close to the fighting is exactly
            // what the blocking-door value flags and the auto-pause halts on — in
            // front of a door the run has no use for.
            case 195647:  // ToC — Main Gate
            case 195648:  // ToC — East Portcullis
            case 195649:  // ToC — South Portcullis
            case 195650:  // ToC — North Portcullis (the entrance)
                return true;
            // The Violet Hold (map 608) — the six Activation Crystals. Like the
            // Steamvault access panels these are wall CONTROLS, not doors, but
            // their template is GAMEOBJECT_TYPE_DOOR (lock 86 / 57, startOpen 0)
            // and they sit permanently in GO_STATE_READY around the arena rim, so
            // the closed-door predicate reads each one as a shut gate standing in
            // the open room the driver walks the party across.
            //
            // Nothing about them is a door. instance_violet_hold spawns them
            // GO_FLAG_NOT_SELECTABLE and only clears that at
            // EVENT_START_ENCOUNTER; using one runs spell 57804
            // (SPELL_EFFECT_SEND_EVENT -> EVENT_ACTIVATE_CRYSTAL), which summons
            // the Defense System, not a door state change. They block nothing —
            // the navmesh runs straight past all six — and a door-blocked walk-in
            // on one would park the run on the far rim and auto-pause it.
            //
            // Deliberately IsNavigationIgnored and NOT IsScriptOnly: the crystals
            // are a legitimate thing to CLICK (the Defense System's three Arcane
            // Lightnings and the 58152 instakill are a real panic valve, and bots
            // do not care about the Defenseless achievement). If that is ever
            // wired it belongs in the events framework as a deliberate step, the
            // way the Steamvault panels are — never opportunistically, by the
            // watchdog, because it is on the wrong side of the room.
            case 193611:  // Violet Hold — Activation Crystal (x5)
            case 193615:  // Violet Hold — Intro Activation Crystal
            // The Violet Hold — the Prison Seal and all twelve Cells. Already
            // IsScriptOnly above (never click one); this is the other half of the
            // rule, and without it the script-only listing is what HURTS: a
            // flagged door the bot is not entitled to open falls straight through
            // to the auto-pause in DcEngageActions' parkAndStall.
            //
            //   * THE CELLS sit almost ON TOP of the fight positions their bosses
            //     are released to — Xevozz's cell (1908.06, 844.89) is 3.0yd from
            //     where he fights, Zuramat's 7.1yd, Lavanthor's 8.5yd, Erekem's
            //     8.3yd. So travelling to ANY released boss parks the party beside
            //     one to three OTHER prisoners' permanently-shut cells, each of
            //     which reads to the closed-door predicate as a gate across the
            //     approach. That is the Ahn'kahet Taldaram-prison shape: a run
            //     auto-paused standing on the objective it had already reached.
            //     Nothing is behind a cell that the party ever needs — the boss
            //     walks OUT to its fight position — so no cell is ever a corridor.
            //
            //   * THE PRISON SEAL (191723) is worse, because it is genuinely shut
            //     for most of the run and genuinely cannot be solved. It closes 15s
            //     after Sinclari's gossip and only the instance reopens it (on a
            //     win, or on InstanceCleanup). A bot that ends up outside during
            //     the siege therefore parks at a door no player can open either,
            //     and pausing the run for a human who has no move to make is pure
            //     loss — the real recovery is Sinclari's late-join gossip (menu
            //     9997 option 1, visible only while IN_PROGRESS), and a full wipe
            //     self-heals because InstanceCleanup reopens the door anyway.
            case 191556:  // Violet Hold — Xevozz cell
            case 191557:  // Violet Hold — empty cell
            case 191558:  // Violet Hold — empty cell
            case 191559:  // Violet Hold — empty cell
            case 191560:  // Violet Hold — empty cell
            case 191562:  // Violet Hold — Erekem Guard 2 cell
            case 191563:  // Violet Hold — Erekem Guard 1 cell
            case 191564:  // Violet Hold — Erekem cell
            case 191565:  // Violet Hold — Zuramat cell
            case 191566:  // Violet Hold — Lavanthor cell
            case 191606:  // Violet Hold — Moragg cell
            case 191722:  // Violet Hold — Ichoron cell
            case 191723:  // Violet Hold — Prison Seal (the main door)
                return true;
            // Blackrock Depths — the Giant Doors apparatus (map 230). Four
            // GAMEOBJECT_TYPE_DOOR entries make up one machine, and only the
            // lever (161460, key-exempt below) is ever meant to be clicked. The
            // other three are the machine's moving parts: they carry no lock, no
            // ScriptName and no gossip, and their GO state is driven ENTIRELY by
            // the lever's SmartAI (161460 source_type 1: on GO state changed ->
            // SMART_ACTION_ACTIVATE_GOBJECT on guids 15639/15576/15640/15352).
            //
            // Their states are INVERTED with respect to each other, so whichever
            // way the machine stands, one of them is sitting in GO_STATE_READY on
            // the corridor and reads to the closed-door predicate as a shut gate:
            //
            //   doors open  (spawn state) — Giant Doors ACTIVE, Fake Collision +
            //     BigDoorDummyCollision02 READY. The Fake Collision spawns on top
            //     of the Giant Doors at (723.1,-105.9,-71.5) with a 18x21x25yd
            //     model box, so it lands within the blocking-door value's 5yd
            //     corridor band on the lower passage the route to Bael'Gar uses.
            //     This is what ended run tr-20260817-044457-30 at 9/19 bosses:
            //     "blocking-door: flagged ... 'Giant Door Fake Collision' (entry
            //     161462) 78.1yd from bot as corridor-blocking" -> walk-in ->
            //     "can't open ... -> auto-pausing".
            //   doors closed (after the lever) — exactly the reverse, so the
            //     Giant Doors themselves (157923) become the flagged blocker,
            //     on the very state the Shadowforge Lock event works to reach.
            //
            // Neither state is ever something a player solves at the door, and
            // neither obstructs a bot: server-side GameObject collision feeds the
            // dynamic LoS tree only — mmaps carry no gameobjects and the movement
            // splines are not collision-checked — and the navmesh runs straight
            // through the doorway at z ~ -71.5 either way. So the whole apparatus
            // is navigation-invisible; the lever alone drives it.
            case 157923:  // Giant Doors (startOpen=1; closed by the lever)
            case 161461:  // Giant Door Mechanism (the winding wheel, 3.3yd from
                          // the lever — lock-free, so BotCanOpenDoorLikePlayer
                          // refuses it and it would auto-pause the run standing
                          // AT the objective it is part of)
            case 161462:  // Giant Door Fake Collision (open-state collision hull)
            case 161516:  // BigDoorDummyCollision02 (the upper-level portcullis
                          // hull, (702.1,-125.7,-45.7))
                return true;
            // Utgarde Keep (map 574) — the three forge FLAME WALLS. The forge
            // hall is a ring around a central hearth, cut into three sectors by
            // three walls of fire, one per forge. Each is a
            // GAMEOBJECT_TYPE_DOOR: lock 0, startOpen 0, autoCloseTime 0, no
            // ScriptName, no AIName, addon flags 32 (NODESPAWN, and notably no
            // GO_FLAG_LOCKED), spawned in GO_STATE_READY. Their model
            // (Vr_Forgefire_01, display 7503) is not a door panel at all — it is
            // a ~60yd-long, ~2yd-thick, ~37yd-tall slab that runs from the
            // hearth out to the outer wall, so it lies across the ring rather
            // than beside it and the closed-door predicate reads it as a shut
            // gate straddling the corridor.
            //
            // Nothing a player does at the wall opens it. instance_utgarde_keep
            // owns all three GO states: SetData(DATA_FORGE_n, ...) opens that
            // forge's bellows + fire + anvil together, and the only caller is
            // npc_dragonflayer_forge_master — DONE on JustDied, NOT_STARTED on
            // Reset (which shuts the wall again). The master that opens a wall
            // stands in the sector on the PARTY'S side of it, so the wall is
            // never the thing to solve: it is the readout of a fight the run has
            // to walk past it to reach.
            //
            // Reading the ring as a bearing off the hearth at (360.7,-16.5) —
            // the navmesh is an annulus r 16..~62 the whole way round, with the
            // entrance corridor running out at bearing ~195-205 deg and the exit
            // corridor toward Keleseth at ~75-110 deg — the three walls sit at
            // 288.5 / 48.5 / 168.5 deg and each master sits one to a sector:
            //
            //   sector 1  168.5..288.5  entrance corridor, forge master 1 (246 deg)
            //     kill him -> 186692 (288.5 deg) drops -> sector 2
            //   sector 2  288.5.. 48.5  forge master 2 (1 deg)
            //     kill him -> 186693 (48.5 deg) drops -> sector 3
            //   sector 3   48.5..168.5  exit corridor, forge master 3 (122 deg)
            //     kill him -> 186691 (168.5 deg) drops, closing the loop back
            //     onto the entrance corridor
            //
            // so the intended clear is one counter-clockwise lap of the ring,
            // and the map-574 forge objectives are what enforce it. The wall
            // itself enforces nothing on a bot: mmaps carry no gameobjects and
            // movement splines are not collision-checked, so a raised wall has
            // never stopped one.
            //
            // Left unlisted it ends the run outright. Runs tr-20260818-070705-4
            // and -7 flagged "'Doodad_VR_ForgeFire_First' (entry 186692) 98.0yd
            // from bot as corridor-blocking", walked in, reported "can't open
            // ... -> auto-pausing", and died at 0/3 bosses 3m45s in with the
            // tank still 57yd short of forge 1.
            //
            // Nor is the flagged wall reliably the one the corridor crosses.
            // PathLegCrossesDoor tests the DBC GeoBox through ToDoorLocal, the
            // GameObject::IsInRange frame, whose matrix [[sinA,cosA],[cosA,-sinA]]
            // has determinant -1 — it MIRRORS. The server's real collision uses
            // GameObjectModel's Rz(orientation) instead. The two agree on a
            // symmetric door panel and disagree on a slab that sits entirely to
            // one side of its origin: here they place the same wall 120 degrees
            // apart, so per-door geometry tuning cannot be made to work on these
            // three anyway.
            //
            // IsScriptOnly would only stop the click — the wall would still be
            // flagged, still parked at, still auto-paused on. IsSelfClearing
            // would hold instead of pausing, but there is no timer to hold for:
            // the wall opens on a kill, and holding at it starves the very fight
            // that opens it. Navigation-invisible is the only correct answer.
            //
            // This does not blind the run to them. DcEngageGeometry::
            // ClosedDoorBetween rays the REAL collision mesh and does not
            // consult this list, so trash and bosses on the far side of a wall
            // that is still up stay vetoed — the party is never dragged through
            // a raised flame wall by a far-side pack.
            case 186691:  // Doodad_VR_ForgeFire_Third  (opens on forge master 3)
            case 186692:  // Doodad_VR_ForgeFire_First  (opens on forge master 1)
            case 186693:  // Doodad_VR_ForgeFire_Second (opens on forge master 2)
                return true;
            // UTGARDE PINNACLE (575) — Svala's mirror, 191745
            // Doodad_Utgarde_Mirror_FX01 at (296.4, -357.0, 91.5).
            //
            // A GAMEOBJECT_TYPE_DOOR that is not a door in any sense: lock 0,
            // autoCloseTime 0, spawned state 0 (OPEN), and toggled BOTH WAYS by
            // boss_svala as pure visual FX — SetGoState(GO_STATE_READY) when the
            // areatrigger starts the intro, back to ACTIVE when she dies. It leads
            // nowhere; the arena has one entrance and it is the ramp to the north.
            //
            // MEASURED, NOT ASSUMED, and the measurement is why this row exists.
            // The authored Leg B (AT 5140 -> Svala) passes within 7.35yd of it and
            // ENDS 11yd from it on her platform — the party fights the whole
            // encounter beside a slab that is READY, i.e. shut by the
            // collision-truth test, for the entire 72-second intro. That is inside
            // DungeonClearBlockingDoorValue's 12yd same-floor fallback band, so a
            // GO-LOS block along the leg's last segment would flag it and auto-
            // pause the run on the boss's own doorstep. t/TestUtgardePinnacle pins
            // the 7.35yd so a re-authored leg re-opens the question rather than
            // silently relying on this row.
            //
            // This is the Ahn'kahet prison-apparatus shape below rather than the
            // Utgarde Keep forge-wall shape above: the prop is not beside the route,
            // it is ON the objective. IsScriptOnly would be the wrong tool for the
            // same reason it is wrong for the forge walls — it only refuses the
            // CLICK, and the auto-pause is what kills the run.
            case 191745:  // Utgarde Pinnacle — Svala's mirror (FX, never a passage)
                return true;
            // Ahn'kahet: The Old Kingdom (map 619) — Prince Taldaram's prison
            // apparatus. Three GAMEOBJECT_TYPE_DOOR entries, all lock 0,
            // startOpen 0, autoCloseTime 0, no ScriptName, spawned
            // GO_STATE_READY, and none of them a door in any useful sense:
            //
            //   193564 Doodad_Azjol_Platform_FX_01 (guid 67330), the prison
            //     effect itself at (528.0,-846.3,11.2). It is not beside the
            //     route, it is ON Taldaram's objective — the party's anchor is
            //     (528.7,-846.0,11.4), under a yard away — so leaving it
            //     unlisted parks the run on its own objective and auto-pauses
            //     there. This is the Steamvault access-panel failure exactly,
            //     one step worse for sitting on the destination rather than
            //     13.8yd from it. instance_ahnkahet owns its state end to end
            //     (HandleGameObject on the sphere count, and again from
            //     OnGameObjectCreate); nothing a player clicks touches it.
            //
            //   193093 / 193094 Ancient Nerubian Device (guids 67331/67332),
            //     at (655.7,-719.0,18.0) and (692.5,-783.9,18.0). These DO have
            //     to be clicked — they are the prison's off-switch — but the
            //     click belongs to the two map-619 device events, which visit
            //     them in a measured order and then VERIFY the instance data
            //     actually moved. The door-blocked watchdog would fire them
            //     opportunistically, out of order, off a corridor heuristic,
            //     with no verification and no objective row to show for it.
            //     Both are lock-free, so BotCanOpenDoorLikePlayer would happily
            //     let it.
            //
            // As everywhere else on this list, invisibility to navigation is not
            // blindness: DcEngageGeometry::ClosedDoorBetween rays the real
            // collision mesh and never consults this table, so nothing on the
            // far side of a still-closed prison is dragged into a pull.
            case 193093:  // Ancient Nerubian Device (west) — the event clicks it
            case 193094:  // Ancient Nerubian Device (east) — the event clicks it
            case 193564:  // Doodad_Azjol_Platform_FX_01 — Taldaram's prison FX
                return true;
            // Drak'Tharon Keep (map 600) — the four Ritual Crystals around
            // Novos the Summoner, at (-392.4,-724.9), (-365.3,-751.1),
            // (-365.4,-724.9) and (-392.3,-751.1), all z 29.4: a 27x26yd square
            // centred on the boss that the party walks THROUGH to reach him.
            //
            // Every one is a GAMEOBJECT_TYPE_DOOR with Data0 = 1 (startOpen, so
            // its state is INVERTED — the Blackrock Depths apparatus shape) and
            // Data1 = lock 1669, LOCK_KEY_ITEM requiring item 38555 "Ritual
            // Crystal Key". instance_drak_tharon_keep registers all four as
            // DOOR_TYPE_ROOM on the pseudo-encounter DATA_NOVOS_CRYSTALS, so the
            // instance script owns their state end to end; the visual of a
            // crystal going dark is driven by spell_novos_crystal_handler_death_
            // aura setting the nearest DOOR within 5yd to GO_STATE_READY when a
            // Crystal Handler dies.
            //
            // A bot must never click one — it has no key and the crystals are
            // not a gate the party solves — so they are correctly NOT
            // IsKeyExempt. But nothing else stops the blocking-door value reading
            // one of the four as a shut gate straddling the route into the
            // chamber and auto-pausing the run at it. That is the Utgarde Keep
            // forge-fire failure exactly: an inverted-state DOOR_TYPE_ROOM lying
            // across the corridor, opened by a kill rather than by a click, with
            // a hold that would starve the very fight that opens it.
            //
            // They obstruct nothing: mmaps carry no gameobjects and movement
            // splines are not collision-checked, and the chamber floor runs flat
            // at z ~28.4 straight under all four. As everywhere on this list,
            // DcEngageGeometry::ClosedDoorBetween still rays the real collision
            // mesh and never consults this table, so nothing on the far side of
            // a genuinely solid obstacle is dragged into a pull.
            case 189299:  // Ritual Crystal (north-west)
            case 189300:  // Ritual Crystal (south-east)
            case 189301:  // Ritual Crystal (north-east)
            case 189302:  // Ritual Crystal (south-west)
                return true;
            // Molten Core (map 409) — the three fire DOODADS. Every
            // GAMEOBJECT_TYPE_DOOR on this map is one of these; there is not a
            // single real door in the raid. All three are lock 0 (Data1),
            // startOpen 0 (Data0), no ScriptName, spawned GO_STATE_READY, so
            // the closed-door predicate reads each one as a shut gate and the
            // walk-in then finds nothing to click:
            //
            //   177000 Hot Coal (guid 56287, display 2470) at
            //     (736.7,-1176.6,-119.8) — 23yd short of MajordomoSummonPos
            //     (759.5,-1173.4,-119.0), squarely on the approach into his
            //     chamber and well inside the value's 80yd DOOR_LOOK_AHEAD.
            //     Nothing in instance_molten_core or boss_majordomo_executus
            //     so much as names entry 177000: it is a decorative coal pile
            //     that is permanently READY and opened by nothing, ever.
            //     This ended tr-20260827-145857-1 (the Plan E1 pilot's first
            //     run) at 8/11 bosses: "blocking-door: flagged ... 'Hot Coal'
            //     (entry 177000) 66.2yd from bot as corridor-blocking" ->
            //     walk-in -> "can't open ... -> auto-pausing".
            //
            //   178107 Lava Steam / 178108 Lava Splash (guids 2135424/2135423)
            //     at (839.0,-830.4,-230.2) and (839.3,-831.1,-230.2). These are
            //     the Ahn'kahet 193564 shape, one step worse: they sit ON the
            //     Ragnaros fight anchor (838.3,-831.5,-232.2) — 1.3yd and 1.4yd
            //     away — and ~20yd from the Summon-Ragnaros gossip anchor, so a
            //     flag here parks the raid on the objective it has arrived at.
            //     They are pure RP visuals: spawned despawned (spawntimesecs
            //     -604800) and made to appear by EVENT_RAGNAROS_SUMMON_1's
            //     SetRespawnTime(900) + Refresh(), which is a RESPAWN, not a
            //     GOState change. Their state is never toggled by anything, so
            //     they are shut for the whole Ragnaros encounter.
            //
            // Deliberately NOT also IsScriptOnly, for the reason the Utgarde
            // Keep forge walls spell out: IsScriptOnly only refuses the click —
            // the prop would still be flagged, still parked at, still
            // auto-paused on. Navigation-invisible is the only answer for a
            // thing that is not a door at all. And as everywhere on this list
            // that costs no awareness: DcEngageGeometry::ClosedDoorBetween rays
            // the real collision mesh and never consults this table.
            case 177000:  // Hot Coal (Majordomo's chamber approach)
            case 178107:  // Lava Steam (on Ragnaros' anchor)
            case 178108:  // Lava Splash (on Ragnaros' anchor)
                return true;
            // Halls of Stone (map 599) — the Tribunal room's five script-owned
            // props. Every one is GAMEOBJECT_TYPE_DOOR with Data0 = 0
            // (startOpen) and spawns GO_STATE_READY, so the closed-door
            // predicate reads all five as shut gates, and all five sit in or
            // beside the room the party MUST stand in between the Tribunal and
            // Sjonnir. They are already IsScriptOnly above; this is the other
            // half of the fix, for the reason the Molten Core row spells out —
            // IsScriptOnly only refuses the click, so the prop is still
            // flagged, still parked at, still auto-paused on.
            //
            //   191527 SKY ROOM FLOOR (guid 65556) at (909.7,345.1,203.4) is
            //     the one that fires. It is the FLOOR of the Tribunal room, and
            //     Brann's post-fight lore shuts it deterministically
            //     (brann_bronzebeard.cpp: SetData(BOSS_TRIBUNAL_OF_AGES,
            //     SPECIAL) -> pSkyRoomFloor->SetGoState(GO_STATE_READY)) —
            //     exactly when the party is parked at his console
            //     (897.2,331.8,203.7) waiting out the door gossip. That leaves
            //     the tank ~11-17yd from a "shut door" it can never open.
            //
            //     tp-20260901-080112-1: EIGHT of ten tanks flagged it
            //     ("blocking-door: flagged ... 'Doodad_UL_SkyRoom_Floor01'
            //     (entry 191527) 13.6yd from bot as corridor-blocking").
            //     Seven cleared within 6-30s because they still had a corridor
            //     and took the walk-in branch. tr-20260901-080117-8 did not:
            //     the 4th Brann gossip had just fired, and the route to Sjonnir
            //     stays UNREACHABLE for the whole 400yd Brann walks from the
            //     console to his door, so the door-blocked action fell to its
            //     no-corridor branch — parkAndStall(IsWithinDistInMap(door,
            //     DC_DOOR_USE_RANGE)), and 13.6yd is inside that 25yd — which
            //     pauses on the spot. "door-blocked: no long-path corridor
            //     (13.6yd from door) -> park in place" and "can't open ... ->
            //     auto-pausing" are stamped the SAME SECOND as the flag. The
            //     pause never lifts: the floor is not a door anyone opens, so
            //     DungeonClearDoorReopenedTrigger polls it forever. 5/6 bosses.
            //
            //   191669 / 191670 / 191671 TRIBUNAL HEADS at (888.6,323.3),
            //     (887.3,367.8) and (931.0,323.8), z 205.3 — 20-35yd from the
            //     same console park spot, and shut until the Tribunal is DONE.
            //     Same landmine, three more triggers on the same standing spot.
            //
            //   193906 SJONNIR CONSOLE (1314.2,666.2,189.4) is shut inside
            //     Sjonnir's chamber, on the far side of the room the party
            //     fights him in.
            //
            // NOT listed, deliberately: 191296, Sjonnir's Door. It is the one
            // genuine gate on the map — the party really is stopped by it, and
            // the run really does need Brann to open it, so it must stay
            // navigation-VISIBLE for the at-boss stand-down to work. The other
            // four Doodad_UL_Ulduar_doors* (191292/3/4/5, 191459) spawn OPEN
            // with Data0 = 1 and are never shut by anything, so they never
            // reach the closed-door predicate at all.
            case 191527:  // Sky Room Floor (the Tribunal room's floor)
            case 191669:  // Tribunal Head, Center
            case 191670:  // Tribunal Head, Right
            case 191671:  // Tribunal Head, Left
            case 193906:  // Sjonnir Console (Brann's, post-kill)
                return true;
            default:
                return false;
        }
    }

    // Doors that shut TEMPORARILY under instance-script control and reopen
    // themselves on a timer. The bot must neither open one (the script owns the
    // GO state) nor auto-pause on one (there is nothing for a player to come
    // and solve) — it holds where it stands and the door frees it.
    //
    // Stratholme's two gate traps are the whole list. instance_stratholme's
    // Update() watches two floor positions — (3612.3,-3335.4) Scarlet side,
    // (3919.9,-3547.3) undead side — and the instant a non-GM player comes
    // within 5.5yd it slams the matching PAIR of portcullises shut, spawns
    // plagued critters on the trapped player 2s later, and reopens both gates
    // 20s after that (EVENT_GATE*_DELAY). The trap then sits on a 30-minute
    // cooldown, so a run meets it at most once per side.
    //
    // Nothing about that shape fits the pause machinery: the gates are
    // lock-free with startOpen=1 (so BotCanOpenDoorLikePlayer already refuses
    // them, and a bare Use() would fight the script's own DoUseDoorOrButton
    // toggle), and they are shut for a bounded 20s. Run tr-20260816-151006-14
    // walked its tank over the Scarlet-side trigger at Crusaders' Square and
    // auto-paused 13.1yd from the portcullis; it burned 36s of a 60s pause
    // budget before the script reopened the gate and the door-reopened trigger
    // resumed it. Holding is the correct behaviour and costs nothing.
    //
    // Deliberately NOT extended to the ziggurat / gauntlet / slaughter gates:
    // those are progress gates the run must EARN (kill the acolytes, finish the
    // gauntlet), not timers, so a hold there would be an infinite one.
    inline bool IsSelfClearing(uint32 goEntry)
    {
        switch (goEntry)
        {
            // --- Stratholme (map 329) — the two rat-trap portcullis pairs ---
            case 175350:  // Doodad_SmallPortcullis04 — gate trap 1, Scarlet side
            case 175351:  // Doodad_SmallPortcullis03 — gate trap 1, Scarlet side
            case 175354:  // Doodad_SmallPortcullis09 — gate trap 2, undead side
            case 175355:  // Doodad_SmallPortcullis08 — gate trap 2, undead side
            // --- Karazhan (map 532) — Shade of Aran's Private Library Door ---
            // 184517 at (-11189.5, -1880.9, 233.3), lock 0, spawned SHUT. Aran's
            // Reset() opens it and clears NOT_SELECTABLE, so it opens by itself
            // as soon as his grid loads under the approaching tank; his JustDied
            // opens it too. 15s after he is engaged it shuts again and goes
            // NOT_SELECTABLE for the fight. Nobody ever clicks it. Holding is
            // right in every one of those states: before his grid loads, the
            // wait is a second or two; during a fight, a member left outside is
            // locked out whatever it does (the sealed-encounter muster is what
            // prevents that); after a wipe, his Reset reopens it.
            case 184517:  // Karazhan — Private Library Door (Aran's Reset opens it)
                return true;
            default:
                return false;
        }
    }

    // Doors whose KEY requirement we deliberately waive: the bot opens them as
    // if it held the key, no item in inventory needed.
    //
    // Scarlet Monastery's Armory (Herod's Door) and Cathedral (Chapel Door)
    // both sit on lock 299 — Scarlet Key (7146) or lockpicking 175. A tank bot
    // carries neither, so an autonomous SM run parked at the wing entrance and
    // auto-paused every time, making those two wings unclearable without a
    // human handing the key over first. The doors are otherwise ordinary
    // traversal gates: no ScriptName, no AIName, no instance-script GO-state
    // control, and nothing behind them the key is meant to gate beyond the
    // wing itself (the key is a convenience item players farm from the
    // Graveyard/Library side, not an encounter lock).
    //
    // Keyed by GO ENTRY, not by lock id, for the same reason as the lists
    // above: a lock id is shared across dungeons (299 covers both the SM wing
    // gates and the Stratholme Scarlet-side doors), so only an entry list can
    // waive one door without waiving another that happens to share its lock.
    //
    // The same argument extends to Dire Maul North, Scholomance and Stratholme
    // (added 2026-08-08): every entry below is a plain traversal gate whose key
    // is a farmed convenience item, not an encounter lock. Each was verified in
    // the world DB before being listed, against the checklist this list demands:
    //
    //   * GAMEOBJECT_TYPE_DOOR with a real lock whose only slots are a key item
    //     and/or lockpicking — never a lock-free script seal (see the
    //     IsLockFreeClickable note for why lock-free is the dangerous shape).
    //   * gameobject_template_addon.flags == 34 (GO_FLAG_LOCKED | NODESPAWN):
    //     no GO_FLAG_NOT_SELECTABLE and no GO_FLAG_INTERACT_COND, so a player
    //     at the keyboard really can click them. (GO_FLAG_LOCKED is exactly
    //     what DcDoorPolicy suppresses bare-hands opening on, which is why
    //     these needed an exemption rather than just working.)
    //   * No ScriptName. Where an AIName exists it is SmartGameObjectAI whose
    //     only action is a gossip-hello SET_INST_DATA recording wing progress —
    //     and GameObject::Use() runs that GossipHello BEFORE the lock check, so
    //     the door-blocked action's Use() drives the identical sequence a keyed
    //     player does. Nothing is skipped or desynced.
    //   * The instance script, where it mentions the door at all, only calls
    //     AllowSaveToDB(true) on it (instance_stratholme / instance_scholomance)
    //     so a player-opened gate persists across a relog. It never reads or
    //     drives the GO state, so no encounter can be desynced by opening one.
    //
    // Deliberately NOT listed: keyed objects that are not doors (Stratholme's
    // postboxes and Scarlet Cannons, Scholomance's Brazier of the Herald), and
    // the script-driven lock-free gates of both dungeons (Scholomance's Kirtonos
    // gate 175570 and the seven Gandling gates, Stratholme's ziggurat doors) —
    // those are instance-script GO-state territory and stay untouched. Same
    // call in Blackrock Depths: the empty-lock-85 doors (170573/170574 Golem
    // Room, 170575 Throne Room, 170576/170577 Tomb of the Seven) and the Bar
    // Door 170571 (lock 739, Grim Guzzler Key) are all cached AND state-driven
    // by instance_blackrock_depths, so they are script territory whatever their
    // lock says; and the Relic Coffer Doors (lock 639, Relic Coffer Key) are the
    // Vault puzzle's loot cells, not a corridor the run has to walk through.
    inline bool IsKeyExempt(uint32 goEntry)
    {
        switch (goEntry)
        {
            case 101854:  // Scarlet Monastery — Herod's Door (Armory, lock 299)
            case 104591:  // Scarlet Monastery — Chapel Door (Cathedral, lock 299)

            // --- Scholomance (map 289) -----------------------------------
            // The only keyed door inside the instance; every other Scholomance
            // door/gate is lock-free (handled by IsLockFreeClickable or by the
            // instance script). Viewing Room Key (13873) drops from Doctor
            // Theolen Krastinov, i.e. from behind a boss the run may not have
            // reached yet, so a keyless party could never open it.
            case 175167:  // Viewing Room Door (lock 1199, Viewing Room Key)
            // Caer Darrow's outdoor entrance door (map 0), the door INTO
            // Scholomance. Not inside the instance, so DC only meets it on a
            // walk-in rather than a teleport-in run; listed for completeness
            // since it is a keyed Scholomance door. Autocloses after 3s, which
            // the door-blocked action's re-click cooldown already handles.
            case 174626:  // Scholomance Door (lock 1159, Skeleton Key 13704)

            // --- Stratholme (map 329) ------------------------------------
            // Scarlet side — lock 299, The Scarlet Key (7146). This is the same
            // lock as the SM wing gates above; both dungeons are now exempt, but
            // still one entry at a time.
            case 175967:  // The Bastion Door
            case 175968:  // Hoard Door
            case 176194:  // Hall of the High Command
            // Undead side — lock 879, Key to the City (12382) or lockpicking
            // 300. The two King's Square Gates carry door.autoCloseTime 3000, so
            // they re-shut ~3s after opening; the door-blocked action re-clicks
            // on its per-door cooldown rather than latching once (that latch bug
            // was found on exactly this gate).
            case 175352:  // King's Square Gate
            case 175353:  // King's Square Gate
            case 175356:  // Gauntlet Gate
            case 175357:  // Gauntlet Gate (SmartAI: gossip-hello SET_INST_DATA)
            case 175368:  // Service Entrance Gate (SmartAI: gossip-hello set data)

            // --- Dire Maul North (map 429) -------------------------------
            // The two Gordok doors already open via the map-429 events 2 and 3
            // (a conditional UseGO — see DireMaulEvents.cpp). Listing them here
            // is the belt to that braces: the events are Optional, and if one
            // misfires the run used to fall through to the door-blocked
            // auto-pause because DcDoorPolicy suppresses bare-hands opening on
            // GO_FLAG_LOCKED. Both paths end in the same GameObject::Use(), so
            // whichever fires first wins and the second is a no-op (a Use() on
            // an already-activated door returns early on lootState).
            case 177219:  // Gordok Courtyard Door (lock 1563, Gordok Courtyard Key)
            case 177217:  // Gordok Inner Door (lock 1564, Gordok Inner Door Key)
            // The North wing's Crescent Key door, in the lower corridor among
            // the Gordok Brute/Mastiff/Mage-Lord packs. Dire Maul's other two
            // lock-1562 doors (177221, 179550) are West-wing and already open
            // via map-429 events 9 and 10; this one has no event because it sits
            // off the West boss path — the exemption is its only opener.
            case 179549:  // Dire Maul North — Door (lock 1562, Crescent Key)

            // --- Blackrock Depths (map 230) ------------------------------
            // Lock 680 — the Shadowforge Key (11000), or lockpicking 250. The
            // key drops from Fineous Darkvire, so a party that killed him could
            // in principle hold it; a bot party never does, and GO_FLAG_LOCKED
            // (addon flags 34 on every entry below) makes DcDoorPolicy suppress
            // the lockpicking slots as well. All five are plain traversal gates:
            // no ScriptName, no autoCloseTime, no SmartAI (bar the lever's), and
            // instance_blackrock_depths only caches two of their GUIDs — the
            // lever's (GoShadowLockGUID) and the Lyceum's (GoLyceumGUID) — and
            // never reads or writes any of their GO states.
            //
            // Lock 680 gates the run in three places, and it is the whole set or
            // nothing: opening one only moves the auto-pause to the next.
            //
            //   * The two Shadowforge Gates (170559 at x 496 / 170560 at x 570,
            //     both on the z ~ -70 floor) are the west and east ends of the
            //     Shadowforge City concourse. 170560 is the one the route east
            //     out of Bael'Gar walks into — it was the recorded blocker in
            //     6 of 10 runs of test plan tp-20260817-171356-1, every one of
            //     them parked at 10/20 bosses with "can't open ... 170560".
            //   * The East Garrison Door (x 560, z ~ -60) is the doorway into
            //     the room that holds the lever: that floor runs x 552..620 /
            //     y -68..-36 and pinches at x ~ 560, with the lever at the far
            //     (east) end. So it has to open before the Shadowforge Lock
            //     objective can be reached at all.
            //   * The Lyceum (x 1312, z ~ -92) is the single door out of the
            //     Shadowforge City side into the back half of the dungeon. Every
            //     boss from Ambassador Flamelash and The Seven through Magmus and
            //     Emperor Dagran Thaurissan is behind it. No run has reached it
            //     yet only because 170560 stopped them first.
            //
            // The lever is listed for the same reason the two Gordok doors above
            // are: map-230 event 2 clicks it (UseGO, which bypasses DcDoorPolicy),
            // but the route to that objective ENDS on the lever, so the
            // blocking-door value flags it first and the door-blocked action would
            // auto-pause the run one step short of the click. Both paths end in
            // the same GameObject::Use(); whichever fires first wins and the other
            // is a no-op (UseDoorOrButton early-returns unless lootState is
            // GO_READY). Clicking it is the intended sequence in full: Use() runs
            // SmartGameObjectAI::GossipHello (which returns false) before the lock
            // check, reaches the DOOR branch, and the resulting GO_ACTIVATED loot
            // state fires the lever's SMART_EVENT_GO_STATE_CHANGED chain that
            // closes the Giant Doors.
            case 170559:  // Shadowforge Gate — west (lock 680, Shadowforge Key)
            case 170560:  // Shadowforge Gate — east (lock 680, Shadowforge Key)
            case 170570:  // East Garrison Door (lock 680, Shadowforge Key)
            case 170558:  // The Lyceum (lock 680, Shadowforge Key)
            case 161460:  // The Shadowforge Lock (lock 680; SmartAI closes the
                          // Giant Doors off its own state change)

            // Lock 699 — the Prison Cell Key (11140), or lockpicking 250, on the
            // eight Detention Block cell doors. Same shape as the lock-680 gates
            // above and screened the same way: GAMEOBJECT_TYPE_DOOR, addon flags
            // 34, no ScriptName, no AIName, no smart_scripts row, no conditions
            // row, and no mention anywhere in instance_blackrock_depths — the
            // instance script's door enum stops at the Lyceum. They are ordinary
            // traversal gates into the cells, and the cells are where the route
            // to Houndmaster Grebmar goes: 170567 auto-paused a run of test plan
            // tp-20260817-171356-1 at 2/20 bosses, parked 0.0yd into the door on
            // a route the diag reported as ok/1seg dev=0.5.
            //
            // Listing all eight rather than only the observed one: the boss route
            // threads several of these cells depending on where the roster's
            // anchors land, they are interchangeable in every respect the
            // checklist tests, and one-at-a-time would just replay this failure
            // from a different cell.
            case 170562:
            case 170563:
            case 170564:
            case 170565:
            case 170566:
            case 170567:
            case 170568:
            case 170569:  // Cell Door ×8 (lock 699, Prison Cell Key)
                return true;
            default:
                return false;
        }
    }

    // The MIRROR-IMAGE special case: door gameobjects carrying NO lock at all
    // (template lockId 0) that a player nonetheless opens by simply clicking
    // them — ordinary traversal gates the dungeon expects you to walk through.
    //
    // BotCanOpenDoorLikePlayer otherwise refuses every lock-free door, because
    // lockId 0 is ALSO the shape of script/event seals the bot must not pop
    // (Uldaman's Seal of Khaz'Mul, lock-free and only opened by the keystone
    // event, isn't flagged GO_FLAG_NOT_SELECTABLE until its encounter is done,
    // so the generic flag screen can't tell them apart). We can't relax the
    // lock-free rule wholesale; instead we allowlist the entries verified in
    // the world DB to be plain clickable doors — no ScriptName, no AIName, no
    // instance-script GO-state control, no SmartAI.
    //
    // Scholomance's Iron Gates (175611-175618, 175620) and plain interior Doors
    // (175610, 175619) are exactly this: lock-free, scriptless room-to-room
    // gates the player clicks open. (The dungeon's *event* gates — Kirtonos
    // 175570 and the seven Gandling gates 177371-177377 — are deliberately
    // EXCLUDED; the instance script drives their state.)
    inline bool IsLockFreeClickable(uint32 goEntry)
    {
        switch (goEntry)
        {
            // Scholomance — interior traversal gates/doors (map 289)
            case 175610:  // Door
            case 175611:  // Iron Gate
            case 175612:  // Iron Gate
            case 175613:  // Iron Gate
            case 175614:  // Iron Gate
            case 175615:  // Iron Gate
            case 175616:  // Iron Gate
            case 175617:  // Iron Gate
            case 175618:  // Iron Gate
            case 175619:  // Door
            case 175620:  // Iron Gate
            // Karazhan (map 532) — Gatehouse Door (183450, guid 27565, at
            // (-11100.6,-1992.9,49.8)), the front portcullis 11yd from the Main
            // Entrance zone-in (areatrigger 4131) and the first thing on EVERY
            // route into the raid. Lock 0, startOpen 0, autoClose 3000ms, template
            // addon flags 0, no ScriptName/AIName, no smart_scripts or conditions
            // row, and instance_karazhan never registers it (its door table covers
            // the stage, library, Netherspite, Gamesman's Hall and terrace doors
            // only) — nothing but a click ever opens it. Refused as lock-free, it
            // auto-paused tr-20260923-165858-1 14s in, parked 7.8yd short of it,
            // 0/10 bosses.
            case 183450:  // Karazhan — Gatehouse Door
            // Karazhan — Strange Bookcase (184453, guid 26721, at
            // (-11151.0,-1753.6,202.5)), the secret door into Terestian
            // Illhoof's room. Lock 0, no flags, no ScriptName/AIName, no
            // smart_scripts row, and instance_karazhan never mentions it. It is
            // the ONLY way in: with the navmesh around it cut, both
            // Curator->Terestian and Midnight->Terestian become unroutable, and
            // the room behind it is a dead end. A player opens it by clicking.
            case 184453:  // Karazhan — Strange Bookcase (Terestian's room)
            // Karazhan — Gamesman's Hall Door (184276, guid 28214, at
            // (-11120.0,-1826.9,241.8)), the way into the chess hall from its west
            // balcony. Lock 0, spawns CLOSED (state 1), autoClose 3000ms, template
            // addon flags 0, no ScriptName/AIName, no smart_scripts or conditions
            // row, and instance_karazhan never registers it (it tracks only the
            // hall's EXIT door, 184277). Nothing but a click ever opens it.
            case 184276:  // Karazhan — Gamesman's Hall Door (chess hall entry)
                return true;
            default:
                return false;
        }
    }
}

#endif
