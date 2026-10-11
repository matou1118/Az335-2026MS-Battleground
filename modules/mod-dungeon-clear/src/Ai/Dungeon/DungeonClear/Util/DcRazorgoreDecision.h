/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCRAZORGOREDECISION_H
#define _PLAYERBOT_DCRAZORGOREDECISION_H

#include <cstdint>
#include <vector>

#include "Define.h"

// PURE kernel for Blackwing Lair's Razorgore egg run — the two decisions the
// driver makes every tick, lifted out of the world so they can be tested without
// a map: WHICH member takes the orb, and WHAT the driver does next.
//
// The glue (grid scans, charm state, movement, the actual cast) lives in
// Overrides/BlackwingLairDriver.cpp. Everything here is arithmetic over a
// snapshot, in the same shape as DcRegroupDecision / DcStrandedDecision.
//
// THE ENCOUNTER, in the terms these functions use:
//   A player clicks the Orb of Domination (GO 177808), which mind-controls
//   Razorgore for 90 SECONDS (spell 19832) and leaves the clicker unable to take
//   the orb again for 60 SECONDS (Mind Exhaustion, 23958). While possessed the
//   boss must be walked to each of THIRTY Black Dragon Eggs (GO 177807) and cast
//   Destroy Egg (19873, 3s cast, 10yd) on it. The measured tour is ~262yd of
//   travel plus 90s of casting — call it 135s — so the run spans TWO OR THREE
//   mind-control windows and needs a rotation of runners, exactly like a human
//   raid. Everything else in the room (an add every ~4s, forever, until the last
//   egg pops) belongs to the playerbots raid strategy.
//
//   AND BEFORE ANY OF THAT: the orb platform is held by three level-62 elites
//   that are there from map load and belong to no encounter — Grethok the
//   Controller and two Blackwing Guardsmen, 6-10yd from the orb. The runner's
//   walk ENDS inside them.
//
//   WHAT IS NOT TRUE, and cost this encounter its first three live runs: that
//   the platform can be cleared in a quiet room. `creature_formations` puts
//   RAZORGORE HIMSELF in Grethok's formation (leader guid 84389, member 84388,
//   groupAI 7 — full mutual assist), so first contact with the guard pack
//   aggros the boss from 77yd across the chamber, one tick later. Measured, on
//   an untouched instance: 22:12:42 a Blackwing Guardsman opens the fight,
//   22:12:43 all twenty-five members log "JOINED AN ONGOING FIGHT: Razorgore
//   the Untamed ... 0.0yd from its spawn".
//
//   SO GRETHOK IS THE BOSS PULL, and DC treats him as the boss: he is roster
//   anchor #0 of map 469 and the ORDINARY pipeline brings the raid to him —
//   advance as one body, raid muster, boss standoff, engage. Nothing in this
//   kernel moves anybody onto the platform before that: no election, no staging,
//   no click. The version that staged the runner "with the raid" had a bespoke
//   rung glide forty bots at the ledge the moment the leader came in range,
//   which is the tank running in and leaving the raid behind, with the runner in
//   the first rank of it.
//
//   AND THE MOMENT HE IS ENGAGED, THE RUNNER TAKES OVER. Not when the platform
//   is empty — from the tag onward the boss is already loose on the raid, the
//   add pump is already running (the instance promotes NOT_STARTED on the pull,
//   not on the orb), and the raid cannot help itself by damaging a boss whose
//   phase-1 death casts 20038 on all forty of them. Every second between the
//   pull and the mind control is pure loss, so the click goes in as soon as the
//   pull has landed and the runner is standing on the orb.
namespace DcRazorgore
{
    // The commit band for a cast. The spell's own range is 10yd; stopping at 6
    // leaves room for the boss's bounding radius and for the half-yard of slide
    // between the stop and the cast landing, and 19873 carries interrupt flags 31
    // — MOVING CANCELS IT — so arriving "just barely" in range is how a cast gets
    // eaten and the driver loops.
    inline constexpr float kCastBand = 6.0f;

    // Re-issue guard for the boss's spline: don't re-path while the destination
    // has moved less than this (eggs never move, but the nearest-egg election can
    // flip between two near-equidistant candidates on a step of drift, and that
    // flip re-plots the spline every tick and travels nowhere — the Violet Hold
    // repath-epsilon lesson).
    inline constexpr float kRepathEpsilon = 3.0f;

    // How long one egg may stay elected without the boss getting closer to it
    // before the driver gives up on it. Covers the two real ways a single egg can
    // wedge a 30-egg run: a partial path that ends short of it (the two tiers of
    // this room are joined by one ramp), and a cast that keeps failing for a
    // reason the caller cannot see. Blacklisted eggs are retried once the rest of
    // the field is gone, so a wedge costs time and never the encounter.
    inline constexpr uint32 kEggStallMs = 15000;

