/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "LFGMgr.h"
#include "Opcodes.h"
#include "Player.h"
#include "PlayerScript.h"
#include "ScriptMgr.h"
#include "ServerScript.h"
#include "WorldSession.h"
#include "WorldScript.h"

#include "DungeonQueueFill/DcDungeonQueueFillManager.h"

// The one seam the RDF instant fill needs from the core: notice that a player
// asked to join the dungeon finder.
//
// PLAYERHOOK_CAN_JOIN_LFG fires at the very top of LFGMgr::JoinLfg, before any
// validation — before RBAC, deserter, dungeon cooldown, the lock map, the
// battleground checks and the compatible-dungeon filter. So this handler is a
// PURE OBSERVER and always returns true. It cannot refuse a queue, and it must
// not: a bug here would look to a player like the dungeon finder being broken,
// which is a far worse failure than a fill not happening.
//
// The manager likewise only RECORDS the intent. Everything the fill acts on is
// read back from LFGMgr once the queue has settled into LFG_STATE_QUEUED, by
// which point every one of those checks has passed. See
// DcDungeonQueueFillJob::TickObserved.
class DungeonClearQueueFillScript : public PlayerScript
{
public:
    DungeonClearQueueFillScript()
        : PlayerScript("DungeonClearQueueFillScript", {
            PLAYERHOOK_CAN_JOIN_LFG,
            PLAYERHOOK_ON_LOGOUT,
        }) {}

    bool OnPlayerCanJoinLfg(Player* player, uint8 roles, lfg::LfgDungeonSet& dungeons,
                            std::string const& /*comment*/) override
    {
        DcDungeonQueueFillManager::Instance().OnQueueIntent(player, roles, dungeons);
        return true;
    }

    // A player who logs out mid-fill leaves bots provisioned for a party that
    // can never form. The job's own tick notices a missing player too, but the
    // hook releases on the same tick the session closes rather than up to one
    // world tick later.
    void OnPlayerLogout(Player* player) override
    {
        DcDungeonQueueFillManager::Instance().OnPlayerLogout(player);
    }
};

// Answer the dungeon proposal for the bots this fill owns.
//
// The packet-sent hook is the one hook that sees a packet addressed to a BOT.
// WorldSession::SendPacket calls it before the `if (!m_Socket) return;` that
// ends the journey for every socket-less playerbot session, so it is the only
// place a module can watch what the core is telling a bot. The upstream core
// spells it ServerScript::OnPacketSent; the playerbots fork core spelled it
// PlayerbotScript::OnPlayerbotPacketSent (see Util/DcCoreCompat.h). A core
// with both takes the upstream one, so the proposal is answered once.
//
// Why the fill answers at all, rather than trusting the bot to: see
// DcDungeonQueueFillManager::OnBotProposal. In short, playerbots' `lfg accept`
// gets exactly one chance per proposal — the AI tick that happens to be handed
// the packet — and a bot that misses it is silently marked DENY 40 seconds
// later, taking the whole party's proposal down with it.
#ifdef DC_CORE_HAS_ON_PACKET_SENT
class DungeonClearQueueFillProposalScript : public ServerScript
{
public:
    // Only this hook: an empty list would enable every ServerScript hook.
    DungeonClearQueueFillProposalScript()
        : ServerScript("DungeonClearQueueFillProposalScript", {
            SERVERHOOK_ON_PACKET_SENT,
        }) {}

    // Fires for every packet on every session: the opcode test stays first.
    void OnPacketSent(WorldSession* session, WorldPacket const& packet) override
    {
        if (packet.GetOpcode() != SMSG_LFG_PROPOSAL_UPDATE || !session)
            return;

        if (Player* player = session->GetPlayer())
            DcDungeonQueueFillManager::Instance().OnBotProposal(player, packet);
    }
};
#else
class DungeonClearQueueFillProposalScript : public PlayerbotScript
{
public:
    DungeonClearQueueFillProposalScript()
        : PlayerbotScript("DungeonClearQueueFillProposalScript") {}

    void OnPlayerbotPacketSent(Player* player, WorldPacket const* packet) override
    {
        if (!player || !packet || packet->GetOpcode() != SMSG_LFG_PROPOSAL_UPDATE)
            return;

        DcDungeonQueueFillManager::Instance().OnBotProposal(player, *packet);
    }
};
#endif

// Worldserver shutdown. A fill in flight owns logged-in bots sitting in the
// LFG queue; without this they are saved to the DB mid-queue and come back next
// startup as characters nobody remembers spawning.
class DungeonClearQueueFillWorldScript : public WorldScript
{
public:
    DungeonClearQueueFillWorldScript()
        : WorldScript("DungeonClearQueueFillWorldScript", {
            WORLDHOOK_ON_SHUTDOWN,
        }) {}

    void OnShutdown() override
    {
        DcDungeonQueueFillManager::Instance().ReleaseAll("worldserver shutdown");
    }
};

void AddSC_dungeon_clear_queue_fill()
{
    new DungeonClearQueueFillScript();
    new DungeonClearQueueFillProposalScript();
    new DungeonClearQueueFillWorldScript();
}
