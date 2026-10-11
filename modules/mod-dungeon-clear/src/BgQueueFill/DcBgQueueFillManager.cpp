/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "BgQueueFill/DcBgQueueFillManager.h"

#include <algorithm>

#include "Battleground.h"
#include "BattlegroundMgr.h"
#include "BattlegroundQueue.h"
#include "DBCStructure.h"
#include "Group.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Opcodes.h"
#include "Player.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"

#include "Ai/Dungeon/DungeonClear/Settings/DcSettings.h"
#include "BgQueueFill/DcBgQueueFillJob.h"
#include "DcModuleEnable.h"
#include "Util/DcPoolBots.h"

namespace
{
    constexpr std::uint32_t kDeserterSpell = 26013;
    constexpr std::uint16_t kPortUnk = 0x1F90;
    constexpr std::size_t kMaxPending = 64;

    bool IsBotNotSelf(Player* p)
    {
        return GET_PLAYERBOT_AI(p) && !IsSelfBot(p);
    }

    std::uint8_t SideOf(Player* p)
    {
        return p->GetTeamId() == TEAM_ALLIANCE ? 0 : 1;
    }
}

DcBgQueueFillManager& DcBgQueueFillManager::Instance()
{
    static DcBgQueueFillManager instance;
    return instance;
}

std::uint32_t DcBgQueueFillManager::CountRealPlayers(Battleground const* bg)
{
    std::uint32_t n = 0;
    if (!bg)
        return n;
    for (auto const& [guid, p] : bg->GetPlayers())
        if (p && !IsBotNotSelf(p))
            ++n;
    return n;
}

bool DcBgQueueFillManager::InLiveMatchWithRealPlayer(Player* bot)
{
    // create=true: a match whose map is still being built is still a match.
    Battleground* const bg = bot ? bot->GetBattleground(true) : nullptr;
    if (!bg)
        return false;
    if (bg->GetStatus() != STATUS_IN_PROGRESS && bg->GetStatus() != STATUS_WAIT_JOIN)
        return false;
    return CountRealPlayers(bg) > 0;
}

void DcBgQueueFillManager::QueuePortPacket(Player* bot, std::uint8_t arenaType, std::uint32_t bgTypeId,
                                           std::uint8_t action)
{
    WorldPacket* data = new WorldPacket(CMSG_BATTLEFIELD_PORT, 1 + 1 + 4 + 2 + 1);
    *data << std::uint8_t(arenaType) << std::uint8_t(0) << std::uint32_t(bgTypeId)
          << std::uint16_t(kPortUnk) << std::uint8_t(action);
    bot->GetSession()->QueuePacket(data);
}

DcBgQueueFillJob* DcBgQueueFillManager::JobFor(ObjectGuid player) const
{
    for (auto const& job : _jobs)
        if (job->PlayerGuid() == player && !job->Done())
            return job.get();
    return nullptr;
}

// Why this join is not ours to fill. Empty = accepted.
//
// Nothing here refuses the QUEUE — the hooks are observers (and OnAddGroup
// only ever moves a group between two pools of the same queue); this decides
// whether a fill is opened. Every refusal is a sentence, because "why didn't
// it fill?" is the one question this feature will always be asked.
std::string DcBgQueueFillManager::RefuseReason(Player* player, std::uint8_t arenaType, bool isRated,
                                               bool* transient) const
{
    if (transient)
        *transient = false;

    if (!DcModule::IsEnabled())
        return "module disabled";
    if (!DcSettings::GetBool(ObjectGuid::Empty, "BgQueueFill.Enable"))
        return "BgQueueFill.Enable is off";
    if (arenaType || isRated)
        return "arena or rated queue";

    // A random bot queueing is stock RandomBotJoinBG behaviour — and, more to
    // the point, every one of OUR bots joining fires this same hook. A selfbot
    // has a person at the keyboard and is filled for.
    if (IsBotNotSelf(player))
        return "queuing character is a bot";

    std::uint32_t const minLevel = DcSettings::GetUInt(ObjectGuid::Empty, "BgQueueFill.MinPlayerLevel");
    if (player->GetLevel() < minLevel)
        return "below MinPlayerLevel (" + std::to_string(minLevel) + ")";

    // A fill whose player has moved on (left the match, left the queue) is on
    // its way out; it must not cost them the next one.
    if (DcBgQueueFillJob const* job = JobFor(player->GetGUID()))
        if (!job->PlayerHasMovedOn())
            return "a fill is already in flight for this player";

    std::uint32_t const maxConcurrent = DcSettings::GetUInt(ObjectGuid::Empty, "BgQueueFill.MaxConcurrent");
    if (maxConcurrent && SettingUpCount() >= maxConcurrent)
    {
        if (transient)
            *transient = true;
        return "concurrent fill cap reached (" + std::to_string(maxConcurrent) + ")";
    }

    return {};
}

