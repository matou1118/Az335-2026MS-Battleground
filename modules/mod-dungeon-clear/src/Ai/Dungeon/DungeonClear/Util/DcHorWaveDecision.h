/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCHORWAVEDECISION_H
#define _PLAYERBOT_DCHORWAVEDECISION_H

#include <cstdint>

#include "Define.h"

// PURE kernel for the Halls of Reflection (map 668) wave phase — everything the
// altar driver decides every tick, lifted out of the world so it can be tested
// without a map, a creature or an instance.
//
// The glue (grid scans, the party walk, the splines, the telemetry) lives in
// Overrides/HallsOfReflectionDriver.cpp; this is arithmetic over a snapshot, in
// the same shape as DcPosGauntletDecision.h.
//
// WHAT THE DRIVER IS FOR, in one paragraph. Between the end of the intro and
// Marwyn's death there is NOTHING ON THIS MAP TO PULL. Falric, Marwyn and all 34
// wave mobs are pre-spawned invisible, UNIT_FLAG_NOT_SELECTABLE and
// SetImmuneToAll; the instance activates them one wave at a time and THEY attack.
// So DC's advance/pull pipeline has nothing to do for six to nine minutes, and
// left to itself it does the one thing that loses the run: it walks the tank to
// the nearest boss anchor — which is an invisible, immune Marwyn 35yd from the
// altar — and stands on him. That is tr-20260907-100815-2, aborted by hand at
// 5m38s with encounterMask 0x0.
//
// THE DRIVER'S JOB IS THEREFORE MOSTLY TO REFUSE, and the shape of the refusal is
// what the states below encode: claim the tick and hold the party at a camp point
// while nothing is fightable, and hand the tick straight back the moment
// something is. It never pulls, never engages and never walks at a mob. There is
// no equivalent of Pit of Saron's standoff-breaker here and there must not be:
// every wave activation ends in SetInCombatWithZone() plus an AttackStart on the
// FARTHEST player, so the wave always finds the party, and a driver that went
// looking for one would only be walking toward the leash.
//
// FOUR FACTS FROM THE INSTANCE SCRIPT THAT THE STATES BELOW ENCODE:
//
//   1. THE ONLY READABLE PROGRESS IS GetData(DATA_WAVE_NUMBER), 0..10, plus the
//      two boss states. It is NOT monotonic: HandleWaveWipe sets it back to 0,
//      and the wave the instance then restarts from is 6 if wave >= 6 was ever
//      started this map-load and 1 otherwise. So `waveNumber == 0` with the intro
//      flag set is not "the waves have not begun" — it is "the event just wiped
//      and is waiting to be restarted".
//
//   2. THE RESTART NEEDS NO GOSSIP AND NO CLICK. instance_halls_of_reflection
//      polls every 5s and re-arms the whole event the moment every non-GM player
//      is ALIVE and within MAX_DIST_FROM_CENTER_TO_START (40yd) of CenterPos. So
//      the RESTART state's entire job is to put the party back on the camp — which
//      is inside that radius by construction — and let the rez ladder (31.5, above
//      the event rung at 31) bring the dead back.
//
//   3. THE LEASH IS THE RUN-ENDER. Any non-GM player more than 70.5yd (2D) from
//      CenterPos while a wave is live wipes the event AND RESPAWNS EVERY DEAD MOB.
//      The camp is authored ~20yd from centre precisely so that a 4s fear plus a
//      knockback cannot reach the line; the kernel additionally reports the
//      furthest member so a run that DOES trip it names who.
//
//   4. A BOSS WAVE HAS AN EIGHT-SECOND ARMING WINDOW. On wave 5 and wave 10 the
//      boss yells (DoAction(1)) and only 8 seconds later gets SetImmuneToPC(false)
//      + SetInCombatWithZone. During those 8 seconds he is visible and alive and
//      completely untouchable, and a driver that yielded would hand the tick to a
//      combat engine with nothing to hit. BossArm holds instead — it is the same
//      ACTION as Camp, and it exists as its own state so the log says which.
namespace DcHorWaves
{
    // Where the party is in the first half of the dungeon. Derived fresh from the
    // instance every tick; the only stored state is the state id and its clock,
    // and those exist for telemetry rather than for the decision.
    enum class State : uint8
    {
        Complete = 0,  // the intro has not finished, or Marwyn is dead
        Camp,          // nothing fightable — hold the altar
        Fight,         // a wave mob or a boss is live and attackable — yield
        BossArm,       // wave 5/10: the boss has yelled but is still IMMUNE_TO_PC
        Rest,          // Falric is down; the instance's only 60s breathing space
        Restart,       // the event wiped — regather inside the 40yd restart radius
    };

