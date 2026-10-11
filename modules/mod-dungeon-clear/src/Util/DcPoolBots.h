/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCPOOLBOTS_H
#define _PLAYERBOT_DCPOOLBOTS_H

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "ObjectGuid.h"

class Player;

// The pool-bot plumbing the two queue fills share: drawing an offline addclass
// character, putting a freshly logged-in one down somewhere safe, clearing
// what its last outing left on it, and handing it back.
//
// Lifted out of DcDungeonQueueFillJob when the battleground fill arrived, so
// the two draw through ONE reservation check. Two copies of the claim would
// each see their own holdings and nothing of the other's, and the first thing
// a busy realm would do is hand one character to both.
//
// World thread only, like both callers.
namespace DcPoolBots
{
    // Is `guid` held by a queue fill — dungeon or battleground? The `.dc test`
    // harness asks this before it claims, and each fill asks it of the other.
    // A claim is invisible to the other subsystems for the tick between
    // picking a guid and AddPlayerBot landing, so "already online" is not a
    // substitute for asking.
    bool IsClaimedByFill(ObjectGuid guid);

    // Draw one free offline addclass-pool character of `classId` for the
    // given faction, or Empty when the class has none available.
    //
    // A DRAW, not a scan: addclassCache is an unordered_set whose iteration
    // order does not change between calls, so taking the first free character
    // would hand the same named bot to every fill that ever rolls that class.
    // `drawState` is the caller's splitmix32 stream, so a fill replays from
    // its seed.
    //
    // Skipped: anything `heldByCaller` claims, anything the `.dc test`
    // harness or either fill holds, anything already online, and anything in
    // a REAL guild. Characters in `avoid` are passed over while an
    // alternative exists, never to the point of failing the draw.
    ObjectGuid ClaimPoolCharacter(bool alliance, std::uint8_t classId, std::uint32_t& drawState,
                                  std::vector<ObjectGuid> const& avoid,
                                  std::function<bool(ObjectGuid)> const& heldByCaller);

    // How many characters ClaimPoolCharacter could still hand out for this
    // faction across every class, under the same exclusions. The battleground
    // planner caps each side at this so a fill that cannot reach the
    // battleground's minimum says so up front instead of timing out.
    std::uint32_t CountFree(bool alliance, std::function<bool(ObjectGuid)> const& heldByCaller);

    // One step of putting `bot` down in its faction's capital. True once it
    // is standing there; false while a teleport is still in flight (call
    // again next tick — the caller's stage timeout bounds the wait).
    //
    // A pool character logs in exactly where it was saved, which for this
    // pool is often inside a dungeon. See DcDungeonQueueFillJob::TickEvicting
    // for the three things that go wrong from there.
    //
    // `logTag` and `jobId` name the caller in the retry warning.
    bool EvictToCapital(Player* bot, bool alliance, char const* logTag, std::string const& jobId);

    // The capital's name, for log lines.
    char const* CapitalName(bool alliance);

    // Credit the death-knight starting chain if `bot` is a death knight who
    // never finished it. True when it had to — the caller logs that. The LFG
    // lock map and the battleground join handler both refuse an Acherus
    // death knight, silently, on the bot's own session.
    bool CreditDeathKnightChain(Player* bot);

    // The generic half of sanitising a recycled pool bot: out of any LFG
    // queue, out of any group, alive, out of combat, at full health. False
    // while a step is still settling (the LFG leave is a queued packet and
    // clears a tick or two later; the group removal can be refused mid-
    // teleport) — call again next tick. `lfgLeaveSent` is the caller's
    // per-bot flag so the leave packet goes out once.
    bool SanitizeCommon(Player* bot, bool& lfgLeaveSent);

    // Hand a pool bot back: out of any LFG queue, out of any group, and — when
    // `logout` — logged out through whichever holder owns its login.
    // `bot` may be null (already offline); the logout still resolves by guid.
    void ReleaseBot(ObjectGuid guid, Player* bot, bool logout, char const* logTag,
                    std::string const& jobId);
}

#endif  // _PLAYERBOT_DCPOOLBOTS_H
