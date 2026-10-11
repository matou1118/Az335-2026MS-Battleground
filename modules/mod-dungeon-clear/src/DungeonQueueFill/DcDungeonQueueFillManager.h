/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCDUNGEONQUEUEFILLMANAGER_H
#define _PLAYERBOT_DCDUNGEONQUEUEFILLMANAGER_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "ObjectGuid.h"

class DcDungeonQueueFillJob;
class Player;
class WorldPacket;

// Registry of live RDF instant-fill jobs, ticked from the module's global world
// tick alongside the `.dc test` harness.
//
// Deliberately thin: it owns the admission policy (is the feature on, is this
// player eligible, is there room for another job) and the job list, and nothing
// else. Everything a fill actually does lives in DcDungeonQueueFillJob.
//
// World thread only — see the threading note on DcDungeonQueueFillJob. Unlike
// DcTestRunManager there are no observer callbacks arriving from map threads,
// so no lock is needed.
class DcDungeonQueueFillManager
{
public:
    static DcDungeonQueueFillManager& Instance();

    // The LFG intent hook. Called at the top of LFGMgr::JoinLfg, BEFORE the
    // core validates the join, so it may only record an intent — never act on
    // the arguments. Returns nothing and refuses nothing: the observer must be
    // incapable of changing whether a player can queue.
    void OnQueueIntent(Player* player, std::uint8_t roles, std::set<std::uint32_t> const& dungeons);

    // A player logging out mid-fill: release their job on the spot rather
    // than a tick later, when their Player* is already gone.
    void OnPlayerLogout(Player* player);

    // SMSG_LFG_PROPOSAL_UPDATE on its way to one of our bots: answer it.
    //
    // The fill used to leave this to playerbots' own `lfg accept`, and that is
    // the single least reliable thing the feature depended on. That action can
    // only ever answer the proposal packet it is handed in the same AI tick —
    // its other branch reads an AI value that NOTHING in playerbots ever sets
    // to a non-zero id, so it is dead code, and so is the `lfg proposal active`
    // trigger built on it. Miss that one tick and the bot never answers at all.
    //
    // The core's punishment for not answering is severe and lands on the whole
    // party: after LFG_TIME_PROPOSAL (40s) RemoveProposal marks every PENDING
    // member DENY, removes them from the queue and stamps each with a 150s
    // cooldown. Observed live — all four of Benjy's bots sat out a proposal
    // and came back wearing spell 71328, and the fill died as "the proposal
    // failed twice" having never had a chance.
    //
    // So the fill answers for its own bots, straight into LFGMgr, off the
    // packet that carries the proposal id. No AI tick, no trigger, no timing.
    void OnBotProposal(Player* bot, WorldPacket const& packet);

    void Tick(std::uint32_t diff);

    // Multi-line "N fills in flight" report for `.dc dungeonqueuefill status`.
    std::string StatusText() const;

    // Force-release the job belonging to a named player. False (with *msg set)
    // when there is no such job.
    bool Cancel(std::string const& playerName, std::string* msg);

    // Open a fill for a player who is ALREADY in the queue, bypassing the
    // intent hook. The whole point is repeatable live testing: without it,
    // every trial means leaving the queue and clicking Find Group again, and
    // the interesting cases (a second fill, an exhausted pool, a re-queue
    // straight after a dungeon) are the ones hardest to reach that way.
    // Refuses for the same reasons the hook does, said out loud.
    bool ForceFill(Player* player, std::string* msg);

    // Release everything — worldserver shutdown, or the feature being turned
    // off mid-flight.
    void ReleaseAll(std::string const& reason);

    std::size_t ActiveCount() const { return _jobs.size(); }

    // Fills still being SET UP — everything before Formed. This, not
    // _jobs.size(), is what MaxConcurrent bounds.
    //
    // A job does not end at Formed: it shadows the group for the whole run so
    // it can log the bots out when the dungeon finishes. Counting those toward
    // the cap meant one party clearing Utgarde Keep held a slot for the best
    // part of an hour, and the second live dungeon on the realm silently got
    // "concurrent fill cap reached" instead of a party. The cost the cap
    // exists to bound — PlayerbotFactory rolls at one per world tick — is
    // spent entirely before Formed and is zero after it.
    std::size_t SettingUpCount() const;

    // Is this pool character already claimed by a fill? The mirror of
    // DcTestRunManager::IsReserved: the two subsystems draw from one addclass
    // pool, and a fill's claim is invisible to a test run for the tick between
    // picking the guid and AddPlayerBot landing.
    bool IsClaimed(ObjectGuid guid) const;

