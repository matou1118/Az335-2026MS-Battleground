/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCTOCDRIVERDECISION_H
#define _PLAYERBOT_DCTOCDRIVERDECISION_H

#include <algorithm>
#include <cstdint>

#include "Define.h"

#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"

// PURE kernel for the Trial of the Champion (map 650) driver — everything hook 37
// decides each tick, lifted out of the world so it can be tested without a map.
// The glue (the censuses, the moves, the gossip, the force-pull, the telemetry)
// lives in Overrides/TrialOfTheChampionDriver.cpp.
//
// WHAT THE DRIVER IS FOR. The three objectives only HOLD — on progress 6, 8 and
// 9 — and nothing in the ordinary clear knows how to make the counter move. Three
// things do, and they are this driver's whole job:
//
//   * THE CLICKS. The announcer starts each phase, at progress 0, 6 and 8, and
//     re-offers his gossip after every full-wipe rewind. At 0 the clicker must be
//     MOUNTED, and the option is the short version.
//   * THE MUSTER. The joust is five riders or it is a wipe, so the click at 0
//     waits for the party to lance and mount (mod-playerbots does that, above
//     every rung this module owns) — for a quorum, and for a bounded time.
//   * THE SIDE PACKS. Nine Argent soldiers activate with DoZoneInCombat
//     commented out, so the two side packs 30yd apart never come on their own.
//
// AND ONE THING IT MUST STOP: this module's movement touching a mounted tank.
// Nothing in DC's route machinery knows about vehicles — the escort glide splines
// the BOT, which on a horse is the passenger — so for the whole joust (0-4) the
// driver never yields out of combat. It holds, or it rides the horse itself.
// That costs mod-playerbots nothing: `toc lance` (65), `toc mount` (64) and
// `toc mounted` (66) outrank every DC rung, so they still get every tick they
// want; a Hold here only stops the DC ladder under them from taking the ticks
// they decline (a stock MoveTo returns false while its last move is in flight,
// which is exactly when a DC mover would fight it).
//
// THE COUNTER GOES BACKWARDS, and that is why this is a pure function of live
// state with no latch anywhere. A full wipe at 1-4 rewinds to 0 (re-lance,
// re-mount, re-click) and at 7 to 6 (re-click); 5 and 8 stay. Every verdict below
// is re-derived from the counter and the census each tick, so a rewind simply
// reads as the earlier phase and the earlier answer comes back.
//
// THE RETURN CONTRACT is the module's: Yield is "Done" (hand the tick back),
// everything else is "Running" (claim it). Blocked is never returned.
namespace DcTocDriver
{
    enum class State : uint8
    {
        Complete = 0,    // progress 9 — the Knight is dead
        Down,            // the leader is dead
        Mounting,        // progress 0-4 without a horse — mod-playerbots mounts it
        Mustering,       // progress 0, mounted, waiting for the riders
        AwaitGossip,     // the announcer is absent or his gossip flag is down
        Approach,        // closing on the announcer
        Click,           // selecting his gossip
        Joust,           // progress 1-4 in combat — mod-playerbots' fight
        TrampleCover,    // progress 1-4, a champion walking to a horse
        Regroup,         // progress 1-4, mounted, riding back to the joust post
        BetweenWaves,    // progress 1-4, mounted, at the post
        OnFoot,          // progress 5 — the champions on foot
        Resting,         // the party is short and nothing is urgent
        ArgentIntro,     // soldiers up, not yet attackable (12.5s)
        ArgentPull,      // an idle attackable soldier — pull it
        ArgentFight,     // phase 2, something engaged
        ArgentBoss,      // progress 7 — Eadric / Paletress walking in or fighting
        ArgentRepull,    // progress 7, the boss evaded after a partial wipe
        KnightIntro,     // the Black Knight's ~50s of speeches
        KnightFight,
        KnightStranded,  // he evaded (= despawned) with the announcer already dead
    };

