/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_OBJECTIVEHOOKREGISTRY_H
#define _PLAYERBOT_OBJECTIVEHOOKREGISTRY_H

#include <functional>
#include <unordered_map>

#include "Common.h"

class Player;
class AiObjectContext;
struct DungeonBossInfo;

// Outcome of an objective's on-arrival hook.
enum class ObjectiveArriveResult
{
    Done,     // objective satisfied — latch it cleared and advance the clear
    Running,  // still working (e.g. mid-interaction) — hold and call again next tick
    Blocked,  // cannot complete unaided (needs the human) — stall the run
};

// Optional per-objective behaviour run when the tank reaches a travel objective
// (DungeonBossInfo::onArriveHook indexes this registry). The DEFAULT — no hook,
// id 0, or an unregistered id — is `Done`: the tank simply arrives and the clear
// advances. Registering a hook lets an objective DO something on arrival (use a
// gameobject, wait for a spawn, cast, etc.); the hook returns Running until it's
// finished and the tank holds at the anchor meanwhile.
//
// Hooks live in a hardcoded table (link-safe in the static-lib build, like the
// other DungeonClear registries). The table ships empty with a documented
// example; add a row and reference its id from a BossRosterRegistry objective.
class ObjectiveHookRegistry
{
public:
    using Hook = std::function<ObjectiveArriveResult(Player*, AiObjectContext*, DungeonBossInfo const&)>;
    using HookTable = std::unordered_map<uint32, Hook>;

    // Registers `hook` under `id` in a table under construction. Use this rather
    // than table.emplace() from the per-dungeon appenders below: hook ids are one
    // FLAT space shared by every dungeon (unlike event conditions, which are
    // function pointers and need no ids at all), so a copy-paste that reuses an id
    // is a live hazard. emplace() would silently keep the first row and drop the
    // second — the losing dungeon's objective then latches Done and its event
    // never runs, with nothing in the log to say so. This LOG_ERRORs the collision
    // and keeps the first row; the registry-integrity gtest fails on it at author
    // time. id 0 is reserved for "no hook" and is rejected the same way.
    static void AddHook(HookTable& table, uint32 id, Hook hook);

    // Runs the hook for `hookId`. hookId 0 means "no hook" and returns Done (the
    // arrive-then-continue default). A NON-ZERO hookId that is unregistered is an
    // authoring error — the objective/step meant to DO something — so it returns
    // Blocked (and LOG_WARNs once) rather than silently latching Done. See F2 in
    // the events-system review; the registry-integrity gtest catches it at author
    // time, this is the runtime backstop.
    static ObjectiveArriveResult Run(uint32 hookId, Player* bot, AiObjectContext* context,
                                     DungeonBossInfo const& info);

    // True if `hookId` names a registered hook (cheap authoring sanity gate;
    // hookId 0 is "no hook" and returns false). Used by the registry-integrity
    // gtest and callers that want to distinguish "no hook" from "broken hook".
    static bool Has(uint32 hookId);
};

// Generic hook 40: the RAID MUSTER, at an objective. DcRaidMuster (stage the
// raid on the tank, top everyone off, run the rebuff round, bounded by its own
// ceiling) normally gates only the engage of a BOSS anchor. An event that starts
// a raid encounter itself — Karazhan's Blackened Urn summons Nightbane — needs
// the same muster before its trigger step, and its anchor is an objective. The
// hook runs the muster against the current objective as if it were the boss:
// Running while it holds, Done once it releases. Raid maps only (Done
// elsewhere, like the muster itself). The objective action yields rest ticks to
// this step, so the tank can drink under the muster's 100/100 rest override.
inline constexpr uint32 DC_HOOK_RAID_MUSTER = 40;

// --- per-dungeon hook appenders ------------------------------------------
// A dungeon gets its own hook TU only when its on-arrival behaviour is a
// CONTROLLER rather than a one-shot action — i.e. when it re-decides from live
// world state every tick and has to own movement/engagement/selection itself.
// One-shot arrival actions (fire a cannon, grant an item, poke an NPC) stay in
// ObjectiveHookRegistry.cpp's table, where they are homogeneous and short; there
// is no gain in scattering nine ~90-line functions across nine files.
//
// Called EXPLICITLY from Hooks() for the same reason the event tables are: this
// module compiles into a static lib, so a TU whose only output is constructor
// side-effects gets dropped by the linker and its hooks silently vanish.
//
// The Black Morass (map 269) — the wave driver. See BlackMorassDriver.cpp.
void RegisterBlackMorassHooks(ObjectiveHookRegistry::HookTable& out);

// The Violet Hold (map 608) — the siege start, the three defend garrisons and
// the wave driver. See VioletHoldDriver.cpp. Ids 15-19.
void RegisterVioletHoldHooks(ObjectiveHookRegistry::HookTable& out);

