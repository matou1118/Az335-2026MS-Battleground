/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCDUNGEONQUEUEFILLJOB_H
#define _PLAYERBOT_DCDUNGEONQUEUEFILLJOB_H

#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "ObjectGuid.h"

#include "DungeonQueueFill/DcDungeonQueueFillPlanner.h"

class Group;
class Player;

// One RDF instant-fill attempt, as a timeout-bounded state machine.
//
// The whole feature is one sentence: a real player clicks Find Group, bots with
// the missing roles queue for the same dungeon through the same queue, and the
// STOCK matchmaker pairs them. The player gets the ordinary Dungeon Found popup
// and accepts it themselves. Nothing about the client flow changes.
//
// So this class deliberately does very little of the work. It does not form the
// group (LFGQueue does), does not send the proposal (LFGMgr does), does not
// accept for the bots (playerbots' own `lfg accept` action does, off the
// SMSG_LFG_PROPOSAL_UPDATE handler that is on every bot's non-combat engine),
// and does not teleport anyone in (LFGMgr::MakeNewGroup does). It provisions
// bots, puts them in the queue, and gets out of the way.
//
// Stage discipline mirrors DcTestRunJob: every stage is bounded by a timeout,
// and a timeout lands in Releasing — never in a stuck stage. Releasing is the
// single funnel every terminal path goes through, so "who logs the bots out"
// has exactly one answer.
//
// Threading: world thread only. The intent hook fires from session packet
// handling and Tick from the module's per-world-tick hook, both of which run
// inside World::Update with the map-update workers idle — so the LFG reads here
// never race the compatibility pass (LFGMgr::Update task 1), which is the one
// piece of LFG that runs on a worker.
class DcDungeonQueueFillJob
{
public:
    enum class Stage : std::uint8_t
    {
        Observed,      // queue seen; waiting for LFGMgr to confirm LFG_STATE_QUEUED
        Planning,      // read back the authoritative dungeons + roles; compute the deficit
        Claiming,      // draw offline pool characters per required role
        LoggingIn,     // AddPlayerBot(guid, 0); wait for IsInWorld + bot AI
        Evicting,      // put every bot down in its faction capital (see TickEvicting)
        Provisioning,  // one PlayerbotFactory roll per world tick (shared budget)
        Sanitizing,    // strip deserter/cooldown auras, ungroup, heal, leave any queue
        Queueing,      // CMSG_LFG_JOIN per bot; wait for each to reach LFG_STATE_QUEUED
        Waiting,       // the matchmaker's turn; watch for the proposal / the group
        Formed,        // the player's LFG group holds our bots — hand off and shadow it
        Releasing,     // teardown; every terminal path lands here
        Done,
    };

    static char const* StageName(Stage s);

    // One planned bot.
    struct Slot
    {
        DcDungeonQueueFillPlanner::Slot plan;
        ObjectGuid guid;          // pool character claimed for the slot
        std::string name;         // resolved once the character is in world
        bool relocated = false;   // standing in its faction capital, not wherever it logged in
        bool provisioned = false;
        bool sanitized = false;
        bool queueSent = false;
        bool queued = false;      // confirmed at LFG_STATE_QUEUED
        bool attuned = false;     // credited with `_accessTargets` (see TickSanitizing)
        bool redrawn = false;     // already swapped for another draw once
        std::uint8_t requeues = 0;  // times put back after the core ejected it
    };

    // Record an intent. `player` is the queuing player (the group leader for a
    // group queue); `roles` and `dungeons` are the hook's RAW arguments and are
    // kept only for the log line — everything the job acts on is read back from
    // LFGMgr AFTER the core has validated the join (see TickPlanning).
    static std::unique_ptr<DcDungeonQueueFillJob> Observe(Player* player, std::uint8_t roles,
                                                          std::set<std::uint32_t> const& dungeons);

    // Drive from the world thread, once per tick.
    void Tick(std::uint32_t diff);