    // ---- the anti-repeat memory -----------------------------------------
    //
    // What a player's LAST fill fielded. Two dungeons in a row must not hand
    // the player the same party, and neither draw can promise that on its own:
    // the class draw picks from a tank pool three deep, and the pool-character
    // draw picks from however many characters of that class the addclass pool
    // holds. Both are told what came last and pass over it while they have an
    // alternative.
    //
    // Deliberately one fill deep and RAM-only. Two-in-a-row is the complaint;
    // remembering further back would start starving a small pool, and a
    // worldserver restart forgetting who you last ran with costs nothing.
    struct RecentParty
    {
        ObjectGuid player;
        std::vector<std::uint8_t> classIds;
        std::vector<ObjectGuid> guids;
    };

    // The previous party for `player`, or nullptr for a player with no fill
    // behind them.
    RecentParty const* RecentPartyFor(ObjectGuid player) const;

    // Record the party a fill just claimed, replacing that player's previous
    // one. Called once the roster is settled, not once the dungeon ends: a
    // fill the player cancels and immediately re-queues is exactly the
    // back-to-back case, and it should not hand them the same five again.
    void RememberParty(ObjectGuid player, std::vector<std::uint8_t> classIds,
                       std::vector<ObjectGuid> guids);

private:
    // Bound on the memory. A linear scan over a handful of entries is nothing
    // next to a fill, and the oldest entry is dropped when it is reached — the
    // player it belonged to has long since finished their dungeon.
    static constexpr std::size_t kRecentPartyMemory = 64;

    DcDungeonQueueFillManager() = default;

    // Why an intent was refused, for the log line. Empty = accepted.
    //
    // `transient` comes back true for a refusal that is only true right now —
    // the concurrent cap. Those are the ones worth coming back to: the player
    // is still standing in the queue, and in a few seconds there will be room.
    std::string RefuseReason(Player* player, std::set<std::uint32_t> const& dungeons,
                             bool* transient = nullptr) const;

    bool HasJobFor(Player* player) const;

    // ---- deferred intents ------------------------------------------------
    //
    // A player refused for a reason that will not last.
    //
    // The intent hook fires on CMSG_LFG_JOIN and nowhere else, so a refusal is
    // final by default: the player stays in the queue, no second packet is
    // ever sent, and no fill is ever opened for them again no matter how much
    // room frees up a moment later. Observed live — Jrad was refused at
    // 22:50:54 because two other fills happened to be mid-setup, one of them
    // finished four seconds later, and Jrad still sat in an empty queue with
    // no bots until he gave up and re-clicked Find Group.
    //
    // So a transient refusal is remembered instead of dropped, and retried on
    // the tick for as long as the player is still queued. The dungeons are not
    // kept: by retry time LFGMgr has validated and filtered the join, so they
    // are read back from it exactly as ForceFill does.
    struct Deferred
    {
        ObjectGuid player;
        std::uint32_t sinceMs = 0;   // for the log line only
        std::uint32_t waitedMs = 0;  // since the last retry
    };

    // Retry every deferred intent that can be retried, and forget the ones
    // whose player is no longer waiting on one.
    void TickDeferred(std::uint32_t diff);

    // Remember `player` for a later retry, unless they are already on the
    // list. Silently does nothing once the list is full — a realm with this
    // many players refused at once has a MaxConcurrent problem, not a
    // bookkeeping one.
    void Defer(Player* player);

    // Drop `player`'s pending retry, if any. Called when they get a fill by
    // any route, and when they stop waiting for one.
    void ForgetDeferred(ObjectGuid player);

    // Cadence for the retry sweep. A refused player is not in a hurry — they
    // are in a queue whose own matchmaker runs every few seconds — and this
    // keeps the sweep off the hot path of every world tick.
    static constexpr std::uint32_t kDeferredRetryMs = 2000;

    // Bound on the list, for the same reason kRecentPartyMemory has one.
    static constexpr std::size_t kMaxDeferred = 32;

    std::vector<Deferred> _deferred;

    // Answering a proposal can complete it, and completing it sends more
    // packets to the same bots, which re-enters OnBotProposal. Nothing below
    // is re-entrant, so refuse the nested call rather than reason about it.
    bool _inProposalAccept = false;

    std::vector<std::unique_ptr<DcDungeonQueueFillJob>> _jobs;

    // Oldest first; RememberParty appends and trims from the front.
    std::vector<RecentParty> _recent;
};

#endif  // _PLAYERBOT_DCDUNGEONQUEUEFILLMANAGER_H
