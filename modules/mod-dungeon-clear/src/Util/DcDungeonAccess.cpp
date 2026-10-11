/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "Util/DcDungeonAccess.h"

#include "AreaDefines.h"
#include "DBCStores.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "StringFormat.h"

namespace
{
    // The death knight class spell the Ebon Hold intro chain grants; the core
    // uses knowing it as the proof that a death knight has finished the
    // starting zone and may leave it (Player::TeleportTo, and again in the
    // battleground join handler).
    constexpr std::uint32_t SPELL_DEATH_GATE = 50977;

    void Append(std::string& summary, std::string const& item)
    {
        if (!summary.empty())
            summary += ", ";
        summary += item;
    }

    // HALLS OF REFLECTION (668) ONLY: the quest that unlocks the SHORTCUT.
    //
    // This is the one grant here that is not an entry gate. The dungeon's whole
    // first act is a gossip on Jaina/Sylvanas offering two options, and both are
    // gated on the SELECTING player's quest log:
    //
    //   option 0  "Can you remove the sword?"   24710 (A) / 24712 (H)  -> 224.5s
    //   option 1  "...I think I hear Arthas"    24500 (A) / 24802 (H)  ->  75.5s
    //
    // The first of those IS the dungeon_access_requirements row, so the loop below
    // already rewards it and every bot the harness teleports in can start the full
    // intro. The second is not required for anything; it only shortens the
    // cutscene, and without it a party stands in an entrance corridor for three
    // and a half minutes per run.
    //
    // WHY IT IS GRANTED ANYWAY, and what it costs. A ten-run battery on this map
    // pays TWENTY-FIVE MINUTES for cutscenes that are identical in every
    // mechanical respect: the skip path reaches the same DATA_INTRO, the same
    // StartNextWave and the same wave 1, and the only things it removes are yells
    // and RP walks. The plan's own test note asks for at least a third of the
    // battery to run the FULL path anyway, which is what an ordinary human party
    // gets — that is a run-selection question, not a reason to make every run slow.
    //
    // Faction is read from the ORIGINAL team, exactly as the access-requirement
    // loop below does, so a bot never receives the opposite faction's quest.
    // The map id is the CORE's own MAP_HALLS_OF_REFLECTION (AreaDefines.h, 668),
    // which this TU already includes — Player::TeleportTo special-cases the same
    // constant for this dungeon's entry lock, so sharing it keeps the two halves
    // of "can this bot get in" naming the same map.
    constexpr std::uint32_t QUEST_WRATH_OF_THE_LICH_KING_A = 24500;
    constexpr std::uint32_t QUEST_WRATH_OF_THE_LICH_KING_H = 24802;
}

namespace DcDungeonAccess
{
    bool UnlockTravel(Player* bot)
    {
        if (!bot || !bot->IsClass(CLASS_DEATH_KNIGHT, CLASS_CONTEXT_TELEPORT))
            return false;
        if (bot->GetMapId() != MAP_EBON_HOLD || bot->HasSpell(SPELL_DEATH_GATE))
            return false;

        bot->learnSpell(SPELL_DEATH_GATE);
        LOG_INFO("playerbots.dungeonclear",
                 "ACCESS {} is a death knight still on Acherus without Death Gate — taught it "
                 "{} so it can leave the starting zone",
                 bot->GetName(), SPELL_DEATH_GATE);
        return true;
    }

    std::string GrantEntry(Player* bot, std::uint32_t mapId, Difficulty difficulty)
    {
        if (!bot)
            return {};

        std::string summary;
        if (UnlockTravel(bot))
            Append(summary, Acore::StringFormat("spell {} (Death Gate)", SPELL_DEATH_GATE));

        // The Halls of Reflection intro shortcut — not an entry gate, so it is
        // granted BEFORE the requirement lookup and survives the `!ar` early
        // return below. See the note by the constants.
        if (mapId == MAP_HALLS_OF_REFLECTION)
        {
            std::uint32_t const skip = bot->GetTeamId(true) == TEAM_HORDE
                                           ? QUEST_WRATH_OF_THE_LICH_KING_H
                                           : QUEST_WRATH_OF_THE_LICH_KING_A;
            if (!bot->GetQuestRewardStatus(skip))
            {
                bot->SetRewardedQuest(skip);
                Append(summary, Acore::StringFormat("quest {} (skips the 224.5s intro)", skip));
            }
        }

        DungeonProgressionRequirements const* ar =
            sObjectMgr->GetAccessRequirement(mapId, difficulty);
        if (!ar)
            return summary;

        // Satisfy() tests a row against the bot's ORIGINAL team, and treats a
        // TEAM_NEUTRAL row as applying to everyone. Mirror both, so we never
        // hand a bot the opposite faction's attunement.
        TeamId const team = bot->GetTeamId(true);
        auto applies = [team](ProgressionRequirement const* req)
        { return req->faction == TEAM_NEUTRAL || req->faction == team; };

        for (ProgressionRequirement const* req : ar->quests)
        {
            if (!applies(req) || bot->GetQuestRewardStatus(req->id))
                continue;
            // The requirement only ever reads GetQuestRewardStatus, so marking
            // the quest rewarded is the whole grant — no reward items, no XP,
            // no chain side effects. It persists with the character, so a
            // recycled bot pays for it once.
            bot->SetRewardedQuest(req->id);
            Append(summary, Acore::StringFormat("quest {}", req->id));
        }

        for (ProgressionRequirement const* req : ar->achievements)
        {
            if (!applies(req) || bot->HasAchieved(req->id))
                continue;
            if (AchievementEntry const* entry = sAchievementStore.LookupEntry(req->id))
            {
                bot->CompletedAchievement(entry);
                Append(summary, Acore::StringFormat("achievement {}", req->id));
            }
        }

        for (ProgressionRequirement const* req : ar->items)
        {
            if (!applies(req) || bot->HasItemCount(req->id, 1))
                continue;
            if (bot->AddItem(req->id, 1))
                Append(summary, Acore::StringFormat("item {}", req->id));
        }

        if (!summary.empty())
            LOG_INFO("playerbots.dungeonclear",
                     "ACCESS granted {} entry to map {} (difficulty {}): {}", bot->GetName(),
                     mapId, std::uint32_t(difficulty), summary);

        return summary;
    }
}
