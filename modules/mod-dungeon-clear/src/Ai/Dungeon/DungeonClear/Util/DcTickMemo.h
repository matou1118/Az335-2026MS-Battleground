/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _DC_TICK_MEMO_H
#define _DC_TICK_MEMO_H

#include <cstddef>
#include <cstdint>

class AiObjectContext;
class Player;
class Unit;
struct DungeonBossInfo;
struct ScriptedPullStage;

// Within-tick memo of a few predicates the DC trigger ladder evaluates more than
// once per engine tick (each internally a group walk / geometry pass / pathfinder
// probe whose answer cannot change between two reads in the same tick):
//
//   - DcEngageGeometry::IsAtBossEngage  — at-boss / blocking-trash / pull triggers
//     plus Advance's own read (4 calls; runs a PathGenerator probe off the
//     same-floor fast path).
//   - DcPartyState::IsBetweenPullsReady — at-boss / blocking-trash / room-trash /
//     stalled / pull triggers (strict, requireNoLoot=true), and the advance/event
//     actions (loose, requireNoLoot=false). Each is a full party walk.
//   - ScriptedPullRegistry::DueStage — the pull-target value, the effective
//     pull-mode value, the pull trigger's fight-in-place exception and the pull
//     action's commit branch all ask for it (4+ calls), and each miss runs one
//     entry-filtered volume scan PER STAGE with a reachability probe on every
//     candidate. Cheap-gated to the one map and the arm radius, but inside that
//     window it is the most expensive thing in the ladder.
//
// This is NOT a cross-tick cache (that was the door-verdict staleness-bug class):
// the 50ms window can never span two AI ticks (>=100ms apart), so it deduplicates
// strictly WITHIN a tick while guaranteeing every tick recomputes. Owned as a
// per-bot value ("dungeon clear tick memo"); leader-only consumers.
struct DcTickMemo
{
    std::uint32_t stampMs = 0;          // getMSTime() of the tick that filled it
    // Cached answers, valid only while stampMs is current: -1 unset / 0 / 1.
    std::int8_t atBossEngage = -1;
    std::int8_t betweenPullsReadyLoose = -1;   // requireNoLoot == false
    std::int8_t betweenPullsReadyStrict = -1;  // requireNoLoot == true
    // Due scripted-pull stage ORDER: -2 unset, -1 none due, >= 0 the stage's order.
    // (-1 is a meaningful answer here, so the "unset" sentinel has to be distinct.)
    std::int32_t scriptedStage = -2;
    // Party-level combat predicates (DcCombatFlag::AnyParty*) — full group
    // walks, asked by several rungs per tick and O(N^2)-shaped at raid sizes.
    // The heavy one (held-by-live-enemy: a pathfind per combat reference) also
    // remembers the radius it answered for; a different radius recomputes.
    std::int8_t partyEngagement = -1;
    std::int8_t partyCombatFlag = -1;
    std::int8_t partyHeldByLiveEnemy = -1;
    float       partyHeldRadius = -1.0f;
    // Standing on a NO_STOP leg of the authored route (DcNoStopZone) — a walk of
    // the route's anchors, asked by the pull-mode value and the leader-assist
    // trigger on the same tick.
    std::int8_t noStopZone = -1;

    // CROSS-LEVEL reachability probes taken this tick, keyed by the candidate's
    // raw GUID. Each miss is a full PathGenerator query, and the follower assist
    // pickers re-ask for the SAME candidates every tick: they rank every attacker
    // of every groupmate within 1.5x PartyMaxSpread, probing each one that would
    // win the rank. In the state the probe was added for — a mob formation
    // overhead flagging a forty-man through the floor — that is a dozen Detour
    // queries per follower per tick.
    //
    // A FIXED ARRAY, not a vector: this lives in a value that is wholesale
    // reassigned once per tick, and a heap buffer there would trade one query for
    // one allocation. Overflow simply falls back to probing, which is what the
    // code did before the memo existed.
    //
    // Same-level candidates never take a slot — that answer is one fabs.
    static constexpr std::size_t kLevelReachableSlots = 16;
    struct LevelReachableEntry
    {
        std::uint64_t guid = 0;
        std::uint8_t  reachable = 0;
    };
    LevelReachableEntry levelReachable[kLevelReachableSlots]{};
    std::uint8_t levelReachableCount = 0;

