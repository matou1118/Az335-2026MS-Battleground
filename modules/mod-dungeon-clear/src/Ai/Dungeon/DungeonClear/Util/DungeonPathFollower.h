/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DUNGEONPATHFOLLOWER_H
#define _PLAYERBOT_DUNGEONPATHFOLLOWER_H

#include <optional>
#include <vector>

#include "Common.h"
#include "G3D/Vector3.h"
#include "Ai/Dungeon/DungeonClear/Util/ChunkedPathfinder.h"

class Player;

// Where along the cached long-path the bot is currently walking. Indexes
// segments[segmentIdx].polyline[pointIdx]. (0, 0, 0) is the freshly-built
// state — start of the first segment, no off-path history.
//
// Reset whenever the cache is rebuilt (boss change, TTL expiry, forced
// rebuild). The follower never reads stale state across path lifetimes.
struct DungeonFollowerState
{
    uint32 segmentIdx{0};
    uint32 pointIdx{0};
    uint32 offPathTicks{0};
};

// Drives the bot along a precomputed long-path (the v3 polyline-walking
// consumer of StridedPathfinder). Pure helper — no member state; all
// per-bot state lives in AiObjectContext values (DungeonFollowerState).
//
// The follower issues short MoveTos to consecutive polyline points so the
// engine's per-MoveTo re-path stays well inside PathGenerator's safe
// output envelope. This is what stops the bot from wall-clipping when the
// engine returns PATHFIND_SHORT for a long destination.
class DungeonPathFollower
{
public:
    // HORIZONTAL distance at which the current polyline target is considered
    // reached and the follower advances pointIdx.
    //
    // Deliberately 2D. The route is a FLOOR path and the bot walks the floor, so
    // "have I arrived" is a plan-view question; the vertical axis is a separate,
    // much looser guard (POINT_REACHED_Z) whose only job is to stop the cursor
    // skipping a point on another storey directly overhead.
    //
    // This used to be a single 3D radius, and on a RAMP that wedged the cursor
    // permanently. Recast rasterizes an incline into discrete plateaus, so a
    // route point on a ramp routinely floats 2-3.5yd ABOVE the collision floor
    // the bot actually stands on. The bot walks to the point in plan view,
    // arrives directly underneath it, and the 3D distance reads just over 3.0 —
    // so the cursor never advances, forever. Live in tr-20260818-073620-14
    // (Blackrock Spire, the ramp below Overlord Wyrmthalak): navmesh Z at
    // (-55,-366) is 78.74 while the bot's real floor Z is 75.6 — a 3.1yd float,
    // one tenth of a yard outside the old tolerance. The tank sat in a 5yd box
    // at (-54,-366,76) for nine minutes while posStuck -> resnap -> rebuild ->
    // re-anchor -> off-line rejoin cycled 444 times without ever advancing one
    // point. Splitting the axes is what makes a ramp arrival decidable.
    static constexpr float POINT_REACHED = 3.0f;

    // Vertical half-height of the arrival cylinder. NOT an arrival criterion —
    // a floor-following bot is "there" as soon as it is there in plan view.
    // This only rejects the pathological case where the point that is within
    // POINT_REACHED horizontally sits on a DIFFERENT STOREY, where advancing
    // would skip the whole ramp/stair leg that connects them.
    //
    // Sized off the module's shared same-level constant (DC_Z_LEVEL_TOLERANCE,
    // 5.0): slopes, stairs and navmesh plateau float all stay under it, while
    // WotLK inter-floor gaps comfortably exceed it. Kept as a local constant
    // rather than an include so the follower stays a leaf header.
    static constexpr float POINT_REACHED_Z = 5.0f;

    // Perpendicular-distance threshold for off-path detection.
    static constexpr float OFF_PATH_THRESHOLD = 6.0f;

    // Consecutive off-path ticks before Resnap fires.
    static constexpr uint32 OFF_PATH_TICK_LIMIT = 3;

    // Resnap window — flattened polyline points searched ahead/behind the
    // current state when the bot has drifted off the corridor. Sized so the
    // point-count search reaches RESNAP_RADIUS even where the polyline is
    // sparse (~4yd spacing → 24 points ≈ 96yd of route), making the 3D
    // RESNAP_RADIUS the true binding limit rather than the point count.
    static constexpr size_t RESNAP_WINDOW = 24;