    bool Done() const { return _stage == Stage::Done; }
    Stage GetStage() const { return _stage; }
    std::string const& Id() const { return _id; }
    ObjectGuid PlayerGuid() const { return _playerGuid; }
    ObjectGuid GroupGuid() const { return _groupGuid; }
    // The guid LFGMgr keys this queue by: the group's when there is one, else
    // the player's. A group queue passes through LFG_STATE_ROLECHECK before
    // LFG_STATE_QUEUED, which is why the wait is on a state and not an event.
    ObjectGuid LfgGuid() const { return _lfgGuid; }

    // Pool characters this fill holds — claimed, logging in, or queued.
    bool HoldsBot(ObjectGuid guid) const;

    // Is `bot` one of ours, at a point where a dungeon proposal is something
    // we want said yes to? True only while the fill is still trying to match:
    // once it has formed, any further proposal belongs to somebody else.
    bool WantsProposalAcceptFor(ObjectGuid bot) const
    {
        return (_stage == Stage::Queueing || _stage == Stage::Waiting) && HoldsBot(bot);
    }
    std::vector<ObjectGuid> BotGuids() const;

    // Force the job onto its terminal path (operator cancel, shutdown, the
    // player logging out). Idempotent.
    //
    // `notifyPlayer` tells the player their fill did not happen. It is set for
    // the OPERATIONAL failures only — an empty pool, bots that would not
    // queue, a timeout — because those are the ones where the player is left
    // waiting on a queue that is quietly slower than they were promised.
    // Cancelling, matching with real players, and the dungeon simply ending
    // are not failures and say nothing.
    void Release(std::string const& reason, bool notifyPlayer = false);

    std::string StatusLine() const;

private:
    DcDungeonQueueFillJob() = default;

    void EnterStage(Stage s);
    void TickObserved();
    void TickPlanning();
    void TickClaiming();
    void TickLoggingIn();
    void TickEvicting();
    void TickProvisioning();
    void TickSanitizing();
    void TickQueueing();
    void TickWaiting();
    void TickFormed();
    void TickReleasing();

    // One-time handoff the moment the group exists: the proposal's leader is
    // picked inside LFGQueue and can land on a bot, which is the single most
    // visible failure this feature has.
    void HandOffGroup(Group* grp);

    // Put back any of our bots the core has thrown out of the queue. Returns
    // how many it had to touch, so the wait can say so out loud.
    //
    // A failed proposal does not put everyone back. LFGMgr::RemoveProposal
    // re-queues the members that answered AGREE and REMOVES the ones that
    // answered DENY, stamping each of those with a 150s
    // LFG_SPELL_DUNGEON_COOLDOWN. Playerbots answers DENY for any bot that is
    // in combat or dead when the proposal lands (LfgAcceptAction), which a
    // freshly-provisioned bot parked out in the open world can easily be.
    //
    // The fill is then short a role for good, and nothing upstream notices:
    // the wait watches the PLAYER's LFG state and the player is still queued
    // perfectly happily, so the job sits there until MatchTimeoutSec runs out.
    // Observed live — Benjy's tank answered DENY three seconds in and the fill
    // spent its whole 122s budget waiting for a proposal that could never be
    // built out of four remaining bots with no tank among them.
    //
    // The cooldown has to come off before the re-join: JoinLfg refuses on it
    // (LFG_JOIN_RANDOM_COOLDOWN) and answers the BOT's own session, so an
    // un-stripped retry is dropped silently.
    std::size_t RequeueDroppedBots();

    // True while `bot` belongs to a formed LFG group that is NOT this job's —
    // the stock matchmaker matched it into somebody else's dungeon. Releasing
    // must leave that bot exactly where it is.
    bool BotIsSomebodyElses(Player* bot) const;

    // Draw one free offline pool character of `classId` for the fill's
    // faction, or Empty when the class has none available. Not const: the draw
    // advances `_drawState`.
    //
    // A DRAW, not a scan. Taking the first free character of a class would
    // hand the same named bot to every fill that ever rolls that class, which
    // is most of the way to "I keep getting the same party" on its own.
    ObjectGuid ClaimPoolCharacter(std::uint8_t classId);

    // `_slots` in this job's own pool order, but shuffled off the fill's seed
    // so a substitution is not always the same class in pool-declaration
    // order. Used by both the claim-time fallback and RedrawSlot.
    std::vector<DcTestComp::Slot> ShuffledRolePool(char const* role,
                                                   DcTestComp::Roster roster);