    static constexpr std::uint32_t kMemoWindowMs = 50;

    // True while a memo stamped at `stampMs` is still within the current tick's
    // window. ms-wraparound safe (uses getMSTimeDiff semantics).
    static bool MemoValid(std::uint32_t stampMs, std::uint32_t now);

    // If the window has elapsed since `stampMs` (or it was never stamped), clear
    // every cached field and re-stamp to `now` — opening a fresh tick. Call once
    // before reading/writing a field.
    void EnsureFresh(std::uint32_t now);
};

// Memoised accessors. Each resolves the per-bot memo value, refreshes it for the
// current tick, and computes its predicate at most once per tick. Null bot/ctx
// fall back to a direct (unmemoised) computation.
class DcTickMemoAccess
{
public:
    static bool AtBossEngage(Player* bot, AiObjectContext* ctx,
                             DungeonBossInfo const& next);
    static bool BetweenPullsReady(Player* bot, AiObjectContext* ctx,
                                  bool requireNoLoot);
    // The ScriptedPullRegistry stage due this tick, or nullptr. Call this rather
    // than ScriptedPullRegistry::DueStage from anything on the per-tick path; the
    // registry entry point is the uncached one. (Rows are static table entries, so
    // the pointer is stable for the process lifetime.)
    static ScriptedPullStage const* ScriptedStage(Player* bot, AiObjectContext* ctx);

    // DcEngageGeometry::IsLevelReachable, memoised per candidate for this tick.
    // Same-level candidates short-circuit without touching the memo; only the
    // off-level ones — the ones that cost a Detour query — are remembered.
    // Call this rather than IsLevelReachable from anything that ranks a POOL of
    // candidates; the geometry entry point is the uncached one.
    static bool LevelReachable(Player* bot, AiObjectContext* ctx, Unit* u);
};

// Per-bot "my DC trigger ladder ran this tick" heartbeat (DcKey::LastTickMs).
//
// Separate from DcTickMemo above, which only stamps when a leader-only consumer
// asks it for a predicate — a bot whose every rung declines never touches it, so
// its stamp cannot answer "is this bot ticking at all". This one is written
// unconditionally, before any gate, on BOTH engines' ladders.
//
// What it buys: the teardown snapshot could not previously tell the two halves of
// a frozen run apart. A tank whose DC rungs are all standing down (ladder
// evaluated, every rung declines) and a tank whose AI is not being updated at all
// look identical in every other field — same frozen phase token, same zeroed
// watchdogs, same stale route. Live (tp-20260831-083759-1, Gundrak): five runs
// wedged at the Colossus altar with the tank silent for ten minutes while its
// four followers ticked thousands of times, and the record could not say which.
//
// Both engines matter: the non-combat ladder is not evaluated while a bot sits on
// the combat engine, so stamping only there would read "frozen" for the whole of
// every boss fight — a clock/latch mismatch of exactly the kind that has burned
// this diagnostic before. Stamped on both, a stale value means the bot's AI is
// not being updated, whatever it is doing.
namespace DcTickHeartbeat
{
    // Record that this bot's DC trigger ladder is being evaluated right now.
    void Stamp(AiObjectContext* ctx);

    // getMSTime() of the last Stamp, or 0 when the ladder has never run for this
    // bot. Reading lazily creates the value (answering 0), which is the truthful
    // answer for a bot that has never had a DC ladder — so this stays safe to
    // call from the read-only diagnostic snapshot.
    std::uint32_t LastMs(AiObjectContext* ctx);
}

#endif  // _DC_TICK_MEMO_H