    // Cast attempts on ONE egg, in range and stationary, before the driver stops
    // asking politely and casts it triggered. The polite cast is the honest one
    // (3s channel, interruptible, exactly what a player's possess bar does); the
    // triggered one is the same spell, same script, same credit, minus the
    // channel. See BwlCastEggDestroy.
    inline constexpr uint8 kPoliteCastAttempts = 3;

    // What the driver should do this tick.
    enum class Step : uint8
    {
        Idle,        // event not live here — yield, drive nothing
        WaitPull,    // the platform is still held and nobody has pulled it — the
                     // tank owns this; touch nothing on the ledge
        NeedRunner,  // no usable designated runner — elect one
        StageRunner, // runner elected but not at the orb yet — it walks itself
        ClickOrb,    // runner is standing at the orb and may take it
        WaitCast,    // the possessed boss is mid-cast — yield the tick
        MoveBoss,    // possessed, target egg out of the commit band — walk
        CastEgg,     // possessed, in the band and stopped — destroy the egg
        Done,        // no eggs remain / the instance says the event is over
    };

    // One tick's worth of world, as facts.
    struct View
    {
        bool   eventDone{false};      // instance GetData(DATA_EGG_EVENT) == DONE
        bool   bossAlive{false};
        uint32 eggsRemaining{0};

        bool   bossCharmed{false};    // ...by OUR designated runner, specifically
        bool   bossCasting{false};

        // Grethok the Controller and the two Blackwing Guardsmen who stand on the
        // orb platform from map load. Not part of the encounter, and fatal to a
        // lone runner: the walk to the orb ENDS on top of them.
        bool   orbGuardsAlive{false};

        // ...and has anybody pulled them? This is the gate the whole staging
        // half hangs off: until the tank has Grethok in combat the ledge belongs
        // to the pull pipeline and nothing here may put a body on it.
        bool   orbGuardsEngaged{false};

        // Is Razorgore already in the fight? Grethok's formation drags him in on
        // the guard pull (groupAI 7), so this is true from a tick after the tag —
        // and it is also the answer when the raid pulled him some other way
        // entirely (a stray add, a wipe recovery), which is just as good a reason
        // to stop waiting.
        bool   bossEngaged{false};

        bool   haveRunner{false};     // a designated runner exists and is alive
        bool   runnerAtOrb{false};
        // The orb script's OWN three refusals and nothing else: alive, no pet, no
        // Mind Exhaustion. Deliberately NOT "and not mid-cast" — a DPS bot is
        // casting most ticks, and disqualifying it for that re-elected a fresh
        // runner every three seconds and never clicked anything. A cast in flight
        // is interrupted at the orb by the runner's own rung, where it is one
        // call; here it would be a rotation.
        bool   runnerCanClick{false};

        bool   haveEgg{false};        // an egg is elected (not all blacklisted)
        float  bossToEgg{0.0f};       // distance from the boss to that egg
    };

    // The whole FSM. Deliberately total and deliberately ordered: completion
    // first (so a late tick after the 30th egg can never re-elect a runner and
    // re-take the orb on a boss the raid is now trying to kill), then the
    // possessed branch (the only one that does real work), then the staging that
    // gets us back into it.
    inline Step Decide(View const& v)
    {
        if (v.eventDone || !v.bossAlive || v.eggsRemaining == 0)
            return Step::Done;

        if (v.bossCharmed)
        {
            // A 3s cast is 30 ticks; claiming them all would starve the raid's
            // combat engine for the whole egg run.
            if (v.bossCasting)
                return Step::WaitCast;
            if (!v.haveEgg)
                return Step::Idle;  // every egg blacklisted this pass — let the
                                    // caller clear the list and come back
            return v.bossToEgg > kCastBand ? Step::MoveBoss : Step::CastEgg;
        }

        // Not (or no longer) possessed: 90s expired, the runner died, or we have
        // not started. Everything here is about getting a live body onto the orb.
        //
        // AND NOTHING GOES UP BEFORE THE TANK DOES. This is the first test rather
        // than the last, and that ordering is the whole point: an election or a
        // staging walk taken while the platform is quiet is one DPS bot strolling
        // into three level-62 elites 78yd ahead of its raid, which is how the
        // first live run died and — through a rung that glided the WHOLE raid up
        // to "escort" it — how the pull became a footrace. Grethok is a boss
        // anchor now; until the pull pipeline has him in combat this kernel has
        // no business on the ledge.
        if (v.orbGuardsAlive && !v.orbGuardsEngaged && !v.bossEngaged)
            return Step::WaitPull;

        // The pull has landed (or the platform is already empty). From here the
        // runner is elected, staged and clicks with no further reference to the
        // guards: they are the raid's problem now, and every tick spent waiting
        // on them is a tick of a freed Razorgore the raid must not kill.
        if (!v.haveRunner)
            return Step::NeedRunner;
        if (!v.runnerAtOrb)
            return Step::StageRunner;
        if (!v.runnerCanClick)
            return Step::NeedRunner;  // exhausted / pet / dead -> someone else

        return Step::ClickOrb;
    }

