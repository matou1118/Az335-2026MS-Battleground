/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCHORESCAPEDECISION_H
#define _PLAYERBOT_DCHORESCAPEDECISION_H

#include <cstdint>

#include "Define.h"

// PURE kernel for the Halls of Reflection (map 668) escape — the whole of the
// escape driver's per-tick reasoning, without a map, a creature or an instance.
//
// The glue (grid scans, the wall-GO read, the splines, the telemetry) lives in
// Overrides/HallsOfReflectionDriver.cpp.
//
// THE ENCOUNTER, in the terms these functions use. From the moment the leader's
// gossip is accepted, the Lich King (36954) walks a fixed 19-waypoint path at
// 1.445 yd/s under Remorseless Winter and NEVER PAUSES. Jaina/Sylvanas runs ahead
// of him and stops at four of those waypoints, each ~24yd short of an Ice Wall
// she cannot pass; each wall opens only when the WHOLE summon batch the Lich King
// spat out behind the party is dead. He reaches 12.5yd of her at T0+75.6 / +156 /
// +233 / +355, and when he does he catches her, kills her three seconds later and
// wipes the party with Fury of Frostmourne.
//
// SO THE CLOCK IS ABSOLUTE AND THE THROUGHPUT IS NOT THE PROBLEM. A level-80
// party clears each batch with margin; what loses this fight is POSITION. Three
// rules are enforced by the encounter itself and all three are geometric:
//
//   1. THE CATCH IS ON THE LEADER, NOT ON PLAYERS. Nothing a bot does can be
//      "caught". The only way players lose her is by not clearing a wall in time.
//
//   2. FALLING BEHIND HIM COSTS 10 000 DAMAGE AND A KNOCKBACK. Every 2 seconds
//      while Winter is up, each alive player with (p.x - lk.x) + (p.y - lk.y) >
//      20 is Zapped (70653) — and because the whole path runs -x AND -y, that
//      single scalar IS "behind him". The knockback throws the victim further
//      behind, so the rule is self-reinforcing and has to be pre-empted rather
//      than reacted to. LK_BEHIND_SUM is a third of the way to the line.
//
//   3. INSIDE 10YD OF HIM IS 7068 +/- 863 FROST PER SECOND. LK_PRESSURE_DIST is
//      that plus margin.
//
// Both corrections are the SAME MOVE — forward along the path, to the party's
// stand point — and that is the single most important property of this kernel. A
// radial "get away from the emitter" vacate is exactly wrong for a bot that is
// already behind him: away-from-him is further behind, back into the zap rule.
// See DungeonClearHorStayAheadAction for the per-follower half of the same rule.
//
// WHAT THE KERNEL DOES *NOT* MODEL, deliberately:
//
//   * currentWall. npc_hor_lich_kingAI keeps it privately and nothing exposes it.
//     The observable equivalent is THE LEADER'S OWN POSITION — she runs to the
//     next stop the instant WallCompleted fires — so `leaderStop` is the state
//     variable, and `wallOpen` (read off the Ice Wall gameobject) is a second,
//     faster witness of the same transition. Either advances the party.
//
//   * how much health the adds have left. ADVANCE fires on the WALL, never on
//     "the adds are dead", because on heroic the last abomination of wall 4 can
//     still be alive when the wall opens and waiting for it would spend the
//     margin that wall's 1.17M HP has already eaten.
namespace DcHorEscape
{
    enum class State : uint8
    {
        Done = 0,    // the escape is not running (not started, finished, or reset)
        Prelude,     // gossip accepted, walking to the first stand point
        Hold,        // standing at the stand point with nothing to do
        Fight,       // the batch is on the party — yield to the rotation
        Pressure,    // too close to him, or behind him — step forward NOW
        Advance,     // the wall opened / the leader moved on — take the next stand
        Final,       // wall 4 is down; run the last 131yd to WP18 with her
        Doomed,      // Harvest Soul is on the leader; Fury lands in 3 seconds
        Threat,      // the leader moved on but the batch is loose — take it first
        Recenter,    // drifted forward into the shut wall — step back off it
        Regroup,     // drifted off the band at THIS stop — back to its near edge
        Engage,      // a summon is loose, none is on me, and it is out of reach
    };