bool DcBgQueueFillManager::OpposingHumanPremadeQueued(BattlegroundQueue const& queue, std::uint32_t bracketId,
                                                     std::uint8_t leaderTeam)
{
    if (bracketId >= MAX_BATTLEGROUND_BRACKETS)
        return false;
    std::uint32_t const other = leaderTeam == 0 ? BG_QUEUE_PREMADE_HORDE : BG_QUEUE_PREMADE_ALLIANCE;
    for (GroupQueueInfo const* g : queue.m_QueuedGroups[bracketId][other])
    {
        if (g->IsInvitedToBGInstanceGUID)
            continue;
        for (ObjectGuid const& guid : g->Players)
            if (Player* const p = ObjectAccessor::FindConnectedPlayer(guid))
                if (!IsBotNotSelf(p))
                    return true;
    }
    return false;
}

// The queue files a group by (bracket, GroupType) and finds it again by the
// same pair (RemovePlayer), so moving it means moving both together.
bool DcBgQueueFillManager::DemoteQueuedGroup(BattlegroundQueue& queue, GroupQueueInfo* ginfo)
{
    if (!ginfo || ginfo->GroupType > BG_QUEUE_PREMADE_HORDE || ginfo->BracketId >= MAX_BATTLEGROUND_BRACKETS)
        return false;

    auto& from = queue.m_QueuedGroups[ginfo->BracketId][ginfo->GroupType];
    auto const it = std::find(from.begin(), from.end(), ginfo);
    if (it == from.end())
        return false;

    from.erase(it);
    ginfo->GroupType += PVP_TEAMS_COUNT;
    queue.m_QueuedGroups[ginfo->BracketId][ginfo->GroupType].push_back(ginfo);
    return true;
}

// Why a human premade has to be moved for the fill to work at all: the premade
// pool only matches an Alliance premade against a Horde premade
// (CheckPremadeMatch), so a group of five queueing for Warsong would sit there
// facing a normal pool full of our solo-queued bots and never pop. Its escape
// hatch (PremadeGroupWaitForMatch) is thirty minutes on this realm.
//
// The rewrite is made only for a join that will actually be filled, and never
// when a human premade of the other faction is already waiting — that is a
// real premade match about to happen, and it is not ours to take.
void DcBgQueueFillManager::OnAddGroup(BattlegroundQueue* queue, std::uint32_t& index, Player* leader,
                                      Group* /*group*/, std::uint32_t bgTypeId,
                                      PvPDifficultyEntry const* bracketEntry, std::uint8_t arenaType,
                                      bool isRated, bool isPremade)
{
    // Our own bots join through here by the dozen: the cheapest test first.
    if (!leader || !queue || arenaType || isRated || IsBotNotSelf(leader))
        return;

    _pending.erase(std::remove_if(_pending.begin(), _pending.end(),
                                  [leader](PendingJoin const& p) { return p.leader == leader->GetGUID(); }),
                   _pending.end());
    if (_pending.size() >= kMaxPending)
        _pending.erase(_pending.begin());

    PendingJoin pj;
    pj.leader = leader->GetGUID();
    pj.bgTypeId = bgTypeId;

    if (isPremade && bracketEntry && RefuseReason(leader, arenaType, isRated).empty() &&
        (index == BG_QUEUE_PREMADE_ALLIANCE || index == BG_QUEUE_PREMADE_HORDE))
    {
        if (OpposingHumanPremadeQueued(*queue, bracketEntry->GetBracketId(), SideOf(leader)))
        {
            pj.premade = true;
            LOG_INFO("playerbots.dungeonclear",
                     "BGQUEUEFILL leaving {}'s premade group in the premade queue — an opposing human "
                     "premade is already waiting",
                     leader->GetName());
        }
        else
        {
            index += PVP_TEAMS_COUNT;
            pj.demoted = true;
        }
    }

    _pending.push_back(pj);
}

