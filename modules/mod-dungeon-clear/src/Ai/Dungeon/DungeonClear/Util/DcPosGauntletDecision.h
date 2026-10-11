/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCPOSGAUNTLETDECISION_H
#define _PLAYERBOT_DCPOSGAUNTLETDECISION_H

#include <cstdint>

#include "Define.h"

// PURE kernel for the Pit of Saron (map 658) Ymirjar gauntlet — the whole of
// what the driver decides every tick, lifted out of the world so it can be
// tested without a map, a creature or an instance.
//
// The glue (grid scans, the party walk, the splines, the forged areatrigger
// packets, the telemetry) lives in Overrides/PitOfSaronDriver.cpp; everything
// here is arithmetic over a snapshot, in the same shape as DcRazorgoreDecision
// and DcSuppressionTransitDecision. Unlike the latter this one is NOT map-free:
// it is one dungeon's finite state machine, and pretending otherwise would cost
// more in indirection than it could ever return.
//
// THE LEG, in the terms these functions use. Between Krick and Scourgelord
// Tyrannus lie THREE AREATRIGGER GATES and TWO SUMMONED WAVES, and the order is
// absolute. npc_pos_tyrannus_eventsAI::SetData refuses SILENTLY unless every
// precondition holds:
//
//   gate 1 (AT 5578)  progress == FINISHED_KRICK_SCENE, AND the orchestrator
//                     (36794) has physically flown to PTSTyrannusWaitPos1 — a
//                     journey that takes 85-100 SECONDS after Ick dies;
//   gate 2 (AT 5579)  progress == AFTER_WARN_1 AND killsLeft == 0 (wave 1 dead);
//   gate 3 (AT 5580)  progress == AFTER_WARN_2 AND killsLeft == 0 (wave 2 dead).
//
// A refusal costs nothing server-side and says nothing in any log. Getting the
// order wrong does not produce an error — it produces a party standing in a
// corridor forever.
//
// FOUR PROPERTIES OF THE DESIGN ARE WORTH STATING BEFORE THE CODE:
//
//   1. THE PROGRESS COUNTER IS THE STATE, not a variable of ours. It is
//      MONOTONIC, readable from any bot, and raised only by Ick's death and by
//      standing inside a specific sphere. So there is no latch here to get out of
//      sync with the world, and a wipe-and-retry re-enters the FSM in exactly the
//      state the instance is really in.
//
//   2. killsLeft IS NOT READABLE. It lives in the orchestrator's AI and nothing
//      exposes it, so the kernel infers "the wave is dead" from there being no
//      live wave creature — which is why WAVE 1 and WAVE 2 are probed
//      differently by the glue (wave 1 by entry alone, wave 2 by a volume) and
//      why the grace window below exists.
//
//   3. THE FORGE IS LEVEL-TRIGGERED. The driver re-sends the areatrigger packet
//      every tick it stands in a gate, so a refusal is free and an "early" entry
//      is harmless: the packet simply starts being accepted the moment its
//      precondition turns true. That is what makes the ARM state a comfort rather
//      than a necessity — and it is emphatically NOT true of the test harness's
//      relay (DcTestAreaTriggers), which is edge-triggered on volume occupancy.
//
//   4. "THE WAVE IS ALIVE" AND "THE WAVE IS DANGEROUS" ARE DIFFERENT QUESTIONS,
//      and on wave 1 they are ~15-20 seconds apart. The gate being accepted does
//      not summon an ambush; it starts a CASCADE that summons two passive
//      Deathbringers 100yd up the ramp, walks them down it, and only then summons
//      the two 4-packs that are the actual fight. So the kernel is handed an
//      ARMED count as well as an alive one, and holds the party at the bottom of
//      the ramp until the ambush exists to walk into. Wave 2 has no such window
//      and opts out by passing a quorum of zero.
namespace DcPosGauntlet
{
    // Where the party is in the gauntlet. Derived fresh from the instance's own
    // progress counter every tick; nothing here is stored across ticks except the
    // two clocks in Inputs.
    enum class State : uint8
    {
        Complete = 0,  // before Ick, after the tunnel warn, or Tyrannus is down
        Arm,           // the Krick outro is still flying the orchestrator east
        Gate1,         // walk into AT 5578 and forge it
        Wave1,         // ten Ymirjar are up; fight them where they are
        Gate2,         // walk into AT 5579 and forge it
        Wave2,         // six (twelve heroic) Wrathbone are up
        Gate3,         // walk into AT 5580 and forge it; that ends the event
    };

