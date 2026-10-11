/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCSUPPRESSIONTRANSITDECISION_H
#define _PLAYERBOT_DCSUPPRESSIONTRANSITDECISION_H

#include <cmath>
#include <cstdint>
#include <vector>

#include "Define.h"

// PURE kernel for a TRANSIT — the two decisions the driver makes every tick,
// lifted out of the world so they can be tested without a map: WHERE ON THE
// ROUTE ARE WE, and DO WE WALK OR DO WE STAND.
//
// The glue (grid scans, the party walk, the splines, the telemetry) lives in the
// per-map driver: Overrides/BlackwingLairDriver.cpp for the Suppression Rooms
// and Overrides/HallsOfLightningDriver.cpp for the Slag Furnace. Everything here
// is arithmetic over a snapshot, in the same shape as DcRaidMusterDecision /
// DcRazorgoreDecision, and it is MAP-FREE: the leg described below is the one it
// was written for, not the only one it serves. Halls of Lightning uses three of
// the four holds and passes the fourth (Disarm) a radius of 0, which is this
// file's own "no such probe".
//
// THE LEG, in the terms these functions use. Between Vaelastrasz the Corrupt and
// Broodlord Lashlayer lie two Suppression Rooms holding 160 Corrupted Whelps on a
// THIRTY-SECOND respawn, twenty elites, and 38 Suppression Devices whose 20yd
// bubbles cover 95% of the 375yd the party has to cross. Three facts follow, and
// between them they are the whole design:
//
//   1. THE CLEAR CANNOT MOVE. DcCombatFlag::MayDrive is false while anything in
//      the party is engaged and Advance is registered only in the NON-combat
//      engine, so with the whelps up the run has no driver at all. This is not a
//      slow leg, it is a stopped one.
//   2. CLEARING IT IS ARITHMETICALLY IMPOSSIBLE. 5.3 spawns/second across the
//      complex. The party can out-DPS that and cannot out-TRAVEL it.
//   3. SO THE LEG IS A TRANSIT, NOT A CLEAR: one body, brakes off, crossed under
//      fire. What is NOT skipped is the elites — 600s respawn, thirteen of twenty
//      within 25yd of the route, six Taskmasters on the only ramp between the
//      rooms — so the driver stands and fights those on purpose.
//
// THE THREE THINGS THAT MAKE IT STOP WALKING, and why each is a HOLD rather than
// a detour:
//
//   * THE PACK IS TRAILING. A raid strung over 100yd sweeps a far larger cylinder
//     of a room whose spawns are the problem, and every straggler independently
//     holds the WHOLE party in combat. The answer is to STAND STILL, not to walk
//     back: the pack rung (DungeonClearTransitPackTrigger) does the walking, and
//     a leader that reverses course drags the column back through ground it has
//     already paid for.
//   * AN ELITE IS ON US. Standing and winning is progress that stays bought.
//   * AN ARMED DEVICE IS IN REACH. mod-playerbots' own disarm rung
//     (BwlTurnOffSuppressionDeviceAction, ACTION_RAID) turns off any READY device
//     within 15yd, and a disarmed device NEVER re-arms. All this owes it is a
//     tick or two of standing still while it fires — 17 of the 19 route-adjacent
//     devices are within 10yd of the line, so no detour is involved.
//
// EVERY HOLD IS BOUNDED. One wedged straggler, one elite that cannot be reached,
// one device the rung will not take must cost the crossing seconds, never the
// run; on expiry the verdict advances anyway and says so (`timedOut`), and the
// glue logs it.
namespace DcSuppressionTransit
{
    // Why the driver is standing still. Ordered by precedence, which is also the
    // order they are tested in: a trailing pack outranks an elite (fighting with
    // half the raid a room behind is how the other half dies), and an elite
    // outranks a device (the disarm rung will still be there in ten seconds; the
    // Taskmaster will not).
    enum class Hold : uint8
    {
        None = 0,      // walk
        Gathering,     // the gather gate at the staging point has not opened
        PackTrailing,  // the trailing living member is outside the pack leash
        Elite,         // a live Taskmaster / Hatcher is inside the hold radius
        Disarm,        // an armed Suppression Device is inside the hold radius
    };

