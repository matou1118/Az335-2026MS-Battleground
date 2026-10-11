/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "Util/DcPoolBots.h"

#include <algorithm>

#include "CharacterCache.h"
#include "Group.h"
#include "LFGMgr.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Player.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include "PlayerbotAI.h"
#include "PlayerbotGuildMgr.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"

#include "BgQueueFill/DcBgQueueFillManager.h"
#include "DungeonQueueFill/DcDungeonQueueFillManager.h"
#include "DungeonQueueFill/DcDungeonQueueFillPlanner.h"
#include "TestRun/DcTestRunManager.h"
#include "Util/DcDungeonAccess.h"

namespace
{
    // The finale of the death-knight starting chain, Alliance and Horde.
    // LFGMgr::InitializeLockedDungeons locks EVERY dungeon for a death knight
    // who has neither (LFG_LOCKSTATUS_QUEST_NOT_COMPLETED), and no pool
    // character has ever run it.
    constexpr std::uint32_t kDkChainQuestAlliance = 13188;  // Where Kings Walk
    constexpr std::uint32_t kDkChainQuestHorde    = 13189;  // Warchief's Blessing

    // Where a freshly logged-in bot is put down, per faction.
    //
    // Read from the world DB's `game_tele` table — the same rows `.tele
    // stormwind` and `.tele orgrimmar` use — so a server that moves its
    // capital spot moves the fill with it. The literals are 3.3.5a's stock
    // rows, kept only for a world DB that has had them deleted.
    struct SafeSpot
    {
        std::uint32_t map;
        float x, y, z, o;
    };

    SafeSpot CapitalSpot(bool alliance)
    {
        if (GameTele const* tele = sObjectMgr->GetGameTele(alliance ? "Stormwind" : "Orgrimmar",
                                                           /*exactSearch*/ true))
            return SafeSpot{tele->mapId, tele->position_x, tele->position_y, tele->position_z,
                            tele->orientation};

        return alliance ? SafeSpot{0, -8833.38f, 628.628f, 94.0066f, 1.06535f}
                        : SafeSpot{1, 1629.85f, -4373.64f, 31.5573f, 3.69762f};
    }

    // Every exclusion ClaimPoolCharacter and CountFree share.
    bool IsFree(ObjectGuid guid, std::function<bool(ObjectGuid)> const& heldByCaller)
    {
        if (heldByCaller && heldByCaller(guid))
            return false;
        // The `.dc test` harness draws from this same pool and reserves what it
        // holds; each fill holds its own.
        if (DcTestRunManager::Instance().IsReserved(guid))
            return false;
        if (DcPoolBots::IsClaimedByFill(guid))
            return false;
        if (ObjectAccessor::FindConnectedPlayer(guid))
            return false;
        // A character in a REAL guild is somebody's, pool membership or not.
        std::uint32_t const guildId = sCharacterCache->GetCharacterGuildIdByGuid(guid);
        if (guildId && PlayerbotGuildMgr::instance().IsRealGuild(guildId))
            return false;
        return true;
    }
}

namespace DcPoolBots
{
    bool IsClaimedByFill(ObjectGuid guid)
    {
        return DcDungeonQueueFillManager::Instance().IsClaimed(guid) ||
               DcBgQueueFillManager::Instance().IsClaimed(guid);
    }

    ObjectGuid ClaimPoolCharacter(bool alliance, std::uint8_t classId, std::uint32_t& drawState,
                                  std::vector<ObjectGuid> const& avoid,
                                  std::function<bool(ObjectGuid)> const& heldByCaller)
    {
        auto const& pool =
            sRandomPlayerbotMgr.addclassCache[RandomPlayerbotMgr::GetTeamClassIdx(alliance, classId)];

        std::vector<ObjectGuid> available;
        std::vector<ObjectGuid> fresh;  // available, and not on the avoid list
        for (ObjectGuid const& guid : pool)
        {
            if (!IsFree(guid, heldByCaller))
                continue;
            available.push_back(guid);
            if (std::find(avoid.begin(), avoid.end(), guid) == avoid.end())
                fresh.push_back(guid);
        }

        // Prefer somebody not on the list, but never fail over it: a pool
        // holding one paladin must still be able to field that paladin twice
        // running rather than break the fill.
        std::vector<ObjectGuid> const& candidates = fresh.empty() ? available : fresh;
        if (candidates.empty())
            return ObjectGuid::Empty;

        return candidates[DcDungeonQueueFillPlanner::NextRand(drawState) % candidates.size()];
    }

    std::uint32_t CountFree(bool alliance, std::function<bool(ObjectGuid)> const& heldByCaller)
    {
        std::uint32_t n = 0;
        for (std::uint8_t classId = CLASS_WARRIOR; classId < MAX_CLASSES; ++classId)
        {
            auto const& pool =
                sRandomPlayerbotMgr.addclassCache[RandomPlayerbotMgr::GetTeamClassIdx(alliance, classId)];
            for (ObjectGuid const& guid : pool)
                if (IsFree(guid, heldByCaller))
                    ++n;
        }
        return n;
    }