    inline char const* StateName(State s)
    {
        switch (s)
        {
            case State::Down:           return "leader down";
            case State::Mounting:       return "waiting for mod-playerbots to lance and mount the tank";
            case State::Mustering:      return "mustering the riders";
            case State::AwaitGossip:    return "waiting for the announcer's gossip";
            case State::Approach:       return "closing on the announcer";
            case State::Click:          return "talking to the announcer";
            case State::Joust:          return "jousting";
            case State::TrampleCover:   return "a champion is walking to a horse — trample cover";
            case State::Regroup:        return "riding back to the joust post";
            case State::BetweenWaves:   return "between waves";
            case State::OnFoot:         return "the champions on foot";
            case State::Resting:        return "resting";
            case State::ArgentIntro:    return "the Argent soldiers' intro";
            case State::ArgentPull:     return "pulling an Argent soldier pack";
            case State::ArgentFight:    return "fighting the Argent soldiers";
            case State::ArgentBoss:     return "the Argent champion";
            case State::ArgentRepull:   return "re-pulling the evaded Argent champion";
            case State::KnightIntro:    return "the Black Knight's intro";
            case State::KnightFight:    return "the Black Knight";
            case State::KnightStranded: return "STRANDED — the Black Knight evaded after killing the announcer";
            case State::Complete:
            default:                    return "complete";
        }
    }

    enum class Action : uint8
    {
        Yield,              // Done: hand the tick back
        Hold,               // Running: issue nothing
        ApproachAnnouncer,  // point-move to GOSSIP_STANDOFF of the announcer
        ReturnToPost,       // point-move the (mounted) tank to the joust post
        Gossip,             // SelectGossip(announcer, gossipOption)
        EngageSoldier,      // force-pull the nearest idle attackable soldier
        EngageBoss,         // force-pull the idle Argent champion
    };

    struct Inputs
    {
        uint32 progress = 0;       // GetData(DATA_INSTANCE_PROGRESS)
        bool   alive = true;       // the leader itself
        bool   partyInCombat = false;
        // The between-pulls rest gate (Smart Rest's latch, or the legacy floors).
        bool   partyReady = true;

        // --- the joust ---------------------------------------------------------
        bool   tankMounted = false;    // on a 35644 / 36558, either entry
        uint32 mountedMembers = 0;     // alive same-map members on a horse, tank included
        uint32 partySize = 1;          // alive same-map members, tank included
        uint32 walkingChampions = 0;   // dismounted, NON_ATTACKABLE, alive
        float  distToPost = 0.0f;      // tank -> the joust post (OBJ(1)'s anchor)

        // --- the announcer (GetGuidData(DATA_ANNOUNCER)) ------------------------
        bool   announcerPresent = false;  // resolvable and alive
        bool   announcerFlagged = false;  // UNIT_NPC_FLAG_GOSSIP
        float  announcerDist = -1.0f;

        // --- the Argent Challenge ------------------------------------------------
        uint32 soldiersAlive = 0;
        uint32 soldiersAttackable = 0;    // neither NON_ATTACKABLE nor IMMUNE_TO_PC
        bool   soldiersEngaged = false;   // any soldier in combat
        bool   argentBossPresent = false;
        bool   argentBossAttackable = false;
        bool   argentBossEngaged = false;

        // --- the Black Knight ------------------------------------------------------
        bool   knightPresent = false;
        bool   knightAttackable = false;

        // --- clocks (stored on DcRunState, round-tripped through the verdict) -----
        uint32 nowMs = 0;
        uint32 mountedSinceMs = 0;    // when the tank was first seen mounted (0 = not)
        uint32 unmountedSinceMs = 0;  // progress 0-4 and the tank on foot since (0 = not)
        uint32 strandSinceMs = 0;     // progress 8, no Knight and no announcer, since
    };

    struct Verdict
    {
        State  state = State::Complete;
        Action action = Action::Yield;
        int32  gossipOption = -1;   // only meaningful on Action::Gossip
        bool   complete = false;

        uint32 mountedSinceMs = 0;
        uint32 unmountedSinceMs = 0;
        uint32 strandSinceMs = 0;

        // Diagnostics the glue logs (throttled): the tank has been on foot through
        // MOUNT_WARN_MS of the joust, and the Knight strand has outlived its grace.
        bool   neverMounted = false;
        bool   stranded = false;

        bool Claims() const { return action != Action::Yield; }
    };

    // Does follow-tank stand down for this FOLLOWER? True for a living member still
    // on foot through the joust (progress 0-4): mod-playerbots' `toc lance` and
    // `toc mount` walk it to a rack and a horse, and they own its movement until it
    // is in the saddle.
    //
    // They cannot hold that ownership on their own. Relevance only buys a tick
    // while the action SUCCEEDS, and a stock MoveTo returns false on every tick
    // its last move is still in flight — so each of those ticks fell through to
    // follow-tank (25), which re-installed MoveFollow and turned the bot back to
    // the tank; the wait expired, `toc mount` re-aimed at the horse, and round it
    // went. tr-20260910-223555-1: the two DPS whose horse was farther from the tank
    // than follow distance never mounted (386 and 385 failed mount ticks, 28 and
    // 49 follow-tank moves), while the three with a horse at the tank's side did.
    inline bool FollowerMountsItself(uint32 mapId, uint32 progress, bool alive, bool onVehicle)
    {
        return mapId == DcTrialOfTheChampion::MAP_ID && alive && !onVehicle &&
               progress <= DcTrialOfTheChampion::PROGRESS_GROUP_DIED_3;
    }