    // Max 3D distance from the bot to a candidate polyline point during
    // Resnap; beyond this the bot is too far to safely re-anchor its cursor
    // to that point — caller should rebuild instead.
    //
    // Widened from 25 to 45: the dominant drift source is a TRASH CHASE.
    // EngageDirect walks the tank to the mob and the combat engine's
    // MoveChase then follows it as it repositions, so when a pack dies the
    // tank routinely ends 30-40yd off the planned line. At 25yd that fell
    // through to a full from-scratch rebuild from the off-route position —
    // the "wanders off and doesn't stick to the path" behavior. 45yd lets
    // the tank re-anchor onto the EXISTING route and resume instead. The
    // per-candidate LOS gate (BotCanSee) keeps the wider radius from snapping
    // across a wall into the wrong corridor.
    static constexpr float RESNAP_RADIUS = 45.0f;

    // SeedCursor's early-out: a freshly built route whose first point is this
    // close to the bot started where the bot stands, so cursor 0 is already
    // right and the projection (and its raycasts) can be skipped. Sized to
    // cover ordinary navmesh snap drift on the start point without reaching
    // the next polyline vertex.
    static constexpr float SEED_AT_START_RADIUS = 10.0f;

    // Upper bound on control points fed to a single continuous-spline
    // issuance (see BuildSplineWindow). Caps spline/packet size on very
    // long routes; the bot pauses ~one tick at the boundary and the next
    // Advance tick extends the spline. Comfortably covers the legs between
    // most trash packs, so combat almost always interrupts first.
    static constexpr size_t MAX_SPLINE_WINDOW_POINTS = 100;

    struct Hop
    {
        G3D::Vector3 point;
        bool isJump{false};   // current segment carries jumpDown/jumpGap AND we're targeting its last point
        bool isDone{false};   // path complete; no further hops
    };

    // Advances state past reached polyline points and returns the next
    // point to walk to. Sets isDone=true when the path is fully walked.
    static Hop NextHop(Player* bot, ChunkedPathfinder::Result const& path, DungeonFollowerState& state);

    // Pure core of NextHop's arrival test (no Player — gtested directly). True
    // when the bot counts as standing ON polyline point `p`: within
    // POINT_REACHED horizontally AND within POINT_REACHED_Z vertically. See
    // POINT_REACHED for why the axes are separate rather than one 3D radius.
    static bool PointIsReached(float botX, float botY, float botZ, G3D::Vector3 const& p);

    // True when the bot has arrived at `p` in PLAN VIEW but is outside the
    // vertical band — i.e. it is standing under (or over) its own route point
    // and no amount of walking will close the gap. Distinct from "not there
    // yet": the cursor cannot advance and the bot cannot make progress, so the
    // caller must escalate rather than re-issue the same move. Kept separate
    // from PointIsReached so the two failure modes are never conflated in a
    // log line or a decision.
    static bool PointIsVerticallyStranded(float botX, float botY, float botZ, G3D::Vector3 const& p);

    // Escalation for the stranded case above: when the cursor's own point is
    // one the bot is already standing under/over, step the cursor ONE point
    // forward so the follower has something walkable to aim at. Returns true if
    // it skipped (out-param `skipped` carries the abandoned point for logging).
    //
    // Advancing is safe precisely BECAUSE the test is horizontal: the bot is
    // already within POINT_REACHED of the point in plan view, so the leg being
    // skipped covers no ground. A route that genuinely climbs here would need
    // to double back inside 3yd horizontally, which is a jump segment — and
    // jump legs never reach this path (callers gate on hop.isJump).
    //
    // One point per call, never a scan: an unbounded skip on a mis-built route
    // would silently teleport the cursor down the corridor. If the next point
    // is stranded too, the next tick handles it and the stall watchdogs still
    // see a bot that is not making ground.
    static bool SkipStrandedPoint(Player* bot, ChunkedPathfinder::Result const& path,
                                  DungeonFollowerState& state, G3D::Vector3& skipped);