    inline char const* HoldName(Hold h)
    {
        switch (h)
        {
            case Hold::Gathering:    return "gathering at the staging point";
            case Hold::PackTrailing: return "pack trailing";
            case Hold::Elite:        return "elite in reach";
            case Hold::Disarm:       return "disarming a suppression device";
            case Hold::None:
            default:                 return "advancing";
        }
    }

    struct Inputs
    {
        // --- where we are on the route ---
        uint32 cursorIndex = 0;      // authored anchor the leader is walking toward
        uint32 anchorCount = 0;      // size of the authored route (last one IS the standoff)
        float  distToCursor = 0.0f;  // leader -> that anchor, 3D
        float  arriveRadius = 0.0f;  // how close counts as reached

        // --- the gather gate (staging point only) ---
        bool   gathered = false;     // it has already opened for this crossing
        bool   quorumMet = false;    // enough living members are inside the gather radius
        bool   topped = false;       // TELEMETRY ONLY — see the gather gate in Decide()

        // --- the three hold probes ---
        float  trailDist = -1.0f;              // furthest living member from the cursor (<0 = nobody else)
        float  packLeash = 0.0f;

        // The pack hold is a QUORUM, not a maximum, and that is a measured
        // retraction. Holding on `trailDist > packLeash` asks all twenty-four
        // followers to be inside the leash at once; in tp-20260828-121941-1 that
        // was true for 87-91% of every run's driver ticks, and the only thing that
        // ever released it was its own 30s watchdog — a crossing that advances one
        // anchor per thirty seconds. One member wedged on a whelp, corpse-running,
        // or parked by the scout-lag rung must cost the raid a member, not the
        // leg: stranded recovery (relevance 42) sits above this whole ladder and
        // owns exactly that member.
        //
        // Same quorum the gather gate uses (TransitGatherQuorum), because it is
        // the same question asked twice — "is enough of the raid here yet".
        uint32 packOutside = 0;   // living followers further than packLeash from the cursor
        uint32 packLiving = 0;    // living followers on this map (0 = the leader is alone)
        float  packQuorum = 0.0f; // fraction that must be inside (0 = fall back to trailDist)
        float  nearestEliteDist = -1.0f;       // <0 = none in scan
        float  eliteHoldRadius = 0.0f;
        float  nearestArmedDeviceDist = -1.0f; // <0 = none in scan
        float  disarmHoldRadius = 0.0f;

        // --- clocks ---
        uint32 nowMs = 0;
        uint32 armedSinceMs = 0;     // DcRunState::transitArmedMs (the gather budget's clock)
        uint32 holdSinceMs = 0;      // DcRunState::transitHoldSinceMs (0 = not holding)
        uint8  holdReason = 0;       // DcRunState::transitHoldReason — which hold that clock belongs to

        // --- budgets (0 = unbounded, which no caller should pass) ---
        uint32 gatherTimeoutMs = 0;
        uint32 packHoldTimeoutMs = 0;
        uint32 eliteHoldTimeoutMs = 0;
        uint32 disarmHoldTimeoutMs = 0;
    };

    struct Verdict
    {
        bool advance = false;        // issue movement toward the cursor this tick
        bool cursorAdvance = false;  // ...after bumping the cursor to the next anchor
        bool complete = false;       // the crossing is over — the leader is at the standoff
        bool openGatherGate = false; // glue should latch DcRunState::transitGathered
        bool timedOut = false;       // this release/advance is a watchdog firing, not success
        Hold hold = Hold::None;
        uint32 holdSinceMs = 0;      // the clock to store back (0 while advancing)
    };

