/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DUNGEONCLEARSTRATEGY_H
#define _PLAYERBOT_DUNGEONCLEARSTRATEGY_H

#include "Strategy.h"

class PlayerbotAI;

class DungeonClearStrategy : public Strategy
{
public:
    DungeonClearStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}
    std::string const getName() override { return "dungeon clear"; }
    uint32 GetType() const override { return STRATEGY_TYPE_NONCOMBAT; }
    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
    void InitMultipliers(std::vector<Multiplier*>& multipliers) override;
};

// Combat-engine companion to "dungeon clear". Holds the advanced-pull maneuver and
// the in-combat follower assist/hold triggers — the in-combat halves that can't live
// in the non-combat strategy because the bot runs its combat engine the instant it
// aggros. Resident on every bot's combat engine but inert unless that bot is the
// leader and mid-pull, or a follower assisting the tank. Its ONE multiplier
// (DungeonClearCombatMultiplier) touches only the stock "drop target" so the
// flip-early assist can hold the combat engine; nothing else in combat is altered.
class DungeonClearCombatStrategy : public Strategy
{
public:
    DungeonClearCombatStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}
    std::string const getName() override { return "dungeon clear combat"; }
    uint32 GetType() const override { return STRATEGY_TYPE_COMBAT; }
    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
    void InitMultipliers(std::vector<Multiplier*>& multipliers) override;

    // TARGET SELECTION, and the one place DC reaches into it.
    //
    // GatherStrategyTargetExclusions walks EVERY strategy on the bot's combat
    // engine — not a hardcoded list of raid strategies — so overriding this pair
    // here gives dungeon-clear the same authority over what the raid shoots that
    // `moltencore` and `karazhan` have, without a line of mod-playerbots. That is
    // what lets an encounter DC drives keep its combat guard next to the driver
    // that needs it (Razorgore: killing him before the eggs are gone instakills
    // the raid). The rows live in Data/DcTargetExclusionRegistry.
    //
    // HasTargetExclusions is the cheap gate mod-playerbots caches per engine, and
    // it is deliberately MAP-KEYED rather than a flat `true`: a flat true makes
    // every DC bot on every map rebuild its combat strategy-name list on every
    // target pick. It stays fresh because the cache is recomputed on every
    // strategy add/remove and ApplyInstanceStrategies does both on every map
    // change.
    void AppendTargetExclusions(GuidSet& exclusions, TargetValueExclusionType type) override;
    bool HasTargetExclusions() const override;
};

#endif