    // The muster quorum: MUSTER_QUORUM riders, or every living member if the party
    // is smaller than that. Never zero — the tank counts itself.
    inline uint32 MusterQuorum(uint32 partySize)
    {
        return std::max<uint32>(1, std::min<uint32>(DcTrialOfTheChampion::MUSTER_QUORUM, partySize));
    }

    inline Verdict Decide(Inputs const& in)
    {
        using namespace DcTrialOfTheChampion;

        Verdict v;

        auto yield = [&v](State s) { v.state = s; v.action = Action::Yield; };
        auto hold  = [&v](State s) { v.state = s; v.action = Action::Hold; };

        if (in.progress >= PROGRESS_FINISHED)
        {
            yield(State::Complete);
            v.complete = true;
            return v;
        }

        // --- the clocks ------------------------------------------------------------
        //
        // Each is "since when has this shape held", zeroed the moment it stops
        // holding, so a rewind or a remount always measures from the current
        // episode and never from an earlier one.
        bool const joustPhase = in.progress <= PROGRESS_GROUP_DIED_3;

        v.mountedSinceMs = in.tankMounted ? (in.mountedSinceMs ? in.mountedSinceMs : in.nowMs) : 0;

        bool const onFoot = joustPhase && in.alive && !in.tankMounted;
        v.unmountedSinceMs = onFoot ? (in.unmountedSinceMs ? in.unmountedSinceMs : in.nowMs) : 0;
        v.neverMounted = v.unmountedSinceMs &&
                         static_cast<uint32>(in.nowMs - v.unmountedSinceMs) >= MOUNT_WARN_MS;

        bool const knightGone = in.progress == PROGRESS_ARGENT_CHALLENGE_DIED &&
                                !in.knightPresent && !in.announcerPresent;
        v.strandSinceMs = knightGone ? (in.strandSinceMs ? in.strandSinceMs : in.nowMs) : 0;
        v.stranded = v.strandSinceMs &&
                     static_cast<uint32>(in.nowMs - v.strandSinceMs) >= STRAND_GRACE_MS;

        if (!in.alive)
        {
            yield(State::Down);
            return v;
        }

        // THE CLICK, shared by all three phases. Every Gossip verdict passes through
        // here, and so every one requires the flag AND the reach: clicking a
        // flagless NPC is a silent no-op for ever (the Violet Hold lesson), and a
        // click from outside interaction range is refused just as silently.
        auto clickAnnouncer = [&](int32 option) -> bool
        {
            if (!in.announcerPresent || !in.announcerFlagged)
                return false;
            if (in.announcerDist < 0.0f || in.announcerDist > GOSSIP_REACH)
            {
                v.state = State::Approach;
                v.action = Action::ApproachAnnouncer;
                return true;
            }
            v.state = State::Click;
            v.action = Action::Gossip;
            v.gossipOption = option;
            return true;
        };

        // --- 0: lance, mount, muster, click ------------------------------------------
        if (in.progress == PROGRESS_INITIAL)
        {
            if (in.partyInCombat)
            {
                yield(State::Joust);
                return v;
            }

            // HOLD, NOT YIELD. mod-playerbots' `toc lance` (65) and `toc mount` (64)
            // are what mount the tank, and they outrank this rung, so they run
            // whenever they fire regardless of what this returns. Yielding would
            // only hand the ticks they decline to Advance, which walks the tank
            // toward OBJ(1) underneath their walk to the rack and the horse.
            if (!in.tankMounted)
            {
                hold(State::Mounting);
                return v;
            }

            if (in.mountedMembers < MusterQuorum(in.partySize) &&
                static_cast<uint32>(in.nowMs - v.mountedSinceMs) < MUSTER_TIMEOUT_MS)
            {
                hold(State::Mustering);
                return v;
            }

            if (clickAnnouncer(GOSSIP_OPTION_SHORT_JOUST))
                return v;

            // The announcer is re-homing after a cleanup, or his flag is down for
            // the instant between the click and the counter moving. Stay mounted
            // and still.
            hold(State::AwaitGossip);
            return v;
        }

        // --- 1-4: the joust -------------------------------------------------------------
        if (joustPhase)
        {
            if (in.partyInCombat)
            {
                yield(State::Joust);
                return v;
            }

            // A champion is walking to a spare horse. `toc mounted` tramples him from
            // BOTH engines and outranks this; holding keeps DC's ladder from riding
            // the tank off him on a tick `toc mounted` declines.
            if (in.walkingChampions > 0)
            {
                hold(State::TrampleCover);
                return v;
            }

            // Horse dead: `toc mount` (64) remounts. Hold for the Mounting reason.
            if (!in.tankMounted)
            {
                hold(State::Mounting);
                return v;
            }

            if (in.distToPost > POST_RADIUS)
            {
                v.state = State::Regroup;
                v.action = Action::ReturnToPost;
                return v;
            }

            hold(State::BetweenWaves);
            return v;
        }

        // --- 5: the champions on foot ---------------------------------------------------
        //
        // The horses are gone for good, the champions are summoned full-HP and
        // aggressive, and `toc ue lance` drops the lance. An ordinary fight.
        if (in.progress == PROGRESS_CHAMPIONS_UNMOUNTED)
        {
            yield(State::OnFoot);
            return v;
        }

        // --- 6: the Argent soldiers -----------------------------------------------------
        if (in.progress == PROGRESS_CHAMPIONS_DEAD)
        {
            if (in.partyInCombat || in.soldiersEngaged)
            {
                yield(State::ArgentFight);
                return v;
            }

            if (in.soldiersAlive > 0)
            {
                // The 12.5s intro: summoned, escorted to their slots, NON_ATTACKABLE.
                if (in.soldiersAttackable == 0)
                {
                    hold(State::ArgentIntro);
                    return v;
                }
                if (!in.partyReady)
                {
                    yield(State::Resting);
                    return v;
                }
                // No DoZoneInCombat and packs 30yd apart: whatever is left idle has
                // to be pulled, one pack at a time, nearest first.
                v.state = State::ArgentPull;
                v.action = Action::EngageSoldier;
                return v;
            }

            // No soldiers: the phase has not been started. Rest first — the anchored
            // hold yields to NeedsRest — then click.
            if (!in.partyReady)
            {
                yield(State::Resting);
                return v;
            }
            if (clickAnnouncer(GOSSIP_OPTION_NEXT_PHASE))
                return v;

            // His flag comes back 15s after the champions' last "death". Yield the
            // wait: OBJ(1) latches on this progress and OBJ(2)'s arrival walks the
            // party to its anchor, 3yd from where he stands.
            yield(State::AwaitGossip);
            return v;
        }

        // --- 7: Eadric or Paletress -------------------------------------------------------
        //
        // The boss walks in and DoZoneInCombat's 3s after the last soldier dies.
        // After a PARTIAL wipe she evades instead — full HP, REACT_PASSIVE on
        // Reset — and nothing brings her back but a pull.
        if (in.progress == PROGRESS_SOLDIERS_DIED)
        {
            if (in.partyInCombat || in.argentBossEngaged)
            {
                yield(State::ArgentBoss);
                return v;
            }
            if (in.argentBossPresent && in.argentBossAttackable)
            {
                if (!in.partyReady)
                {
                    yield(State::Resting);
                    return v;
                }
                v.state = State::ArgentRepull;
                v.action = Action::EngageBoss;
                return v;
            }
            yield(State::ArgentBoss);
            return v;
        }

        // --- 8: the Black Knight ------------------------------------------------------------
        if (in.partyInCombat)
        {
            yield(State::KnightFight);
            return v;
        }

        if (in.knightPresent)
        {
            if (in.knightAttackable)
                yield(State::KnightFight);
            else
                hold(State::KnightIntro);  // NON_ATTACKABLE; DamageTaken is 0 anyway
            return v;
        }

        // EVADE = DESPAWN, and he kills the announcer on the way in. With both gone
        // there is nothing to click and nothing to fight until a full wipe runs
        // InstanceCleanup. Reported once the grace runs out, never worked around:
        // the run's no-progress watchdog ends it.
        if (v.stranded)
        {
            yield(State::KnightStranded);
            return v;
        }

        if (!in.partyReady)
        {
            yield(State::Resting);
            return v;
        }
        if (clickAnnouncer(GOSSIP_OPTION_NEXT_PHASE))
            return v;

        yield(State::AwaitGossip);
        return v;
    }
}

#endif  // _PLAYERBOT_DCTOCDRIVERDECISION_H
