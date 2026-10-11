/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCBGQUEUEFILLJOB_H
#define _PLAYERBOT_DCBGQUEUEFILLJOB_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "ObjectGuid.h"

#include "BgQueueFill/DcBgQueueFillPlanner.h"

class Battleground;
class Player;

// One battleground instant-fill attempt, as a timeout-bounded state machine.
//
// The feature in one sentence: a real player queues for a battleground, pool
// bots of BOTH factions queue for the same battleground in the same bracket
// through the same packet a client sends, and the STOCK matchmaker pops the
// match. The player gets the ordinary "enter battle" popup and answers it
// themselves.
//
// Like DcDungeonQueueFillJob, the job deliberately does little of the work: it
// does not build the match (BattlegroundQueue does), does not invite anybody,
// and normally does not accept for the bots (playerbots' `bg status` handler
// does, off SMSG_BATTLEFIELD_STATUS — see TickInvites for the safety net).
//
// Unlike the dungeon fill, the bots are not one batch. A 40v40 Alterac Valley
// is 79 bots, and the match pops at 20v20, so the slots each run their own
// pipeline (SlotStage) and the ones that get each side to the battleground's
// minimum run it first. The job's own Stage follows the PLAYER — queued,
// invited, in the match — while the remaining slots keep streaming into the
// running instance through its free slots.
//
// Threading: world thread only. The join hooks fire from THREADUNSAFE opcode
// handlers (World::UpdateSessions) and Tick from the module's world tick, both
// inside World::Update with the map workers idle. Nothing here is driven from a
// battleground hook that can run on a map thread — invites, arrivals and the
// match ending are all read back on the tick.
class DcBgQueueFillJob
{
public:
    enum class Stage : std::uint8_t
    {
        Observed,    // join seen; confirm the player really is in the queue
        Planning,    // read the queue, the template and the pool; plan the slots
        Filling,     // slots are being set up; the player is queued, not yet invited
        Waiting,     // every start-critical bot is queued, or the match popped; the player has not entered
        InBattle,    // the player is in the match; late slots keep streaming in
        Releasing,   // teardown; every terminal path lands here
        Done,
    };

    // Each slot's own pipeline — the dungeon fill's stages, per bot.
    enum class SlotStage : std::uint8_t
    {
        Claiming,      // not yet drawn from the pool (waiting for a login slot)
        LoggingIn,     // AddPlayerBot(guid, 0); wait for IsInWorld + bot AI
        Evicting,      // put down in its faction capital
        Provisioning,  // one PlayerbotFactory roll per world tick (shared budget)
        Sanitizing,    // deserter, death-knight chain, stale queues, group, health
        Queueing,      // CMSG_BATTLEMASTER_JOIN sent; wait for the queue to hold it
        Queued,        // in the queue, invited, or in the match
        Dropped,       // gave up on this one; released on its own
    };

    static char const* StageName(Stage s);
    static char const* SlotStageName(SlotStage s);

    struct Slot
    {
        DcBgQueueFillPlanner::Slot plan;
        ObjectGuid guid;
        std::string name;
        SlotStage stage = SlotStage::Claiming;
        std::uint32_t stageMs = 0;
        bool lfgLeaveSent = false;
        bool bgLeaveSent = false;
        bool queueSent = false;
        bool redrawn = false;           // already swapped for another draw once
        bool strategiesChecked = false; // battleground strategies confirmed once inside
        std::uint8_t requeues = 0;      // times put back after falling out of the queue
        std::uint32_t invitedMs = 0;    // how long the current invite has gone unanswered
        std::uint32_t portRetryMs = 0;  // countdown to the next fallback accept
    };

    // Record an intent. `player` is the queuing player (the group leader for a
    // group join), `bgTypeId` the type they queued for (BATTLEGROUND_RB for a
    // Random queue), `demoted` true when the join hook moved their premade
    // group into the normal queue.
    static std::unique_ptr<DcBgQueueFillJob> Observe(Player* player, std::uint32_t bgTypeId,
                                                     bool demoted);

    void Tick(std::uint32_t diff);

    bool Done() const { return _stage == Stage::Done; }
    Stage GetStage() const { return _stage; }
    std::string const& Id() const { return _id; }
    ObjectGuid PlayerGuid() const { return _playerGuid; }
    std::uint32_t QueueTypeId() const { return _queueTypeId; }
    std::uint32_t BracketId() const { return _bracketId; }

    // Still costing setup work the MaxConcurrent cap exists to bound. A job
    // whose player is in the match does not count, even while late slots are
    // still streaming in: the match is already running.
    bool IsSettingUp() const
    {
        return _stage == Stage::Observed || _stage == Stage::Planning || _stage == Stage::Filling ||
               _stage == Stage::Waiting;
    }

