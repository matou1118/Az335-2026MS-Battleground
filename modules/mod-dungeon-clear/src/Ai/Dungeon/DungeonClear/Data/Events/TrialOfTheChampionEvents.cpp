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
#include "Ai/Dungeon/DungeonClear/Util/DcTocDriverDecision.h"

// --- Trial of the Champion (map 650) -----------------------------------------
//
// The constants, and the reasoning behind them, live in namespace
// DcTrialOfTheChampion in DungeonEventTables.h. This file is the authored shape:
// three anchored counter holds, one conditional driver, one roster patch.
//
// THE SHAPE IS THE CULLING'S, OBJECTIVES-ONLY. Each encounter is an objective
// whose event walks the tank to an anchor in the bowl and holds there until the
// progress counter passes that encounter. The holds do nothing else — they do
// not gossip, pull, mount or muster. That is the driver's job, and the driver
// outranks them (EventDue 31 > AtObjective 30) whenever it claims the tick.
//
// A WIPE REWIND NEVER TOUCHES THE HOLDS. Progress 1-4 falls back to 0 and 7 to 6,
// but the holds wait on 6, 8 and 9, which no rewind can reach early; they simply
// keep holding while the driver re-does the clicks.

namespace
{
    using namespace DcTrialOfTheChampion;

    // --- event 4's gate: the arena is ours until the Knight is dead -------------
    //
    // DUE for the whole run. That is not the usual shape for this list, and the
    // near-gate rule it would normally break does not apply: the map IS the arena.
    // The party lands 57yd from the centre of a bowl ~110yd across, every actor
    // the driver deals with stands inside it, and the driver owns its own
    // movement — there is no "far from the anchor" on this map to fire from.
    bool TocDriverDue(Player* bot, AiObjectContext* /*context*/)
    {
        if (!bot || bot->GetMapId() != MAP_ID)
            return false;

        InstanceScript* inst = DcTargeting::GetInstanceScript(bot);
        return inst && inst->GetData(DATA_INSTANCE_PROGRESS) < PROGRESS_FINISHED;
    }
}

bool TocFollowerMountsItself(Player* bot)
{
    if (!bot || bot->GetMapId() != MAP_ID)
        return false;

    InstanceScript* inst = DcTargeting::GetInstanceScript(bot);
    if (!inst)
        return false;

    return DcTocDriver::FollowerMountsItself(bot->GetMapId(), inst->GetData(DATA_INSTANCE_PROGRESS),
                                             bot->IsAlive(), bot->GetVehicle() != nullptr);
}

std::vector<uint32> const& TocChampionEntries()
{
    static std::vector<uint32> const kEntries = {
        NPC_MOKRA, NPC_ERESSEA, NPC_RUNOK, NPC_ZULTORE, NPC_VISCERI,
        NPC_JACOB, NPC_AMBROSE, NPC_COLOSOS, NPC_JAELYNE, NPC_LANA,
    };
    return kEntries;
}

std::vector<uint32> const& TocSoldierEntries()
{
    static std::vector<uint32> const kEntries = {
        NPC_ARGENT_MONK, NPC_ARGENT_PRIESTESS, NPC_ARGENT_LIGHTWIELDER,
    };
    return kEntries;
}