    // Record the roster this fill is fielding as the player's last party.
    void RememberRoster();

    // Re-draw slot `i` on a different class of the same role. False when
    // nothing is left to try (or the slot has already been swapped once).
    bool RedrawSlot(std::size_t i);

    // Resolved gear ceiling for the fill, read from the conf once at Planning.
    std::uint32_t _gearQuality = 0;
    std::uint32_t _gearScoreLimit = 0;
    std::uint32_t _gearIlvl = 0;

    Player* FindPlayer() const;

    // Every member of the queuing side, with the role mask LFGMgr recorded for
    // them. One entry (the player) for a solo queue.
    std::vector<DcDungeonQueueFillPlanner::Human> ReadHumans() const;

    // The lowest LFGDungeons.dbc ExpansionLevel across `_dungeons` — the same
    // field LFGMgr's own lock map judges the queue by. Lowest, not first: a
    // multi-dungeon queue may enter any of them, so the roster has to satisfy
    // the oldest one.
    std::uint32_t ReadDungeonExpansion() const;

    // Every (map, difficulty) the queued set can put the party into, as the
    // pairs DcDungeonAccess::GrantEntry takes. Resolved once at Planning: the
    // set cannot change under us, because the job releases the moment the
    // player leaves the queue.
    //
    // A random queue stores the single rDungeonId, whose own map is 0 and
    // which carries no access rows of its own, so it is expanded to the pool
    // it draws from — otherwise the roll can only ever land on the dungeons
    // the bots happen to already qualify for.
    std::vector<std::pair<std::uint32_t, std::uint8_t>> ReadAccessTargets() const;

    std::string _id;
    ObjectGuid _playerGuid;
    ObjectGuid _groupGuid;   // Empty for a solo queue
    ObjectGuid _lfgGuid;
    std::string _playerName;

    std::uint32_t _level = 0;
    // The LOWEST expansion in the queued dungeon set (0 classic, 1 TBC, 2
    // Wrath). Read once at Planning and kept, because the redraw path needs
    // the same death-knight gate the plan was made under.
    std::uint32_t _expansion = 0;
    bool _isAlliance = true;

    // The dungeon set the bots copy, read back post-validation. For a random
    // queue this is the single rDungeonId LFGMgr stored, which is exactly the
    // id the bots must queue for; for a specific queue it is the
    // compatible-filtered list. Either way it is copied verbatim.
    std::set<std::uint32_t> _dungeons;

    // (map, difficulty) pairs for `_dungeons`, read once at Planning. See
    // ReadAccessTargets, and TickSanitizing for what is done with them.
    std::vector<std::pair<std::uint32_t, std::uint8_t>> _accessTargets;

    // Hook arguments, for the observation log line only.
    std::uint8_t _requestedRoles = 0;
    std::size_t _requestedDungeonCount = 0;

    std::vector<Slot> _slots;
    std::size_t _provisionIdx = 0;

    // The fill's seed, and the running PRNG state every draw in this job
    // shares. Both derive from the job id, so a fill replays from its log line
    // and nothing here reaches for rand().
    std::uint32_t _seed = 0;
    std::uint32_t _drawState = 0;

    // What this player's PREVIOUS fill fielded, copied out of the manager at
    // Planning before this fill overwrites it. The class draw and the
    // pool-character draw both pass over these while they have an alternative
    // — see DcDungeonQueueFillManager::RecentParty.
    std::vector<std::uint8_t> _avoidClasses;
    std::vector<ObjectGuid> _avoidGuids;

    // The LFG group our bots landed in. Set once, in Formed.
    ObjectGuid _formedGroupGuid;
    bool _handedOff = false;
    bool _autoClearIssued = false;
    // A proposal that fails re-queues the survivors; the core will try again.
    // Sit through exactly one of those before giving up.
    bool _sawProposal = false;
    std::uint32_t _proposalFailures = 0;

    Stage _stage = Stage::Observed;
    std::uint32_t _stageMs = 0;
    std::uint32_t _totalMs = 0;
    std::string _releaseReason;
    bool _notifyPlayer = false;
};

#endif  // _PLAYERBOT_DCDUNGEONQUEUEFILLJOB_H
