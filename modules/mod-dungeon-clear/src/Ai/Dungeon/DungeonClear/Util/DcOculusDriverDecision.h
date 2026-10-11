/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCOCULUSDRIVERDECISION_H
#define _PLAYERBOT_DCOCULUSDRIVERDECISION_H

#include <algorithm>
#include <cstdint>

#include "Define.h"

#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"
#include "Ai/Dungeon/DungeonClear/Overrides/BossRosterRegistry.h"

// PURE kernels for The Oculus (map 578) — the party-level driver (hook 38) and
// the per-member rider rung — lifted out of the world so they can be tested
// without a map. The glue is Overrides/OculusDriver.cpp (the census, the plan
// memo, the telemetry) and Action/DcOculusRiderAction.cpp (the essence, the
// mount, the drake, the dismount).
//
// ONE DECISION, TWO HALVES. The driver answers "what is the PARTY doing": which
// site it is heading for (the next roster row's island), and whether it is
// mustering, flying, landing or fighting Eregos. That answer is the PLAN, and it
// is stored on the run owner's state and re-derived at most every PLAN_MEMO_MS by
// whichever member asks first. Each member's rider rung then answers "what do I
// do about it" for its own bot — every member steers its OWN drake, and a leader
// reaching into a follower's MotionMaster loses to that bot's own AI every tick.
//
// NO LATCH ANYWHERE. Every verdict is a pure function of live state — the four
// encounter slots, where the tank stands, who is mounted — plus three "since
// when" clocks. A wipe, a regroup or a rez simply reads as an earlier shape.
//
// THE RETURN CONTRACT is the module's, the Trial of the Champion's: Hold and
// Regroup claim the tick (Running), Yield hands it back (Done). Never Blocked.
namespace DcOculusDriver
{
    enum class Phase : uint8
    {
        Idle = 0,  // nothing for the riders to do
        Muster,    // get an essence, mount, wait on the ground for the quorum
        Fly,       // fly the leg to the destination; hover once there
        Land,      // dismount everyone whose drake has landed
        Eregos,    // station on Eregos (or fly to the Ring 4 stage first)
    };

    inline char const* PhaseName(Phase p)
    {
        switch (p)
        {
            case Phase::Muster: return "muster";
            case Phase::Fly:    return "fly";
            case Phase::Land:   return "land";
            case Phase::Eregos: return "eregos";
            case Phase::Idle:
            default:            return "idle";
        }
    }

    enum class State : uint8
    {
        Complete = 0,  // Eregos is dead and nobody is in the air
        NotDue,        // Drakos is alive — the portal and Drakos are the ordinary clear's
        Regroup,       // an unreachable corpse or a fallen member: revive at the portal landing
        Eregos,        // the Eregos fight, or the stage before it
        Down,          // the tank is dead
        OnSite,        // the tank is on foot on the destination island
        Fight,         // somebody is fighting on the ground
        Resting,       // the party is short; rest before a fresh mount
        Muster,        // essences and mounts, not yet enough riders
        Fly,           // in the air
        LandWait,      // the tank has landed; waiting for the riders behind it
        Land,          // everyone down
    };

    inline char const* StateName(State s)
    {
        switch (s)
        {
            case State::NotDue:   return "not due (Drakos alive)";
            case State::Regroup:  return "REGROUP — a member cannot be reached on foot";
            case State::Eregos:   return "Eregos";
            case State::Down:     return "tank down";
            case State::OnSite:   return "on site";
            case State::Fight:    return "fighting on the ground";
            case State::Resting:  return "resting before the muster";
            case State::Muster:   return "mustering the riders";
            case State::Fly:      return "flying";
            case State::LandWait: return "landed, waiting for the riders";
            case State::Land:     return "landing";
            case State::Complete:
            default:              return "complete";
        }
    }

    enum class Action : uint8
    {
        Yield,    // Done: hand the tick back
        Hold,     // Running: the riders are working; keep the ladder off the tank
        Regroup,  // Running: DcRezRecovery::RegroupAtEntrance (portal landing on this map)
    };

