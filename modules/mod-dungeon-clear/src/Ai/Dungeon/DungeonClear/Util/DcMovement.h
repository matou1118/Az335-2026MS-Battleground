/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DC_MOVEMENT_H
#define _PLAYERBOT_DC_MOVEMENT_H

#include "G3D/Vector3.h"
#include "MoveSplineInitArgs.h"
#include "Ai/Base/Value/LastMovementValue.h"

class Player;
class PlayerbotAI;

// DcMovement is the single funnel through which all DungeonClear code issues or
// stops movement. It owns the two facts that otherwise have to be re-remembered
// at every call site (see the nav/pull review, finding #4):
//
//   1. A plain Unit::StopMoving() does NOT cancel a launched escort-spline
//      glide (the EscortMovementGenerator keeps driving the bot down the old
//      route). Cancelling one needs the escort-aware StopActiveSplineGlide
//      incantation (kill the ESCORT generator + zero the LastMovement wait), and
//      a hold against an in-flight glide additionally needs StopMovingOnCurrentPos.
//   2. The engine queues actions from triggers evaluated at tick start, so an
//      action can execute AFTER the state that should have gated it changed (the
//      pause race). Movement issuance re-checks the paused flag HERE so the
//      whole queued-action race class is killed in one place.
//
// Call sites state intent (Soft / Hold / HardPin, or "move there"); the
// mechanism lives here. The MoveTo funnel proper lives on DcMovementAction (a
// thin MovementAction subclass) because MovementAction::MoveTo is protected;
// the free functions below cover everything that does not need that protected
// method.
namespace DcMovement
{
    // The endpoint of the last escort glide DC itself issued through SplinePath.
    // mod-playerbots #2747 dispatches every multi-point MoveTo through
    // MotionMaster::MoveSplinePath — an EscortMovementGenerator, the same generator
    // DC's glides use — so "an ESCORT generator is running" stopped meaning "my
    // glide is running". The stop helpers compare the live spline's final point
    // against this before cancelling anything; stock's splines are left alone.
    // (Pre-#2747 stock issued MovePoint, a POINT generator, and the check is moot.)
    struct DcGlideRecord
    {
        bool issued = false;
        G3D::Vector3 end;
    };

    // Is the ESCORT generator currently driving `bot` one DC issued (SplinePath)
    // and still in flight? False for stock's own MoveSplinePath glides.
    bool OwnsRunningEscort(Player* bot);

    // Stop strengths, by intent.
    enum class Stop
    {
        // Settle in place; no-op if already standing. Replaces the bare
        // "if (isMoving()) StopMoving()" dwell stops. NOT escort-aware: only
        // use where no escort glide can be in flight.
        Soft,

        // Hold position against an in-flight escort glide (= the old HaltForHold):
        // kill the ESCORT spline, zero the LastMovement wait, and
        // StopMovingOnCurrentPos. Also tears down a persistent FOLLOW generator
        // for followers (the selfbot-stale-MoveFollow class). Idempotent per
        // tick, so per-tick hold calls do not spam stop packets.
        Hold,

        // Hold + unconditionally pin a queued point-move sitting under an active
        // CC / higher-priority generator (= the CC-abort / turn-and-plant
        // incantation). Unlike Hold it does not early-out on a standing bot,
        // because under CC the bot is not "moving" yet a MOVEMENT_COMBAT MoveTo
        // is still queued and would resume the instant the impairment clears.
        HardPin,
    };

    // Stop the bot at the requested strength.
    void StopBot(Player* bot, Stop strength);

    // Drop the "I am still travelling" block recorded by the last movement, WITHOUT
    // stopping the bot or touching any generator.
    //
    // The framework refuses a later move whose priority is not STRICTLY GREATER
    // than the one on record for as long as that record says the move lasts —
    // before mod-playerbots #2747 as the delay window every MoveTo armed, after it
    // as a hold (stock still arms MOVEMENT_FORCED holds from SetNextMovementDelay,
    // and our own SplinePath records one). Everything the pull issues is
    // MOVEMENT_COMBAT, so one leg's leftover block silently refuses the
    // NEXT leg for the remainder of its budget — the tank stands in the pack it
    // just aggroed instead of turning and running home. Stop::Hold / Stop::HardPin
    // already zero it as a side effect, but they also stop the bot, and Hold
    // early-outs on a bot that is standing still, which is exactly the case that
    // needs clearing. This is the seam for "I am replacing the leg, not halting
    // it": call it immediately before issuing the replacement move.
    void ClearMovementWait(Player* bot);

    // ClearMovementWait plus the duplicate-destination guard: stock IsDuplicateMove
    // refuses the SAME point for a flat MaxWaitForMove (5s) after it was issued,
    // however short the leg and whether or not the bot is still walking it. For a
    // standing bot re-issuing its own last point, that refusal guards nothing — the
    // move it protects is over. Only call it for a bot that is not moving.
    void ReleaseMoveLock(Player* bot);

    // Issue the upcoming polyline as ONE EscortMovementGenerator spline (the
    // continuous glide that replaces per-point stops). Absorbs the issuance
    // ritual that was hand-duplicated at the advance, swim, and pull-maneuver
    // sites: stand up, interrupt any cast, MoveSplinePath, then record a
    // NORMAL-priority LastMovement sized to the window travel time (a delay window
    // before mod-playerbots #2747, a hold after it; for priority arbitration only
    // — the re-issue guards key off splineRunning, not this duration). `pts` must
    // hold the live position at [0] and at least one more
    // point. Returns true iff the spline was issued (pts.size() >= 2).
    bool SplinePath(PlayerbotAI* botAI, Movement::PointsArray& pts,
                    MovementPriority recordPrio = MovementPriority::MOVEMENT_NORMAL);

    // The pause/teardown gate. False once the run is paused, so movement issuance
    // bails (the queued-action race fix). Exposed for the MoveTo funnel and for
    // the few actions that bail before doing non-movement work too.
    bool DcMovementAllowed(PlayerbotAI* botAI);

    // Cancel a stale escort-spline glide of DC's OWN. No-op unless one of our
    // glides is actually in flight (OwnsRunningEscort), so it never perturbs the
    // LastMovement block at sites with no glide — and never touches a stock
    // spline: since mod-playerbots #2747 every multi-point stock MoveTo is an
    // escort glide too, and cancelling that before each DcMoveTo re-issue was a
    // stop/start per tick — the half-step stutter on the pull approach. Folded
    // into DcMovementAction::DcMoveTo (so a point move is
    // never left coasting under an old glide); also called directly at the bare
    // glide-kill sites that drop a stale glide before a non-move (swim hand-back,
    // posStuck rebuild, off-path resnap).
    void ResolveEscortConflict(Player* bot);
}

#endif
