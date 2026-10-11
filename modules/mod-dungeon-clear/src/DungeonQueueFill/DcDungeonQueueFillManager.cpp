/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DungeonQueueFill/DcDungeonQueueFillManager.h"

#include "Group.h"
#include "LFGMgr.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Opcodes.h"
#include "Player.h"
#include "WorldPacket.h"

#include "PlayerbotAI.h"
#include "Playerbots.h"

#include "DcModuleEnable.h"
#include "DungeonQueueFill/DcDungeonQueueFillJob.h"
#include "Ai/Dungeon/DungeonClear/Settings/DcSettings.h"

using namespace lfg;

DcDungeonQueueFillManager& DcDungeonQueueFillManager::Instance()
{
    static DcDungeonQueueFillManager instance;
    return instance;
}

bool DcDungeonQueueFillManager::HasJobFor(Player* player) const
{
    ObjectGuid const guid = player->GetGUID();
    Group const* const grp = player->GetGroup();
    ObjectGuid const gguid = grp ? grp->GetGUID() : ObjectGuid::Empty;

    for (auto const& job : _jobs)
    {
        if (job->PlayerGuid() == guid)
            return true;
        // A group's second member clicking Find Group must not open a second
        // fill for the same party.
        if (gguid && job->GroupGuid() == gguid)
            return true;
    }
    return false;
}

// Why this intent is not ours to act on. Empty = accepted.
//
// Every refusal is a plain sentence, including the ordinary ones ("feature
// off", "the player is a bot"), because the one question this feature will
// always be asked is "why didn't it fill?" and a DEBUG line naming the gate is
// the answer. Nothing here refuses the QUEUE — the hook returns true
// unconditionally; this only decides whether a fill job is opened.
std::string DcDungeonQueueFillManager::RefuseReason(Player* player,
                                                    std::set<std::uint32_t> const& dungeons,
                                                    bool* transient) const
{
    if (transient)
        *transient = false;

    if (!DcModule::IsEnabled())
        return "module disabled";
    if (!DcSettings::GetBool(ObjectGuid::Empty, "DungeonQueueFill.Enable"))
        return "DungeonQueueFill.Enable is off";

    // A bot queueing is the stock RandomBotJoinLfg behaviour and none of our
    // business; filling around one would also let two fills chase each other.
    //
    // A SELFBOT is not that. It carries a PlayerbotAI, but its master is
    // itself — there is a real person at the keyboard who opened the Dungeon
    // Finder and clicked Find Group, which is the exact case this feature
    // exists for. Testing GET_PLAYERBOT_AI alone refused every one of them.
    if (GET_PLAYERBOT_AI(player) && !IsSelfBot(player))
        return "queuing character is a bot";

    std::uint32_t const minLevel =
        DcSettings::GetUInt(ObjectGuid::Empty, "DungeonQueueFill.MinPlayerLevel");
    if (player->GetLevel() < minLevel)
        return "below MinPlayerLevel (" + std::to_string(minLevel) + ")";

    if (Group const* grp = player->GetGroup())
    {
        if (grp->GetMembersCount() >= DcDungeonQueueFillPlanner::kPartySize)
            return "party is already full";
        // Only the leader's click opens a fill: a group queue is one intent,
        // and JoinLfg only honours the leader's anyway.
        if (grp->GetLeaderGUID() != player->GetGUID())
            return "not the group leader";
    }

    if (HasJobFor(player))
        return "a fill is already in flight for this player/party";

    // Against the jobs still being SET UP, not every job on the books. A
    // formed fill has already spent everything the cap is here to bound and
    // then sits in Formed for the length of the dungeon; counting it locked
    // out every other player on the realm for the rest of the run.
    std::uint32_t const maxConcurrent =
        DcSettings::GetUInt(ObjectGuid::Empty, "DungeonQueueFill.MaxConcurrent");
    if (maxConcurrent && SettingUpCount() >= maxConcurrent)
    {
        // The one refusal worth coming back for: nothing about this player is
        // wrong, the realm is just busy for a moment.
        if (transient)
            *transient = true;
        return "concurrent fill cap reached (" + std::to_string(maxConcurrent) + ")";
    }

    // The raid browser is a different system with a different queue, a
    // different party size and no proposal popup. Never fill for it.
    for (std::uint32_t const id : dungeons)
        if (LFGDungeonData const* data = sLFGMgr->GetLFGDungeon(id))
            if (data->type == LFG_TYPE_RAID)
                return "raid browser queue";

    return {};
}