    struct Inputs
    {
        bool   drakosDone = false;
        bool   eregosDone = false;
        uint8  dest = DcOculus::SITE_NONE;  // SiteForRow(the next roster row)
        bool   destHover = false;           // Eregos alive: hover over the site, do not land

        bool   tankAlive = true;
        bool   tankMounted = false;
        // The island under the tank: on foot, where it stands; mounted, where its
        // drake hovers (SITE_NONE once it is in the air between islands).
        uint8  tankIsland = DcOculus::SITE_NONE;
        bool   tankLanded = false;   // mounted and DcOculusFlight::Landed on dest
        bool   tankAtStage = false;  // mounted and within STAGE_ARRIVE of dest's hover point

        uint32 partySize = 1;        // alive same-map members, tank included
        uint32 mounted = 0;          // ...on an Oculus drake
        uint32 landed = 0;           // ...of which Landed on dest

        bool   anyEngaged = false;   // DcCombatFlag engagement, anyone
        bool   eregosEngaged = false;
        bool   partyReady = true;    // the between-pulls rest gate
        bool   unreachableCorpse = false;  // a body off every mesh (mid-air, the basement)
        bool   fallen = false;       // a living member on foot off the mesh or in the basement

        uint32 nowMs = 0;
        uint32 musterSinceMs = 0;
        uint32 landWaitSinceMs = 0;
        uint32 lastRegroupMs = 0;
    };

    struct Verdict
    {
        State  state = State::Complete;
        Action action = Action::Yield;
        Phase  phase = Phase::Idle;
        uint8  dest = DcOculus::SITE_NONE;
        bool   hover = false;
        bool   complete = false;
        bool   musterTimedOut = false;

        uint32 musterSinceMs = 0;
        uint32 landWaitSinceMs = 0;

        bool Claims() const { return action != Action::Yield; }
    };

    // MUSTER_QUORUM riders, or everyone alive if the party is smaller.
    inline uint32 MusterQuorum(uint32 partySize)
    {
        return std::max<uint32>(1, std::min<uint32>(DcOculus::MUSTER_QUORUM, partySize));
    }

    // Which island a roster row is fought on. The portal and Drakos are on foot
    // and the ordinary clear's: SITE_NONE.
    inline uint8 SiteForRow(uint32 entry)
    {
        using namespace DcOculus;
        uint32 (*const obj)(uint32) = [](uint32 seq) { return BossRosterRegistry::ObjectiveEntry(seq); };
        if (entry == obj(EVENT_R2C))
            return SITE_R2C;
        if (entry == obj(EVENT_R2S))
            return SITE_R2S;
        if (entry == obj(EVENT_R2N))
            return SITE_R2N;
        if (entry == NPC_VAROS)
            return SITE_R2V;
        if (entry == obj(EVENT_UROM_P0))
            return SITE_R3P0;
        if (entry == obj(EVENT_UROM_P1))
            return SITE_R3P1;
        if (entry == obj(EVENT_UROM_P2))
            return SITE_R3P2;
        if (entry == NPC_UROM)
            return SITE_R3IN;
        if (entry == obj(EVENT_EREGOS))
            return SITE_R4;
        return SITE_NONE;
    }