    // A candidate for the orb, reduced to the facts that decide it.
    struct RunnerCandidate
    {
        uint32 guidLow{0};
        bool   alive{false};
        bool   isBot{false};       // a real player has no PlayerbotAI to drive
        bool   isLeader{false};    // the elected DC leader is the main tank
        bool   isTank{false};
        bool   isHealer{false};
        bool   isRanged{false};
        bool   hasPet{false};      // the orb script refuses a user with a pet
        bool   exhausted{false};   // Mind Exhaustion (23958) still up
        float  distToOrb{0.0f};    // how far this candidate has to walk
    };

    // Distance bucket for the election tie-break, in yards. Coarse on purpose: the
    // question is "who is materially closer to the orb", not "who is closer by half
    // a yard", and a fine comparison would flip the elected runner every time two
    // bots shuffled a step inside the camp — which re-issues a glide and walks
    // nobody anywhere (the Violet Hold repath-epsilon lesson, one level up).
    inline constexpr float kOrbDistanceBucket = 10.0f;

    // Elect the orb runner. Returns an index into `pool`, or -1 when nobody is
    // eligible (a raid of hunters and warlocks — the caller logs and stalls,
    // which is honest: the encounter genuinely cannot be done that way).
    //
    // Hard gates come from the orb script itself (alive, a bot, no pet, not
    // exhausted) plus one of ours: never the elected leader, which is the main
    // tank and is the only thing standing between the raid and the add wave.
    //
    // The preference order is the human one. A RANGED DPS is first — and the
    // reason has changed, so it is worth restating. It is NOT that a ranged bot
    // keeps DPSing through its window: the runner is MUTE for the whole ninety
    // seconds whatever class it is (the rung owns every tick and the multiplier
    // clamp behind it zeroes everything else), because 19832 is a channel that its
    // own rotation ends. It is that a melee runner is the one that has a swing to
    // suppress at all — autoattack runs off GetVictim() inside Unit::Update rather
    // than off the action engine, so it is the one thing here that is stopped by a
    // side effect rather than by simply declining to act. Preferring ranged keeps
    // that path cold. Melee is still perfectly eligible, and safe.
    //
    // Healers are last — losing one to a 90s root is how the raid dies to the adds
    // — but nobody is excluded, because a comp can be short of anything.
    //
    // WHAT DOES DECIDE, once rank ties, is how far the candidate has to WALK. The
    // gap between one window ending and the next beginning is dead time in which a
    // freed Razorgore is loose on the raid, and the walk is nearly all of it — so
    // among equals the election takes the bot that is already closest to the
    // ledge, bucketed (kOrbDistanceBucket) so it is a real difference and not a
    // step of drift. GUID breaks what is still tied, so every context that runs
    // this on the same snapshot computes the same runner.
    inline int SelectRunner(std::vector<RunnerCandidate> const& pool)
    {
        auto bucket = [](float d) { return static_cast<int>(d / kOrbDistanceBucket); };

        int best = -1;
        int bestRank = 0;
        for (size_t i = 0; i < pool.size(); ++i)
        {
            RunnerCandidate const& c = pool[i];
            if (!c.alive || !c.isBot || c.hasPet || c.exhausted || c.isLeader)
                continue;

            int rank;
            if (c.isHealer)         rank = 1;   // last resort
            else if (c.isTank)      rank = 2;   // an off-tank: better than a healer
            else if (!c.isRanged)   rank = 3;   // melee dps: fine, and mute like anyone
            else                    rank = 4;   // ranged dps: the right answer

            if (best < 0 || rank > bestRank)
            {
                best = static_cast<int>(i);
                bestRank = rank;
                continue;
            }
            if (rank < bestRank)
                continue;

            RunnerCandidate const& incumbent = pool[best];
            int const mine = bucket(c.distToOrb);
            int const theirs = bucket(incumbent.distToOrb);
            if (mine < theirs || (mine == theirs && c.guidLow < incumbent.guidLow))
                best = static_cast<int>(i);
        }
        return best;
    }
}

#endif  // _PLAYERBOT_DCRAZORGOREDECISION_H
