/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCBGQUEUEFILLMANAGER_H
#define _PLAYERBOT_DCBGQUEUEFILLMANAGER_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "ObjectGuid.h"

class Battleground;
class BattlegroundQueue;
class DcBgQueueFillJob;
class Group;
class Player;
struct GroupQueueInfo;
struct PvPDifficultyEntry;

// Registry of live battleground instant-fill jobs, ticked from the module's
// world tick next to the dungeon fill.
//
// Owns the admission policy (is the feature on, is this join ours to fill, is
// there room for another fill), the premade demotion, the deferred-join retry,
// and the bots a released fill could not take with it. Everything a fill
// actually does lives in DcBgQueueFillJob.
//
// World thread only. Both join hooks fire from THREADUNSAFE opcode handlers,
// and Tick from WorldScript::OnUpdate.
class DcBgQueueFillManager
{
public:
    static DcBgQueueFillManager& Instance();

    // BattlegroundQueue::AddGroup, right before the queue index is stored.
    // Records the join for OnPlayerJoinBG, and — when this join will be
    // filled — moves a human premade group into the normal queue by
    // rewriting `index` (see the conf.dist and ShouldDemote).
    void OnAddGroup(BattlegroundQueue* queue, std::uint32_t& index, Player* leader, Group* group,
                    std::uint32_t bgTypeId, PvPDifficultyEntry const* bracketEntry, std::uint8_t arenaType,
                    bool isRated, bool isPremade);

    // The join hook: fires after the core validated and queued the player, so
    // it is a pure observer. Once per member for a group join; only the
    // leader's opens a fill.
    void OnPlayerJoinBG(Player* player);

    void OnPlayerLogout(Player* player);

    void Tick(std::uint32_t diff);

    // Startup: say so if the stock random-bot BG joiners are on, which would
    // queue their own bots into the same matches.
    void WarnOnConflictingConfig() const;

    std::string StatusText() const;
    bool Cancel(std::string const& playerName, std::string* msg);

    // Open a fill for a player who is ALREADY queued for a battleground,
    // bypassing the join hook — the repeatable live-test entry point.
    bool ForceFill(Player* player, std::string* msg);

    // Release everything, pulling every bot whatever match it is in —
    // worldserver shutdown, or the feature being switched off.
    void ReleaseAll(std::string const& reason);

    std::size_t SettingUpCount() const;

    // Held by a live fill, or waiting here to be logged out.
    bool IsClaimed(ObjectGuid guid) const;

    // Bots every fill but `except` is still bringing to `side` of queue
    // (`queueTypeId`, `bracketId`) that the queue cannot see yet.
    std::uint32_t InFlightBots(std::uint32_t queueTypeId, std::uint32_t bracketId, std::uint8_t side,
                               DcBgQueueFillJob const* except) const;

    // Hand a pool bot back. A bot standing in a live match with a real player
    // in it is left there (unless `force`) and logged out once that match is
    // over: pulling a dozen bots out of somebody's Alterac Valley to tidy up
    // after a fill that ended is breaking their match. A guid with no player
    // yet (a login still in flight) is watched for a while and logged out if
    // it arrives.
    void ReleaseBot(ObjectGuid guid, bool logout, bool force, std::string const& jobId);

    // Real players (not bots; selfbots count) inside `bg`.
    static std::uint32_t CountRealPlayers(Battleground const* bg);

    // Is `bot` inside a match that is still being played by a real player?
    static bool InLiveMatchWithRealPlayer(Player* bot);

    // Queue the CMSG_BATTLEFIELD_PORT a client's "Enter Battle" (action 1)
    // or "Leave Queue" (action 0) button sends. Queued, never handled inline:
    // the handler is THREADUNSAFE and runs on the bot's next session update,
    // when the queue is not mid-iteration.
    static void QueuePortPacket(Player* bot, std::uint8_t arenaType, std::uint32_t bgTypeId,
                                std::uint8_t action);

