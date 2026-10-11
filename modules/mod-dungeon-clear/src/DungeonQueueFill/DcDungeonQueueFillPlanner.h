/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCDUNGEONQUEUEFILLPLANNER_H
#define _PLAYERBOT_DCDUNGEONQUEUEFILLPLANNER_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "TestRun/DcTestComp.h"
#include "TestRun/DcTestDungeonRegistry.h"

// The role arithmetic behind an RDF instant fill: given the humans who just
// queued and the roles they ticked, which bots does the party still need?
//
// Engine-free on purpose — no Player, no LFGMgr, no ObjectGuid — for the same
// reason DcTestRoster and DcTestGearTiers are: this is the half of the feature
// with combinatorics in it (multi-role players, partial parties, the
// death-knight level gate), and a pure kernel is the only half that can be
// exhaustively unit-tested. Turning a plan into logins, factory rolls and
// CMSG_LFG_JOIN packets belongs to DcDungeonQueueFillJob.
//
// Two rules the caller depends on:
//
//   1. A human who ticked several roles plays exactly ONE of them, drawn at
//      random from the roles they ticked. Ticking "tank or dps" says the
//      player is willing to do either, not that they will do both — the party
//      still owes them the other four slots, so the bot deficit is always
//      kPartySize - humans, never one short. The draw is over the legal
//      assignments only (see AssignHumanRoles), so a random pick can never
//      strand a party the roles could have made.
//   2. Every bot is given a SINGLE-role mask. LFGMgr::CheckGroupRoles solves a
//      role assignment over the whole proposal; a bot offering two roles adds
//      a branch that can resolve into a party with no tank. One bit each means
//      the deficit this planner computed is the deficit the matchmaker sees.
//   3. Two dungeons in a row must not hand the player the same party. The
//      class draw alone cannot promise that: the tank pool holds three classes
//      (four in Wrath) and the healer pool four, so an independent draw
//      repeats the tank a quarter of the time. `avoidClasses` carries the
//      classes the player's LAST fill fielded, and they are vetoed while any
//      alternative remains. It is an argument rather than state so the kernel
//      stays pure: same humans, same seed, same avoid set, same party.
namespace DcDungeonQueueFillPlanner
{
    // splitmix32: the one PRNG this feature draws on, seeded by its caller and
    // pure. Public because the pool-character draw in DcDungeonQueueFillJob
    // must share the stream rather than reach for rand(): a fill has to replay
    // from the seed in its log line, and the unit tests must not flake.
    // Advances `state` and returns the draw.
    std::uint32_t NextRand(std::uint32_t& state);

    // Role bits, mirroring lfg::LfgRoles. Spelled out here rather than included
    // so the kernel stays free of core headers; pinned against the core values
    // by a gtest.
    inline constexpr std::uint8_t kRoleNone   = 0x00;
    inline constexpr std::uint8_t kRoleLeader = 0x01;
    inline constexpr std::uint8_t kRoleTank   = 0x02;
    inline constexpr std::uint8_t kRoleHealer = 0x04;
    inline constexpr std::uint8_t kRoleDamage = 0x08;

    // 1 tank / 1 healer / 3 dps — lfg::LFG_TANKS_NEEDED and friends. Same
    // spell-it-out-and-pin-it treatment as the role bits.
    inline constexpr std::size_t kTanksNeeded   = 1;
    inline constexpr std::size_t kHealersNeeded = 1;
    inline constexpr std::size_t kDpsNeeded     = 3;
    inline constexpr std::size_t kPartySize     = 5;

    // One human already in the queue. The mask is the post-validation value
    // read back from LFGMgr::GetRoles, not the client's raw request.
    struct Human
    {
        std::uint8_t roleMask = kRoleNone;
    };

    // One bot to provision and queue.
    struct Slot
    {
        char const* role = "dps";               // "tank" | "heal" | "dps"
        std::uint8_t roleMask = kRoleDamage;    // exactly one of the three bits
        std::uint8_t classId = 0;
        char const* specName = "";              // premade-spec template to force
        char const* fallbackSpec = "";          // substring match if absent
    };

    enum class Kind : std::uint8_t
    {
        Ok,
        NoHumans,      // nothing to fill around
        PartyFull,     // kPartySize humans already — nothing to add
        Unresolvable   // the humans' roles cannot make a legal 1/1/3 party
    };

    struct Result
    {
        Kind kind = Kind::Unresolvable;
        std::vector<Slot> slots;  // filled only when Ok; may be empty for PartyFull
        std::string detail;       // human sentence for every non-Ok kind
        // The one role each human was given, in the order they were passed in.
        // Empty for NoHumans/PartyFull; all kRoleNone when the roles cannot make
        // a party at all. The job logs it, because "which role is the player
        // playing" is the first question asked of a fill that looks short.
        std::vector<std::uint8_t> humanRoles;
    };

    // The single role each human is assigned, in the order they were given —
    // one role per human, drawn uniformly from the LEGAL assignments (every
    // human on a role they ticked, nobody over the 1/1/3 quota). All kRoleNone
    // when there is no legal assignment — two tank-only players, say — which is
    // itself the signal that the party cannot be completed (see Unresolvable).
    // `seed` picks which legal assignment: same seed, same roles.
    std::vector<std::uint8_t> AssignHumanRoles(std::vector<Human> const& humans,
                                               std::uint32_t seed);

    // Plan the fill. `level` is the level the bots will be provisioned at (the
    // queuing player's, or the group leader's) and `expansion` the lowest
    // expansion in the queued dungeon set; together they gate the death-knight
    // rows exactly the way a test run's do. `seed` makes the role and class
    // draws deterministic — same inputs, same party — so a fill that trips a
    // bug can be reproduced from the seed in its log line.
    // `avoidClasses` are the classes the player's previous fill fielded (see
    // rule 3). Each is passed over while its role pool still offers a class
    // that is neither already in this party nor on the list; when the pool
    // runs out the veto is dropped rather than the fill failing, because a
    // repeated class is a disappointment and a missing tank is a broken
    // dungeon. Empty for a player with no fill behind them.
    Result Plan(std::vector<Human> const& humans, std::uint32_t level, std::uint32_t expansion,
                std::uint32_t seed, std::vector<std::uint8_t> const& avoidClasses = {});

    // Role token -> the single role bit. kRoleNone for an unknown token.
    std::uint8_t MaskForRole(char const* role);

    // Which death-knight rule applies to a fill for `expansion` content at
    // `level` — the same two-part gate DcTestRunJob applies to a test comp,
    // which this deliberately mirrors: a death knight is a Wrath class whose
    // kit starts at 55, so a classic or TBC dungeon never draws one however
    // high the party is levelled. Exposed for the status command and pinned by
    // a test; DcTestComp and DcTestDungeonRegistry own the constants.
    inline DcTestComp::Roster RosterFor(std::uint32_t level, std::uint32_t expansion)
    {
        return expansion >= DcTestDungeonRegistry::kExpansionWrath &&
                       level >= DcTestComp::kDeathKnightMinLevel
                   ? DcTestComp::Roster::WithDeathKnights
                   : DcTestComp::Roster::NoDeathKnights;
    }
}

#endif  // _PLAYERBOT_DCDUNGEONQUEUEFILLPLANNER_H