    inline char const* StateName(State s)
    {
        switch (s)
        {
            case State::Arm:   return "arming — waiting out the Krick outro";
            case State::Gate1: return "gate 1 (areatrigger 5578)";
            case State::Wave1: return "wave 1 — ten Ymirjar";
            case State::Gate2: return "gate 2 (areatrigger 5579)";
            case State::Wave2: return "wave 2 — Wrathbone";
            case State::Gate3: return "gate 3 (areatrigger 5580)";
            case State::Complete:
            default:           return "complete";
        }
    }

    // The gauntlet's three phases, keyed on the instance progress counter. The
    // grace and stall clocks below belong to a PHASE, not to a State: Arm and
    // Gate1 are both phase 2, Wave1 and Gate2 are both phase 3, and a clock that
    // restarted on the sub-state transition would restart exactly when it is
    // needed most.
    inline uint8 PhaseOf(uint32 progress) { return static_cast<uint8>(progress); }

    struct Inputs
    {
        // --- the world -------------------------------------------------------
        uint32 progress = 0;        // GetData(DATA_INSTANCE_PROGRESS)
        bool   bossesDone = false;  // Garfrost AND Ick are both DONE
        bool   tyrannusDone = false;

        // Is the orchestrator (36794) ALIVE and within 3.0yd of
        // PTSTyrannusWaitPos1? That is gate 1's real precondition, checked by the
        // core with GetExactDist against the same 3.0. Nothing else on this map
        // can tell an outro that has finished from one that has not.
        bool   rpInPlace = false;

        // --- the current wave -------------------------------------------------
        uint32 waveAlive = 0;           // live wave creatures the glue could see
        float  nearestWaveDist = -1.0f; // leader -> nearest of them (<0 = none)
        float  waveEngageRange = 0.0f;
        bool   partyInCombat = false;

        // ...of those, how many are ARMED: alive AND attackable AND
        // REACT_AGGRESSIVE. Not the same question as waveAlive, and on wave 1 the
        // difference is the whole bug — see waveArmedQuorum below.
        uint32 waveArmed = 0;

        // --- geometry ---------------------------------------------------------
        float  distToStage = 0.0f;  // leader -> the ARM staging point
        float  stageLeash = 0.0f;
        float  distToGate = 0.0f;   // leader -> the CURRENT gate's stand point
        float  gateLeash = 0.0f;

        // --- the gather gate --------------------------------------------------
        uint32 living = 0;     // living members on this map, leader included
        uint32 nearGate = 0;   // ...of which, inside the gather radius of the gate
        float  gatherQuorum = 0.0f;

        // --- clocks -----------------------------------------------------------
        uint32 nowMs = 0;
        uint32 phase = 0;            // the stored phase these two clocks belong to
        uint32 phaseSinceMs = 0;     // when the party entered it
        uint32 gateHoldSinceMs = 0;  // when the leader first stood in the current gate
        uint32 waveHoldSinceMs = 0;  // when the leader first stood in reach of a live
                                     // wave mob with the party OUT of combat
        bool   stallReported = false;
        bool   waveArmedLatched = false;  // has the wave-1 arming hold already
                                          // released this phase? ONE-WAY, and reset
                                          // by the phase clock — see the hold itself

        // --- budgets ----------------------------------------------------------
        uint32 waveGraceMs = 0;     // how long "no wave mob yet" still means "wait"
        uint32 waveStandoffMs = 0;  // how long a silent wave mob in reach is tolerated
        uint32 gateStallMs = 0;     // how long a forged, unaccepted gate stays quiet

        // How many wave mobs must be ARMED before the driver is allowed to close
        // on the wave at all, and how long it will wait for that. ZERO DISABLES
        // THE GATE, which is how wave 2 opts out: its six (twelve heroic) mobs are
        // summoned in one go, on one spline, with no passive window worth waiting
        // through, and a quorum there would only be a way to hang.
        uint32 waveArmedQuorum = 0;
        uint32 waveArmMs = 0;
    };

    struct Verdict
    {
        State  state = State::Complete;

        // --- what the driver should DO ---------------------------------------
        bool   walkToStage = false;  // steer to the ARM staging point
        bool   walkToGate = false;   // steer to the current gate's stand point
        bool   engageWave = false;   // steer at the nearest live wave creature
        bool   pullWave = false;     // START the fight with the nearest one, by force
        bool   forge = false;        // send the areatrigger packet this tick
        bool   holdStill = false;    // issue no movement, but still claim the tick