    inline char const* StateName(State s)
    {
        switch (s)
        {
            case State::Camp:    return "holding the altar";
            case State::Fight:   return "fighting the wave";
            case State::BossArm: return "the boss has yelled but is still immune";
            case State::Rest:    return "resting — Falric is down, wave 6 in 60s";
            case State::Restart: return "the wave event WIPED — regathering at the altar";
            case State::Complete:
            default:             return "complete";
        }
    }

    struct Inputs
    {
        // --- the world -------------------------------------------------------

        // GetPersistentData(PERSISTENT_DATA_INTRO). Set by the intro script when
        // it finishes, which is the same tick StartNextWave() runs — so this is
        // exactly "wave 1 has begun or has been running". It survives a map
        // reload, which is what makes it safe as the driver's near-gate.
        bool introDone = false;

        // GetBossState(...) == DONE for each.
        bool falricDone = false;
        bool marwynDone = false;

        // GetData(DATA_WAVE_NUMBER), 0..10. NOT monotonic — see fact 1 above.
        uint32 waveNumber = 0;

        // --- what is actually fightable --------------------------------------

        // Live wave creatures within the scan that are SELECTABLE and not
        // IMMUNE_TO_PC — i.e. activated. The filter is the whole probe: 34 of
        // these entries exist from map load and a bare aliveness count would read
        // "the wave is up" before the party has even gossiped.
        uint32 mobsArmed = 0;

        // Falric or Marwyn, alive and NOT IMMUNE_TO_PC. The boss fight proper.
        bool bossAttackable = false;

        // ...alive and visible but STILL immune. Only meaningful on waves 5 and
        // 10; see fact 4.
        bool bossVisible = false;

        bool partyInCombat = false;

        // --- geometry ---------------------------------------------------------
        float distToCamp = 0.0f;
        float campLeash = 0.0f;

        // Living party members on this map, leader included, and how many of them
        // are inside the instance's own restart radius of CenterPos. The RESTART
        // state reports on these; it does not gate on them, because the thing that
        // re-arms the event is the INSTANCE's poll and not anything the driver can
        // do beyond standing in the right place.
        uint32 living = 0;
        uint32 nearCenter = 0;
        float  furthestFromCenter = 0.0f;

        // --- clocks -----------------------------------------------------------
        uint32 nowMs = 0;
        uint8  state = 0;          // the stored State this clock belongs to
        uint32 stateSinceMs = 0;
        bool   restartLogged = false;
    };

    struct Verdict
    {
        State state = State::Complete;

        // --- what the driver should DO ---------------------------------------
        bool walkToCamp = false;  // steer the tank back onto the camp point
        bool holdStill = false;   // issue no movement, but still claim the tick

        // Hand the tick back to the stock combat engine. TRUE only where the right
        // thing for the party to be doing is FIGHTING.
        bool yieldTick = false;

        // --- derived facts the glue logs or stores ---------------------------
        bool complete = false;
        bool reportRestart = false;  // this is the tick to name the wipe, once

        uint8  storeState = 0;
        uint32 stateSinceMs = 0;
        bool   restartLogged = false;
    };