    // Wrap-safe elapsed test. getMSTime() wraps; unsigned subtraction of an
    // interval does not, which is why every clock here is compared this way and
    // never with `now - since >= budget` on signed types.
    inline bool Expired(uint32 nowMs, uint32 sinceMs, uint32 budgetMs)
    {
        return budgetMs != 0 && sinceMs != 0 && static_cast<uint32>(nowMs - sinceMs) >= budgetMs;
    }

    // Is the column strung out badly enough to stand still for?
    //
    // With a quorum supplied this counts heads: hold while fewer than
    // ceil(packLiving * packQuorum) followers are inside the leash. Without one
    // (packLiving 0, which is what the kernel's own tests and any caller that
    // cannot count pass) it degrades to the old furthest-member test, so the
    // probe is never silently disabled by a caller that forgot to fill it in.
    inline bool PackLags(Inputs const& in)
    {
        if (in.packLeash <= 0.0f)
            return false;

        if (in.packLiving > 0 && in.packQuorum > 0.0f)
        {
            uint32 const inside = in.packLiving - (in.packOutside > in.packLiving
                                                       ? in.packLiving
                                                       : in.packOutside);
            float const need = static_cast<float>(in.packLiving) * in.packQuorum;
            uint32 const required = static_cast<uint32>(std::ceil(need));
            return inside < required;
        }

        return in.trailDist >= 0.0f && in.trailDist > in.packLeash;
    }

    // Budget for a given hold. Kept as a function rather than one number because
    // the three holds are not the same KIND of wait: a device disarm is a tick or
    // two, a straggler catching up is tens of seconds, and an elite fight is
    // minutes. One shared watchdog would either release the disarm hold before the
    // rung fired or leave the leg parked behind a wedged straggler for the whole
    // elite budget.
    // Does a watchdog release LATCH — keep walking until the condition itself
    // clears — or is it one tick of progress before the same wait re-arms?
    //
    // ELITE and DISARM latch. A timeout there means the thing we stopped for is
    // not going to resolve, and WALKING is exactly what clears it: the leader
    // leaves the elite's hold radius, the device falls behind. Re-arming buys one
    // tick of movement per budget period — and because a hold never issues a stop
    // packet, one tick is one anchor. Twenty-four yards per 120 seconds over the
    // 342yd crossing is the 18-24 minute leg we kept measuring.
    //
    // PACKTRAILING deliberately does NOT latch, and that asymmetry is the point.
    // Walking is what makes a straggler hold WORSE, not better, so a latched
    // release would march the leader to the Broodlord standoff on its own —
    // against the one thing this whole driver exists for ("travelling as one body
    // is the precondition for travelling"). Its release is a duty cycle instead:
    // one anchor of progress, then wait out the budget again. That bounds a wedged
    // member's cost to the crossing without ever abandoning the raid to it.
    inline bool ReleaseLatches(Hold h)
    {
        return h == Hold::Elite || h == Hold::Disarm;
    }

    inline uint32 BudgetFor(Hold h, Inputs const& in)
    {
        switch (h)
        {
            case Hold::Gathering:    return in.gatherTimeoutMs;
            case Hold::PackTrailing: return in.packHoldTimeoutMs;
            case Hold::Elite:        return in.eliteHoldTimeoutMs;
            case Hold::Disarm:       return in.disarmHoldTimeoutMs;
            case Hold::None:
            default:                 return 0;
        }
    }