    // Returns true if the bot's 2D perpendicular distance to the current
    // polyline segment exceeds OFF_PATH_THRESHOLD. Updates
    // state.offPathTicks (incremented on off-path, reset on on-path).
    static bool IsOffPath(Player* bot, ChunkedPathfinder::Result const& path, DungeonFollowerState& state);

    // Pure 2D perpendicular distance from the bot to the current route segment
    // (the prev->cur / cur->next legs around the cursor). No state mutation,
    // unlike IsOffPath. Callers use it to decide whether the bot is physically
    // on the corridor: a continuous escort spline launched from off the line
    // opens with a STRAIGHT leg back to the route that clips wall corners, so an
    // off-line bot must rejoin via a generated path (MoveTo) first. Returns 0
    // when the route has no resolvable current point.
    static float RouteDeviation(Player* bot, ChunkedPathfinder::Result const& path,
                                DungeonFollowerState const& state);

    // The route point the follower cursor currently anchors to — the same point
    // RouteDeviation measures its 2D perpendicular distance around. nullopt when
    // the cursor is past the path end. Exposed so callers can add a vertical
    // (Z) displacement check alongside the 2D-only deviation, since a bot on a
    // different floor directly under/over its route reads deviation ~= 0.
    static std::optional<G3D::Vector3> CurrentPoint(ChunkedPathfinder::Result const& path,
                                                    DungeonFollowerState const& state);

    // True when the cursor's next hop lies BEHIND the bot along the route — the
    // bot has been carried PAST its own cursor and walking to the hop is walking
    // backward over ground it already covered.
    //
    // RouteDeviation cannot see this: it is a PERPENDICULAR distance, so a bot
    // 10yd further along the same corridor reads as barely off the line, and the
    // off-line rejoin then issues a MoveTo to the point behind it. That is the
    // short backward step the tank does on approach — glide forward, cursor
    // lags, walk back to it, re-anchor, glide forward again. DC_REANCHOR_DISTANCE
    // was written for exactly this ("would otherwise make NextHop target a point
    // behind the tank and walk it backward") but only fires past 12yd of
    // staleness, while the off-line rejoin fires at 6yd of deviation — so the
    // 6-12yd band was left to walk backward.
    //
    // Direction is the route tangent at the cursor (cursor -> next point, or
    // previous -> cursor at a segment tail), 2D: a hop directly below/above is a
    // floor problem, not a facing one. False when the route has no resolvable
    // heading — callers must degrade to their existing behaviour, never to a
    // freeze.
    static bool HopIsBehind(Player* bot, ChunkedPathfinder::Result const& path,
                            DungeonFollowerState const& state, Hop const& hop);

    // Pure core of HopIsBehind: true when (pX,pY) sits on the far side of the bot
    // from the route heading (dirX,dirY) — i.e. reaching it means travelling
    // against the route. A degenerate heading returns false (unknowable, so leave
    // the caller's behaviour alone).
    static bool PointIsBehind(float botX, float botY, float pX, float pY,
                              float dirX, float dirY);

    // Walks a window of polyline points AT OR AHEAD of the current state
    // (never behind it — the escort is one-way, so the cursor must not
    // regress to already-cleared corridor), picks the one closest to the
    // bot in 3D that it can see, and updates state.segmentIdx /
    // state.pointIdx to it. Returns false if no forward candidate within
    // RESNAP_RADIUS — caller should rebuild from current position.
    // Clears offPathTicks on success.
    //
    // `movedOut` (optional) reports whether the cursor ACTUALLY CHANGED, which
    // is a different fact from the return value and must never be conflated
    // with it. The search window starts AT the cursor, so the point the cursor
    // already holds is itself a candidate — and on an authored anchor route,
    // whose points sit 15-20yd apart, a bot that has overshot its cursor by a
    // few yards is nearer to the point it just passed than to the next one.
    // Resnap then re-picks the cursor, returns true, and has repaired nothing.
    // A rung whose complaint IS the cursor (the hop is behind me; I am wedged
    // and want a new anchor) reads that as a cure and asks again next tick,
    // forever: 198 such re-anchors at an unchanging 4.0-4.4yd in
    // tr-20260902-134056-2, and the "resnapped onto existing route" rung pinned
    // at attempt 1 for a whole run in tr-20260804-153254-2.
    //
    // Callers that only need "is there a valid forward anchor" (the off-path
    // probe, whose cure for perpendicular drift is the rejoin rather than a
    // rebuild) may ignore it and keep the old meaning.
    static bool Resnap(Player* bot, ChunkedPathfinder::Result const& path, DungeonFollowerState& state,
                       bool* movedOut = nullptr);