void DcBgQueueFillManager::OnPlayerJoinBG(Player* player)
{
    if (!player || IsBotNotSelf(player))
        return;

    // Only the join OnAddGroup recorded for THIS player: a solo join, or the
    // leader of a group join. Every other member of a group join lands here
    // with nothing pending and is the leader's fill.
    auto const it = std::find_if(_pending.begin(), _pending.end(),
                                 [player](PendingJoin const& p) { return p.leader == player->GetGUID(); });
    if (it == _pending.end())
        return;
    PendingJoin const pj = *it;
    _pending.erase(it);

    if (pj.premade)
        return;  // said so in OnAddGroup

    bool transient = false;
    std::string const refusal = RefuseReason(player, 0, false, &transient);
    if (!refusal.empty())
    {
        if (transient)
        {
            Defer(player, pj.bgTypeId);
            LOG_INFO("playerbots.dungeonclear",
                     "BGQUEUEFILL deferring {}'s battleground queue: {} — will retry while they wait",
                     player->GetName(), refusal);
            return;
        }
        LOG_DEBUG("playerbots.dungeonclear", "BGQUEUEFILL declining {}'s battleground queue: {}",
                  player->GetName(), refusal);
        return;
    }

    ForgetDeferred(player->GetGUID());
    OpenJob(player, pj.bgTypeId, pj.demoted);
}

void DcBgQueueFillManager::OpenJob(Player* player, std::uint32_t bgTypeId, bool demoted)
{
    // Refusal already let this through, so any fill still on the books for
    // this player is one they have moved on from.
    if (DcBgQueueFillJob* const old = JobFor(player->GetGUID()))
        old->Release("player queued again");

    if (std::unique_ptr<DcBgQueueFillJob> job = DcBgQueueFillJob::Observe(player, bgTypeId, demoted))
        _jobs.push_back(std::move(job));
}

void DcBgQueueFillManager::OnPlayerLogout(Player* player)
{
    if (!player)
        return;
    ForgetDeferred(player->GetGUID());
    for (auto const& job : _jobs)
        if (job->PlayerGuid() == player->GetGUID())
            job->Release("player logged out");
}

std::size_t DcBgQueueFillManager::SettingUpCount() const
{
    std::size_t n = 0;
    for (auto const& job : _jobs)
        if (job->IsSettingUp())
            ++n;
    return n;
}

bool DcBgQueueFillManager::IsClaimed(ObjectGuid guid) const
{
    for (auto const& job : _jobs)
        if (job->HoldsBot(guid))
            return true;
    for (Orphan const& o : _orphans)
        if (o.guid == guid)
            return true;
    return false;
}

std::uint32_t DcBgQueueFillManager::InFlightBots(std::uint32_t queueTypeId, std::uint32_t bracketId,
                                                 std::uint8_t side, DcBgQueueFillJob const* except) const
{
    std::uint32_t n = 0;
    for (auto const& job : _jobs)
        if (job.get() != except)
            n += job->InFlightFor(queueTypeId, bracketId, side);
    return n;
}

std::vector<ObjectGuid> const* DcBgQueueFillManager::RecentBotsFor(ObjectGuid player) const
{
    for (RecentBots const& r : _recent)
        if (r.player == player)
            return &r.guids;
    return nullptr;
}

void DcBgQueueFillManager::RememberBots(ObjectGuid player, std::vector<ObjectGuid> guids)
{
    for (RecentBots& r : _recent)
        if (r.player == player)
        {
            r.guids = std::move(guids);
            return;
        }
    if (_recent.size() >= kRecentMemory)
        _recent.erase(_recent.begin());
    _recent.push_back(RecentBots{player, std::move(guids)});
}

// ---- release and adoption -------------------------------------------------

void DcBgQueueFillManager::ReleaseBot(ObjectGuid guid, bool logout, bool force, std::string const& jobId)
{
    Player* const bot = ObjectAccessor::FindConnectedPlayer(guid);

    if (!force)
    {
        // No player yet: a login still in flight will land after the fill is
        // gone and leave a bot standing in a capital. Watch for it.
        if (!bot)
        {
            if (logout && std::none_of(_orphans.begin(), _orphans.end(),
                                       [guid](Orphan const& o) { return o.guid == guid; }))
                _orphans.push_back(Orphan{guid, jobId, 0, logout});
            return;
        }

        if (InLiveMatchWithRealPlayer(bot))
        {
            if (std::none_of(_orphans.begin(), _orphans.end(),
                             [guid](Orphan const& o) { return o.guid == guid; }))
                _orphans.push_back(Orphan{guid, jobId, 0, logout});
            LOG_DEBUG("playerbots.dungeonclear",
                      "BGQUEUEFILL {} leaving {} in its match — real players are still playing it",
                      jobId, bot->GetName());
            return;
        }
    }

    FinishRelease(guid, bot, logout, jobId);
}