    // One evaluation. The glue calls this on the LEADER only, on every tick the
    // transit event is due.
    inline Verdict Decide(Inputs const& in)
    {
        Verdict v;

        if (in.anchorCount == 0)
            return v;  // no route — the glue reports Done and the leg is ordinary

        bool const reached = in.distToCursor <= in.arriveRadius;
        bool const atLastAnchor = in.cursorIndex + 1 >= in.anchorCount;

        // THE STANDOFF. The last authored anchor is Broodlord's standoff, so
        // reaching it IS the crossing. Nothing else here can report complete: the
        // event's own predicate stops being due at the same moment, which is what
        // makes the completion self-resetting (a leader shoved back into the
        // gauntlet simply re-arms, with no latch to clear).
        if (reached && atLastAnchor)
        {
            v.complete = true;
            return v;
        }

        // THE GATHER GATE, and it is only at the staging point. Anchor 0 is the
        // last genuinely clean ground before the gauntlet — nearest whelp 40.8yd,
        // nearest device 50.1yd — so it is the one place the raid can stand and
        // wait for its stragglers without the wait itself costing anything.
        //
        // Gated on `reached` as well as on the index: the walk INTO staging is an
        // ordinary advance, and holding the leader mid-approach because the column
        // behind it has not arrived is how a raid never arrives at all.
        //
        // QUORUM ONLY — `in.topped` is telemetry here, NOT a gate, and that is a
        // measured retraction rather than a simplification. The gate used to ask
        // for topped as well, on the reasoning that staging is the last drink
        // before a four-minute crossing. In the live plan (tp-20260828-111227-1)
        // all FOUR gathers hit the full 60s watchdog and not one opened on merit —
        // including one that logged `25 of 25 raid members formed up (GATHER
        // TIMEOUT)`. The reason is structural, not tuning: the staging shelf is
        // 40.8yd from the nearest whelp on a 30s respawn, so somebody in a
        // forty-man raid is in combat essentially always, and a raid in combat
        // never tops off. Asking for a bar that cannot be reached does not buy the
        // drink — it buys sixty seconds of standing still and then crosses anyway,
        // exactly as it would have without the clause. Health and mana are the
        // rest ladder's business on the approach, which is now authored ground
        // (RegisterBlackwingLairRoute anchors 0-19) and where the party CAN drink.
        if (!in.gathered && in.cursorIndex == 0 && reached)
        {
            if (in.quorumMet)
                v.openGatherGate = true;
            else if (Expired(in.nowMs, in.armedSinceMs, in.gatherTimeoutMs))
            {
                // Advance with what we have and SAY SO. A muster that can hang is
                // a muster that eventually will (the S2025 lesson) — a bot that
                // cannot path in must never hold twenty-four others at the door.
                v.openGatherGate = true;
                v.timedOut = true;
            }
            else
            {
                v.hold = Hold::Gathering;
                v.holdSinceMs = in.armedSinceMs ? in.armedSinceMs : in.nowMs;
                return v;  // cursor stays on staging: that is where the pack forms
            }
        }

        // Reaching an anchor bumps the cursor whether or not we then hold. The
        // cursor is where we are GOING, and a hold only means "not this tick" — so
        // the pack keeps forming on the leg ahead rather than on the one just
        // finished.
        v.cursorAdvance = reached;

        // --- the three holds, in precedence order ---------------------------
        Hold want = Hold::None;
        if (PackLags(in))
            want = Hold::PackTrailing;
        else if (in.nearestEliteDist >= 0.0f && in.nearestEliteDist <= in.eliteHoldRadius)
            want = Hold::Elite;
        else if (in.nearestArmedDeviceDist >= 0.0f &&
                 in.nearestArmedDeviceDist <= in.disarmHoldRadius)
            want = Hold::Disarm;

        if (want == Hold::None)
        {
            v.advance = true;
            return v;
        }

        // A hold that CHANGES reason gets a fresh clock. Carrying the old one over
        // would let a party that spent 100s fighting elites be released from a
        // straggler hold two seconds after it started — the watchdogs bound one
        // wait each, not the crossing.
        uint32 const since =
            (in.holdReason == static_cast<uint8>(want) && in.holdSinceMs) ? in.holdSinceMs
                                                                         : in.nowMs;

        if (Expired(in.nowMs, since, BudgetFor(want, in)))
        {
            // The watchdog. Whatever we were waiting for is not coming: walk on
            // and let the glue log which hold gave up. Stranded recovery (42) is
            // still above the whole ladder for the member this leaves behind.
            //
            // WHETHER IT LATCHES IS PER HOLD — see ReleaseLatches. This used to
            // return hold=None with holdSinceMs=0 for all three; the glue stores
            // both back, so the next tick found the SAME unchanged condition with
            // no clock, stamped a fresh one and waited out the whole budget again.
            // For the elite hold that is one anchor per 120 seconds.
            //
            // Reporting the hold with its ORIGINAL clock is what makes a latched
            // release stick: `Expired` stays true on every later tick, so the leg
            // keeps walking until the condition clears — at which point `want` is
            // None, holdSinceMs goes to 0, and the next hold gets a full budget.
            v.advance = true;
            v.timedOut = true;
            if (ReleaseLatches(want))
            {
                v.hold = want;
                v.holdSinceMs = since;
            }
            return v;
        }

        v.hold = want;
        v.holdSinceMs = since;
        return v;
    }