// Blackwing Lair (map 469) — Razorgore's orb and egg run, the one hook that
// drives INSIDE a raid boss encounter. See BlackwingLairDriver.cpp. Id 20.
void RegisterBlackwingLairHooks(ObjectiveHookRegistry::HookTable& out);

// Halls of Stone (map 599) — the Tribunal of Ages garrison (and the one recovery
// path this dungeon needs, for a dead Brann) plus the wave driver. See
// HallsOfStoneDriver.cpp. Ids 22-23.
void RegisterHallsOfStoneHooks(ObjectiveHookRegistry::HookTable& out);

// Halls of Lightning (map 602) — the Slag Furnace transit, the Bjarngrim ->
// Volkhan leg the ordinary clear has no driver for. See
// HallsOfLightningDriver.cpp. Id 24.
void RegisterHallsOfLightningHooks(ObjectiveHookRegistry::HookTable& out);

// Utgarde Pinnacle (map 575) — the two areatrigger starts, the Grauf harpoon
// driver and the Svala ritual retarget. See UtgardePinnacleDriver.cpp. Ids 25-28.
//
// Three of the four are the SHORT kind that would ordinarily live in this file's
// own table. They are in a TU of their own because they share map 575's constants
// and its reasoning with the driver, and splitting one dungeon's four hooks across
// two files to satisfy a size rule would cost more than it saves.
void RegisterUtgardePinnacleHooks(ObjectiveHookRegistry::HookTable& out);

// Pit of Saron (map 658) — the Ymirjar gauntlet driver and the Tyrannus ledge
// gate. See PitOfSaronDriver.cpp. Ids 29-30.
//
// The gauntlet hook is the controller: three edge-ordered areatrigger gates and
// two summoned waves, with the arithmetic in the pure kernel
// Util/DcPosGauntletDecision.h. The ledge hook is the short kind that would
// ordinarily live in this file's own table; it is in the driver's TU because it
// shares map 658's constants and its areatrigger forge with the controller.
void RegisterPitOfSaronHooks(ObjectiveHookRegistry::HookTable& out);

// Halls of Reflection (map 668) — the intro gossip, the altar wave driver, the
// throne-room cutscene, the point-of-no-return gossip and the escape driver.
// See HallsOfReflectionDriver.cpp. Ids 31-35.
//
// TWO of the five are controllers, which is one more than any other map in the
// module has, and that is what this dungeon is: hooks 32 and 35 own the party
// for roughly three quarters of the run (there is nothing to pull between the
// first gossip and Marwyn's death, and nothing but a footrace after the second).
// Their arithmetic is in the pure kernels Util/DcHorWaveDecision.h and
// Util/DcHorEscapeDecision.h. Hooks 31, 33 and 34 are the short kind that would
// ordinarily live in this file's own table; they are in the driver's TU because
// they share map 668's constants and its areatrigger forge with the controllers.
void RegisterHallsOfReflectionHooks(ObjectiveHookRegistry::HookTable& out);

// The Culling of Stratholme (map 595) — the ten-wave controller, and nothing else.
// See CullingOfStratholmeDriver.cpp. Id 36.
//
// ONE hook for a whole dungeon is the point worth recording: nothing on map 595 is
// started by an areatrigger (so no packet has to be forged the way Pit of Saron's
// and Utgarde Pinnacle's do), every gossip on the critical path is reachable
// through the declarative Gossip step or the escort driver's resume branch, and the
// five crates are the new UseItemAt step. The waves are the single thing with no
// declarative expression, because what they need is a standing preference
// re-decided every tick rather than a sequence. Its arithmetic is in the pure
// kernel Util/DcCosWaveDecision.h.
void RegisterCullingOfStratholmeHooks(ObjectiveHookRegistry::HookTable& out);

// Trial of the Champion (map 650) — the arena driver: the muster, the three
// announcer clicks (the first from horseback), and the Argent side-pack pulls.
// See TrialOfTheChampionDriver.cpp. Id 37. Its decisions are the pure kernel
// Util/DcTocDriverDecision.h; the joust itself is mod-playerbots' `wotlk-toc`.
void RegisterTrialOfTheChampionHooks(ObjectiveHookRegistry::HookTable& out);

// The Oculus (map 578) — the flight driver (38: muster, fly, land, Eregos) and the
// Eregos hold's telemetry (39). See OculusDriver.cpp. Its decisions are the pure
// kernel Util/DcOculusDriverDecision.h; each member flies its own drake from the
// rider rung (Action/DcOculusRiderAction.cpp).
void RegisterOculusHooks(ObjectiveHookRegistry::HookTable& out);

// Karazhan (map 532) — the chess event: arm the conductor (43) and report its
// verdict back to the event (42). See KarazhanChessDriver.cpp. The game itself is
// the conductor inside the member rung (Action/DcChessPieceAction.cpp); its
// decisions are the pure kernels Util/DcChessBoard.h, Util/DcChessDecision.h and
// Util/DcChessConductor.h.
void RegisterKarazhanChessHooks(ObjectiveHookRegistry::HookTable& out);

#endif