    // Has the player moved on from this fill — no longer queued for it, not
    // invited to it and not inside its match? True means a new join from the
    // same player must not be refused because of this job.
    bool PlayerHasMovedOn() const;

    // Pool characters this fill holds, in any slot stage but Dropped.
    bool HoldsBot(ObjectGuid guid) const;

    // Bots this fill is still bringing to `side` of queue (`queueTypeId`,
    // `bracketId`) that the queue cannot see yet — claimed, logging in, being
    // set up. Another fill planning for the same queue counts them as present.
    std::uint32_t InFlightFor(std::uint32_t queueTypeId, std::uint32_t bracketId,
                              std::uint8_t side) const;

    std::vector<ObjectGuid> BotGuids() const;

    // Force the job onto its terminal path. Idempotent. `notifyPlayer` is for
    // the operational failures only (see DcDungeonQueueFillJob::Release).
    // `force` pulls every bot out regardless of whose match it is standing
    // in — shutdown, and the feature being switched off.
    void Release(std::string const& reason, bool notifyPlayer = false, bool force = false);

    std::string StatusLine() const;

private:
    DcBgQueueFillJob() = default;

    void EnterStage(Stage s);
    void EnterSlotStage(Slot& slot, SlotStage s);

    void TickObserved();
    void TickPlanning();
    // The player side: queued, invited, inside, gone.
    void TickPlayer();
    void TickInBattle(std::uint32_t diff);
    // Every slot's pipeline, one step each.
    void TickSlots();
    // The accept safety net and the in-match strategy check.
    void TickInvites(std::uint32_t diff);
    // Re-run the queue while a bot of ours sits in it uninvited.
    void TickNudge(std::uint32_t diff);
    void TickReleasing();

    void TickSlotClaiming(std::size_t i, std::uint32_t& loginsStarted);
    void TickSlotLoggingIn(Slot& slot);
    void TickSlotEvicting(Slot& slot);
    bool TickSlotProvisioning(std::size_t i);  // true if it spent the tick's roll
    void TickSlotSanitizing(Slot& slot);
    void TickSlotQueueing(std::size_t i);
    void TickSlotQueued(Slot& slot);

    // A slot could not be set up. A start-critical slot before the match has
    // popped takes the whole fill with it (without it the match cannot pop);
    // any other is dropped and the match starts that one short.
    void FailSlot(std::size_t i, std::string const& reason);

    // Re-draw slot `i` on a different pool character (and, if its class has
    // none, a different class of the same role). False when nothing is left
    // to try or the slot has already been swapped once.
    bool RedrawSlot(std::size_t i);

    ObjectGuid ClaimFor(Slot& slot);

    // Random Battleground, phase B: the core has rolled the concrete map, so
    // re-plan against its maximum and add the extra slots. Once per fill.
    void MaybeTopUp(Battleground* bg);

    // The battleground the player is invited to or inside, if any.
    Battleground* FindPlayersBattleground() const;

    std::uint32_t CountSlots(SlotStage s) const;
    bool AnyQueued() const;
    bool AllCriticalQueued() const;
    bool AllSettled() const;

    Player* FindPlayer() const;

    std::string _id;
    ObjectGuid _playerGuid;
    std::string _playerName;
    std::uint32_t _level = 0;
    std::uint8_t _team = DcBgQueueFillPlanner::kTeamAlliance;
    bool _demoted = false;

    std::uint32_t _bgTypeId = 0;     // what the player queued for (BATTLEGROUND_RB for Random)
    std::uint32_t _queueTypeId = 0;
    std::uint32_t _bracketId = 0;
    std::uint32_t _bracketMinLevel = 0;
    std::uint32_t _bracketMaxLevel = 0;
    std::uint32_t _minPerTeam = 0;
    std::uint32_t _maxPerTeam = 0;
    bool _isRandom = false;
    bool _toppedUp = false;          // Random: the phase-B top-up has been planned

    std::uint32_t _bgInstanceId = 0;
    std::uint32_t _noHumanMs = 0;    // time the match has had no real player in it
    std::uint32_t _endedMs = 0;      // time since the match ended (WAIT_LEAVE)
    std::uint32_t _queueNudgeMs = 0;

    std::uint32_t _gearQuality = 0;
    std::uint32_t _gearScoreLimit = 0;
    std::uint32_t _gearIlvl = 0;

    std::uint32_t _seed = 0;
    std::uint32_t _drawState = 0;
    std::vector<ObjectGuid> _avoidGuids;  // this player's previous fill

    std::vector<Slot> _slots;

    Stage _stage = Stage::Observed;
    std::uint32_t _stageMs = 0;
    std::uint32_t _totalMs = 0;
    std::string _releaseReason;
    bool _notifyPlayer = false;
    bool _forceRelease = false;
};

#endif  // _PLAYERBOT_DCBGQUEUEFILLJOB_H