    // The smallest gather radius the pack rung can ever satisfy.
    //
    // The rung walks a member to `packLeash - holdMargin` of the cursor and then
    // refuses to move it again once it is within `arriveLeash` of that point, so
    // the raid comes to rest in a band whose OUTER edge is the sum below. A gather
    // gate that asks for a radius inside that band is asking for a formation
    // nothing produces: it holds until its watchdog and crosses with whoever
    // happens to be standing close, which live (tr-20260830-125018-2) was two of
    // twenty-five with the raid otherwise perfectly formed.
    //
    // Both inputs are runtime-tunable and were tunable into direct contradiction,
    // which is why this is a floor computed from them rather than a number chosen
    // beside them. Same lesson as [[dc-moving-camp-rung-hysteresis]], one level up:
    // there the TRIGGER had to read the radius its own action aims at; here the
    // PARTY GATE does.
    inline float GatherRadiusFloor(float packLeash, float holdMargin, float arriveLeash)
    {
        float const floor = packLeash - holdMargin + arriveLeash;
        return floor > 0.0f ? floor : 0.0f;
    }

    // --- where on the route are we -----------------------------------------

    struct Anchor
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    // The cursor: the index of the first authored anchor AHEAD of the leader.
    //
    // Resolved by projecting the leader onto the polyline rather than by counting
    // arrivals, because counting cannot recover. A leader that dies, corpse-runs
    // and walks back in has an arrival count that is a lie about where it is
    // standing, and this leg is one the party can legitimately re-enter twice.
    //
    // MONOTONE BY DEFAULT. The projection is taken in 3D and the two rooms are
    // stacked only ~9yd apart, so a leader on the upper floor is closer to some
    // lower-floor segment than the geometry deserves; clamping to `stored` stops a
    // cursor that has crossed the ramp from being dragged back down it by float.
    //
    // ...WITH ONE ESCAPE, which is the recovery case above: if the leader is
    // further than `resyncDist` from the stored anchor AND the projection puts it
    // somewhere nearer, the stored value is a stale fact about a previous attempt
    // and the projection wins. A wipe is the case; nothing inside a working
    // crossing is ever that far from its own cursor.
    inline uint32 ResolveCursor(std::vector<Anchor> const& route,
                                float lx, float ly, float lz,
                                uint32 stored, float resyncDist)
    {
        if (route.size() < 2)
            return 0;

        auto dist3 = [](float ax, float ay, float az, float bx, float by, float bz)
        {
            float const dx = ax - bx, dy = ay - by, dz = az - bz;
            return std::sqrt(dx * dx + dy * dy + dz * dz);
        };

        // Closest SEGMENT, and its far end is the anchor we are walking toward.
        uint32 projected = 1;
        float best = -1.0f;
        for (std::size_t i = 1; i < route.size(); ++i)
        {
            Anchor const& a = route[i - 1];
            Anchor const& b = route[i];
            float const abx = b.x - a.x, aby = b.y - a.y, abz = b.z - a.z;
            float const len2 = abx * abx + aby * aby + abz * abz;
            float t = 0.0f;
            if (len2 > 0.0001f)
            {
                t = ((lx - a.x) * abx + (ly - a.y) * aby + (lz - a.z) * abz) / len2;
                t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
            }
            float const d = dist3(lx, ly, lz, a.x + abx * t, a.y + aby * t, a.z + abz * t);
            if (best < 0.0f || d < best)
            {
                best = d;
                projected = static_cast<uint32>(i);
            }
        }

        if (stored >= route.size())
            stored = static_cast<uint32>(route.size()) - 1;

        // The recovery escape: far from where we thought we were, and the
        // projection says somewhere else entirely.
        float const toStored = dist3(lx, ly, lz, route[stored].x, route[stored].y, route[stored].z);
        if (toStored > resyncDist && projected < stored)
            return projected;

        return projected > stored ? projected : stored;
    }

