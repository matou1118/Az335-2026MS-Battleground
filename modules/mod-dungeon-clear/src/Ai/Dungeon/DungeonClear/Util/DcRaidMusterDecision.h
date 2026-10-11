/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCRAIDMUSTERDECISION_H
#define _PLAYERBOT_DCRAIDMUSTERDECISION_H

#include <cstdint>

// Pure decision kernel for the RAID pre-boss muster — the full-stop between
// "approach finished" and "engage" that Plan C of the raid-support program
// specifies: stage the raid at the standoff, top everyone off to full, run a
// ForceRebuff round (group buffs + the worldbuff simulated consumables), and
// only then release the pull. Trash flow runs on quorum; THIS gate is strict —
// every boss opens with everyone staged and topped, or with a logged timeout.
//
// The kernel is the phase machine only; the glue (DungeonClearEngageBossAction)
// owns the snapshots (staged/topped/rebuff-pending walks), the clocks in
// DcRunState (musterPhase / musterPhaseSinceMs / musterBossEntry /
// musterArmedMs / musterRebuffIssuedMs), the rebuff drive (ForceRebuffState::
// Begin on every member) and the announcements. Wait-at-Boss, when enabled,
// sits AFTER this gate — the human gets a raid that is already staged and
// buffed.
//
// Every phase is timeout-bounded AND the muster as a whole is bounded: a muster
// must never deadlock a run on a bot that cannot eat or a buff that cannot
// land, and it must never spend more wall-clock standing around than the raid
// would spend fighting. The per-phase bounds (restTimeoutMs, rebuffTimeoutMs)
// hand the phase forward; the WHOLE-muster bound (totalTimeoutMs, measured from
// the tick the muster armed for this boss) is the hard ceiling that fires from
// any phase and goes straight to Ready. On expiry the muster advances with what
// it has, cancels any rebuff window still open (verdict.cancelRebuff — buffing
// must not bleed into the pull), and the verdict says so (announce, never hang).
namespace DcRaidMusterDecision
{
    enum class Phase : std::uint8_t
    {
        Idle = 0,   // no muster armed (not a raid boss engage)
        Resting,    // staging + topping off (members walk in while they eat)
        Rebuffing,  // ForceRebuff round running
        Ready,      // gate passed — release the pull
    };

    struct Inputs
    {
        bool          staged = false;      // every living member inside the muster spread
        bool          topped = false;      // bots at the full bars, humans at their margin
        bool          rebuffDone = false;  // rebuff round issued and no member still pending
        std::uint32_t nowMs = 0;
        std::uint32_t phaseSinceMs = 0;    // DcRunState::musterPhaseSinceMs (0 = fresh)
        std::uint32_t armedSinceMs = 0;    // DcRunState::musterArmedMs (0 = fresh)
        std::uint32_t restTimeoutMs = 0;   // bound on Resting (0 = none)
        std::uint32_t rebuffTimeoutMs = 0; // bound on Rebuffing (0 = none)
        std::uint32_t totalTimeoutMs = 0;  // bound on the whole muster (0 = none)
    };

    struct Verdict
    {
        Phase phase = Phase::Idle;   // the (possibly advanced) phase to store
        bool  hold = false;          // engage must not fire this tick
        bool  beginRebuff = false;   // glue should issue the rebuff round NOW
        bool  cancelRebuff = false;  // glue should CLOSE every open rebuff window NOW
        bool  timedOut = false;      // this advance was a timeout, not success
    };

    inline bool Expired(std::uint32_t nowMs, std::uint32_t sinceMs, std::uint32_t budgetMs)
    {
        return budgetMs != 0 && sinceMs != 0 && nowMs - sinceMs >= budgetMs;
    }

    inline bool TimedOut(Inputs const& in, std::uint32_t budgetMs)
    {
        return Expired(in.nowMs, in.phaseSinceMs, budgetMs);
    }

    // One evaluation. The glue calls this only while a raid BOSS engage is
    // imminent (raid map, next anchor is a boss, boss resolved); Idle therefore
    // arms straight into Resting.
    inline Verdict Decide(Phase current, Inputs const& in)
    {
        Verdict v;

        // Whole-muster ceiling: from any pre-Ready phase this releases the pull
        // outright — no "one more phase" — and cancels the buff round with it.
        if (current != Phase::Idle && current != Phase::Ready &&
            Expired(in.nowMs, in.armedSinceMs, in.totalTimeoutMs))
        {
            v.phase = Phase::Ready;
            v.hold = false;
            v.cancelRebuff = true;
            v.timedOut = true;
            return v;
        }

        switch (current)
        {
            case Phase::Idle:
                v.phase = Phase::Resting;
                v.hold = true;
                return v;

            case Phase::Resting:
                if (in.staged && in.topped)
                {
                    v.phase = Phase::Rebuffing;
                    v.beginRebuff = true;
                }
                else if (TimedOut(in, in.restTimeoutMs))
                {
                    v.phase = Phase::Rebuffing;
                    v.beginRebuff = true;
                    v.timedOut = true;
                }
                else
                    v.phase = Phase::Resting;
                v.hold = true;
                return v;

            case Phase::Rebuffing:
                if (in.rebuffDone)
                    v.phase = Phase::Ready;
                else if (TimedOut(in, in.rebuffTimeoutMs))
                {
                    v.phase = Phase::Ready;
                    v.cancelRebuff = true;  // a window still open: close it, pull
                    v.timedOut = true;
                }
                else
                {
                    v.phase = Phase::Rebuffing;
                    v.hold = true;
                    return v;
                }
                // Fresh Ready releases this same tick — the gate below reads it.
                v.hold = false;
                return v;

            case Phase::Ready:
            default:
                v.phase = Phase::Ready;
                v.hold = false;
                return v;
        }
    }
}

#endif  // _PLAYERBOT_DCRAIDMUSTERDECISION_H