    inline Verdict Decide(Inputs const& in)
    {
        using namespace DcOculus;

        Verdict v;
        auto set = [&v](State s, Action a, Phase p)
        {
            v.state = s;
            v.action = a;
            v.phase = p;
        };
        auto since = [&in](uint32 was) { return was ? was : (in.nowMs ? in.nowMs : 1); };

        if (!in.drakosDone)
        {
            set(State::NotDue, Action::Yield, Phase::Idle);
            return v;
        }

        // Nothing left to reach and nobody in the air: done. Somebody still in the
        // air with no row left (Eregos just died) is flown down onto the Ring 4 floor.
        if (in.dest == SITE_NONE && in.mounted == 0)
        {
            set(State::Complete, Action::Yield, Phase::Idle);
            v.complete = in.eregosDone;
            return v;
        }
        uint8 const dest = in.dest == SITE_NONE ? SITE_R4 : in.dest;
        bool const hover = in.destHover && in.dest != SITE_NONE;
        v.dest = dest;
        v.hover = hover;

        // --- the one exit from a body nobody can reach ----------------------------
        //
        // A corpse in mid-air (the Eregos fight, a picket in transit) or a member
        // standing off the mesh has no walk that reaches it and no rez that lands.
        // Only with nobody mounted and nothing fighting: a regroup mid-leg would
        // pull the riders out of their saddles.
        if ((in.unreachableCorpse || in.fallen) && in.mounted == 0 && !in.anyEngaged && !in.eregosEngaged &&
            (!in.lastRegroupMs || in.nowMs - in.lastRegroupMs >= REGROUP_COOLDOWN_MS))
        {
            set(State::Regroup, Action::Regroup, Phase::Idle);
            return v;
        }

        // --- Eregos: the fight is in the air, whoever is left in the saddle ------
        if (in.eregosEngaged && in.mounted > 0)
        {
            set(State::Eregos, Action::Hold, Phase::Eregos);
            return v;
        }
        if (hover && in.tankAlive && in.tankMounted && in.tankAtStage)
        {
            set(State::Eregos, Action::Hold, Phase::Eregos);
            return v;
        }

        if (!in.tankAlive)
        {
            // The riders already in the air finish the leg and land on their own —
            // Land, not Fly, because a follower normally steps off only once the
            // tank is on foot at the site, and this tank never will be. Nobody
            // starts a new leg without it.
            set(State::Down, Action::Yield,
                in.mounted > 0 ? (hover ? Phase::Eregos : Phase::Land) : Phase::Idle);
            return v;
        }

        // --- the tank is on foot ---------------------------------------------------
        if (!in.tankMounted)
        {
            if (in.tankIsland == dest && !hover)
            {
                // Arrived. The objective's own steps (or the at-boss pipeline) take
                // it from here; any rider still in the air comes down.
                set(State::OnSite, Action::Yield, in.mounted > 0 ? Phase::Land : Phase::Idle);
                return v;
            }
            if (in.anyEngaged)
            {
                // NEVER LIFT OFF IN COMBAT: a drake spell on a ground mob kills its
                // rider. Fight it out on foot; riders already up hold the saddle.
                set(State::Fight, Action::Yield, in.mounted > 0 ? Phase::Muster : Phase::Idle);
                if (in.mounted > 0)
                    v.musterSinceMs = in.musterSinceMs;
                return v;
            }
            if (!in.partyReady && in.mounted == 0)
            {
                // A fresh mount is wasted while the party drinks.
                set(State::Resting, Action::Yield, Phase::Idle);
                return v;
            }
            set(State::Muster, Action::Hold, Phase::Muster);
            v.musterSinceMs = since(in.musterSinceMs);
            return v;
        }

        // --- the tank is mounted -----------------------------------------------------
        //
        // "Lifted" needs no latch: the tank only leaves its island once the quorum
        // is up, so a tank in the air (or already over the destination) got there
        // through this gate.
        bool const lifted = in.tankIsland == SITE_NONE || in.tankIsland == dest;
        if (!lifted)
        {
            uint32 const start = since(in.musterSinceMs);
            v.musterSinceMs = start;
            if (in.anyEngaged)
            {
                set(State::Fight, Action::Hold, Phase::Muster);
                return v;
            }
            bool const quorum = in.mounted >= MusterQuorum(in.partySize);
            bool const timedOut = in.nowMs - start >= MUSTER_TIMEOUT_MS;
            v.musterTimedOut = timedOut && !quorum;
            bool const enough =
                quorum || (timedOut && in.mounted >= std::min<uint32>(MUSTER_MIN_ON_TIMEOUT, in.partySize));
            set(enough ? State::Fly : State::Muster, Action::Hold, enough ? Phase::Fly : Phase::Muster);
            return v;
        }

        if (hover || !in.tankLanded)
        {
            set(State::Fly, Action::Hold, Phase::Fly);
            return v;
        }

        // Landed. Stay in the saddle until every rider behind has landed too, or
        // LAND_WAIT_MS — then everyone steps off together. Under fire nobody waits
        // up there: a saddle has no answer to a ground mob, so the landed step off
        // and fight on foot while the rest come down.
        uint32 const wait = since(in.landWaitSinceMs);
        v.landWaitSinceMs = wait;
        if (in.landed >= in.mounted || in.anyEngaged || in.nowMs - wait >= LAND_WAIT_MS)
        {
            set(State::Land, Action::Hold, Phase::Land);
            return v;
        }
        set(State::LandWait, Action::Hold, Phase::Fly);
        return v;
    }
}