    inline char const* StateName(State s)
    {
        switch (s)
        {
            case State::Prelude:  return "prelude — getting to the first wall ahead of him";
            case State::Hold:     return "holding the stand point";
            case State::Fight:    return "fighting the wall's summons";
            case State::Pressure: return "TOO CLOSE / BEHIND HIM — stepping forward";
            case State::Advance:  return "the wall is open — moving up";
            case State::Final:    return "wall 4 is down — running for the gunship";
            case State::Doomed:   return "the leader has been caught; Fury of Frostmourne is coming";
            case State::Threat:   return "taking the batch before moving up";
            case State::Recenter: return "too far forward — stepping back off the wall";
            case State::Regroup:  return "drifted off the stand — back to the leash edge";
            case State::Engage:   return "nothing is on me — walking at the loose summon";
            case State::Done:
            default:              return "not running";
        }
    }

    struct Inputs
    {
        // --- is this ours ----------------------------------------------------

        // GetBossState(DATA_LICH_KING) == IN_PROGRESS. Raised by exactly one
        // thing (the escape gossip in hook 34) and cleared by DONE or by the
        // full-reset FAIL, so it is both the predicate and the near-gate.
        bool active = false;

        // --- the leader -------------------------------------------------------
        bool leaderAlive = false;

        // Harvest Soul (70070) is on her: she dies in three seconds and the party
        // dies with her. Nothing useful is left to decide.
        bool leaderHasHarvestSoul = false;

        // WHICH WP_STOP she is at or heading for: the nearest stop within
        // LEADER_STOP_SNAP, else the next one along the path. 0 is the start
        // (WP0), 1..4 the four walls, 5 the end (WP18).
        uint8 leaderStop = 0;

        // --- the wall ---------------------------------------------------------

        // The Ice Wall gameobject at the CURRENT wall is absent or GO_STATE_ACTIVE
        // — i.e. it has opened. A faster witness of WallCompleted than the leader
        // actually getting moving, and the one heroic wall 4 needs (see the header
        // note): the party leaves on the wall, not on the last abomination.
        bool wallOpen = false;

        // --- the Lich King ----------------------------------------------------

        // Remorseless Winter (69780) is on him. While it is NOT, neither the zap
        // rule nor the ring exists — that is true both for the ~16s before he
        // casts it and for ever after the fourth wall falls.
        bool lkHasWinter = false;

        // THIS BOT's distance to him, and its own (x + y) minus his. The second is
        // the core's own scalar, unscaled: > 20 is a Zap, and LK_BEHIND_SUM is the
        // fraction of it the driver acts at.
        float distToLk = 0.0f;
        float sumMinusLkSum = 0.0f;

        // How near he has got to the LEADER, for the stall watchdog only.
        float lkToLeaderDist = 0.0f;

        // --- geometry and the fight --------------------------------------------
        float distToStand = 0.0f;   // this bot -> the stand point for the target stop

        // THE SCHMITT TRIGGER. standLeash is how near counts as ARRIVED,
        // standLeaveLeash how far counts as GONE, and they are deliberately not
        // the same number — see DcHallsOfReflection::STAND_LEAVE_LEASH for the
        // fifty-seven-transition thrash that having only one of them produced.
        float standLeash = 0.0f;
        float standLeaveLeash = 0.0f;

        uint32 addsAlive = 0;

        // How many of those are actually ON this bot (their victim is it). The
        // driver runs on the tank, so `addsAlive > addsTargetingMe` is "part of
        // the batch is loose" — which is the thing that must not be walked away
        // from, as distinct from a batch that is merely still alive.
        uint32 addsTargetingMe = 0;

        bool   partyInCombat = false;

        // This bot's distance to the CURRENT wall's gameobject origin, and the
        // stand point's own distance to it. Their difference is how far forward of
        // the stand the bot has drifted; wallStandoffSlack is how much of that is
        // tolerated before it is walked back. Only meaningful while the wall is
        // shut — once it opens the party walks through the thing.
        float wallDist = 0.0f;
        float standWallDist = 0.0f;
        float wallStandoffSlack = 0.0f;

        // ...and how near the standoff the tank has to get back BEFORE Recenter
        // lets go. Arming and disarming on one number is what made the state
        // flip Fight -> Recenter -> Fight twenty-seven times in a single run;
        // see DcHallsOfReflection::WALL_STANDOFF_CLEAR.
        float wallStandoffClear = 0.0f;

        // --- the loose summon --------------------------------------------------