// The logout path needs none of the leave steps: WorldSession::LogoutPlayer
// leaves the battleground and every battleground queue itself. They matter only
// when the bot stays in world.
void DcBgQueueFillManager::FinishRelease(ObjectGuid guid, Player* bot, bool logout, std::string const& jobId)
{
    if (bot && !logout && bot->IsInWorld())
    {
        if (bot->InBattleground() && !bot->IsBeingTeleported())
            bot->LeaveBattleground();
        for (std::uint32_t q = 0; q < PLAYER_MAX_BATTLEGROUND_QUEUES; ++q)
            if (BattlegroundQueueTypeId const qt = bot->GetBattlegroundQueueTypeId(q))
                QueuePortPacket(bot, BattlegroundMgr::BGArenaType(qt), BattlegroundMgr::BGTemplateId(qt), 0);
        bot->RemoveAurasDueToSpell(kDeserterSpell);
    }

    DcPoolBots::ReleaseBot(guid, bot, logout, "BGQUEUEFILL", jobId);
}

void DcBgQueueFillManager::TickOrphans(std::uint32_t diff)
{
    if (_orphans.empty())
        return;
    _orphanSweepMs += diff;
    if (_orphanSweepMs < kOrphanSweepMs)
        return;
    std::uint32_t const elapsed = _orphanSweepMs;
    _orphanSweepMs = 0;

    for (auto it = _orphans.begin(); it != _orphans.end();)
    {
        it->ageMs += elapsed;
        Player* const bot = ObjectAccessor::FindConnectedPlayer(it->guid);
        if (!bot)
        {
            it = it->ageMs >= kOrphanLoginWaitMs ? _orphans.erase(it) : it + 1;
            continue;
        }
        if (!bot->IsInWorld() || bot->IsBeingTeleported() || InLiveMatchWithRealPlayer(bot))
        {
            ++it;
            continue;
        }

        LOG_DEBUG("playerbots.dungeonclear", "BGQUEUEFILL {} releasing {} — its match is over", it->jobId,
                  bot->GetName());
        Orphan const o = *it;
        it = _orphans.erase(it);
        FinishRelease(o.guid, bot, o.logout, o.jobId);
    }
}

// ---- deferred joins -------------------------------------------------------

void DcBgQueueFillManager::Defer(Player* player, std::uint32_t bgTypeId)
{
    ObjectGuid const guid = player->GetGUID();
    for (Deferred& d : _deferred)
        if (d.player == guid)
        {
            d.bgTypeId = bgTypeId;
            return;
        }
    if (_deferred.size() >= kMaxDeferred)
        return;
    _deferred.push_back(Deferred{guid, bgTypeId, 0, 0});
}

void DcBgQueueFillManager::ForgetDeferred(ObjectGuid player)
{
    _deferred.erase(std::remove_if(_deferred.begin(), _deferred.end(),
                                   [player](Deferred const& d) { return d.player == player; }),
                    _deferred.end());
}

