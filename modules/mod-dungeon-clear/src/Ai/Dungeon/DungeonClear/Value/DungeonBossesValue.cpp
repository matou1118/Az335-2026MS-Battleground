/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DungeonBossesValue.h"


#include "InstanceScript.h"
#include "Log.h"
#include "Map.h"
#include "Player.h"
#include "Ai/Dungeon/DungeonClear/Data/BossSpawnIndex.h"
#include "Ai/Dungeon/DungeonClear/Data/DcFactionEntrySwapRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonWingRegistry.h"
#include "Ai/Dungeon/DungeonClear/Overrides/BossRosterRegistry.h"
#include "Ai/Dungeon/DungeonClear/Util/DcRunWing.h"
#include "Ai/Dungeon/DungeonClear/Util/NavmeshSnap.h"
#include "Playerbots.h"

namespace
{
    // Snap each boss's spawn position onto the navmesh. DBC / creature_data
    // coordinates sometimes sit a few yards above the floor (alcove ledges,
    // platforms where the spawn was placed before mmap regen, etc.). Without
    // snapping, PathGenerator returns PATHFIND_FARFROMPOLY_END for those
    // bosses and Advance's at-boss trigger never matches the raw spawn pos.
    // 40yd is generous enough to catch flying-phase bosses whose anchor is
    // suspended in air (Eregos, Skadi's drake-phase landing) without
    // wandering into a different room.
    constexpr float BOSS_SNAP_RADIUS = 40.0f;

    std::vector<DungeonBossInfo> SnapAll(Map const* map, std::vector<DungeonBossInfo> bosses)
    {
        if (!map)
            return bosses;
        for (DungeonBossInfo& info : bosses)
        {
            NavmeshSnap::Result const r = NavmeshSnap::Snap(map, info.x, info.y, info.z, BOSS_SNAP_RADIUS);
            if (r.ok)
            {
                info.x = r.x;
                info.y = r.y;
                info.z = r.z;
            }
            // Snap failure leaves the original coords. The at-boss trigger
            // will then never fire for that boss and the stalled fallback
            // takes over — same behavior as before this change.
        }
        return bosses;
    }

    // Some encounters are one-sided in faction: friendly to one team, hostile to
    // the other. The friendly team can never bring them to combat, so for that
    // team the entry is reclassified from a kill BOSS to a travel OBJECTIVE — the
    // party still clears its way to the spot (identical route + trash handling),
    // but on arrival it latches and advances instead of trying (forever) to
    // engage. The hostile team keeps the normal combat boss, unchanged.
    //
    //   Uldaman (70) — The Lost Dwarves (credit Baelog 6906, faction 122):
    //   friendly to Alliance, hostile to Horde. Alliance walks up and moves on;
    //   Horde fights them. (Faction 122: ourMask/friendMask Alliance, enemyMask
    //   Horde — verified in factiontemplate_dbc.)
    struct FactionObjectiveRule
    {
        uint32 mapId;
        uint32 entry;
        TeamId friendlyTeam;  // team that treats the boss as a flyby objective
    };

    constexpr FactionObjectiveRule kFactionObjectives[] =
    {
        { 70, 6906, TEAM_ALLIANCE },  // Uldaman — The Lost Dwarves
    };

    std::vector<DungeonBossInfo> ApplyFactionObjectives(Player* bot, uint32 mapId,
                                                        std::vector<DungeonBossInfo> bosses)
    {
        TeamId const team = bot->GetTeamId();
        for (FactionObjectiveRule const& rule : kFactionObjectives)
        {
            if (rule.mapId != mapId || rule.friendlyTeam != team)
                continue;
            for (DungeonBossInfo& info : bosses)
            {
                if (info.entry != rule.entry || info.kind != DungeonAnchorKind::Boss)
                    continue;
                // Reach-then-continue: no engage, no kill-bit; the objective
                // latches into "cleared anchors" on arrival (DcObjectiveArriveAction).
                info.kind = DungeonAnchorKind::Objective;
                LOG_DEBUG("playerbots.dungeonclear",
                          "[dungeon-clear] map {} boss {} '{}' is friendly to this "
                          "team — treating as a clear-to travel objective (no engage)",
                          mapId, info.entry, info.name);
            }
        }
        return bosses;
    }

    // Bosses the instance script UpdateEntry's to the opposing faction's creature
    // depending on who opened the instance (The Nexus' Frozen Commander). The
    // rules — and the full account of what breaks without this — live in
    // DcFactionEntrySwapRegistry.h; here we only resolve the team and rewrite.
    //
    // The swap is decided by the INSTANCE's team (stamped from the group leader
    // when the map was created — MapInstanced::CreateInstance), not by the bot's
    // own team: with two-side grouping enabled a bot's race can disagree with the
    // instance it is standing in, and the creature in the world follows the
    // instance. Fall back to the bot's team only when nothing was stamped
    // (TEAM_NEUTRAL — a map that is not two-faction, where no rule can match).
    TeamId ResolveInstanceTeam(Player* bot)
    {
        if (InstanceScript* inst = bot->GetInstanceScript())
        {
            TeamId const stamped = inst->GetTeamIdInInstance();
            if (stamped != TEAM_NEUTRAL)
                return stamped;
        }
        return bot->GetTeamId();
    }