        // Distance to the NEAREST living summon whose victim is not this bot, or
        // <= 0 when there is none. The pair below bounds what the driver will
        // walk at: inside engageMelee the rotation reaches it unaided, past
        // engageReach it is still running in from the Lich King and walking at it
        // is walking at HIM.
        float looseAddDist = 0.0f;
        float engageReach = 0.0f;
        float engageMelee = 0.0f;

        // How long "a summon is up and none of it is on me" must have held
        // before the driver acts, so the ordinary sub-second gap between one add
        // dying and the next arriving is not a reason to walk anywhere.
        uint32 engageIdleMs = 0;
        uint32 idleSinceMs = 0;     // when that first became true; 0 = not idle

        // --- the two thresholds -------------------------------------------------
        float pressureDist = 0.0f;  // DcHallsOfReflection::LK_PRESSURE_DIST
        float behindSum = 0.0f;     // DcHallsOfReflection::LK_BEHIND_SUM

        // --- clocks --------------------------------------------------------------
        uint32 nowMs = 0;
        uint8  state = 0;
        uint32 stateSinceMs = 0;
        uint8  stallStop = 0;       // the stop the stall report was last fired for
        bool   stallReported = false;
        float  stallDist = 0.0f;    // DcHallsOfReflection::LK_STALL_DIST
        uint32 stallMs = 0;
        uint32 stopHoldSinceMs = 0; // when the party first stood at THIS stop

        // When targetStop last CHANGED — i.e. when the leader moved on and the
        // party acquired a new place to be. The threat-pickup gate is measured
        // from here, so it is spent once per wall and cannot accumulate.
        uint32 targetStopSinceMs = 0;
        uint32 grabThreatMs = 0;    // DcHallsOfReflection::GRAB_THREAT_MS

        // HAS THE PARTY EVER MADE THIS STOP? The difference between the two
        // reasons a bot can be off the stand point, and they want opposite
        // moves: not yet arrived is a 100-180yd LEG and must end on the point,
        // while arrived-then-drifted is a chase step and must end on the near
        // EDGE of the band (see STAND_EDGE_MARGIN). Cleared whenever the stop
        // changes, so the first crossing of every wall is always a leg.
        bool reachedStand = false;
    };

    struct Verdict
    {
        State state = State::Done;

        // WHICH stand point the party should be on: 1..4 are the four walls, 5 is
        // WP18. Always at least 1 — the party is ahead of the leader, never on the
        // start waypoint she is leaving.
        uint8 targetStop = 1;

        // --- what the driver should DO ---------------------------------------
        bool travel = false;     // TravelTo the stand point for targetStop
        bool holdStill = false;  // issue no movement, but claim the tick
        bool yieldTick = false;  // hand the tick to the stock combat engine

        // WHERE `travel` is aimed, when it is not the stand point itself.
        // Exactly one of these is ever set, and neither without `travel`.
        bool travelToEdge = false;  // the near edge of the band, not the centre
        bool travelToAdd = false;   // the loose summon the rotation cannot reach

        // --- derived facts the glue logs or stores ---------------------------
        bool complete = false;
        bool reportStall = false;
        bool reportDoomed = false;

        uint8  storeState = 0;
        uint32 stateSinceMs = 0;
        uint8  stallStop = 0;
        bool   stallReported = false;
        uint32 stopHoldSinceMs = 0;
        uint32 targetStopSinceMs = 0;
        uint32 idleSinceMs = 0;
        bool   reachedStand = false;
    };

    // Which stand point the party should hold, given where the leader is. The
    // party is always AHEAD of her: at the start (stop 0) it is already walking to
    // stand 1, and once she is at stop k it stands a few yards past stop k.
    //
    // Clamped at the top because stop 5 is WP18 — the end of the path, where there
    // is no wall and no offset and the party simply stands with her.
    inline uint8 TargetStopFor(uint8 leaderStop)
    {
        if (leaderStop < 1)
            return 1;
        return leaderStop > 5 ? 5 : leaderStop;
    }

