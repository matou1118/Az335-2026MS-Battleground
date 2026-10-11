/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCBGQUEUEFILLPLANNER_H
#define _PLAYERBOT_DCBGQUEUEFILLPLANNER_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "TestRun/DcTestComp.h"

// The arithmetic behind a battleground instant fill: given who is already
// queued on each side, which bots does each faction still need, at what level,
// on which class, and in what order?
//
// Engine-free for the same reason DcDungeonQueueFillPlanner is: no Player, no
// BattlegroundQueue, no ObjectGuid, so every rule below is pinned by a gtest.
// Turning a plan into logins, factory rolls and CMSG_BATTLEMASTER_JOIN packets
// belongs to DcBgQueueFillJob.
//
// The rules the job depends on:
//
//   1. Every side is filled to the battleground's MAXIMUM, never its minimum —
//      a 40v40 Alterac Valley is 40v40. Everyone already on a side (players
//      in the queue, bots another fill is still bringing) is subtracted, so a
//      second human on the far side is counted rather than over-filled.
//   2. The bots that get each side to its MINIMUM are marked start-critical
//      and ordered first, alternating sides. BattlegroundQueue only pops a
//      normal match once both sides reach the minimum, so these are the bots
//      the player is actually waiting on; the rest stream in afterwards
//      through the running instance's free slots.
//   3. A side the pool cannot bring to its minimum is Unresolvable — the fill
//      could only ever queue half a match. A side that reaches its minimum
//      but not its maximum is NOT a failure: the shortfall is reported and
//      the match starts short.
//   4. The same seed gives the same plan, so a fill replays from its log line.
//
// Both passes of a Random Battleground fill are the same call with different
// numbers: first against the Random template (10 a side), then — once the core
// has rolled the concrete map — against that map's maximum with everyone
// already invited counted as present. See DcBgQueueFillJob.
namespace DcBgQueueFillPlanner
{
    // Side indexes, mirroring TeamId. Spelled out so the kernel stays free of
    // core headers; pinned against TEAM_ALLIANCE / TEAM_HORDE by a gtest.
    inline constexpr std::uint8_t kTeamAlliance = 0;
    inline constexpr std::uint8_t kTeamHorde    = 1;
    inline constexpr std::size_t kTeams         = 2;

    // "No limit" for SideCounts::poolFree.
    inline constexpr std::uint32_t kUnlimited = 0xFFFFFFFFu;

    // One healer per this many seats, rounded up, when no quota is configured.
    inline constexpr std::uint32_t kSeatsPerHealer = 5;

    // Everything already on one side before this plan adds a bot.
    struct SideCounts
    {
        // Players already counting toward the side: queued and not yet
        // invited for a first pass; invited to or inside the instance for a
        // Random top-up. Humans and bots alike — the core does not tell them
        // apart and neither does the seat count.
        std::uint32_t humansQueued = 0;
        // Pool bots still on their way to this side: another fill's bots not
        // yet in the queue, or this fill's own earlier bots not yet invited.
        std::uint32_t botsInFlight = 0;
        // How many of the bots already fielded on this side (either count
        // above) are healers. Counted against the healer quota so a top-up
        // does not add a second full healer complement.
        std::uint32_t healerBots = 0;
        // Free pool characters of this faction. The side is capped at it.
        std::uint32_t poolFree = kUnlimited;
    };

    struct Request
    {
        std::uint32_t minPerTeam = 0;
        std::uint32_t maxPerTeam = 0;
        // The PvPDifficulty bracket the player queued into. Every bot's level
        // lands inside it, or it would queue into a different bracket.
        std::uint32_t bracketMinLevel = 0;
        std::uint32_t bracketMaxLevel = 0;
        std::uint32_t playerLevel = 0;
        // Bots are drawn within this many levels of the player, clamped to the
        // bracket — a fair match, not a mirror of the player's level.
        std::uint32_t levelSpread = 2;
        // Healers per side; 0 = auto (one per kSeatsPerHealer seats of the
        // battleground's maximum, rounded up).
        std::uint32_t healersPerSide = 0;
        std::uint8_t playerTeam = kTeamAlliance;
        SideCounts sides[kTeams];
        std::uint32_t seed = 0;
        // Classes the player's previous fill fielded. Passed over within a
        // draw tier while the tier offers an alternative; never a reason to
        // fail.
        std::vector<std::uint8_t> avoidClasses;
    };

    // One bot to provision and queue.
    struct Slot
    {
        std::uint8_t team = kTeamAlliance;
        std::uint8_t classId = 0;
        char const* role = "dps";           // "heal" | "dps"
        char const* specName = "";          // premade-spec template to force
        char const* fallbackSpec = "";      // substring match if absent
        std::uint32_t level = 0;
        bool startCritical = false;         // needed for the side to reach the minimum
    };

    enum class Kind : std::uint8_t
    {
        Ok,
        NothingToDo,   // both sides are already at the maximum
        Unresolvable   // bad inputs, or a side the pool cannot bring to the minimum
    };

    struct Result
    {
        Kind kind = Kind::Unresolvable;
        // Start-critical slots first, alternating sides; then the rest,
        // alternating sides. Filled only when Ok.
        std::vector<Slot> slots;
        std::string detail;  // human sentence for every non-Ok kind
        std::uint32_t need[kTeams] = {0, 0};       // bots wanted per side
        std::uint32_t planned[kTeams] = {0, 0};    // bots planned per side
        std::uint32_t shortfall[kTeams] = {0, 0};  // need - planned: the pool ran dry past the minimum
    };

    Result Plan(Request const& req);

    // The healer quota the plan applies to one side of a `maxPerTeam` match
    // when `configured` is 0 (auto).
    std::uint32_t HealerQuota(std::uint32_t maxPerTeam, std::uint32_t configured);

    // The [lo, hi] level window bots are drawn from: the player's level
    // +/- levelSpread, clamped to the bracket. A player whose own level sits
    // outside the bracket (it cannot, via the join handler) is clamped first.
    std::pair<std::uint32_t, std::uint32_t> LevelWindow(Request const& req);

    // Death knights only at or above the level their kit starts at. A
    // battleground bracket has no expansion to gate on — a level-55 death
    // knight is as legal in Alterac Valley as a level-80 one.
    inline DcTestComp::Roster RosterFor(std::uint32_t level)
    {
        return level >= DcTestComp::kDeathKnightMinLevel ? DcTestComp::Roster::WithDeathKnights
                                                         : DcTestComp::Roster::NoDeathKnights;
    }

    char const* TeamName(std::uint8_t team);
}

#endif  // _PLAYERBOT_DCBGQUEUEFILLPLANNER_H