        // Hand the tick back to the stock combat engine. TRUE only where the
        // right thing for the party to be doing is FIGHTING — which on this leg
        // is most of both waves.
        bool   yieldTick = false;

        // --- derived facts the glue logs or stores ---------------------------
        bool   quorumMet = false;
        bool   waveArming = false;     // holding for the wave to become real
        bool   complete = false;
        bool   stalled = false;        // the gate has been forged unaccepted too long
        bool   reportStall = false;    // ...and this is the tick to say so, once
        uint32 phase = 0;              // clocks to store back
        uint32 phaseSinceMs = 0;
        uint32 gateHoldSinceMs = 0;
        uint32 waveHoldSinceMs = 0;
        bool   stallReported = false;
        bool   waveArmedLatched = false;
    };

    // Is this state one of the three gates?
    inline bool IsGate(State s)
    {
        return s == State::Gate1 || s == State::Gate2 || s == State::Gate3;
    }

    inline Verdict Decide(Inputs const& in)
    {
        Verdict v;
        v.phase = in.phase;
        v.phaseSinceMs = in.phaseSinceMs;
        v.gateHoldSinceMs = in.gateHoldSinceMs;
        v.waveHoldSinceMs = in.waveHoldSinceMs;
        v.stallReported = in.stallReported;
        v.waveArmedLatched = in.waveArmedLatched;

        // --- 1. is the gauntlet ours at all? ---------------------------------
        //
        // The window is EXACTLY progress 2..4. Below it Ick is not dead and the
        // orchestrator refuses everything; at 5 the tunnel warn has landed and the
        // leg is ordinary clear work again; Tyrannus being down ends it outright
        // (a re-entered instance walks this corridor with nothing to do).
        //
        // Both bosses DONE is checked as well as the progress, because the
        // orchestrator's SetData bails on it FIRST — before it even looks at which
        // id it was handed — so a party that somehow reached progress 3 with a
        // boss alive could forge every gate on the map for ever.
        if (!in.bossesDone || in.tyrannusDone ||
            in.progress < 2 /*PROGRESS_FINISHED_KRICK_SCENE*/ ||
            in.progress > 4 /*PROGRESS_AFTER_WARN_2*/)
        {
            v.state = State::Complete;
            v.complete = true;
            v.yieldTick = true;
            v.phase = 0;
            v.phaseSinceMs = 0;
            v.gateHoldSinceMs = 0;
            v.waveHoldSinceMs = 0;
            v.stallReported = false;
            v.waveArmedLatched = false;
            return v;
        }

        // --- 2. the phase clock ----------------------------------------------
        //
        // Re-stamped on every progress bump, and it resets the gate clock and the
        // one-shot stall report with it: each phase gets its own grace window and
        // its own right to complain once.
        uint8 const phase = PhaseOf(in.progress);
        if (v.phase != phase)
        {
            v.phase = phase;
            v.phaseSinceMs = in.nowMs;
            v.gateHoldSinceMs = 0;
            v.waveHoldSinceMs = 0;
            v.stallReported = false;
            v.waveArmedLatched = false;
        }
        uint32 const inPhaseMs = in.nowMs >= v.phaseSinceMs ? in.nowMs - v.phaseSinceMs : 0;

        // --- 3. the gather quorum --------------------------------------------
        //
        // Measured against the GATE, not against the leader, for the Blackwing
        // Lair reason: during a gather the leader keeps walking while the point
        // the party is forming on does not, and the two distances ADD. A lone
        // leader (living <= 1) is trivially a quorum.
        v.quorumMet = in.living <= 1 ||
                      static_cast<float>(in.nearGate) >=
                          static_cast<float>(in.living) * in.gatherQuorum;

        // --- 4. which state ---------------------------------------------------
        //
        // THE WAVE PROBE IS "ALIVE, OR TOO SOON TO TELL". SetData raises the
        // counter and schedules the summons at 0ms, so the two Deathbringers land
        // on the NEXT UpdateAI and the other eight at +3.0s / +3.5s behind a
        // spline-arrival poll. Without the grace the driver reads an empty scan
        // one tick after the gate is accepted, walks straight to the next gate,
        // and is refused there for killsLeft != 0 — leaving the party standing in
        // the ambush it just triggered.
        bool const waveUp = in.waveAlive > 0 || inPhaseMs < in.waveGraceMs;

        switch (in.progress)
        {
            case 2:  v.state = in.rpInPlace ? State::Gate1 : State::Arm; break;
            case 3:  v.state = waveUp ? State::Wave1 : State::Gate2;     break;
            default: v.state = waveUp ? State::Wave2 : State::Gate3;     break;
        }

        // --- 5. what to do about it ------------------------------------------
        if (v.state == State::Arm)
        {
            // HOLD BACK. The orchestrator is still flying; nothing is hostile
            // here and there is nothing to fight, so the driver claims the tick
            // and simply refuses to let the clear walk on. Walking to the staging
            // point is the only movement it issues.
            //
            // It claims the tick rather than yielding because a yield hands the
            // leg to DcRel::Advance (15), which would walk the party the 88yd into
            // gate 1's sphere and stand them in an ambush trigger for a minute and
            // a half. Followers keep their own ladder — including rest — so the
            // cost of the hold is the TANK's rest ticks and nothing else.
            v.gateHoldSinceMs = 0;
            v.waveHoldSinceMs = 0;
            v.walkToStage = in.distToStage > in.stageLeash;
            v.holdStill = !v.walkToStage;
            return v;
        }

        if (v.state == State::Wave1 || v.state == State::Wave2)
        {
            v.gateHoldSinceMs = 0;

            // Nothing visible yet — we are inside the summon grace. Stand still
            // and claim the tick: yielding here lets Advance walk the leader into
            // the next gate before the wave has even landed.
            if (in.nearestWaveDist < 0.0f)
            {
                v.waveHoldSinceMs = 0;
                v.holdStill = true;
                return v;
            }

            // THE WAVE IS VISIBLE BUT NOT YET REAL. Wave 1 is summoned as a ~15-20s
            // CASCADE (see WAVE1_ARMED_QUORUM in DungeonEventTables.h), and for
            // most of it the only thing alive is two Deathbringers standing
            // REACT_PASSIVE + NON_ATTACKABLE at the TOP OF THE RAMP, 100yd away,
            // while the two 4-packs that are the actual ambush have not been
            // summoned at all. The engage rule below is unconditional on distance,
            // so without this the driver marches the party up the ramp at a mob it
            // cannot attack, straight through the ground both packs are about to
            // land on — which is the "it leaves the hold too early" report.
            //
            // Waiting here is not the same mistake as the unbounded standoff hold
            // further down. That one waited for AGGRO, which is a thing that may
            // never come; this waits for a SUMMON CHAIN, which is on a timer and
            // observable — the mobs exist and their react state flips — and it is
            // bounded anyway.
            //
            // THREE WAYS OUT, and the hold releases on the first of them:
            //   * the quorum arms (the ordinary case, ~15-20s in);
            //   * somebody is already fighting, so the wave has found the party on
            //     its own and holding would abandon them mid-pull;
            //   * waveArmMs expires — the cascade wedged and a quorum that can no
            //     longer be met must not become a party standing at gate 1 for ever.
            //
            // AND IT IS A ONE-WAY LATCH, which is the one piece of stored state in
            // this kernel that is not derived fresh from the world, because the
            // quorum ALONE would re-arm itself halfway through the fight: eight
            // armed mobs become five as the party kills them, and a party that
            // dropped combat for a tick between pulls at t+40s would be sent back to
            // holding by its own progress. killsLeft is not readable, so there is no
            // world fact that distinguishes "three armed because six are dead" from
            // "three armed because five have not spawned" — only the history does.
            //
            // The latch is scoped exactly like stallReported: cleared on the phase
            // bump and on leaving the window, so wave 2 gets its own decision and a
            // wipe-and-retry re-enters the cascade holding nothing.
            if (!v.waveArmedLatched)
            {
                if (!in.waveArmedQuorum || in.waveArmed >= in.waveArmedQuorum ||
                    in.partyInCombat || inPhaseMs >= in.waveArmMs)
                {
                    v.waveArmedLatched = true;
                }
                else
                {
                    v.waveArming = true;
                    v.waveHoldSinceMs = 0;
                    v.holdStill = true;
                    return v;
                }
            }

            // A wave mob out of reach is a wave that never clears: killsLeft only
            // moves on a DEATH, and both waves contain mobs that are parked at a
            // home position rather than walking at the party (wave 1's two
            // Deathbringers spline to fixed homes 30-60yd apart and go aggressive
            // in place). So the driver goes and gets them.
            if (in.nearestWaveDist > in.waveEngageRange)
            {
                v.waveHoldSinceMs = 0;
                v.engageWave = true;
                return v;
            }

            // In reach and fighting: exactly a tick the party should spend
            // fighting rather than walking — YIELD, so the stock combat engine
            // picks a target, swings, casts and holds threat.
            if (in.partyInCombat)
            {
                v.waveHoldSinceMs = 0;
                v.yieldTick = true;
                return v;
            }

            // IN REACH AND NOBODY IS FIGHTING. The wave has not aggroed, and the
            // hold that used to live here was UNBOUNDED — which is how two of five
            // Pit of Saron runs stood 8yd from ten live Ymirjar for six and a half
            // minutes with the whole party at full health.
            //
            // WAITING CANNOT FIX IT, because the thing being waited for does not
            // announce itself. Wave 1's eight escorts carry a smart_scripts pair —
            // AI_INIT -> REACT_PASSIVE, then a ONE-SHOT 3500ms update ->
            // REACT_AGGRESSIVE — and the two Deathbringers get the same window from
            // pit_of_saron.cpp (REACT_PASSIVE + UNIT_FLAG_NON_ATTACKABLE, cleared
            // 3.5s behind the escorts). SetReactState performs NO aggro scan, and
            // AzerothCore's proximity aggro is RELOCATION-DRIVEN ONLY:
            // CreatureAI::MoveInLineOfSight is reached from
            // CreatureUnitRelocationWorker and nowhere else, so the mob's one shot
            // at the party is spent while it is still passive. After the flip
            // SOMEBODY HAS TO MOVE — and a driver that holds still guarantees
            // nobody does, on both sides at once. The three runs that passed were
            // rescued by residual spline motion, not by design.
            //
            // So: hold only long enough to let an ordinary aggro land (the emerge
            // window is 3.5s), then START THE FIGHT. The clock is re-stamped on the
            // pull so a pull that does not take is retried on the same budget
            // rather than spinning.
            if (!v.waveHoldSinceMs)
                v.waveHoldSinceMs = in.nowMs;

            uint32 const standoffMs =
                in.nowMs >= v.waveHoldSinceMs ? in.nowMs - v.waveHoldSinceMs : 0;
            if (standoffMs < in.waveStandoffMs)
            {
                v.holdStill = true;
                return v;
            }

            v.pullWave = true;
            v.waveHoldSinceMs = in.nowMs;
            return v;
        }

        // --- the three gates --------------------------------------------------
        //
        // Walk in, form up, and forge every tick. The forge is unconditional once
        // the leader is inside — the core range-checks the packet, so a forge from
        // outside the sphere is a free no-op, and a forge whose precondition is not
        // yet met is refused just as freely.
        //
        // A gate is only ever reached with the wave dead, so the standoff clock is
        // spent here: drop it, or wave 2 opens holding wave 1's stamp.
        v.waveHoldSinceMs = 0;

        if (in.distToGate > in.gateLeash)
        {
            v.gateHoldSinceMs = 0;
            v.walkToGate = true;
            return v;
        }

        // Standing in the gate. Stamp the gate clock on arrival so the stall
        // watchdog measures the time spent FORGING, not the time spent walking or
        // (for gate 1) the ninety seconds of Krick outro that preceded it.
        if (!v.gateHoldSinceMs)
            v.gateHoldSinceMs = in.nowMs;

        uint32 const atGateMs =
            in.nowMs >= v.gateHoldSinceMs ? in.nowMs - v.gateHoldSinceMs : 0;
        v.stalled = in.gateStallMs && atGateMs >= in.gateStallMs;

        // ONCE PER GATE, not once per tick. A stall here is unrecoverable from
        // inside the driver — a dead orchestrator, or a wave mob that despawned
        // instead of dying and left killsLeft stuck — so the honest act is to name
        // it loudly and keep forging, and let the event's own timeout stall the
        // run for a human.
        if (v.stalled && !v.stallReported)
        {
            v.reportStall = true;
            v.stallReported = true;
        }

        // The quorum gates the FORGE, not the walk. Crossing a gate strung out is
        // how a party meets a ten-mob ambush with two of its five members.
        v.forge = v.quorumMet;
        v.holdStill = true;
        return v;
    }
}

#endif  // _PLAYERBOT_DCPOSGAUNTLETDECISION_H
