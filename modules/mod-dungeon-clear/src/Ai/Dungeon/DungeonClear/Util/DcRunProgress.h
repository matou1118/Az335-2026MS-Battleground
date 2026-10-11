/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCRUNPROGRESS_H
#define _PLAYERBOT_DCRUNPROGRESS_H

#include <cstdint>

class Player;
class PlayerbotAI;

// "Is this run still moving?" — the one definition of PROGRESS the module's
// no-progress failsafes share, and the last-seen snapshot each of them needs to
// answer it.
//
// Three signals, mirroring the test harness's livelock net (DcTestRunJob) so the
// live failsafes and the harness can never disagree about what a frozen run
// looks like:
//
//   1. the instance's completed-encounter mask grew (a boss/objective finished),
//   2. the clear's cleared-anchor set grew,
//   3. the tank CLOSED on the next anchor versus its closest-ever approach.
//
// WHAT IS DELIBERATELY NOT IN HERE: combat. A fight is progress for the
// stranded-member failsafe, which must never yank a bot out of a live fight — so
// DcStrandedRecovery re-arms its own clock on party engagement, on top of this.
// It is NOT progress for DcCombatPurge, whose entire subject is a fight that can
// never end: a purge clock that combat re-armed could not fire in the one state
// it exists for. Keeping combat OUT of the shared detector is what lets the two
// clocks read the same world and still answer their own question.
//
// EACH CALLER OWNS ITS OWN Mark. The detector reports an EDGE (the mask grew,
// the anchor set grew, the tank got closer) and consumes it by writing the new
// value into the mark it was handed. Two clocks sharing one mark would race for
// that edge and one of them would miss it, so DcRunState carries a separate mark
// per failsafe.
namespace DcRunProgress
{
    // Last-seen progress snapshot for one clock. Default-constructed = unarmed.
    struct Mark
    {
        std::uint32_t stampMs    = 0;      // getMSTime() of the last sign of progress (0 = unarmed)
        std::uint32_t mask       = 0;      // completed-encounter mask last seen
        std::uint32_t anchors    = 0;      // cleared-anchor count last seen
        std::uint32_t bossEntry  = 0;      // anchor entry `bestDist` is keyed to (re-arm on change)
        float         bestDist   = -1.0f;  // closest tank approach to that anchor (<0 = unset)
    };

    // Did the run show a sign of life this tick? Updates `mark` in place with
    // whatever it saw. Reads the leader's own AI context, so call it on the
    // elected leader.
    bool Detect(Player* leader, PlayerbotAI* leaderAI, Mark& mark);

    // Re-arm `mark`'s clock to `nowMs` (never to 0, which means "unarmed").
    void Stamp(Mark& mark, std::uint32_t nowMs);

    // Has the clock been stale for `windowMs`? False while unarmed, and false
    // when `windowMs` is 0 (the feature is off).
    bool Stale(Mark const& mark, std::uint32_t nowMs, std::uint32_t windowMs);
}

#endif  // _PLAYERBOT_DCRUNPROGRESS_H
