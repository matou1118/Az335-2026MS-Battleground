/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCRUNWING_H
#define _PLAYERBOT_DCRUNWING_H

#include <cstddef>
#include <string>
#include <vector>

#include "Define.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonBossInfo.h"

class Player;
struct DungeonWing;
struct DungeonWingLayout;

// The RUN WING: which wing of an Explicit-select split map (Blackrock Spire —
// see DungeonWingRegistry) this run clears. Proximity can't pick it there: LBRS
// and UBRS share one portal, and the nearest bosses to it are UBRS's.
//
// The choice is a latch on the run owner's DcRunState, keyed by instance id, so
// the whole party reads one answer (followers resolve the owner, like rez
// recovery) and a new instance starts clean. Resolution order, first hit wins:
//
//   1. Explicit — a test run sets it from its row; `.dc on <wing>` / `.dc wing
//      <wing>` set it by hand.
//   2. Lfg      — the dungeon queue-fill passes the group's LFG dungeon id.
//   3. Fallback — the party stands inside a wing's region (the wing-tagged
//      DcNavPenaltyRegistry rows) -> that wing; else the layout's defaultWing.
//
// A higher source replaces a lower one; a lower one never replaces a higher one
// in the same instance, so a fallback can't flip the wing as the party walks
// (LBRS passes under UBRS floors). A fallback is only latched while the run is
// enabled — before `dc on` it is recomputed from where the party stands, and
// `dc on` itself latches it.
namespace DcRunWing
{
    enum class Source : uint8
    {
        None = 0,
        Fallback = 1,
        Lfg = 2,
        Explicit = 3,
    };

    struct Latch
    {
        uint32 instanceId = 0;
        std::string token;
        Source source = Source::None;
    };

    char const* SourceName(Source source);

    // ---- pure kernels (gtest-pinned) ----------------------------------------

    // Whether `latch` names a wing for instance `instanceId`.
    inline bool Holds(Latch const& latch, uint32 instanceId)
    {
        return latch.source != Source::None && !latch.token.empty() &&
               latch.instanceId == instanceId;
    }

    // Whether a choice from `incoming` may overwrite `latch` in `instanceId`:
    // always for a stale or empty latch, else only from an equal or higher source.
    inline bool ShouldReplace(Latch const& latch, uint32 instanceId, Source incoming)
    {
        if (incoming == Source::None)
            return false;
        return !Holds(latch, instanceId) || incoming >= latch.source;
    }

    // The fallback wing token: `regionWing` (the wing whose region the party
    // stands in, or nullptr) when it names one of the layout's wings, else the
    // layout's defaultWing, else the first wing's token.
    std::string ResolveFallback(DungeonWingLayout const& layout, char const* regionWing);

    // `bosses` narrowed to the wing's entries. May return empty (an Explicit wing
    // is never widened back to the whole map — that is exactly the bug).
    std::vector<DungeonBossInfo> FilterToWing(std::vector<DungeonBossInfo> const& bosses,
                                              DungeonWing const& wing);

    // Proximity wing pick (Dire Maul, Scarlet Monastery): the index of the wing
    // owning the boss in `bosses` nearest (x,y,z), or wings.size() when no
    // registered boss is in the list.
    std::size_t PickByProximity(DungeonWingLayout const& layout,
                                std::vector<DungeonBossInfo> const& bosses,
                                float x, float y, float z);

    // Whether the wing's terminal boss is done: its kill bit is in
    // `completedMask`, or `terminalCorpse` (it is spawned and every copy is dead).
    // False when the wing has no terminal boss or it is not in `bosses`.
    bool TerminalDone(DungeonWing const& wing, std::vector<DungeonBossInfo> const& bosses,
                      uint32 completedMask, bool terminalCorpse);

    // ---- game side ----------------------------------------------------------

    // The run wing for `bot`'s instance on an Explicit-select map; nullptr on
    // every other map. Latches a fallback on the owner when the run is enabled.
    DungeonWing const* Resolve(Player* bot);

    // The wing to scope nav fences by: Resolve()'s token, "" off Explicit maps
    // (wing-tagged fence rows are then inactive).
    std::string FenceWing(Player* bot);

    // The wing token this run is in on ANY isolated split map: Resolve() on an
    // Explicit map, the wing owning the run owner's first listed boss on a
    // Proximity map (its list is already filtered to the wing), "" otherwise.
    // Rez recovery picks the entrance row by it.
    std::string ActiveWingToken(Player* bot);

    // Latch `wing` from `source` on `bot`'s run-state holder for its instance, if
    // ShouldReplace allows, and refresh every party bot's boss list so the new
    // wing takes effect this tick. Returns whether the latch now names `wing`.
    bool Set(Player* bot, DungeonWing const& wing, Source source);

    // Drop a Fallback latch (never an Explicit/Lfg one) so the next Resolve
    // re-reads where the party stands. `dc on` calls it before latching afresh.
    void ClearFallback(Player* bot);
}

#endif  // _PLAYERBOT_DCRUNWING_H