    inline Verdict Decide(Inputs const& in)
    {
        Verdict v;
        v.stateSinceMs = in.stateSinceMs;
        v.restartLogged = in.restartLogged;

        // --- 1. is the wave phase ours at all? -------------------------------
        //
        // The window opens the instant the intro script finishes (which is the
        // same tick wave 1 activates) and closes for ever when Marwyn dies. It is
        // deliberately NOT gated on combat, for the Halls of Lightning reason: the
        // hold has to be in force during the gaps between waves, which are five
        // seconds long, and a predicate that waited for combat would arm the
        // driver only after the clear had already walked the tank at a boss.
        //
        // This is also the map's near-gate justification for the allow-list row in
        // t/TestEventRegistry.cpp, and it is a strong one: the persistent intro
        // flag is set by exactly one thing — the intro script running to the end —
        // and the intro script is started by exactly one thing, the leader gossip
        // that event 1 performs 60yd inside the front door.
        if (!in.introDone || in.marwynDone)
        {
            v.state = State::Complete;
            v.complete = true;
            v.yieldTick = true;
            v.storeState = static_cast<uint8>(State::Complete);
            v.stateSinceMs = 0;
            v.restartLogged = false;
            return v;
        }

        // --- 2. which state --------------------------------------------------
        //
        // ORDERED BY URGENCY, and the order is the design:
        //
        //   Fight first, because a tick spent deciding anything else while a mob
        //   is swinging is a tick the tank does not swing back.
        //
        //   Restart second, because it is the one state whose CAUSE is invisible
        //   from every other probe: after a leash wipe the counter reads 0, every
        //   mob is hidden again and nothing is fightable, so without this branch
        //   the driver would report a perfectly healthy Camp while the whole first
        //   half of the dungeon silently replayed.
        //
        //   BossArm and Rest are Camp with a name on them.
        if (in.bossAttackable || in.mobsArmed > 0)
            v.state = State::Fight;
        else if (!in.waveNumber)
            v.state = State::Restart;
        else if (in.bossVisible && (in.waveNumber % 5) == 0)
            v.state = State::BossArm;
        else if (in.falricDone && in.waveNumber == 5)
            v.state = State::Rest;
        else
            v.state = State::Camp;

        // --- 3. the state clock ----------------------------------------------
        v.storeState = static_cast<uint8>(v.state);
        if (in.state != v.storeState)
        {
            v.stateSinceMs = in.nowMs;
            // The wipe report is scoped to ONE restart episode: re-armed whenever
            // the state changes, so a second wipe later in the run is reported
            // again and a Restart that lasts ninety seconds is reported once.
            v.restartLogged = false;
        }

        // --- 4. what to do about it ------------------------------------------
        if (v.state == State::Fight)
        {
            // YIELD. Everything here comes to the party — every activation ends in
            // SetInCombatWithZone plus an AttackStart on the farthest player — so
            // there is never anything to walk at, and the driver sits above the
            // stock combat movers (DcRel::EventDueCombat, 61). Claiming this tick
            // is claiming the tick the tank would have used to hold threat on five
            // mobs that opened on the DPS.
            v.yieldTick = true;
            return v;
        }

        if (v.state == State::Restart && !v.restartLogged)
        {
            v.reportRestart = true;
            v.restartLogged = true;
        }

        // Camp, BossArm, Rest and Restart are ONE ACTION: be on the camp point.
        //
        // It claims the tick either way. Yielding while nothing is fightable is
        // what hands the leg back to DcRel::Advance (15) and DcRel::AtBoss (30),
        // and on this map the next boss anchor is an invisible immune Marwyn — so
        // a yield here is not "let the party fight", it is "walk the tank onto a
        // creature it cannot touch and leave it there".
        //
        // The cost is the TANK's rest ticks, as it is on every other driver in the
        // module: the event rung is leader-only, so every follower keeps its own
        // ladder and drinks normally through the 60s Rest window. That is the
        // right trade here for the same reason as elsewhere — a tank that does not
        // eat for a minute is much cheaper than a healer that does not.
        if (in.distToCamp > in.campLeash)
            v.walkToCamp = true;
        else
            v.holdStill = true;
        return v;
    }
}

#endif  // _PLAYERBOT_DCHORWAVEDECISION_H
