/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCCOSWAVEDECISION_H
#define _PLAYERBOT_DCCOSWAVEDECISION_H

#include <algorithm>
#include <cstdint>

#include "Define.h"

// PURE kernel for the Culling of Stratholme (map 595) wave phase — everything the
// wave controller decides each tick, lifted out of the world so it can be tested
// without a map, a creature or an instance.
//
// The glue (the grid census, the splines, the force-pull, the telemetry) lives in
// Overrides/CullingOfStratholmeDriver.cpp; this is arithmetic over a snapshot, in
// the same shape as DcHorWaveDecision.h and DcPosGauntletDecision.h.
//
// WHAT THE DRIVER IS FOR, and it is the exact opposite of Halls of Reflection's.
// There, every wave comes to the party and the driver's job is mostly to REFUSE —
// hold the altar, hand the tick back. Here NOTHING comes to the party at all:
//
//   * Arthas stops at waypoint 11 when the city intro ends and does not move
//     again until Salramm is dead. He is not the wave's target, he does not pull
//     for the party, and he is 90 to 260 yards from where the fighting is.
//   * Each wave is four TempSummons dropped at one of FOUR CLUSTERS up to 264
//     yards apart, with NO movement script and ordinary ~20yd detection. They
//     stand where they were summoned, indefinitely.
//
// So the driver's job is to TRAVEL. Without it the ordinary clear walks the party
// to the next objective anchor — Market Row — and stands there while four mobs
// wait 260 yards away at Elders' Square for a wave counter that will never move.
//
// FOUR FACTS FROM THE INSTANCE SCRIPT THAT THE LADDER BELOW ENCODES:
//
//   1. THE ONLY READABLE PROGRESS IS GetData(DATA_ARTHAS_EVENT), and for this
//      phase it has exactly two values: 4 (waves 1-5) and 5 (waves 6-10, set by
//      Meathook's death). It is MONOTONIC — an Arthas death repositions him and
//      re-summons the current wave group but never rolls the counter back — so
//      unlike Halls of Reflection there is no wipe state to detect and no restart
//      to orchestrate. The wave number itself is NOT readable: npc_arthasAI keeps
//      it in a private `waveGroupId` and only publishes it as a client world
//      state.
//
//   2. THERE IS NO INTER-WAVE TIMER. SummonedCreatureDies -> SendNextWave summons
//      the next four mobs in the SAME CALL as the fourth death. The census is
//      therefore essentially never empty while the phase runs, which is what
//      makes the ladder's Hold branch a genuine "nothing to do this tick" rather
//      than a gap that needs filling: the only empty windows are the 20 seconds
//      before wave 1 and the 10 seconds after an Arthas death, and in both the
//      right answer is to stand still and wait for the census.
//
//      It is ALSO why the rest branch has to be allowed to fire with a live wave
//      standing on the map. There is no safe gap to rest in — the whole phase is
//      one continuous fight — and the mobs do not come to you, so resting 40+
//      yards from the nearest one is as safe as resting gets here.
//
//   3. THE PHASE ENDS ON SALRAMM, NOT ON A WAVE COUNT. The predicate goes false
//      when the counter reaches 6, which is the single signal this kernel needs
//      for completion; Complete is also what it reports for a party that is not in
//      the window at all.
//
//   4. ARTHAS IS NOT PART OF THIS FIGHT. He is REACT_DEFENSIVE at waypoint 11 and
//      the wave mobs never walk to him, so — unlike every other leg of this
//      dungeon — there is no escortee to protect, nothing to heal, and no reason
//      for the party to stay anywhere near him. That is the whole reason the
//      escort is split and the waves get a driver of their own.
namespace DcCosWaves
{
    // What the party should be doing about the current wave. Derived fresh from
    // the instance and a grid census every tick; the only stored state is the
    // standoff clock and the state id the telemetry reports transitions on.
    enum class State : uint8
    {
        Complete = 0,  // not in the wave window (counter is not 4 or 5)
        Fight,         // something is engaged — yield to the combat engine
        Travel,        // a live wave stands out of reach — walk to it
        Standoff,      // in reach, nobody in combat, and it has been too long
        Rest,          // out of combat, nothing near, and the party needs it
        Hold,          // census empty (or waiting out the standoff clock)
    };

