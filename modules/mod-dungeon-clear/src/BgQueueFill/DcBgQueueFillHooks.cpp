/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "AllBattlegroundScript.h"
#include "BattlegroundQueue.h"
#include "Player.h"
#include "PlayerScript.h"
#include "ScriptMgr.h"
#include "WorldScript.h"

#include "BgQueueFill/DcBgQueueFillManager.h"

// The two seams the battleground fill needs from the core: notice a player
// joining a battleground queue, and — for a human premade group — move it from
// the premade pool to the normal one before the queue files it.
//
// Both fire from WorldSession::HandleBattlemasterJoinOpcode, which is
// THREADUNSAFE and so runs on the world thread. Everything the fill then does
// with invites, arrivals and the match ending is read back on the manager's
// world tick rather than hooked: the battleground hooks that would carry those
// (AddPlayer, RemovePlayerAtLeave) can run on a map thread.
class DungeonClearBgQueueFillPlayerScript : public PlayerScript
{
public:
    DungeonClearBgQueueFillPlayerScript()
        : PlayerScript("DungeonClearBgQueueFillPlayerScript", {
            PLAYERHOOK_ON_PLAYER_JOIN_BG,
            PLAYERHOOK_ON_LOGOUT,
        }) {}

    // After AddGroup and the queue slot: the player IS queued, validated.
    void OnPlayerJoinBG(Player* player) override
    {
        DcBgQueueFillManager::Instance().OnPlayerJoinBG(player);
    }

    void OnPlayerLogout(Player* player) override
    {
        DcBgQueueFillManager::Instance().OnPlayerLogout(player);
    }
};

class DungeonClearBgQueueFillBattlegroundScript : public AllBattlegroundScript
{
public:
    // Only this hook: an empty list would enable every one.
    DungeonClearBgQueueFillBattlegroundScript()
        : AllBattlegroundScript("DungeonClearBgQueueFillBattlegroundScript", {
            ALLBATTLEGROUNDHOOK_ON_ADD_GROUP,
        }) {}

    // Right before AddGroup stores ginfo->GroupType = index: the one moment
    // a premade can be filed as a normal group instead.
    void OnAddGroup(BattlegroundQueue* queue, GroupQueueInfo* /*ginfo*/, uint32& index, Player* leader,
                    Group* group, BattlegroundTypeId bgTypeId, PvPDifficultyEntry const* bracketEntry,
                    uint8 arenaType, bool isRated, bool isPremade, uint32 /*arenaRating*/,
                    uint32 /*matchmakerRating*/, uint32 /*arenaTeamId*/,
                    uint32 /*opponentsArenaTeamId*/) override
    {
        DcBgQueueFillManager::Instance().OnAddGroup(queue, index, leader, group, bgTypeId, bracketEntry,
                                                    arenaType, isRated, isPremade);
    }
};

// Worldserver shutdown. A fill in flight owns dozens of logged-in bots sitting
// in queues and matches; release them before the world goes.
class DungeonClearBgQueueFillWorldScript : public WorldScript
{
public:
    DungeonClearBgQueueFillWorldScript()
        : WorldScript("DungeonClearBgQueueFillWorldScript", {
            WORLDHOOK_ON_SHUTDOWN,
        }) {}

    void OnShutdown() override
    {
        DcBgQueueFillManager::Instance().ReleaseAll("worldserver shutdown");
    }
};

void AddSC_dungeon_clear_bg_queue_fill()
{
    new DungeonClearBgQueueFillPlayerScript();
    new DungeonClearBgQueueFillBattlegroundScript();
    new DungeonClearBgQueueFillWorldScript();
}