void DcDungeonQueueFillManager::OnQueueIntent(Player* player, std::uint8_t roles,
                                              std::set<std::uint32_t> const& dungeons)
{
    if (!player || dungeons.empty())
        return;

    bool transient = false;
    std::string const refusal = RefuseReason(player, dungeons, &transient);
    if (!refusal.empty())
    {
        if (transient)
        {
            Defer(player);
            LOG_INFO("playerbots.dungeonclear",
                     "QUEUEFILL deferring {}'s queue: {} — will retry while they wait",
                     player->GetName(), refusal);
            return;
        }

        LOG_DEBUG("playerbots.dungeonclear", "QUEUEFILL declining {}'s queue: {}",
                  player->GetName(), refusal);
        return;
    }

    // They got in under their own steam; drop any pending retry for them.
    ForgetDeferred(player->GetGUID());

    if (std::unique_ptr<DcDungeonQueueFillJob> job =
            DcDungeonQueueFillJob::Observe(player, roles, dungeons))
        _jobs.push_back(std::move(job));
}

std::size_t DcDungeonQueueFillManager::SettingUpCount() const
{
    std::size_t n = 0;
    for (auto const& job : _jobs)
        switch (job->GetStage())
        {
            case DcDungeonQueueFillJob::Stage::Formed:
            case DcDungeonQueueFillJob::Stage::Releasing:
            case DcDungeonQueueFillJob::Stage::Done:
                break;
            default:
                ++n;
                break;
        }
    return n;
}

bool DcDungeonQueueFillManager::IsClaimed(ObjectGuid guid) const
{
    for (auto const& job : _jobs)
        if (job->HoldsBot(guid))
            return true;
    return false;
}

DcDungeonQueueFillManager::RecentParty const* DcDungeonQueueFillManager::RecentPartyFor(
    ObjectGuid player) const
{
    for (RecentParty const& e : _recent)
        if (e.player == player)
            return &e;
    return nullptr;
}

void DcDungeonQueueFillManager::RememberParty(ObjectGuid player,
                                              std::vector<std::uint8_t> classIds,
                                              std::vector<ObjectGuid> guids)
{
    for (RecentParty& e : _recent)
    {
        if (e.player != player)
            continue;
        // One fill deep: the party before last is not what the player is about
        // to be handed again.
        e.classIds = std::move(classIds);
        e.guids = std::move(guids);
        return;
    }

    if (_recent.size() >= kRecentPartyMemory)
        _recent.erase(_recent.begin());
    _recent.push_back(RecentParty{player, std::move(classIds), std::move(guids)});
}

void DcDungeonQueueFillManager::Defer(Player* player)
{
    ObjectGuid const guid = player->GetGUID();
    for (Deferred const& d : _deferred)
        if (d.player == guid)
            return;

    if (_deferred.size() >= kMaxDeferred)
        return;

    _deferred.push_back(Deferred{guid, 0, 0});
}

void DcDungeonQueueFillManager::ForgetDeferred(ObjectGuid player)
{
    for (auto it = _deferred.begin(); it != _deferred.end(); ++it)
        if (it->player == player)
        {
            _deferred.erase(it);
            return;
        }
}

