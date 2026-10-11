/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DungeonWingRegistry.h"

#include <algorithm>
#include <cctype>
#include <unordered_map>

#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"

namespace
{
    // mapId -> wings. Boss entries are the ENCOUNTER_CREDIT_KILL_CREATURE
    // credit-entries from instance_encounters, i.e. exactly what BossSpawnIndex
    // keys its list on, so a wing's entries match the "dungeon bosses" output
    // 1:1. Every boss of a split map must appear in exactly one wing — any
    // entry left out would silently never be cleared.
    //
    // Each split map's layout lives in its own Data/Events/<Dungeon>Events.cpp
    // as a Register<Dungeon>Wings appender (a dungeon's whole clear definition —
    // event rows, roster patch, wings — in one file). This aggregator calls each
    // EXPLICITLY: same link-safety reason as the event/roster tables — a
    // self-registering initializer in a TU the program never references is
    // stripped from the module static lib (Maraudon's file has no events, so the
    // calls here and in BossRosterRegistry are what keep it linked).
    std::unordered_map<uint32, DungeonWingLayout> const& Store()
    {
        static std::unordered_map<uint32, DungeonWingLayout> const store = []
        {
            std::unordered_map<uint32, DungeonWingLayout> s;
            RegisterDireMaulWings(s);
            RegisterScarletMonasteryWings(s);
            RegisterMaraudonWings(s);
            RegisterBlackrockSpireWings(s);
            RegisterBlackrockDepthsWings(s);
            return s;
        }();
        return store;
    }
}

DungeonWingLayout const* DungeonWingRegistry::Get(uint32 mapId)
{
    auto const& s = Store();
    auto it = s.find(mapId);
    return it == s.end() ? nullptr : &it->second;
}

std::string DungeonWingRegistry::WingName(uint32 mapId, uint32 bossEntry)
{
    DungeonWingLayout const* layout = Get(mapId);
    if (!layout)
        return "";
    for (DungeonWing const& wing : layout->wings)
        for (uint32 entry : wing.bossEntries)
            if (entry == bossEntry)
                return wing.name;
    return "";
}

namespace
{
    bool EqualsNoCase(std::string const& a, std::string const& b)
    {
        return a.size() == b.size() &&
               std::equal(a.begin(), a.end(), b.begin(), [](char x, char y)
                          {
                              return std::tolower(static_cast<unsigned char>(x)) ==
                                     std::tolower(static_cast<unsigned char>(y));
                          });
    }
}

DungeonWing const* DungeonWingRegistry::FindWing(DungeonWingLayout const& layout, std::string const& key)
{
    if (key.empty())
        return nullptr;
    for (DungeonWing const& wing : layout.wings)
        if ((!wing.token.empty() && EqualsNoCase(wing.token, key)) || EqualsNoCase(wing.name, key))
            return &wing;
    return nullptr;
}

DungeonWing const* DungeonWingRegistry::WingForLfgDungeon(uint32 mapId, uint32 lfgDungeonId)
{
    DungeonWingLayout const* layout = Get(mapId);
    if (!layout || !lfgDungeonId)
        return nullptr;
    for (DungeonWing const& wing : layout->wings)
        if (wing.lfgDungeonId == lfgDungeonId)
            return &wing;
    return nullptr;
}

DungeonWing const* DungeonWingRegistry::WingOf(uint32 mapId, uint32 bossEntry)
{
    DungeonWingLayout const* layout = Get(mapId);
    if (!layout)
        return nullptr;
    for (DungeonWing const& wing : layout->wings)
        if (std::find(wing.bossEntries.begin(), wing.bossEntries.end(), bossEntry) !=
            wing.bossEntries.end())
            return &wing;
    return nullptr;
}