    // ---- the anti-repeat memory -----------------------------------------
    //
    // The pool characters a player's LAST fill fielded, passed over by the
    // next fill's draw while it has an alternative. One fill deep, RAM-only,
    // like the dungeon fill's.
    std::vector<ObjectGuid> const* RecentBotsFor(ObjectGuid player) const;
    void RememberBots(ObjectGuid player, std::vector<ObjectGuid> guids);

private:
    DcBgQueueFillManager() = default;

    // Why a join is not ours to fill. Empty = accepted. `transient` comes back
    // true for the one refusal worth coming back for: the concurrent cap.
    std::string RefuseReason(Player* player, std::uint8_t arenaType, bool isRated,
                             bool* transient = nullptr) const;

    // The fill this player (or their group's leader) already has, if any.
    DcBgQueueFillJob* JobFor(ObjectGuid player) const;

    // Is a HUMAN premade of the other faction already waiting in this
    // bracket? Then premade-versus-premade is on, and demoting would steal
    // its opponent.
    static bool OpposingHumanPremadeQueued(BattlegroundQueue const& queue, std::uint32_t bracketId,
                                           std::uint8_t leaderTeam);

    // Move an already-queued premade group into the normal queue — the
    // deferred-join counterpart of the OnAddGroup rewrite, for a group that
    // was left premade because the cap was full when it joined.
    static bool DemoteQueuedGroup(BattlegroundQueue& queue, GroupQueueInfo* ginfo);

    void OpenJob(Player* player, std::uint32_t bgTypeId, bool demoted);

    // ---- joins seen by OnAddGroup, consumed by OnPlayerJoinBG ------------
    struct PendingJoin
    {
        ObjectGuid leader;
        std::uint32_t bgTypeId = 0;
        bool premade = false;  // filed premade (and not demoted)
        bool demoted = false;
    };
    std::vector<PendingJoin> _pending;

    // ---- deferred joins ---------------------------------------------------
    //
    // A join refused only because the cap was full. The join hook fires once
    // per click, so without this the player would sit in a queue nobody fills
    // however much room frees up a moment later. Retried every
    // kDeferredRetryMs while they are still queued (and not yet invited).
    struct Deferred
    {
        ObjectGuid player;
        std::uint32_t bgTypeId = 0;
        std::uint32_t sinceMs = 0;
        std::uint32_t waitedMs = 0;
    };
    void TickDeferred(std::uint32_t diff);
    void Defer(Player* player, std::uint32_t bgTypeId);
    void ForgetDeferred(ObjectGuid player);
    static constexpr std::uint32_t kDeferredRetryMs = 2000;
    static constexpr std::size_t kMaxDeferred = 32;
    std::vector<Deferred> _deferred;

    // ---- bots released while somebody else still needed them ------------
    struct Orphan
    {
        ObjectGuid guid;
        std::string jobId;
        std::uint32_t ageMs = 0;
        bool logout = true;
    };
    void TickOrphans(std::uint32_t diff);
    // The release itself, with no adoption: leave the match and the queue,
    // strip Deserter, log out.
    void FinishRelease(ObjectGuid guid, Player* bot, bool logout, std::string const& jobId);
    static constexpr std::uint32_t kOrphanSweepMs = 1000;
    // A released guid with no player behind it is watched this long for a
    // login still in flight.
    static constexpr std::uint32_t kOrphanLoginWaitMs = 60000;
    std::vector<Orphan> _orphans;
    std::uint32_t _orphanSweepMs = 0;

    struct RecentBots
    {
        ObjectGuid player;
        std::vector<ObjectGuid> guids;
    };
    static constexpr std::size_t kRecentMemory = 64;
    std::vector<RecentBots> _recent;

    std::vector<std::unique_ptr<DcBgQueueFillJob>> _jobs;
    bool _configChecked = false;
};

#endif  // _PLAYERBOT_DCBGQUEUEFILLMANAGER_H