    char const* CapitalName(bool alliance)
    {
        return alliance ? "Stormwind" : "Orgrimmar";
    }

    bool EvictToCapital(Player* bot, bool alliance, char const* logTag, std::string const& jobId)
    {
        SafeSpot const spot = CapitalSpot(alliance);

        // The wait is on IsBeingTeleported, not on the destination: a far
        // teleport is a two-step handshake (the semaphore here,
        // HandleMoveWorldportAck on the bot's own AI tick), and the bot is in
        // neither map in between.
        if (bot->IsBeingTeleported())
            return false;

        if (bot->GetMapId() == spot.map && !bot->GetMap()->Instanceable() &&
            bot->GetExactDist2d(spot.x, spot.y) < 100.0f)
            return true;

        // Combat and a corpse both refuse to travel: TeleportTo itself does not
        // care, but a bot that arrives dead or still tagged is a bot the
        // sanitise step has to undo anyway, and a bot in combat is one the
        // core will not let go of cleanly.
        if (bot->IsInCombat())
            bot->CombatStop(true);
        if (!bot->IsAlive())
        {
            bot->ResurrectPlayer(1.0f);
            bot->SpawnCorpseBones();
        }

        // A recycled death knight is still standing on Acherus and cannot far
        // teleport ANYWHERE until it knows Death Gate — TeleportTo just returns
        // false, so the bot would retry this hop for the whole placement
        // window and the fill would time out with no visible reason.
        DcDungeonAccess::UnlockTravel(bot);

        if (!bot->TeleportTo(spot.map, spot.x, spot.y, spot.z, spot.o))
        {
            LOG_WARN("playerbots.dungeonclear",
                     "{} {} could not move {} to {} (map {}) from map {} — retrying", logTag, jobId,
                     bot->GetName(), CapitalName(alliance), spot.map, bot->GetMapId());
        }
        return false;
    }

    bool CreditDeathKnightChain(Player* bot)
    {
        if (!bot->IsClass(CLASS_DEATH_KNIGHT) || bot->IsQuestRewarded(kDkChainQuestAlliance) ||
            bot->IsQuestRewarded(kDkChainQuestHorde))
            return false;

        bot->SetRewardedQuest(bot->GetTeamId(true) == TEAM_ALLIANCE ? kDkChainQuestAlliance
                                                                   : kDkChainQuestHorde);
        return true;
    }

    bool SanitizeCommon(Player* bot, bool& lfgLeaveSent)
    {
        // Any LFG state at all is a queue we did not ask for (a leftover from
        // the random-bot rotation, or an earlier fill). Leave it and wait: the
        // packet is asynchronous, so the state clears a tick or two later.
        if (sLFGMgr->GetState(bot->GetGUID()) != lfg::LFG_STATE_NONE)
        {
            if (!lfgLeaveSent)
            {
                bot->GetSession()->QueuePacket(new WorldPacket(CMSG_LFG_LEAVE));
                lfgLeaveSent = true;
            }
            return false;
        }

        if (Group* grp = bot->GetGroup())
        {
            grp->RemoveMember(bot->GetGUID());
            return false;
        }

        if (!bot->IsAlive())
        {
            bot->ResurrectPlayer(1.0f);
            bot->SpawnCorpseBones();
        }
        if (bot->IsInCombat())
            bot->CombatStop(true);
        bot->SetFullHealth();
        return true;
    }

    // The leave packet is queued and the logout follows immediately, which
    // looks like a race but is not: WorldSession::LogoutPlayer fires
    // LFGPlayerScript::OnPlayerLogout, which calls LeaveLfg AND
    // LeaveAllLfgQueues itself. The packet only matters on the no-logout path,
    // where nothing else would.
    void ReleaseBot(ObjectGuid guid, Player* bot, bool logout, char const* logTag,
                    std::string const& jobId)
    {
        if (bot)
        {
            if (sLFGMgr->GetState(guid) != lfg::LFG_STATE_NONE)
                bot->GetSession()->QueuePacket(new WorldPacket(CMSG_LFG_LEAVE));
            if (Group* grp = bot->GetGroup())
                grp->RemoveMember(guid);
        }

        if (!logout)
            return;

        // Whichever holder owns the login. The masterless path files these
        // under sRandomPlayerbotMgr, but check the map rather than assume it:
        // a bot handed to a player's party can have been re-filed since.
        if (sRandomPlayerbotMgr.GetPlayerBot(guid))
            sRandomPlayerbotMgr.LogoutPlayerBot(guid);
        else if (ObjectAccessor::FindConnectedPlayer(guid))
            LOG_WARN("playerbots.dungeonclear",
                     "{} {} bot {} is online but owned by no holder — left logged in", logTag,
                     jobId, guid.ToString());
    }
}