void DcDungeonQueueFillManager::TickDeferred(std::uint32_t diff)
{
    for (auto it = _deferred.begin(); it != _deferred.end();)
    {
        it->sinceMs += diff;
        it->waitedMs += diff;
        if (it->waitedMs < kDeferredRetryMs)
        {
            ++it;
            continue;
        }
        it->waitedMs = 0;

        Player* const player = ObjectAccessor::FindConnectedPlayer(it->player);
        if (!player)
        {
            it = _deferred.erase(it);
            continue;
        }

        // The queue is the whole reason to keep them: the moment they are not
        // waiting on one, there is nothing to fill. A group queue is keyed by
        // the leader's guid, exactly as ForceFill reads it.
        ObjectGuid const lfgGuid =
            player->GetGroup() ? player->GetGroup()->GetLeaderGUID() : player->GetGUID();
        if (sLFGMgr->GetState(lfgGuid) != LFG_STATE_QUEUED)
        {
            it = _deferred.erase(it);
            continue;
        }

        lfg::LfgDungeonSet const& dungeons = sLFGMgr->GetSelectedDungeons(lfgGuid);
        if (dungeons.empty())
        {
            it = _deferred.erase(it);
            continue;
        }

        bool transient = false;
        std::string const refusal = RefuseReason(player, dungeons, &transient);
        if (!refusal.empty())
        {
            // Still no room: keep waiting. Anything else has become a real
            // refusal since they queued (they levelled, the party filled up,
            // the feature was switched off) and is not going to resolve.
            if (transient)
            {
                ++it;
                continue;
            }

            LOG_DEBUG("playerbots.dungeonclear",
                      "QUEUEFILL giving up on {}'s deferred queue after {}s: {}",
                      player->GetName(), it->sinceMs / 1000, refusal);
            it = _deferred.erase(it);
            continue;
        }

        std::unique_ptr<DcDungeonQueueFillJob> job =
            DcDungeonQueueFillJob::Observe(player, sLFGMgr->GetRoles(player->GetGUID()), dungeons);
        if (job)
        {
            LOG_INFO("playerbots.dungeonclear",
                     "QUEUEFILL {} opening {}'s deferred fill — room freed up after {}s",
                     job->Id(), player->GetName(), it->sinceMs / 1000);
            _jobs.push_back(std::move(job));
        }
        it = _deferred.erase(it);
    }
}

void DcDungeonQueueFillManager::OnBotProposal(Player* bot, WorldPacket const& packet)
{
    if (!bot || _jobs.empty() || _inProposalAccept)
        return;

    bool wanted = false;
    for (auto const& job : _jobs)
        if (job->WantsProposalAcceptFor(bot->GetGUID()))
        {
            wanted = true;
            break;
        }
    if (!wanted)
        return;

    // Read a COPY. The real packet is on its way to the bot's own session and
    // playerbots reads it from the start too; moving the read position under
    // it would be a very quiet way to break the stock handler.
    WorldPacket copy(packet);
    if (copy.size() < 9)
        return;

    std::uint32_t dungeonEntry = 0;
    std::uint8_t state = 0;
    std::uint32_t proposalId = 0;
    copy >> dungeonEntry >> state >> proposalId;

    // Only a live proposal. The core sends this same opcode to announce a
    // proposal that has already FAILED or SUCCEEDED, and answering one of
    // those is at best a no-op — LFGMgr::UpdateProposal would not find it.
    if (state != lfg::LFG_PROPOSAL_INITIATING || !proposalId)
        return;

    _inProposalAccept = true;
    sLFGMgr->UpdateProposal(proposalId, bot->GetGUID(), true);
    _inProposalAccept = false;

    LOG_DEBUG("playerbots.dungeonclear", "QUEUEFILL accepted proposal {} for {}", proposalId,
              bot->GetName());
}

void DcDungeonQueueFillManager::OnPlayerLogout(Player* player)
{
    if (!player)
        return;

    ForgetDeferred(player->GetGUID());
    for (auto const& job : _jobs)
        if (job->PlayerGuid() == player->GetGUID())
            job->Release("player logged out");
}

void DcDungeonQueueFillManager::Tick(std::uint32_t diff)
{
    if (_jobs.empty() && _deferred.empty())
        return;

    // Turning the feature off mid-flight releases what is in flight rather
    // than stranding logged-in bots in a queue nobody is watching.
    if (!DcModule::IsEnabled() ||
        !DcSettings::GetBool(ObjectGuid::Empty, "DungeonQueueFill.Enable"))
    {
        _deferred.clear();
        ReleaseAll("feature turned off mid-flight");
        return;
    }

    // Before the jobs, not after: a slot freed by a job finishing LAST tick is
    // a slot a waiting player can have now, and retrying first keeps the sweep
    // off the path of the common case where nothing is deferred at all.
    TickDeferred(diff);

    for (auto const& job : _jobs)
        job->Tick(diff);

    for (auto it = _jobs.begin(); it != _jobs.end();)
        it = (*it)->Done() ? _jobs.erase(it) : it + 1;
}