namespace DcOculusRider
{
    enum class Action : uint8
    {
        None,              // nothing for this rung: the tick falls through
        WalkToGiver,       // walk to this member's colour's giver
        WaitForGiver,      // at (or near) a giver still walking out of its cage
        Gossip,            // SelectGossip for the essence
        FabricateEssence,  // off Drakos's ring without an essence: hand it over
        LeaveMount,        // off the bot's own ground/flying mount first — the Call is refused from one
        Mount,             // use the essence (the Call spell) and start the settle
        SettleHold,        // hold still while the drake comes down and the saddle lands
        Fly,               // fly the leg's next waypoint
        Reissue,           // the leg stalled: re-issue the waypoint once
        Nudge,             // still stalled and not over mesh: climb and re-plan
        FlyPadCentre,      // the lane landing will not pass: land on the pad centre
        Dismount,          // step off — only ever over snapped mesh
        Station,           // hold station on Eregos
        PullEregos,        // the tank opens on Eregos
    };

    inline char const* ActionName(Action a)
    {
        switch (a)
        {
            case Action::WalkToGiver:      return "walk to giver";
            case Action::WaitForGiver:     return "wait for giver";
            case Action::Gossip:           return "gossip";
            case Action::FabricateEssence: return "fabricate essence";
            case Action::LeaveMount:       return "leave own mount";
            case Action::Mount:            return "mount";
            case Action::SettleHold:       return "settle";
            case Action::Fly:              return "fly";
            case Action::Reissue:          return "reissue";
            case Action::Nudge:            return "nudge";
            case Action::FlyPadCentre:     return "fly to pad centre";
            case Action::Dismount:         return "dismount";
            case Action::Station:          return "station";
            case Action::PullEregos:       return "pull eregos";
            case Action::None:
            default:                       return "none";
        }
    }

    struct Inputs
    {
        DcOculusDriver::Phase phase = DcOculusDriver::Phase::Idle;
        uint8  dest = DcOculus::SITE_NONE;
        bool   planFresh = false;
        bool   isTank = false;
        bool   alive = true;
        bool   engaged = false;      // this member, on foot

        // --- on foot -------------------------------------------------------------
        bool   onVehicle = false;
        bool   onMount = false;      // the bot's OWN mount (IsMounted), not a drake

        // --- the playerbots MASTER ----------------------------------------------------
        //
        // Stock `wotlk-occ` keys three triggers and a multiplier on the bot's master
        // riding a drake. Its MountingDrakeMultiplier zeroes EVERY action but stock
        // `mount drake` (this rung included) on a bot that is on foot while its master
        // rides — so a member must never be on foot while its master is in a saddle.
        // The master mounts LAST and steps off FIRST. (A `.dc test` master is the
        // off-map GM: both fields stay false and nothing here changes.)
        bool   masterMounted = false;  // this bot's master is another member, on a drake
        bool   mastersOthers = false;  // this bot is another member's master
        uint32 othersAlive = 0;        // alive same-map members other than this bot
        uint32 othersMounted = 0;      // ...of which on a drake
        uint32 masterWaitSinceMs = 0;  // this master has held its own mount since (round-tripped)
        uint8  island = DcOculus::SITE_NONE;
        bool   hasEssence = false;   // this member's colour
        bool   essenceOnCooldown = false;
        bool   giverPresent = false;
        bool   giverFlagged = false;
        float  giverDist = -1.0f;
        float  postDist = -1.0f;         // to where this colour's giver stands once freed (ColourRow::post*)
        uint32 essenceWaitSinceMs = 0;   // on Drakos's ring without an essence since (round-tripped)
        uint32 nowMs = 0;
        uint32 mountIssuedMs = 0;