    inline char const* StateName(State s)
    {
        switch (s)
        {
            case State::Fight:    return "fighting the wave";
            case State::Travel:   return "walking to the wave";
            case State::Standoff: return "the wave is in reach and silent — pulling it";
            case State::Rest:     return "resting between waves";
            case State::Hold:     return "holding — waiting for the next wave to spawn";
            case State::Complete:
            default:              return "complete";
        }
    }

    struct Inputs
    {
        // --- the world -------------------------------------------------------

        // GetData(DATA_ARTHAS_EVENT). The phase is exactly {4, 5}; anything else
        // means this kernel has nothing to say.
        uint32 progress = 0;
        uint32 progressWavesStart = 4;  // PROGRESS_FINISHED_CITY_INTRO
        uint32 progressMeathook   = 5;  // PROGRESS_KILLED_MEATHOOK

        // --- what is actually on the field -----------------------------------

        // Live creatures of the wave entries WITH spawnId 0 inside the census
        // radius. The spawnId filter is the whole probe and it is not optional:
        // three of the wave entries also have static spawns on this map (103 Risen
        // Zombies and 7 Enraging Ghouls line Fire Street, with two Crypt Fiends),
        // and the Stratholme citizens UpdateEntry into Risen Zombies during the
        // city intro. Counting one of those would read "the wave is up" from the
        // moment Fire Street is in scan range and send the party the wrong way
        // across the city.
        uint32 waveAlive = 0;

        // 3D distance to the NEAREST of them. Negative when waveAlive is 0.
        float nearestWaveDist = -1.0f;

        // Is any party member engaged? During this phase there is nothing else on
        // the map to be in combat WITH — Fire Street is 250 yards away behind the
        // party's own future, the citizens are not hostile, and Arthas's own
        // fights are over — so this reads as "the party is fighting the wave".
        bool partyInCombat = false;

        // ...and the same question asked of the mobs, which answers one case
        // `partyInCombat` cannot: a wave that has aggroed a member whose combat
        // flag has not caught up yet.
        bool waveEngaged = false;

        // --- geometry and budgets --------------------------------------------

        // Past this the driver travels; inside it, it fights or breaks a standoff.
        float engageRange = 25.0f;

        // Do not sit down with a live wave mob nearer than this.
        float restSafeDist = 40.0f;

        // --- recovery --------------------------------------------------------

        // The ordinary between-pulls rest gate, measured over the whole party.
        // False means somebody is short of HP or mana.
        bool partyRecovered = true;

        // --- clocks -----------------------------------------------------------
        uint32 nowMs = 0;
        uint32 standoffMs = 5000;    // budget
        uint32 standoffSinceMs = 0;  // stored: when the silence began (0 = not silent)

        // HOW LONG ONE REST WINDOW MAY LAST, and the clock it is measured on.
        //
        // The bound exists because `partyRecovered` is a predicate the party can
        // fail FOR EVER: a bot out of water, a member the rez ladder cannot reach,
        // a config whose rest target is above what drinking can deliver. An
        // unbounded hold on it would stop the dungeon dead in a gap between waves,
        // with every watchdog reporting a perfectly healthy party standing still —
        // which is the shape this module has been bitten by more than any other.
        //
        // The budget is CUMULATIVE over consecutive FUTILE rest, not per gap, and
        // that distinction is the whole point of the bound.
        //
        // It used to be per window: the clock ran only while resting was wanted and
        // happening, and ANY combat cleared it. Live (tp-20260910-091724-1) that
        // gave a prot-paladin tank an unbounded hold in disguise. It drops below
        // the 65% mana floor every wave fight and cannot climb back in a gap, so
        // every window ran the full budget, gave up, walked, fought for five
        // seconds — and the fight reset the clock, opening a fresh full budget.
        // Runs -9 and -10 spent 714s and 381s resting across 16 and 6 windows,
        // against 1-34s for the eight runs whose tank was not a mana user.
        //
        // So futile rest ACCUMULATES across the phase and a fight no longer
        // forgives it. What does forgive it is success: `partyRecovered` zeroes the
        // accumulator, because the bound exists to catch a party that will never
        // reach its target, never one that keeps reaching it. 0 disables the bound.
        uint32 restBudgetMs = 0;
        uint32 restSinceMs = 0;      // stored: when this rest window opened
        uint32 restSpentMs = 0;      // stored: futile rest from CLOSED windows this phase

        uint8  state = 0;            // the stored State the transition log belongs to
        uint32 stateSinceMs = 0;
    };

    struct Verdict
    {
        State state = State::Complete;

        // --- what the driver should DO ---------------------------------------