    inline Verdict Decide(Inputs const& in)
    {
        Verdict v;
        v.stateSinceMs = in.stateSinceMs;
        v.stallStop = in.stallStop;
        v.stallReported = in.stallReported;
        v.stopHoldSinceMs = in.stopHoldSinceMs;
        v.targetStopSinceMs = in.targetStopSinceMs;
        v.idleSinceMs = in.idleSinceMs;
        v.reachedStand = in.reachedStand;
        v.targetStop = TargetStopFor(in.leaderStop);

        // --- 1. is the escape ours? ------------------------------------------
        //
        // Also treated as "not ours" when the leader is gone: the escape cannot
        // be completed without her (the walls open only for her channel and the
        // boss state only flips when the Lich King reaches WP17 with four walls
        // down), so a dead leader is a run wipe and the honest thing is to stop
        // claiming ticks the party could spend surviving.
        if (!in.active || !in.leaderAlive)
        {
            v.state = State::Done;
            v.complete = true;
            v.yieldTick = true;
            v.storeState = static_cast<uint8>(State::Done);
            v.stateSinceMs = 0;
            v.stallStop = 0;
            v.stallReported = false;
            v.stopHoldSinceMs = 0;
            v.targetStopSinceMs = 0;
            v.idleSinceMs = 0;
            v.reachedStand = false;
            return v;
        }

        // --- 2. the two rules that outrank everything ------------------------
        //
        // DOOMED FIRST, because it is not a state the driver can act in. Harvest
        // Soul is on her, summonsCount has been set to 255 so no wall can ever
        // open again, and Fury of Frostmourne lands in three seconds. Say so once
        // and yield; there is no positioning that survives it.
        if (in.leaderHasHarvestSoul)
        {
            v.state = State::Doomed;
            v.yieldTick = true;
            v.storeState = static_cast<uint8>(State::Doomed);
            if (in.state != v.storeState)
            {
                v.stateSinceMs = in.nowMs;
                v.reportDoomed = true;
            }
            return v;
        }

        // PRESSURE SECOND, and it OVERRIDES FIGHT. Both of its triggers cost more
        // per second than any add does: 7068 frost/s inside the ring, or 10 000
        // plus a knockback that makes the next check worse. The move is always
        // FORWARD to the stand point — never radially away from him, which for a
        // bot that is already behind is a step deeper into the zap rule.
        //
        // Gated on Winter, because that is what gates both of the encounter's own
        // rules: before he casts it and after the fourth wall removes it, standing
        // near him is merely pointless rather than fatal.
        bool const pressured = in.lkHasWinter &&
                               (in.distToLk < in.pressureDist ||
                                in.sumMinusLkSum > in.behindSum);

        // --- 3. WHICH STOP, and has it just changed? --------------------------
        //
        // Hoisted above the state choice because the threat-pickup gate below is
        // measured from the change. Re-armed per stop rather than per state,
        // because the party legitimately cycles Hold -> Fight -> Hold several
        // times at one wall.
        if (v.stallStop != v.targetStop)
        {
            v.stallStop = v.targetStop;
            v.stallReported = false;
            v.stopHoldSinceMs = 0;
            v.targetStopSinceMs = in.nowMs;
            // A NEW STOP IS ALWAYS A LEG. Whatever the party had reached belongs
            // to the wall it is leaving, so the crossing that follows ends on the
            // point rather than on the band's edge, and the idle clock restarts
            // rather than carrying a stale arm across 180 yards.
            v.reachedStand = false;
            v.idleSinceMs = 0;
        }

        // --- 4. which state ---------------------------------------------------
        //
        // AM I AT THE STAND POINT is a SCHMITT TRIGGER, not a comparison. Arriving
        // costs standLeash; leaving costs the wider standLeaveLeash, and only the
        // states that mean "holding ground here" latch the wide one. With a single
        // radius the tank oscillated across it every one to two seconds for the
        // whole of a wall fight, re-issuing a spline that cancelled its own melee
        // approach each time — see STAND_LEAVE_LEASH.
        //
        // Regroup and Engage HOLD GROUND TOO, and both must. Regroup ends inside
        // the wide band by construction (STAND_EDGE_MARGIN inside it), so reading
        // it against the narrow leash would leave atStand false at the very point
        // it just walked to and re-arm itself for ever; Engage deliberately walks
        // AWAY from the stand point, up to engageReach, and reading that against
        // the narrow leash would turn every pickup into a recall.
        bool const holdingGround = in.state == static_cast<uint8>(State::Hold) ||
                                   in.state == static_cast<uint8>(State::Fight) ||
                                   in.state == static_cast<uint8>(State::Recenter) ||
                                   in.state == static_cast<uint8>(State::Regroup) ||
                                   in.state == static_cast<uint8>(State::Engage);
        float const atStandWithin = holdingGround ? in.standLeaveLeash : in.standLeash;
        bool const atStand = in.distToStand <= atStandWithin;

        // ARRIVAL IS THE NARROW LEASH, ALWAYS, whatever the state. This is not
        // the "am I there" question the Schmitt trigger answers — it is the
        // one-way record of having genuinely made this stop, and it is what tells
        // a later drift-out from a leg that has not finished yet.
        if (in.distToStand <= in.standLeash)
            v.reachedStand = true;

        // PART OF THE BATCH IS LOOSE. The summons chase whoever holds them, so a
        // tank that walks to the next wall WITH them on it is doing the right
        // thing and one that walks off and leaves them on the healer is not.
        // Bounded by grabThreatMs from the stop change so a witch doctor that
        // never melees anyone cannot hold the party at a dead wall.
        uint32 const sinceStopChangeMs =
            in.nowMs >= v.targetStopSinceMs ? in.nowMs - v.targetStopSinceMs : 0;
        bool const batchLoose = in.addsAlive > in.addsTargetingMe;
        bool const grabbing =
            !atStand && batchLoose && in.grabThreatMs && sinceStopChangeMs < in.grabThreatMs;

        // DRIFTED FORWARD INTO A SHUT WALL. Yielding the fight to the combat
        // engine lets MoveChase walk the tank past the stand point and into the
        // slab; the summons are all BEHIND, so stepping back is also stepping
        // toward them.
        //
        // TWO THRESHOLDS, not one. Arming is the slack; CLEARING is
        // wallStandoffClear of the stand point's own standoff, so the state holds
        // through the whole step back instead of dropping out the moment the tank
        // is a yard better off and being re-armed by the next MoveChase.
        bool const wasRecentering = in.state == static_cast<uint8>(State::Recenter);
        float const clipArmAt = in.standWallDist - in.wallStandoffSlack;
        float const clipClearAt = in.standWallDist - in.wallStandoffClear;
        bool const clippingWall =
            atStand && !in.wallOpen && in.standWallDist > 0.0f &&
            in.wallDist < (wasRecentering ? clipClearAt : clipArmAt);

        // --- THE IDLE TANK ----------------------------------------------------
        //
        // A summon is up, none of them is on this bot, and the rotation has
        // nothing it can reach. Held on a clock so the sub-second gap between one
        // add dying and the next arriving is not a reason to walk anywhere, and
        // bounded on both sides so the walk is a pickup and never a trip back
        // down the path at the Lich King.
        bool const nothingOnMe = in.addsAlive > 0 && in.addsTargetingMe == 0;
        if (nothingOnMe)
        {
            if (!v.idleSinceMs)
                v.idleSinceMs = in.nowMs;
        }
        else
        {
            v.idleSinceMs = 0;
        }

        uint32 const idleForMs =
            v.idleSinceMs && in.nowMs >= v.idleSinceMs ? in.nowMs - v.idleSinceMs : 0;
        bool const engaging = atStand && nothingOnMe && in.engageIdleMs &&
                              idleForMs >= in.engageIdleMs &&
                              in.looseAddDist > in.engageMelee &&
                              in.looseAddDist <= in.engageReach;

        if (pressured)
            v.state = State::Pressure;
        else if (v.targetStop >= 5)
            v.state = State::Final;
        else if (grabbing)
            v.state = State::Threat;
        else if (!atStand)
            v.state = in.leaderStop < 1 ? State::Prelude
                      : v.reachedStand  ? State::Regroup
                                        : State::Advance;
        else if (clippingWall)
            v.state = State::Recenter;
        else if (engaging)
            v.state = State::Engage;
        else if (in.addsAlive > 0 || in.partyInCombat)
            v.state = State::Fight;
        else
            v.state = State::Hold;

        v.storeState = static_cast<uint8>(v.state);
        if (in.state != v.storeState)
            v.stateSinceMs = in.nowMs;

        // --- 5. the stall watchdog --------------------------------------------
        //
        // ONE LINE PER STOP, and only for the shape that is genuinely
        // unrecoverable from inside the driver: he is closing on her, the wall is
        // still shut, and the adds that hold it are not dying. On this map the
        // known cause is a Risen Witch Doctor parked inside the 10yd ring — it
        // casts from 20yd at the rearmost player, standing on HIS side, so melee
        // cannot finish it without eating the pulse and the pressure rule keeps
        // pulling them off it. Naming which adds are alive and where they stand is
        // what turns that from "the run stalled at wall 3" into a fix.
        //
        // The per-stop re-arm happens in section 3, above the state choice.
        bool const wallShut = !in.wallOpen && in.addsAlive > 0;
        if (wallShut && in.lkToLeaderDist > 0.0f && in.lkToLeaderDist <= in.stallDist)
        {
            if (!v.stopHoldSinceMs)
                v.stopHoldSinceMs = in.nowMs;

            uint32 const heldMs =
                in.nowMs >= v.stopHoldSinceMs ? in.nowMs - v.stopHoldSinceMs : 0;
            if (in.stallMs && heldMs >= in.stallMs && !v.stallReported)
            {
                v.reportStall = true;
                v.stallReported = true;
            }
        }
        else
        {
            v.stopHoldSinceMs = 0;
        }

        // --- 6. what to do about it -------------------------------------------
        switch (v.state)
        {
            case State::Recenter:
                // BACKWARDS, and this is the one state where that is right: the
                // stand point is behind the drifted tank and the summons are
                // behind it too, so the step off the wall is a step onto them.
                v.travel = true;
                return v;

            case State::Threat:
                // Stand still and swing. There is nothing to walk at — the batch
                // is coming to the party — and the whole point is to be the thing
                // it arrives on before the party moves off.
                v.yieldTick = true;
                return v;

            case State::Regroup:
                // THE NEAR EDGE, NOT THE CENTRE. The stand point is a leash and
                // not a parking space: every geometric rule this fight enforces
                // holds anywhere in the band, so a tank that chased a witch
                // doctor to the boundary is put back just inside it and returned
                // to the fight, rather than walked the whole way home and out
                // again. See DcHallsOfReflection::STAND_EDGE_MARGIN for the two
                // round trips per wall that cost.
                v.travel = true;
                v.travelToEdge = true;
                return v;

            case State::Engage:
                // The one place this driver walks the tank AT a hostile, and it
                // is not a pull: the fight is already running, the summon is
                // already on the party, and the only thing missing is twenty
                // yards the stock engine has no rung left to close. See
                // ENGAGE_REACH_YD.
                v.travel = true;
                v.travelToAdd = true;
                return v;

            case State::Pressure:
            case State::Prelude:
            case State::Advance:
                // Steer. Deliberately unconditional on the wall for Advance: if
                // the leader has moved on, the party is behind her, and every
                // second spent finishing the last ghoul at the OLD stop is a
                // second the next batch — which spawns at him 7.5s after the wall
                // opens and runs to the NEW stop — spends catching up anyway.
                v.travel = true;
                return v;

            case State::Final:
                // WP18 is 131yd from stop 4 and the leader takes ~19 seconds over
                // it. Winter is gone, so nothing is chasing; walk with her and let
                // the boss state end the event.
                v.travel = in.distToStand > in.standLeash;
                v.holdStill = !v.travel;
                return v;

            case State::Fight:
                // The adds run TO the party's stop and open on whoever is nearest
                // the Lich King. There is nothing to walk at and every claimed
                // tick is one the tank does not spend holding them off the healer.
                v.yieldTick = true;
                return v;

            case State::Hold:
            default:
                v.holdStill = true;
                return v;
        }
    }