        // --- in the saddle -----------------------------------------------------------
        bool   onOcDrake = false;
        bool   seatCanControl = false;
        bool   baseInCombat = false;   // the drake has a victim or attackers
        bool   groundAttacker = false; // ...one that cannot fly (DcFlightLeg::HasGroundAttacker)
        bool   channeling = false;     // Temporal Rift / Dream Funnel — a move would cancel it
        bool   legArrived = false;
        bool   legStalled = false;
        bool   stallReissued = false;
        bool   landed = false;         // DcOculusFlight::Landed on dest
        bool   atPadCentre = false;    // the leg already targets lane 0
        uint32 arrivedSinceMs = 0;
        bool   tankOnFootOnDest = false;

        // --- Eregos --------------------------------------------------------------------
        bool   eregosPresent = false;
        bool   eregosAttackable = false;
        bool   eregosEngaged = false;
        bool   nearEregos = false;     // within EREGOS_ENGAGE_RANGE: station chords are open air
        float  stationError = 0.0f;    // drake -> its station
    };

    struct Verdict
    {
        Action action = Action::None;
        uint32 arrivedSinceMs = 0;
        uint32 masterWaitSinceMs = 0;
        uint32 essenceWaitSinceMs = 0;

        bool Claims() const { return action != Action::None; }
    };

    // A PICKET found the drake mid-leg: hover and let `occ drake attack` shoot it
    // down. Only a picket, and only in the air on the way: a ground mob on a drake
    // has no answer from the saddle (a drake spell on one kills its rider), so that
    // drake keeps flying its leg down and its rider steps off to fight on foot.
    // Live, the central ring: Ring-Lords found the drakes over the pad, every
    // attacked drake hovered where it was, and nobody landed for a minute.
    inline bool HoldsForPicket(Inputs const& in)
    {
        return in.baseInCombat && !in.groundAttacker && !in.landed && in.phase == DcOculusDriver::Phase::Fly;
    }