        // Walk to the nearest live wave mob's own position. Deliberately AT the
        // mob and not at an authored cluster point: the clusters' geometric centre
        // has no navmesh at city height (measured), and this map carries a flat
        // z = 0.14 sheet under the city that an off-mesh destination resolves to —
        // a ~130 yard sink. A live summon's position is standable by construction.
        bool travel = false;

        // Start the fight: EngageWithTarget + AttackStart on the nearest live mob,
        // and flip the bot onto the combat engine.
        bool pullWave = false;

        // Issue no movement but still claim the tick.
        bool holdStill = false;

        // Hand the tick back to the stock combat engine. TRUE only where the right
        // thing for the party to be doing is FIGHTING.
        bool yieldTick = false;

        // --- derived facts the glue logs or stores ---------------------------
        bool complete = false;

        uint32 standoffSinceMs = 0;
        uint32 restSinceMs = 0;
        uint32 restSpentMs = 0;

        // This is the tick the rest budget ran out on, once per phase. The glue
        // logs it: a party that stops resting because it gave up is a very
        // different observation from one that stops because it is full.
        bool restBudgetExpired = false;

        uint8  storeState = 0;
        uint32 stateSinceMs = 0;
    };

    inline Verdict Decide(Inputs const& in)
    {
        Verdict v;
        v.stateSinceMs = in.stateSinceMs;

        // --- 1. is the wave phase ours at all? -------------------------------
        //
        // Two values and no others. Below 4 the city intro has not finished (so
        // no wave exists and Arthas is still walking); at 6 Salramm is dead and
        // the Town Hall objective owns the party.
        if (in.progress != in.progressWavesStart && in.progress != in.progressMeathook)
        {
            v.state = State::Complete;
            v.complete = true;
            v.yieldTick = true;
            v.storeState = static_cast<uint8>(State::Complete);
            v.stateSinceMs = 0;
            v.standoffSinceMs = 0;
            v.restSinceMs = 0;
            return v;
        }

        bool const haveWave = in.waveAlive > 0 && in.nearestWaveDist >= 0.0f;
        bool const inReach = haveWave && in.nearestWaveDist <= in.engageRange;

        // --- 2. the standoff clock -------------------------------------------
        //
        // Runs only while a live wave mob stands INSIDE engage range with nobody
        // in combat — the one shape that can deadlock. Cleared in every other,
        // so the budget is always measured from the start of the current silence
        // and never from some earlier one.
        //
        // This should never actually expire on this map: the wave mobs are
        // ordinary REACT_AGGRESSIVE trash with ~20yd detection and the tank walks
        // into them, which is a relocation and therefore an aggro check. It is
        // here because Pit of Saron proved a parked party and a parked mob can
        // stand eight yards apart indefinitely once neither is relocating, and
        // because the cost of being wrong about that is the whole run.
        bool const silent = inReach && !in.partyInCombat && !in.waveEngaged;
        if (!silent)
            v.standoffSinceMs = 0;
        else
            v.standoffSinceMs = in.standoffSinceMs ? in.standoffSinceMs : in.nowMs;

        // --- 2b. the rest clock ----------------------------------------------
        //
        // For the bound described on Inputs::restBudgetMs, and NOT the same shape as
        // the standoff clock above: this one banks rather than resets.
        //
        // A rest is in progress when it is both wanted and possible — out of combat,
        // the party short, and nothing live within restSafeDist. Every tick of one
        // accrues into the accumulator, so the budget measures the phase's total
        // futile rest and an interrupting fight can no longer launder it.
        bool const idle = !in.partyInCombat && !in.waveEngaged;
        bool const restSafe = !haveWave || in.nearestWaveDist > in.restSafeDist;
        bool const wantRest = idle && !in.partyRecovered && restSafe;

        uint32 banked = in.restSpentMs;
        uint32 since = in.restSinceMs;

        if (in.partyRecovered)
        {
            // THE ONE THING THAT FORGIVES THE BUDGET. The party got what it sat
            // down for, so nothing it spent getting there was futile — and a party
            // that keeps recovering must never be talked out of resting later in
            // the phase by rest it already spent and used.
            banked = 0;
            since = 0;
        }
        else if (!wantRest && since)
        {
            // The window closed with the party STILL SHORT — a fight opened, or a
            // wave mob walked inside restSafeDist. It bought nothing, so it counts.
            // This is the branch that used to zero the clock instead.
            banked += static_cast<uint32>(in.nowMs - since);
            since = 0;
        }
        else if (wantRest && !since)
            since = in.nowMs;

        // The live total is what is banked plus whatever the open window has run up
        // so far, so a single window can spend the budget on its own exactly as it
        // did before — the accumulator only ever adds windows to it.
        uint32 const openMs = since ? static_cast<uint32>(in.nowMs - since) : 0;

        // Hold both at the budget. Once it is spent the party stops entering Rest,
        // but wantRest can stay true for the rest of the phase (idle, short, nothing
        // near) and an uncapped accumulator would just keep climbing.
        uint32 spent = banked + openMs;
        if (in.restBudgetMs)
        {
            spent = std::min(spent, in.restBudgetMs);
            banked = std::min(banked, in.restBudgetMs);
        }

        v.restSinceMs = since;
        v.restSpentMs = banked;

        bool const restExpired = in.restBudgetMs && spent >= in.restBudgetMs;

        // Report it ONCE. The stored state is the latch: on the tick the budget runs
        // out the party was still Rest, and from the next tick on it is Travel or
        // Hold — so this is true exactly on the transition and needs no extra field
        // to remember it. A later recovery zeroes the bank, so a party that earns a
        // second budget can also earn a second line.
        v.restBudgetExpired = restExpired && in.state == static_cast<uint8>(State::Rest);

        // --- 3. which state --------------------------------------------------
        //
        // ORDERED BY URGENCY, and the order is the design:
        //
        //   Fight first, because a tick spent deciding anything else while four
        //   mobs are swinging is a tick the tank does not swing back.
        //
        //   Rest second — ahead of Travel — because the wave is not going
        //   anywhere and the party is. There is no gap anywhere in these ten
        //   waves (fact 2), so if the party is not topped off before it walks into
        //   the next 4-pack it will not be topped off until Salramm is dead.
        //
        //   Travel before Standoff because they are two halves of one answer
        //   keyed on the same distance, and Travel is the one that is almost
        //   always right.
        if (in.waveEngaged || in.partyInCombat)
            v.state = State::Fight;
        else if (wantRest && !restExpired)
            v.state = State::Rest;
        else if (haveWave && in.nearestWaveDist > in.engageRange)
            v.state = State::Travel;
        else if (silent && v.standoffSinceMs &&
                 static_cast<uint32>(in.nowMs - v.standoffSinceMs) >= in.standoffMs)
            v.state = State::Standoff;
        else
            v.state = State::Hold;

        // --- 4. the state clock ----------------------------------------------
        v.storeState = static_cast<uint8>(v.state);
        if (in.state != v.storeState)
            v.stateSinceMs = in.nowMs;

        // --- 5. what to do about it ------------------------------------------
        switch (v.state)
        {
            case State::Fight:
                // YIELD. There is nothing to steer during a wave fight — the mobs
                // are on the party and the party is on them — and this driver sits
                // above the stock combat movers (DcRel::EventDueCombat). Claiming
                // this tick is claiming the tick the tank would have used to hold
                // threat on four mobs that opened on the healer.
                v.yieldTick = true;
                break;

            case State::Travel:
                v.travel = true;
                break;

            case State::Standoff:
                v.pullWave = true;
                break;

            case State::Rest:
            case State::Hold:
            default:
                // HOLD AND CLAIM THE TICK — never yield out of combat.
                //
                // Yielding here would hand the leg to DcRel::Advance (15) and
                // DcRel::AtBoss (30), and on this map the next objective anchor is
                // Market Row: the party would be walked off the Elders' Square
                // wave it is halfway to, or off the rest it needs, to stand on a
                // wave-counter hold that cannot clear until the wave it just left
                // is dead. (The Violet Hold lesson — a Done out of combat hands
                // the tick to the garrison.)
                //
                // The cost is the TANK's rest ticks, as it is for every driver in
                // this module: the event rung is leader-only, so every FOLLOWER
                // keeps its own ladder and eats and drinks normally through a Rest
                // hold. That is the right trade here for the usual reason — a tank
                // that does not drink for a few minutes is much cheaper than a
                // healer that does not.
                //
                // It is ALSO why the rest budget above has to be cumulative. This
                // paragraph used to end "and on this map the tank is the member
                // least likely to need mana", which a prot paladin falsified: it is
                // the member most likely to be short and least able to fix it, and
                // holding the phase on its mana bar is what the budget bounds.
                v.holdStill = true;
                break;
        }
        return v;
    }
}

#endif  // _PLAYERBOT_DCCOSWAVEDECISION_H