    std::vector<DungeonBossInfo> ApplyFactionEntrySwaps(Player* bot, uint32 mapId,
                                                        std::vector<DungeonBossInfo> bosses)
    {
        if (!DcFactionEntrySwap::HasRules(mapId))
            return bosses;

        TeamId const team = ResolveInstanceTeam(bot);
        for (DungeonBossInfo& info : bosses)
        {
            uint32 const resolved = DcFactionEntrySwap::Resolve(mapId, info.entry, team);
            if (resolved == info.entry)
                continue;
            LOG_DEBUG("playerbots.dungeonclear",
                      "[dungeon-clear] map {} boss '{}' is faction-swapped for this "
                      "instance's team — tracking entry {} instead of {}",
                      mapId, info.name, resolved, info.entry);
            info.entry = resolved;
        }
        return bosses;
    }

    // For split maps (Dire Maul et al.) keep only the bosses of the wing the
    // bot is in. How the wing is picked is the layout's `select`:
    //
    //   Proximity (DM, SM): the wings occupy far-apart regions of the map and the
    //   bot enters directly into one, so the nearest registered boss is reliably
    //   in-wing — including its dead/cleared bosses, whose spawn coords are
    //   static, so the choice stays stable as the bot clears its way through.
    //
    //   Explicit (Blackrock Spire): the run's latched wing (DcRunWing). LBRS and
    //   UBRS share a portal, so proximity would pick the wrong half there.
    //
    // Maps with no wing registration pass through.
    std::vector<DungeonBossInfo> FilterToCurrentWing(Player* bot, uint32 mapId,
                                                     std::vector<DungeonBossInfo> bosses)
    {
        DungeonWingLayout const* layout = DungeonWingRegistry::Get(mapId);
        if (!layout || layout->wings.empty() || bosses.empty())
            return bosses;

        // Connected-wing maps (Maraudon) are not split for filtering: every
        // wing is reachable from any entrance, so all bosses must stay in the
        // list. The wing data is a display label only — never filter, or the
        // bot would clear one region and read the whole dungeon as done.
        if (!layout->isolated)
            return bosses;

        // Explicit: never widened back to the whole map, even when the wing's
        // list comes out empty — a whole-map list is the "LBRS walks on to the
        // UBRS door" bug this selection exists to stop.
        if (layout->select == WingSelect::Explicit)
        {
            DungeonWing const* wing = DcRunWing::Resolve(bot);
            if (!wing)
                return bosses;
            std::vector<DungeonBossInfo> filtered = DcRunWing::FilterToWing(bosses, *wing);
            LOG_DEBUG("playerbots.dungeonclear",
                      "[dungeon-clear] map {} split into wings; run wing '{}' — "
                      "{} of {} bosses kept", mapId, wing->name, filtered.size(), bosses.size());
            return filtered;
        }

        std::vector<DungeonWing> const& wings = layout->wings;

        // Pick the wing owning the boss nearest the bot.
        size_t const bestWing = DcRunWing::PickByProximity(*layout, bosses, bot->GetPositionX(),
                                                           bot->GetPositionY(), bot->GetPositionZ());

        // No registered boss matched the live list — leave it untouched rather
        // than blanking it (would falsely read as "all cleared").
        if (bestWing >= wings.size())
            return bosses;

        std::vector<DungeonBossInfo> filtered = DcRunWing::FilterToWing(bosses, wings[bestWing]);

        LOG_DEBUG("playerbots.dungeonclear",
                  "[dungeon-clear] map {} split into wings; bot in '{}' — "
                  "{} of {} bosses kept", mapId, wings[bestWing].name,
                  filtered.size(), bosses.size());

        // Defensive: if the chosen wing somehow contributed nothing, keep the
        // full list so the bot still has a target.
        return filtered.empty() ? bosses : filtered;
    }
}

namespace
{
    // Calculate()'s pipeline up to (not including) the wing filter.
    std::vector<DungeonBossInfo> BuildRoster(Player* bot)
    {
        Map* map = bot->GetMap();
        uint32 const mapId = bot->GetMapId();

        // Apply per-dungeon roster corrections (remove wrong/event-locked bosses,
        // add real bosses or travel objectives) before wing-filtering and snapping.
        std::vector<DungeonBossInfo> roster =
            BossRosterRegistry::Apply(mapId, DcDifficulty::Of(map),
                                      BossSpawnIndex::Get(mapId, map->GetDifficulty()));

        // Per-team faction reclassification (friendly-faction bosses -> flyby
        // objectives) before wing-filtering and snapping.
        roster = ApplyFactionObjectives(bot, mapId, std::move(roster));

        // Per-team entry rewrite for bosses the instance script UpdateEntry's to the
        // opposing faction's creature (The Nexus' Frozen Commander).
        return ApplyFactionEntrySwaps(bot, mapId, std::move(roster));
    }
}

std::vector<DungeonBossInfo> DungeonBossesValue::AllWings(Player* bot)
{
    if (!bot || !bot->IsInWorld() || !bot->GetMap() || !bot->GetMap()->IsDungeon())
        return {};
    return SnapAll(bot->GetMap(), BuildRoster(bot));
}

std::vector<DungeonBossInfo> DungeonBossesValue::Calculate()
{
    if (!bot || !bot->IsInWorld())
        return {};

    Map* map = bot->GetMap();
    if (!map || !map->IsDungeon())
        return {};

    return SnapAll(map, FilterToCurrentWing(bot, bot->GetMapId(), BuildRoster(bot)));
}