void RegisterTrialOfTheChampionEvents(std::vector<DungeonEvent>& out)
{
    // (1)-(3) THE THREE ENCOUNTERS — anchored on OBJ(1..3).
    //
    // A leading MoveTo onto the anchor, then a garrison on the counter. The
    // leading MoveTo is the sticky-at-step-1 rule: a persistent anchored event
    // only lets the tank leave its anchor once its front step is a MoveTo, and
    // the joust, the soldier pulls and the Knight's fight all range across the
    // bowl. The garrison re-parks the tank after each fight and, once arrived,
    // yields to NeedsRest when the party is short (EventRestDecision).
    //
    // Out of combat only, and while the tank is on a horse it never runs at all:
    // the driver holds every out-of-combat tick of progress 0-4.
    out.push_back(
        EventBuilder(MAP_ID, EVENT_CHAMPIONS, "The Grand Champions")
            .Anchored(ORDER_CHAMPIONS)
            .Persistent()
            .MoveTo(CHAMPIONS_X, CHAMPIONS_Y, CHAMPIONS_Z, OBJ_ARRIVE)
            .MoveToHoldUntilInstanceData(CHAMPIONS_X, CHAMPIONS_Y, CHAMPIONS_Z, HOLD_RADIUS,
                                         DATA_INSTANCE_PROGRESS, PROGRESS_CHAMPIONS_DEAD)
                .Timeout(CHAMPIONS_TIMEOUT_MS)
            .Build());

    out.push_back(
        EventBuilder(MAP_ID, EVENT_ARGENT, "The Argent Challenge")
            .Anchored(ORDER_ARGENT)
            .Persistent()
            .MoveTo(ARGENT_X, ARGENT_Y, ARGENT_Z, OBJ_ARRIVE)
            .MoveToHoldUntilInstanceData(ARGENT_X, ARGENT_Y, ARGENT_Z, HOLD_RADIUS,
                                         DATA_INSTANCE_PROGRESS, PROGRESS_ARGENT_CHALLENGE_DIED)
                .Timeout(ARGENT_TIMEOUT_MS)
            .Build());

    out.push_back(
        EventBuilder(MAP_ID, EVENT_BLACK_KNIGHT, "The Black Knight")
            .Anchored(ORDER_BLACK_KNIGHT)
            .Persistent()
            .MoveTo(KNIGHT_X, KNIGHT_Y, KNIGHT_Z, OBJ_ARRIVE)
            .MoveToHoldUntilInstanceData(KNIGHT_X, KNIGHT_Y, KNIGHT_Z, HOLD_RADIUS,
                                         DATA_INSTANCE_PROGRESS, PROGRESS_FINISHED)
                .Timeout(KNIGHT_TIMEOUT_MS)
            .Build());

    // (4) THE CHAMPION'S ARENA — the conditional driver.
    //
    // ONE Custom step for the Culling wave-driver reason: what this dungeon needs
    // is a standing preference re-decided every tick from a counter that can go
    // backwards, not a sequence a rewind would leave half-run.
    //
    // DRIVES IN COMBAT only so it can SEE combat ticks and hand them straight
    // back; it never steers under fire (the joust is mod-playerbots', every other
    // fight is the stock engine's), and its combat rung (61) sits below
    // `toc mounted` (66) anyway.
    //
    // STEPS OWN MOVEMENT because it rides the tank's horse itself — the per-tick
    // position hold would cancel that move before the hook saw it — and because
    // it is what makes a Done return YIELD the tick.
    //
    // OWNS THE PULL: the arena has no trash, only encounters, and the advanced
    // pull's drag-back-to-a-clear-camp answer to the soldiers would walk the
    // party away from the packs it has to kill.
    //
    // REPEATABLE because it never completes — the predicate going false at
    // progress 9 is what ends it — and so that its step timeout re-arms rather
    // than skips. PERSISTENT so a combat gap cannot rewind it.
    out.push_back(
        EventBuilder(MAP_ID, EVENT_DRIVER, "The Champion's Arena")
            .Conditional(&TocDriverDue)
            .Repeatable()
            .Persistent()
            .Optional()
            .OwnsThePull()
            .DrivesInCombat()
            .StepsOwnMovement()
            .Custom(HOOK_TOC_DRIVER)
                .Timeout(DRIVER_TIMEOUT_MS)
            .Build());
}

// --- the roster: three objectives, no boss rows ---------------------------------
//
// encounterIndex 0 on every row: an objective's index is an ordering hint only,
// and given a real bit the completed-mask check would read the objective done the
// instant its encounter credit landed — for the Knight, a tick before the counter
// the hold waits on. The order is orderOverride alone.
void RegisterTrialOfTheChampionRoster(std::vector<BossRosterPatch>& t)
{
    using namespace DcRoster;

    BossRosterPatch p;
    p.mapId = MAP_ID;
    p.add = {
        MakeObjective(OBJ(EVENT_CHAMPIONS), /*encounterIndex*/ 0, MAP_ID,
                      "The Grand Champions",
                      CHAMPIONS_X, CHAMPIONS_Y, CHAMPIONS_Z, OBJ_ARRIVE,
                      /*gateEntry*/ 0, /*hook*/ 0, /*eventId*/ EVENT_CHAMPIONS,
                      /*orderOverride*/ ORDER_CHAMPIONS),

        MakeObjective(OBJ(EVENT_ARGENT), /*encounterIndex*/ 0, MAP_ID,
                      "The Argent Challenge",
                      ARGENT_X, ARGENT_Y, ARGENT_Z, OBJ_ARRIVE,
                      /*gateEntry*/ 0, /*hook*/ 0, /*eventId*/ EVENT_ARGENT,
                      /*orderOverride*/ ORDER_ARGENT),

        MakeObjective(OBJ(EVENT_BLACK_KNIGHT), /*encounterIndex*/ 0, MAP_ID,
                      "The Black Knight",
                      KNIGHT_X, KNIGHT_Y, KNIGHT_Z, OBJ_ARRIVE,
                      /*gateEntry*/ 0, /*hook*/ 0, /*eventId*/ EVENT_BLACK_KNIGHT,
                      /*orderOverride*/ ORDER_BLACK_KNIGHT),
    };
    t.push_back(std::move(p));
}