    // Escalation for a cursor the bot has WALKED PAST: step it one point
    // forward so the follower stops aiming backward. The companion to
    // SkipStrandedPoint (which handles a point the bot is standing under), and
    // safe for the same reason — the caller has established that the hop lies
    // behind the bot along the route, so the leg being skipped is ground the
    // bot has already covered.
    //
    // This is the escape Resnap cannot provide: Resnap is forward-only and the
    // cursor is its own nearest forward candidate, so once a sparse route's
    // next point is farther away than the passed one, no number of resnaps can
    // move the cursor off it. Out-param `skipped` carries the abandoned point
    // for logging. One point per call, never a scan — same doctrine as
    // SkipStrandedPoint: if the next point is behind us too, the next tick
    // handles it and the stall watchdogs still see a bot making no ground.
    static bool SkipPassedPoint(ChunkedPathfinder::Result const& path,
                                DungeonFollowerState& state, G3D::Vector3& skipped);

    // Seed a follower cursor for a route that has JUST been installed: reset it,
    // then — when the route does not start where the bot stands, i.e. an
    // authored anchor route — project the bot onto the polyline and take the
    // nearest VISIBLE point as the cursor. See the definition for why a flat
    // reset to 0 walked the BWL raid backwards through the suppression gauntlet.
    static void SeedCursor(Player* bot, ChunkedPathfinder::Result const& path,
                           DungeonFollowerState& state);

    // Returns a point on the ALREADY-TRAVELED route roughly `distance` yards
    // behind the follower's current cursor, measured along the polyline from
    // the bot's live position back through the cleared corridor. Used by the
    // advanced pull to set its camp back along the spline so the party has
    // room to work and the tank drags the pack away from its spawn cluster —
    // the one place the otherwise one-way escort is allowed to look behind the
    // cursor. Returns nullopt when there's no cleared route behind the bot
    // (freshly built path / very start of the run); callers fall back to the
    // bot's own position. Pure read of `state` (does not move the cursor).
    static std::optional<G3D::Vector3> PointBehind(Player* bot,
        ChunkedPathfinder::Result const& path, DungeonFollowerState const& state, float distance);

    // Collects a run of upcoming polyline points, starting at the follower's
    // current cursor, for one continuous-spline issuance (MoveSplinePath).
    // The returned array's [0] is the bot's live position (the escort
    // convention; MoveSplineInit::Launch overwrites path[0] with the live
    // position regardless), and [1..] are consecutive polyline points up to
    // MAX_SPLINE_WINDOW_POINTS. The run STOPS at the first jump leg
    // (jumpDown/jumpGap) — jumps need MoveJump, not a spline — so callers
    // still drive jump points through the per-hop JumpTo branch.
    //
    // maxYards: stop the window once its accumulated 3D length reaches this many
    // yards (0 = unbounded, the historical behaviour). A spline window is a
    // MOVEMENT COMMITMENT — while the glide is healthy Advance performs no route
    // evaluation at all — so an unbounded window on a long route commits the tank
    // to ~400yd of unobserved travel, far past the ~35yd blocking-trash
    // lookahead (the heroic over-pull transit leg). The window always contains
    // at least one polyline point past the live position when one exists, so a
    // cap smaller than the next leg degrades to a single-hop window rather than
    // to an empty one (which would drop the caller into the MoveTo fallback and
    // reintroduce the per-point stutter).
    //
    // Pure read of `state` (does not mutate the cursor). Returns fewer than 2
    // points when the immediate next leg is a jump or the route is exhausted,
    // signalling the caller to fall back to single-hop movement.
    static std::vector<G3D::Vector3> BuildSplineWindow(Player* bot,
        ChunkedPathfinder::Result const& path, DungeonFollowerState const& state,
        float maxYards = 0.0f);