    // ======================================================================
    // THE PER-FOLLOWER HALF — the same rule for the four bots the driver does
    // not steer.
    //
    // DungeonClearEventDueTrigger is leader-only, so the escape driver above
    // moves the TANK and nothing else. Everyone else spends the escape in the
    // stock combat engine, and the ONE thing the module told them was "you are
    // inside his ring, step forward" (DungeonClearHorStayAheadAction).
    //
    // THAT IS A PRESSURE-RELIEF VALVE, NOT A WAY OF GETTING ANYWHERE, and on
    // tp-20260908-000109-1 the difference cost all ten runs. When a wall opens
    // the leader runs 100-186yd to her next stop and the driver walks the tank
    // after her; the followers were never told, so they stayed on the adds at
    // the OLD wall while the Lich King walked into them. The valve then armed at
    // LK_PRESSURE_DIST, pushed them a few yards, and DISARMED — so MoveChase
    // pulled them straight back onto a witch doctor standing on his side of the
    // party, and they oscillated on the edge of the ring instead of travelling.
    //
    // Redas, tr-20260908-000117-1, the rung firing every 1.5-2s: 16.0 -> 13.2
    // -> 11.1 -> 15.1 -> 5.9 -> 14.6 -> 9.1yd from him over twenty seconds,
    // net displacement nil, while the tank crossed 136yd to stand 2 alone. The
    // three dps died 12-41s later to a Lumbering Abomination that was never
    // tanked, the healer 77s after that, and the bear met wall 4's nineteen
    // summons by itself. Ten runs, ten wipes, all at 5/6 bosses.
    //
    // SO THE FOLLOWERS GET THE SECOND REASON THE DRIVER ALREADY HAS: Advance.
    // Where the driver reads it off the leader's stop index, a follower reads it
    // off its OWN distance to the stand point that index picks — same point, so
    // the party converges rather than each bot solving a different problem.
    //
    // It is a LATCH and not a comparison, for the reason STAND_LEAVE_LEASH
    // exists at all: a bot that armed at 40yd and disarmed at 39 would hand the
    // tick back to MoveChase, which walks it back toward the add it left, which
    // re-arms the rung. Arming costs standLeaveLeash and clearing it costs
    // standLeash, so once a follower sets off it keeps the tick — and therefore
    // keeps its spline — for the whole leg.
    // ======================================================================