    inline Verdict Decide(Inputs const& in)
    {
        using namespace DcOculus;
        using Phase = DcOculusDriver::Phase;

        Verdict v;
        if (!in.alive)
            return v;

        // --- on foot: essence, mount, settle -----------------------------------------
        if (!in.onVehicle)
        {
            if (!in.planFresh)
                return v;
            bool const wantsDrake =
                in.phase == Phase::Muster || in.phase == Phase::Fly || in.phase == Phase::Eregos;
            if (!wantsDrake)
                return v;
            if (in.phase != Phase::Eregos && in.island != SITE_NONE && in.island == in.dest)
                return v;  // already standing where the party is going
            if (in.mountIssuedMs && in.nowMs - in.mountIssuedMs < MOUNT_SETTLE_MS)
            {
                // Before the combat test: the Call cast is already out, and moving
                // now burns the cooldown with no drake. A bot that got back onto its
                // own mount in the window loses the saddle cast the same way.
                v.action = in.onMount ? Action::LeaveMount : Action::SettleHold;
                return v;
            }
            if (in.engaged)
                return v;  // never mount into a fight

            if (!in.hasEssence)
            {
                if (in.island == SITE_R1_GIVERS)
                {
                    // BOUNDED. The on-foot tank's muster has no timeout, so a giver
                    // that never answers would hold the party for ever: past
                    // GIVER_WAIT_MS the essence is handed over as it is off the ring.
                    v.essenceWaitSinceMs =
                        in.essenceWaitSinceMs ? in.essenceWaitSinceMs : (in.nowMs ? in.nowMs : 1);
                    if (FABRICATE_ESSENCE_OFF_RING && in.nowMs - v.essenceWaitSinceMs >= GIVER_WAIT_MS)
                    {
                        v.action = Action::FabricateEssence;
                        return v;
                    }
                    if (!in.giverPresent)
                    {
                        // Out of scan: walk to where it stands once freed. Live
                        // tr-20260913-003200-3, the tank waited 63.9yd from
                        // Belgaristrasz — just past GIVER_SCAN — and never moved.
                        v.action = in.postDist > GIVER_REACH ? Action::WalkToGiver : Action::WaitForGiver;
                        return v;
                    }
                    if (in.giverDist < 0.0f || in.giverDist > GIVER_REACH)
                    {
                        v.action = Action::WalkToGiver;
                        return v;
                    }
                    v.action = in.giverFlagged ? Action::Gossip : Action::WaitForGiver;
                    return v;
                }
                if (FABRICATE_ESSENCE_OFF_RING)
                    v.action = Action::FabricateEssence;
                return v;
            }

            if (in.essenceOnCooldown)
                return v;
            // THE MASTER MOUNTS LAST: every member it masters that is still on foot
            // when it takes a saddle is frozen by stock MountingDrakeMultiplier. Bounded
            // by MASTER_MOUNT_WAIT_MS, so one member that cannot mount never holds the
            // muster (the driver's own timeout lifts off at four).
            if (in.mastersOthers && in.othersMounted < in.othersAlive)
            {
                v.masterWaitSinceMs = in.masterWaitSinceMs ? in.masterWaitSinceMs : (in.nowMs ? in.nowMs : 1);
                if (in.nowMs - v.masterWaitSinceMs < MASTER_MOUNT_WAIT_MS)
                    return v;
            }
            // THE BOT'S OWN MOUNT FIRST. The map allows mounts, stock `check mount
            // state` puts every bot on one between fights, and the essence's Call
            // spell is refused from the saddle of a normal mount (observed live: no
            // drake ever came). One tick to step off, the cast on the next.
            v.action = in.onMount ? Action::LeaveMount : Action::Mount;
            return v;
        }

        if (!in.onOcDrake || !in.seatCanControl)
            return v;

        // --- Eregos ------------------------------------------------------------------------
        //
        // An engaged Eregos is answered from the saddle whatever the plan says: the
        // plan can be a tick stale, and a rider hovering mid-leg in the middle of
        // the fight is the one that eats Arcane Barrage untended.
        if (in.eregosEngaged || in.phase == Phase::Eregos)
        {
            if (in.eregosPresent && in.nearEregos)
            {
                if (in.channeling)
                    return v;
                if (!in.eregosEngaged && in.isTank && in.eregosAttackable)
                {
                    v.action = Action::PullEregos;
                    return v;
                }
                if (in.stationError > EREGOS_RESTATION)
                    v.action = Action::Station;
                return v;
            }
            if (!in.legArrived)
                v.action = Action::Fly;  // up to the stage first; stations start from open air
            return v;
        }

        if (!in.planFresh)
            return v;
        if (in.phase != Phase::Fly && in.phase != Phase::Land)
            return v;  // Muster / Idle: sit in the saddle
        if (HoldsForPicket(in))
            return v;

        // ...and the master steps off FIRST: a member on foot under a mounted master
        // has every action zeroed by stock MountingDrakeMultiplier until it lands too.
        // A landed drake under attack steps off at once, Land phase or not: the
        // saddle has no answer to a ground mob, and every tick up there is damage.
        bool const mayStepOff =
            !in.masterMounted && (in.phase == Phase::Land || (!in.isTank && in.tankOnFootOnDest) ||
                                  (in.landed && in.baseInCombat));

        if (!in.legArrived)
        {
            if (in.legStalled)
            {
                if (!in.stallReissued)
                {
                    v.action = Action::Reissue;
                    return v;
                }
                if (in.landed && mayStepOff)
                {
                    v.action = Action::Dismount;
                    return v;
                }
                // Never dismount in the air; a wedged drake climbs and re-plans.
                v.action = in.landed ? Action::None : Action::Nudge;
                return v;
            }
            v.action = Action::Fly;
            return v;
        }

        v.arrivedSinceMs = in.arrivedSinceMs ? in.arrivedSinceMs : (in.nowMs ? in.nowMs : 1);
        if (in.landed)
        {
            if (mayStepOff)
                v.action = Action::Dismount;
            return v;
        }
        if (!in.atPadCentre && in.nowMs - v.arrivedSinceMs >= PAD_FALLBACK_MS)
            v.action = Action::FlyPadCentre;
        return v;
    }
}

#endif  // _PLAYERBOT_DCOCULUSDRIVERDECISION_H