std::string DcDungeonQueueFillManager::StatusText() const
{
    bool const on = DcSettings::GetBool(ObjectGuid::Empty, "DungeonQueueFill.Enable");
    if (!on)
        return "dungeon queue fill is OFF (DungeonClear.DungeonQueueFill.Enable = 0)";

    // Deferred players are reported even when nothing is in flight: "ON, no
    // fills" while somebody sits refused in the queue is the exact report that
    // sends an operator looking in the wrong place.
    std::string deferred;
    for (Deferred const& d : _deferred)
    {
        Player const* const p = ObjectAccessor::FindConnectedPlayer(d.player);
        deferred += "\n  waiting for room: " + (p ? p->GetName() : std::string("(offline)")) +
                    " (" + std::to_string(d.sinceMs / 1000) + "s)";
    }

    if (_jobs.empty())
        return deferred.empty() ? "dungeon queue fill is ON, no fills in flight"
                                : "dungeon queue fill is ON, no fills in flight, " +
                                      std::to_string(_deferred.size()) + " deferred:" + deferred;

    std::uint32_t const maxConcurrent =
        DcSettings::GetUInt(ObjectGuid::Empty, "DungeonQueueFill.MaxConcurrent");
    std::string out = std::to_string(_jobs.size()) +
                      (_jobs.size() == 1 ? " fill in flight" : " fills in flight") + " (" +
                      std::to_string(SettingUpCount()) + " setting up, cap " +
                      (maxConcurrent ? std::to_string(maxConcurrent) : std::string("unlimited")) +
                      "):";
    for (auto const& job : _jobs)
        out += "\n  " + job->StatusLine();
    return out + deferred;
}

bool DcDungeonQueueFillManager::Cancel(std::string const& playerName, std::string* msg)
{
    for (auto const& job : _jobs)
    {
        Player* const player = ObjectAccessor::FindConnectedPlayer(job->PlayerGuid());
        if (!player || player->GetName() != playerName)
            continue;
        job->Release("cancelled by operator");
        if (msg)
            *msg = "releasing fill " + job->Id() + " for " + playerName;
        return true;
    }
    if (msg)
        *msg = "no fill in flight for '" + playerName + "'";
    return false;
}

bool DcDungeonQueueFillManager::ForceFill(Player* player, std::string* msg)
{
    if (!player)
        return false;

    lfg::LfgDungeonSet const& dungeons =
        sLFGMgr->GetSelectedDungeons(player->GetGroup() ? player->GetGroup()->GetLeaderGUID()
                                                        : player->GetGUID());
    if (dungeons.empty())
    {
        if (msg)
            *msg = player->GetName() + " has no selected dungeons — they are not in the queue";
        return false;
    }

    std::string const refusal = RefuseReason(player, dungeons);
    if (!refusal.empty())
    {
        if (msg)
            *msg = "refusing a fill for " + player->GetName() + ": " + refusal;
        return false;
    }

    std::unique_ptr<DcDungeonQueueFillJob> job = DcDungeonQueueFillJob::Observe(
        player, sLFGMgr->GetRoles(player->GetGUID()), dungeons);
    if (!job)
    {
        if (msg)
            *msg = "could not open a fill for " + player->GetName();
        return false;
    }

    if (msg)
        *msg = "opened fill " + job->Id() + " for " + player->GetName();
    _jobs.push_back(std::move(job));
    return true;
}

void DcDungeonQueueFillManager::ReleaseAll(std::string const& reason)
{
    for (auto const& job : _jobs)
        job->Release(reason);
    // Run the teardown immediately rather than waiting for the next tick: the
    // shutdown path has no next tick.
    for (auto const& job : _jobs)
        job->Tick(0);
    _jobs.clear();
}