    struct FollowInputs
    {
        // The escape is running and he is alive. NOT gated on Remorseless
        // Winter: the pressure half is (his rules only exist while it is up),
        // but the advance half has to cover the last 131yd to WP18 as well,
        // which is run with Winter already gone.
        bool active = false;

        bool  lkHasWinter = false;
        float distToLk = 0.0f;
        float sumMinusLkSum = 0.0f;
        float pressureDist = 0.0f;   // LK_PRESSURE_DIST
        float behindSum = 0.0f;      // LK_BEHIND_SUM

        // This bot -> the stand point for the stop the LEADER picks, and the
        // same Schmitt pair the driver uses on the tank.
        float distToStand = 0.0f;
        float standLeash = 0.0f;
        float standLeaveLeash = 0.0f;

        // The stored advance latch (DcRunState::horFollowAdvancing).
        bool advancing = false;

        // THIS BOT IS THE ONE HOOK 35 STEERS (the dungeon-clear leader). The
        // driver already walks it to the same stand point through its own
        // re-issue floor, so the advance half must stand down or one bot carries
        // two of them. The pressure half is unaffected.
        bool driverSteered = false;
    };

    struct FollowVerdict
    {
        bool travel = false;     // TravelTo the stand point
        bool pressured = false;  // ...because he is on top of me / I am behind him
        bool advancing = false;  // ...because the party has moved on; latch to store
    };