    // Can the bot walk a straight leg to `to` without cutting through static
    // geometry? A single VMAP raycast, the same one Resnap uses to screen cursor
    // candidates — exposed because the SPLINE WINDOW needs it too.
    //
    // BuildSplineWindow seeds window[0] with the bot's live position and then
    // appends route points verbatim, so the opening leg window[0] -> window[1] is
    // a straight segment that NOTHING screens. On the corridor that leg is a few
    // yards along the route and harmless, which is why it went unnoticed; off the
    // corridor it is a chord across whatever the route was bending around. The
    // off-line rejoin rung is the guard, and it only owns the tick past
    // OFF_PATH_THRESHOLD — so the residual band under it launched unscreened
    // chords. Live that put a BWL tank inside solid rock for five seconds
    // (tr-20260830-115416-5, anchors 16->18).
    //
    // Callers screen the opening leg ONLY when the bot is off the line far enough
    // for the leg to be a chord at all; the raycast is the expensive part of this
    // header and must not run on every healthy glide tick.
    static bool LegIsClear(Player* bot, G3D::Vector3 const& to);

    // Can the bot WALK a straight leg to `to` without leaving the navmesh? A
    // single dtNavMeshQuery::raycast from the bot's snapped poly, which stops at
    // the first navmesh boundary edge it crosses.
    //
    // WHY THIS EXISTS ALONGSIDE LegIsClear. LegIsClear is a static-VMAP
    // sightline, and a sightline does not care about the FLOOR. An open hole —
    // a pit, a moat, a gap over a lower deck — has no geometry in the way, so
    // the raycast sails across it and the leg screens CLEAN while the bot cannot
    // take a single step along it. The two probes answer different questions and
    // the opening leg needs both: LegIsClear rejects a chord through a wall,
    // this rejects a chord over a void.
    //
    // Live (tr-20260831-174013-100, Gundrak heroic): the tank stood on a narrow
    // spur north of the Colossus arena at (1632.0, 728.3, 142.9), 7.2yd off its
    // route, and the opening leg ran straight at (1666.9, 740.8) across the
    // navmesh void at x 1638-1648 / y <= 733. LegIsClear passed it — nothing is
    // built over that gap — so the window survived and the bot pushed into the
    // rim, moving 0.98yd NET while walking 202yd, reversing direction 142 times
    // over 3m47s. 96% of those reversals landed within a second of a posStuck
    // recovery: the ladder cycled resnap -> rebuild to completion, nine times
    // over, and every rung validated the ROUTE, which was never the broken part.
    //
    // Fails OPEN (returns true) with no map, no navmesh, or no poly under the
    // bot, matching DungeonClearGeometry's rule that an unqueryable geometry
    // check never blocks routing.
    static bool LegIsOnMesh(Player* bot, G3D::Vector3 const& to);

    // Should the opening leg be dropped from the window? The whole screen policy
    // in one pure predicate so it can be pinned by a test.
    //
    // Screening is BANDED: a bot within `releaseBand` of the line has an opening
    // leg that lies along the corridor and cannot cut a corner, and both probes
    // are far too expensive to run on every healthy glide tick. Outside the band
    // the leg is a chord the route never vouched for, and EITHER probe failing
    // condemns it — see LegIsOnMesh for why one is not enough.
    static bool DropOpeningLeg(size_t windowPoints, float deviation, float releaseBand,
                               bool losClear, bool onMesh);

    // Pure collection core of BuildSplineWindow (no Player — gtested directly).
    // Appends consecutive polyline points from cursor (seg, pt) to `window`,
    // which must already hold the live-position seed as its last element (length
    // accumulation starts from window.back()). Stops at the first jump leg, at
    // MAX_SPLINE_WINDOW_POINTS total, and — when maxYards > 0 — as soon as the
    // accumulated 3D length of the appended run reaches maxYards. The point that
    // CROSSES the cap is still appended (the window crosses the cap rather than
    // stopping short of it), and the cap is only tested after a point has been
    // appended, so at least one forward point always survives any cap.
    static void AppendWindowPoints(ChunkedPathfinder::Result const& path,
                                   uint32 seg, uint32 pt, float maxYards,
                                   std::vector<G3D::Vector3>& window);
};

#endif