void DcBgQueueFillManager::TickDeferred(std::uint32_t diff)
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
        BattlegroundQueueTypeId const qt = BattlegroundMgr::BGQueueTypeId(BattlegroundTypeId(it->bgTypeId), 0);

        // Only while they are still waiting in that queue: invited means a
        // match found them anyway.
        if (!player || !player->InBattlegroundQueueForBattlegroundQueueType(qt) ||
            player->IsInvitedForBattlegroundQueueType(qt))
        {
            it = _deferred.erase(it);
            continue;
        }

        bool transient = false;
        std::string const refusal = RefuseReason(player, 0, false, &transient);
        if (!refusal.empty())
        {
            if (transient)
            {
                ++it;
                continue;
            }
            LOG_DEBUG("playerbots.dungeonclear", "BGQUEUEFILL giving up on {}'s deferred queue after {}s: {}",
                      player->GetName(), it->sinceMs / 1000, refusal);
            it = _deferred.erase(it);
            continue;
        }

        // A premade group was left in the premade pool while the cap was
        // full (OnAddGroup only moves a group it is about to fill). Now that
        // it will be filled, move it — unless a human premade is waiting for
        // it on the other side.
        bool demoted = false;
        BattlegroundQueue& queue = sBattlegroundMgr->GetBattlegroundQueue(qt);
        auto const qit = queue.m_QueuedPlayers.find(player->GetGUID());
        if (qit != queue.m_QueuedPlayers.end() && qit->second &&
            qit->second->GroupType <= BG_QUEUE_PREMADE_HORDE)
        {
            GroupQueueInfo* const ginfo = qit->second;
            if (OpposingHumanPremadeQueued(queue, ginfo->BracketId, ginfo->teamId == TEAM_ALLIANCE ? 0 : 1))
            {
                LOG_INFO("playerbots.dungeonclear",
                         "BGQUEUEFILL not filling {}'s deferred premade — an opposing human premade is waiting",
                         player->GetName());
                it = _deferred.erase(it);
                continue;
            }
            demoted = DemoteQueuedGroup(queue, ginfo);
        }

        LOG_INFO("playerbots.dungeonclear", "BGQUEUEFILL opening {}'s deferred fill — room freed up after {}s",
                 player->GetName(), it->sinceMs / 1000);
        std::uint32_t const bgTypeId = it->bgTypeId;
        it = _deferred.erase(it);
        OpenJob(player, bgTypeId, demoted);
    }
}

// ---- the tick ---------------------------------------------------------------

void DcBgQueueFillManager::WarnOnConflictingConfig() const
{
    // No conflict to warn about. This used to warn whenever RandomBotJoinBG or
    // RandomBotAutoJoinBG was on, but the two mechanisms cannot collide:
    //   - a fill is only ever opened behind RefuseReason(), whose IsBotNotSelf() check
    //     rejects every non-selfbot, so BgQueueFill never fills for a queued bot;
    //   - the bots a fill claims come from the addclass pool (account type 2), while
    //     RandomBotAutoJoinBG only ever drives rotation bots (account type 1) - disjoint
    //     account sets, so a fill's bots are never eligible to self-queue.
    // With RandomBotAutoJoinBG on the two merely share the stock queue, which can make a
    // real player's match pop before the fill is ready; the job then exits gracefully.
    LOG_DEBUG("playerbots.dungeonclear",
              "BGQUEUEFILL is on alongside AiPlayerbot.RandomBotAutoJoinBG; both draw on the "
              "same queue, so a fill may be released unused if the match pops early.");
}

void DcBgQueueFillManager::Tick(std::uint32_t diff)
{
    // Released bots are looked after whatever the switch says: they are
    // logged-in characters a fill left behind, and nothing else will log
    // them out.
    TickOrphans(diff);

    if (_jobs.empty() && _deferred.empty() && _pending.empty() && _configChecked)
        return;

    bool const on = DcModule::IsEnabled() && DcSettings::GetBool(ObjectGuid::Empty, "BgQueueFill.Enable");

    // Once, the first tick the feature is on — the playerbots config is
    // certainly loaded by then, which it may not be at script registration.
    if (!_configChecked && on)
    {
        _configChecked = true;
        WarnOnConflictingConfig();
    }
    if (!on)
    {
        _deferred.clear();
        _pending.clear();
        if (!_jobs.empty())
            ReleaseAll("feature turned off mid-flight");
        return;
    }

    // A join OnAddGroup recorded is consumed by OnPlayerJoinBG in the same
    // handler call; anything left over by now is from a join that went
    // nowhere.
    _pending.clear();

    TickDeferred(diff);

    for (std::size_t i = 0; i < _jobs.size(); ++i)
        _jobs[i]->Tick(diff);

    _jobs.erase(std::remove_if(_jobs.begin(), _jobs.end(),
                               [](std::unique_ptr<DcBgQueueFillJob> const& j) { return j->Done(); }),
                _jobs.end());
}

void DcBgQueueFillManager::ReleaseAll(std::string const& reason)
{
    for (auto const& job : _jobs)
        job->Release(reason, false, /*force*/ true);
    // Run the teardown now: the shutdown path has no next tick.
    for (auto const& job : _jobs)
        job->Tick(0);
    _jobs.clear();

    for (Orphan const& o : _orphans)
        FinishRelease(o.guid, ObjectAccessor::FindConnectedPlayer(o.guid), o.logout, o.jobId);
    _orphans.clear();
}

