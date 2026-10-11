/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DUNGEONCLEARDPSTARGETVALUE_H
#define _PLAYERBOT_DUNGEONCLEARDPSTARGETVALUE_H

#include <string>

#include "DpsTargetValue.h"
#include "Value.h"
#include "Ai/Dungeon/DungeonClear/DcValueKeys.h"

class PlayerbotAI;
class Unit;

// THE HOLE IN THE TARGET-EXCLUSION REGISTRY, and the two values that close it.
//
// DcTargetExclusionRegistry answers "will killing this RIGHT NOW lose the run",
// and it reaches the stock engine through
// DungeonClearCombatStrategy::AppendTargetExclusions ->
// GatherStrategyTargetExclusions -> TargetValue::FindTarget, which drops the
// barred guid out of the attacker sweep. That is a complete guard only if the
// sweep is what decides. On the encounter the registry was written for, it is not:
//
//     Unit* DpsTargetValue::Calculate()
//     {
//         Unit* rti = RtiTargetValue::Calculate();
//         if (rti)
//             return rti;                 // <- returns HERE, exclusions unread
//         ...
//     }
//
// and `bwl razorgore mark boss` paints the moon icon on Razorgore for exactly as
// long as an egg stands, so every DPS in the raid was handed the one creature
// whose phase-1 death casts 20038 and instakills all forty of them. Measured on
// tr-20260827-233058-1: the tank pulled at 23:31:36, Razorgore was dead at
// 23:31:44, and seventeen of twenty-five bots died with him.
//
// THE MARK IS NOT THE BUG and is not touched here. It is how the off-tank knows to
// pick the boss up between mind controls, which is how the fight is played. What
// was missing is a brake that does not go through the icon.
//
// So these two decorators front the stock `dps target` / `dps aoe target` values
// by name (DC's context is Add()ed after playerbots' base context and the merged
// creator map is last-wins — see NamedObjectContext::Add), delegate to the stock
// calculation unchanged, and intervene in exactly one case: the stock answer is a
// creature the registry bars for this bot on this map at this moment. Then they
// re-run the SAME stock pick with the raid icon disarmed, so what comes back is
// the ordinary attacker sweep's answer — the adds — with the exclusion pass
// applied to it, and never a hand-rolled second opinion about what a DPS should
// shoot.
//
// Disarming the icon needs no playerbots change either: RtiTargetValue reads its
// icon out of a value it is NAMED with, so a second picker constructed against
// DcKey::NoRti (a string value that always reads "") resolves GetRtiIndex("") to
// -1, returns nullptr from the short-circuit, and falls through to the sweep.
//
// Off a map with exclusion rows — which is every map but Blackwing Lair today —
// the answer is the stock object's, returned verbatim, for one table scan keyed on
// the map id. Tanks are untouched here as well as in the registry: they read `tank
// target`, and somebody must still HOLD the creature everyone else is barred from.
//
// Related: DungeonClearHoldFireTrigger, which takes a barred creature back off a
// bot that had already acquired it — the case no target picker can reach.

// The empty icon name. See DcKey::NoRti.
class DungeonClearNoRtiValue : public CalculatedValue<std::string>
{
public:
    DungeonClearNoRtiValue(PlayerbotAI* botAI)
        : CalculatedValue<std::string>(botAI, DcKey::NoRti)
    {
    }

    std::string Calculate() override { return ""; }
};

class DungeonClearDpsTargetValue : public DpsTargetValue
{
public:
    DungeonClearDpsTargetValue(PlayerbotAI* botAI)
        : DpsTargetValue(botAI, "rti", DcKey::Stock::DpsTarget),
          unmarked(botAI, DcKey::NoRti, "dungeon clear dps target unmarked")
    {
    }

    Unit* Calculate() override;

private:
    // The same picker with the raid-icon short-circuit disarmed. Held as a member
    // rather than built per call: a target pick happens several times a second on
    // every bot in the raid.
    DpsTargetValue unmarked;
};

class DungeonClearDpsAoeTargetValue : public DpsAoeTargetValue
{
public:
    DungeonClearDpsAoeTargetValue(PlayerbotAI* botAI)
        : DpsAoeTargetValue(botAI, "rti", DcKey::Stock::DpsAoeTarget),
          unmarked(botAI, DcKey::NoRti, "dungeon clear dps aoe target unmarked")
    {
    }

    Unit* Calculate() override;

private:
    DpsAoeTargetValue unmarked;
};

#endif