    inline FollowVerdict DecideFollow(FollowInputs const& in)
    {
        FollowVerdict v;
        if (!in.active)
            return v;

        // The original rung, unchanged and still Winter-gated. Outranks nothing
        // and is outranked by nothing — either reason alone is enough to move,
        // and both want the same destination.
        v.pressured = in.lkHasWinter &&
                      (in.distToLk < in.pressureDist || in.sumMinusLkSum > in.behindSum);

        // THE LATCH. Set wide, cleared narrow; see the block comment above.
        // THE DRIVER OWNS ITS BOT'S LEGS: on the leader the latch is held clear
        // rather than merely unset, so a bot that was promoted to leader mid-leg
        // does not carry an armed latch into hook 35's territory.
        v.advancing = false;
        if (!in.driverSteered)
        {
            v.advancing = in.advancing;
            if (in.distToStand > in.standLeaveLeash)
                v.advancing = true;
            else if (in.distToStand <= in.standLeash)
                v.advancing = false;
        }

        // ALREADY THERE OUTRANKS BOTH. A follower inside the leash that still
        // reads pressured has had the Lich King walk up to IT — the ordinary
        // state of a wall that is taking a while — and there is nowhere better
        // to stand. Re-issuing a move to where the bot already is would only
        // tear down the melee approach the combat engine is running.
        if (in.distToStand <= in.standLeash)
            return v;

        v.travel = v.pressured || v.advancing;
        return v;
    }
}

#endif  // _PLAYERBOT_DCHORESCAPEDECISION_H
