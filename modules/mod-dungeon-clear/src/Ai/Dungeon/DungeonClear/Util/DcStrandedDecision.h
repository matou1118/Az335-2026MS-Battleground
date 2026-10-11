/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCSTRANDEDDECISION_H
#define _PLAYERBOT_DCSTRANDEDDECISION_H

#include <cstdint>
#include <vector>

// Pure decision kernel for the stranded-member recovery failsafe. The dominant
// way a run stalls now is a party member falling THROUGH the world geometry (or
// wedging somewhere the navmesh can't recover from): it drifts out of range, the
// between-pulls spread gate then holds the tank forever waiting for it to catch
// up, and the whole run freezes with nothing to do about it. Follow-tank keeps
// re-issuing a path that can never close the gap, so the deadlock is permanent.
//
// This kernel answers, from the run's no-progress clock plus a plain snapshot of
// each same-map member's distance to the tank: should the leader teleport the
// stuck member(s) to itself right now, and which members?
//
// Two guards, in order:
//   1. The run must have shown NO sign of progress for noProgressTimeoutMs. The
//      GLUE (DcStrandedRecovery) owns that clock on DcRunState::progressMs,
//      re-stamping it on any sign of life (a boss/objective completed, the tank
//      closing on the next anchor); a FIGHT re-arms it too, so a legitimately
//      slow pull or a long fight never trips the failsafe. The kernel only
//      compares. "Fight" is engagement (a victim / attackers), never the bare
//      combat flag — a hostile area aura sets that flag with nothing aggroed, and
//      keying on it made this failsafe permanently inert exactly when the run
//      needed it (see DcCombatFlag).
//   2. At least one BOT member must be stranded beyond maxSpread of the tank.
//      That threshold is NOT simply PartyMaxSpread: the glue passes whatever the
//      tank's advance gate is really enforcing (see RescueSpread below), because
//      a rescue looser than the gate it exists to unblock is a permanent stall.
//      A human is never relocated (player agency); dead members are the rez
//      recovery's job.
//
// Extracted engine-free so it is unit-testable in isolation, mirroring
// DcRezDecision / DcSmartRestDecision. Header-only; nothing here touches a
// Player/Unit/context, so no game headers are needed.

namespace DcStrandedDecision
{
    // The distance past which this failsafe calls a member STRANDED.
    //
    // This used to be PartyMaxSpread alone, and that opened a DEAD BAND against
    // the gate the rescue exists to unblock. GetSpreadGate does not always
    // enforce the setting: on a SEALED-ENCOUNTER final approach it overrides it
    // with the row's musterSpread (10yd on all three rows) measured against the
    // TANK, because the boss's room locks the instant the fight starts and the
    // party has to cross the threshold with the tank. A straggler in
    // (musterSpread, PartyMaxSpread] then fails the advance gate while sitting
    // inside the rescue's own threshold: the tank yields "party not ready — out
    // of range" forever and the rescue never looks at it.
    //
    // Live (tr-20260831-164201-61, Gundrak heroic): nine of ten objectives done,
    // party healthy and out of combat, healer 24.5yd from the tank on Gal'darah's
    // approach. Gate 10 (SealedEncounterRegistry row 604/29306), setting 25 —
    // 10 < 24.5 <= 25. 3174 consecutive party-not-ready yields, ~300/minute for
    // ten minutes, then the 600s no-progress watchdog killed a run that was one
    // boss from finishing.
    //
    // MIN, never max, and only when the gate is TANK-ANCHORED. Taking the min
    // means this can only ever become STRICTER than the old behaviour, never
    // laxer: a waived gate (100000 while a pull maneuver holds) and the ordinary
    // tank-anchored gate (== the setting) both leave it exactly where it was.
    // And the anchor test is load-bearing, not tidy — in pull mode the gate is
    // measured from the CAMP while this rescue measures from the TANK, so
    // borrowing that radius would compare a distance to one origin against a
    // radius sized for another and could yank a member who is correctly set at
    // its camp. Same lesson as the party-gate ring: match the ORIGIN and the
    // RADIUS, or match neither.
    inline float RescueSpread(float partySpread, float leaderGateSpread, bool gateIsTankAnchored)
    {
        if (!gateIsTankAnchored || leaderGateSpread <= 0.0f)
            return partySpread;
        return leaderGateSpread < partySpread ? leaderGateSpread : partySpread;
    }

    // One same-map group member, snapshotted by the glue.
    struct Member
    {
        bool  isBot = false;       // has a PlayerbotAI to drive (only bots teleport)
        bool  isAlive = false;     // dead members belong to the rez recovery, not here
        bool  onMap = false;       // same map/instance as the tank
        bool  isTank = false;      // the reference we teleport TO; never itself moved
        float distToTank = 0.0f;   // tank->GetDistance(member)
    };

    struct Inputs
    {
        bool          enabled = true;             // DungeonClear.StrandedRecovery
        std::uint32_t nowMs = 0;
        std::uint32_t lastProgressMs = 0;         // clock; 0 = unarmed (no verdict yet)
        std::uint32_t noProgressTimeoutMs = 60000;   // StrandedRecoveryNoProgressSecs * 1000
        bool          partyEngaged = false;      // a real fight (not the bare flag)
        float         maxSpread = 25.0f;          // out-of-range threshold; see RescueSpread
    };

    struct Result
    {
        bool             recover = false;   // teleport the strays this tick
        std::vector<int> strandedIdx;       // members to teleport (indices into the snapshot)
    };

    // The verdict. Returns recover=false (no strays) whenever the feature is off,
    // the party is in combat, the clock is unarmed / disabled, or the no-progress
    // window has not elapsed. See the header comment for the two guards.
    inline Result Decide(Inputs const& in, std::vector<Member> const& members)
    {
        Result r;

        if (!in.enabled || in.partyEngaged)
            return r;

        // Clock must be armed (a run underway) and the timeout enabled.
        if (in.lastProgressMs == 0 || in.noProgressTimeoutMs == 0)
            return r;

        // Wrap-safe elapsed compare (matches the getMSTimeDiff the glue passes in
        // via nowMs - lastProgressMs on the same monotonic clock).
        if (in.nowMs - in.lastProgressMs < in.noProgressTimeoutMs)
            return r;

        for (std::size_t i = 0; i < members.size(); ++i)
        {
            Member const& m = members[i];
            if (m.isTank || !m.isBot || !m.isAlive || !m.onMap)
                continue;
            if (m.distToTank > in.maxSpread)
                r.strandedIdx.push_back(static_cast<int>(i));
        }
        r.recover = !r.strandedIdx.empty();
        return r;
    }
}

#endif  // _PLAYERBOT_DCSTRANDEDDECISION_H