    // Which authored anchor a PUBLISHED cursor position is. The driver publishes
    // `hints[cursor]` verbatim, so this is an exact vertex match in every ordinary
    // case; the nearest-vertex search is the tolerant form of the same question,
    // for the tick on which the row and the stamp disagree by float.
    //
    // Nearest VERTEX and not nearest SEGMENT, deliberately — this answers "which
    // anchor was published", not "where is the reader standing", and those are
    // different questions with different right answers on a hairpin.
    inline uint32 AnchorIndexOf(std::vector<Anchor> const& route, float x, float y, float z)
    {
        uint32 best = 0;
        float bestD = -1.0f;
        for (std::size_t i = 0; i < route.size(); ++i)
        {
            float const dx = route[i].x - x, dy = route[i].y - y, dz = route[i].z - z;
            float const d = dx * dx + dy * dy + dz * dz;
            if (bestD < 0.0f || d < bestD)
            {
                bestD = d;
                best = static_cast<uint32>(i);
            }
        }
        return best;
    }

    // The point `backDist` yards BEHIND `cursorIndex` measured ALONG the authored
    // polyline, rather than `backDist` yards away from it in a straight line.
    //
    // WHY THE DISTINCTION IS THE WHOLE POINT. A straight-line hold point is a
    // CHORD, and a chord is only on the floor when the leg is straight. The climb
    // out of Blackwing Lair's staging shelf is a C — the walkable surface bows
    // seven yards east around a hole in the mesh (probe the column at
    // (-7628.5, -932.5): zero surfaces) — so the chord from a follower on the
    // north arm to a cursor on the south arm passes through the void, and its
    // linearly interpolated z lands 2.3yd UNDER the only floor there. A Player is
    // handed PATHFIND_NORMAL whether or not a route exists
    // ([[ac-pathgenerator-players-always-get-pathfind-normal]]), so the bot gets a
    // straight-line shortcut and walks into the rock. Route distance cannot do
    // that: every point it returns is ON the corridor the probe suite certifies.
    //
    // Clamps to the head of the route — there is no polyline behind anchor 0, and
    // the honest answer there is the head itself.
    inline Anchor PointBehindOnRoute(std::vector<Anchor> const& route, uint32 cursorIndex,
                                     float backDist)
    {
        if (route.empty())
            return Anchor{};
        if (cursorIndex >= route.size())
            cursorIndex = static_cast<uint32>(route.size()) - 1;

        Anchor const& cursor = route[cursorIndex];
        if (!(backDist > 0.0f))
            return cursor;

        float remaining = backDist;
        for (uint32 i = cursorIndex; i > 0; --i)
        {
            Anchor const& b = route[i];
            Anchor const& a = route[i - 1];
            float const dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
            float const len = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (len <= 0.0001f)
                continue;
            if (remaining <= len)
            {
                float const frac = remaining / len;
                return Anchor{ b.x + dx * frac, b.y + dy * frac, b.z + dz * frac };
            }
            remaining -= len;
        }
        return route[0];
    }
}

#endif  // _PLAYERBOT_DCSUPPRESSIONTRANSITDECISION_H
