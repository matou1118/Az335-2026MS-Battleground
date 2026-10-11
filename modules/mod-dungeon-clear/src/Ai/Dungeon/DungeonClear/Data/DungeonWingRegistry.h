/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DUNGEONWINGREGISTRY_H
#define _PLAYERBOT_DUNGEONWINGREGISTRY_H

#include <string>
#include <vector>

#include "Common.h"

// Some 3.3.5 dungeons pack several mutually-inaccessible "wings" onto a single
// instance map. Dire Maul (map 429) is the canonical case: East, West and North
// share one map id but you enter each through its own portal and cannot reach
// the others from inside. DungeonEncounter.dbc lists all wings' bosses under
// that one map id, so the stock (mapId, difficulty) boss list mixes every
// wing's bosses together — the bot then targets bosses it can never reach and
// the dungeon never reads as "cleared".
//
// This registry records, per split map, which boss credit-entries belong to
// which wing. How DungeonBossesValue uses that depends on the wing topology:
//
//   isolated == true  (Dire Maul, Scarlet Monastery): the wings are physically
//     disconnected — separate portals, no in-instance route between them — so
//     the boss list is filtered down to the wing the bot is standing in (picked
//     by proximity, since the wings sit far apart in world space). Bosses in
//     other wings can never be reached and would otherwise wedge the clear.
//
//   isolated == false (Maraudon): the "wings" share one connected interior —
//     orange, purple and the inner Pristine Waters all link up, so every boss
//     stays reachable from any entrance. Here the wing data is NOT a filter; it
//     is a display/label only. All bosses remain in the list and clearable, and
//     the wing name is just surfaced in status/UI. Filtering here would make the
//     bot clear one wing and falsely read the dungeon as done.
//
// HOW an isolated map's wing is chosen is the layout's `select`:
//
//   Proximity (Dire Maul, Scarlet Monastery): the wing owning the boss nearest
//     the bot. Right when the wings sit hundreds of yards apart and each has its
//     own portal — the bot always stands in the wing it is clearing.
//
//   Explicit (Blackrock Spire): the wing is a per-RUN choice, latched on the run
//     owner for the instance (DcRunWing). Needed when the wings share a portal
//     and are stacked: at the BRS portal the nearest bosses are UBRS's (Emberseer
//     87yd, Drakkisath 96yd) though the party is about to clear LBRS, so
//     proximity would pick the wrong half. A test row, `.dc on <wing>`, `.dc
//     wing` or the LFG dungeon picks it; with none of those, the party standing
//     inside a wing's region (DcNavPenaltyRegistry rows tagged with the wing)
//     picks that wing, else `defaultWing`.
//
// Maps NOT listed here are single-wing and pass through unfiltered. The lists
// are static game data (vanilla dungeon layouts never change), so they live in
// code rather than the DB.
struct DungeonWing
{
    std::string name;                  // human-readable, for logs/chat
    std::vector<uint32> bossEntries;   // creature credit-entries in this wing

    // Short id, equal to the wing's DcTestDungeonRegistry row token ("lbrs",
    // "sm-gy"), so a run's row, a `.dc on <wing>` argument and the latched run
    // wing all name the wing the same way. "" = no token (label-only maps).
    std::string token{};

    // LFG dungeon id (lfg_dungeon_template / LFGDungeons.dbc) that queues for
    // this wing; 0 = none. Lets an LFG-formed group pick its wing without a
    // command.
    uint32 lfgDungeonId{0};

    // The wing's last boss: once it is done the wing is complete, whatever is
    // left in the list (NextDungeonBossValue). 0 = no terminal boss — the wing
    // completes when its whole list is cleared, as every map always has.
    uint32 terminalBossEntry{0};

    // DungeonEncounter.dbc bits (1 << encounterIndex) that belong to this wing,
    // including encounters with no static spawn (never in the boss list, but
    // their kill still sets a bit). Scopes the test harness's kill count and
    // stale-instance guard to the wing. 0 = not authored (count every bit).
    uint32 encounterMask{0};
};

enum class WingSelect : uint8
{
    Proximity,   // nearest registered boss picks the wing (DM, SM)
    Explicit,    // per-run choice latched on the run owner (BRS) — see DcRunWing
};

// A split map's full wing layout: the wings plus whether they are physically
// isolated (filter the clear to one wing) or merely labelled regions of one
// connected interior (keep every boss, label only). See the header note above.
struct DungeonWingLayout
{
    bool isolated;                     // true = filter to current wing
    std::vector<DungeonWing> wings;
    WingSelect select{WingSelect::Proximity};
    // Explicit layouts: the wing token used when nothing chose one and the party
    // stands in no wing's region. Ignored by Proximity layouts.
    std::string defaultWing{};
};

class DungeonWingRegistry
{
public:
    // Returns the wing layout for a split map, or nullptr if the map is not
    // split into wings (the common case — caller then keeps the full list).
    static DungeonWingLayout const* Get(uint32 mapId);

    // Human-readable wing label owning `bossEntry` on `mapId`, or "" when the
    // map has no wing split or the entry isn't registered. For status/UI.
    static std::string WingName(uint32 mapId, uint32 bossEntry);

    // The wing whose token (case-insensitive) or name equals `key`, or nullptr.
    static DungeonWing const* FindWing(DungeonWingLayout const& layout, std::string const& key);

    // The wing of `mapId` queued for by LFG dungeon `lfgDungeonId`, or nullptr.
    static DungeonWing const* WingForLfgDungeon(uint32 mapId, uint32 lfgDungeonId);

    // The wing owning `bossEntry` on `mapId`, or nullptr.
    static DungeonWing const* WingOf(uint32 mapId, uint32 bossEntry);
};

#endif