// ---- operator surface -------------------------------------------------------

std::string DcBgQueueFillManager::StatusText() const
{
    if (!DcSettings::GetBool(ObjectGuid::Empty, "BgQueueFill.Enable"))
        return "battleground queue fill is OFF (DungeonClear.BgQueueFill.Enable = 0)" +
               (_orphans.empty() ? std::string()
                                 : ", " + std::to_string(_orphans.size()) + " released bot(s) awaiting logout");

    std::string tail;
    for (Deferred const& d : _deferred)
    {
        Player const* const p = ObjectAccessor::FindConnectedPlayer(d.player);
        tail += "\n  waiting for room: " + (p ? p->GetName() : std::string("(offline)")) + " (" +
                std::to_string(d.sinceMs / 1000) + "s)";
    }
    if (!_orphans.empty())
        tail += "\n  " + std::to_string(_orphans.size()) +
                " released bot(s) still in a match somebody else is playing — logged out when it ends";

    if (_jobs.empty())
        return "battleground queue fill is ON, no fills in flight" + tail;

    std::uint32_t const maxConcurrent = DcSettings::GetUInt(ObjectGuid::Empty, "BgQueueFill.MaxConcurrent");
    std::string out = std::to_string(_jobs.size()) + (_jobs.size() == 1 ? " fill" : " fills") +
                      " in flight (" + std::to_string(SettingUpCount()) + " setting up, cap " +
                      (maxConcurrent ? std::to_string(maxConcurrent) : std::string("unlimited")) + "):";
    for (auto const& job : _jobs)
        out += "\n  " + job->StatusLine();
    return out + tail;
}

bool DcBgQueueFillManager::Cancel(std::string const& playerName, std::string* msg)
{
    for (auto const& job : _jobs)
    {
        Player* const player = ObjectAccessor::FindConnectedPlayer(job->PlayerGuid());
        if (!player || player->GetName() != playerName || job->Done())
            continue;
        job->Release("cancelled by operator");
        if (msg)
            *msg = "releasing fill " + job->Id() + " for " + playerName;
        return true;
    }
    if (msg)
        *msg = "no battleground fill in flight for '" + playerName + "'";
    return false;
}

bool DcBgQueueFillManager::ForceFill(Player* player, std::string* msg)
{
    if (!player)
        return false;

    std::uint32_t bgTypeId = 0;
    BattlegroundQueueTypeId qt = BATTLEGROUND_QUEUE_NONE;
    for (std::uint32_t q = 0; q < PLAYER_MAX_BATTLEGROUND_QUEUES; ++q)
    {
        BattlegroundQueueTypeId const t = player->GetBattlegroundQueueTypeId(q);
        if (t && !BattlegroundMgr::BGArenaType(t) && !player->IsInvitedForBattlegroundQueueType(t))
        {
            qt = t;
            bgTypeId = BattlegroundMgr::BGTemplateId(t);
            break;
        }
    }
    if (!bgTypeId)
    {
        if (msg)
            *msg = player->GetName() + " is not waiting in a battleground queue";
        return false;
    }

    std::string const refusal = RefuseReason(player, 0, false);
    if (!refusal.empty())
    {
        if (msg)
            *msg = "refusing a battleground fill for " + player->GetName() + ": " + refusal;
        return false;
    }

    bool demoted = false;
    BattlegroundQueue& queue = sBattlegroundMgr->GetBattlegroundQueue(qt);
    auto const qit = queue.m_QueuedPlayers.find(player->GetGUID());
    if (qit != queue.m_QueuedPlayers.end() && qit->second && qit->second->GroupType <= BG_QUEUE_PREMADE_HORDE)
    {
        if (OpposingHumanPremadeQueued(queue, qit->second->BracketId, SideOf(player)))
        {
            if (msg)
                *msg = "refusing: " + player->GetName() + "'s group is a premade and a human premade is waiting "
                       "to play it";
            return false;
        }
        demoted = DemoteQueuedGroup(queue, qit->second);
    }

    ForgetDeferred(player->GetGUID());
    OpenJob(player, bgTypeId, demoted);
    if (msg)
        *msg = "opened a battleground fill for " + player->GetName() +
               (demoted ? " (premade moved to the normal queue)" : "");
    return true;
}
