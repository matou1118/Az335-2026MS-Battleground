/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCNAVPENALTYREGISTRY_H
#define _PLAYERBOT_DCNAVPENALTYREGISTRY_H

#include <string_view>

#include "Define.h"

// Hand-authored "no-go" volumes for the dungeon-clear route producer.
//
// Some dungeon navmeshes contain shortcuts a real player cannot follow: the mmap
// generator stitches a walkable poly up a ledge/chasm face, so Detour's A* climbs
// a near-vertical wall instead of taking the intended ramp, and the human party
// (which can't make that climb) gets left behind. Slope alone can't separate these
// from legitimate steep ramps (the mmap generator's own walkable limit is 60°,
// which overlaps the shortcut's ~50°), so a targeted, hand-authored volume is the
// reliable lever for a known-bad spot.
//
// One row per known-bad spot: an axis-aligned world-space box on a map plus a
// cost multiplier. DcRouteFilter::getCost multiplies the cost of any A* edge
// whose midpoint falls inside the box by that factor, so the search prefers the
// legitimate way around. In the primary producer that is a COST, never a hard
// rejection (passFilter is untouched) — if the boxed edge is genuinely the only
// way through, the route still uses it. The StridedPathfinder fallback is the
// exception: it builds its corridor with the stock engine PathGenerator, which is
// blind to route costs, so there the region is a hard reject on any probe that
// ENTERS it.
//
// A ROW IS NEVER A CAGE. Both consumers exempt a route that BEGINS inside a
// region, because a party can legitimately be standing in one and a fence is only
// ever an instruction about where routes may GO. Author rows on that basis: an
// over-sized row costs you a slower route or a wider berth, not a stranded party.
// What it must NOT do is leave a pocket of walkable floor cut off on its far side
// — check the row against the navmesh, not against eyeballed wall coordinates.
//
// WING-SCOPED ROWS. A row may carry a `wing` token (DungeonWing::token). Such a
// row fences a wing OFF for every OTHER wing's run: a row tagged "ubrs" is active
// only while the run wing is set and is not "ubrs" — i.e. it keeps an LBRS run's
// routes out of Upper Blackrock Spire, and a UBRS run walks straight through it.
// An untagged row (every navmesh-shortcut row) is always active, as before. With
// no run wing at all (a caller that knows none, any map without an Explicit wing
// layout) the tagged rows are inactive, so they can never fence a party that has
// no wing to be kept in. The same rows double as the wing REGIONS
// (WingRegionAt): DcRunWing's fallback asks which wing the party stands in.
//
// Mirrors RoomAggroRegistry / BossRosterRegistry: adding a fix is a single table
// edit inside DungeonClear/, never an mmap regen or a core change.
struct DcNavPenaltyVolume
{
    uint32 mapId{0};
    float  minX{0.0f}, minY{0.0f}, minZ{0.0f};
    float  maxX{0.0f}, maxY{0.0f}, maxZ{0.0f};
    float  costMult{1.0f};   // edge-cost multiplier inside the box (>= 1)
    char const* wing{nullptr};  // wing region this row fences off; nullptr = always active
};

// Same contract as DcNavPenaltyVolume, but the XY footprint is a simple polygon
// (convex or not) plus a Z band instead of an axis-aligned box — for a bad spot
// whose shape is a room corner or curved edge that no single box can hug without
// also covering walkable floor. Vertices are world-space, in order around the
// ring; the first and last are implicitly joined. Up to 8 vertices; `vertCount`
// says how many of vx/vy are live.
struct DcNavPenaltyPolygon
{
    uint32 mapId{0};
    float  minZ{0.0f}, maxZ{0.0f};
    float  costMult{1.0f};
    uint32 vertCount{0};
    float  vx[8]{};
    float  vy[8]{};
    char const* wing{nullptr};  // as DcNavPenaltyVolume::wing
};

class DcNavPenaltyRegistry
{
public:
    // Every query takes the RUN WING (DcRunWing::FenceWing — "" when the run has
    // none), which decides whether the wing-tagged rows apply; see RowActive.

    // Whether a row tagged `rowWing` applies to a run in `runWing`: untagged rows
    // always; tagged rows only for a run in some OTHER wing.
    static bool RowActive(char const* rowWing, std::string_view runWing);

    // True iff `mapId` has at least one ACTIVE penalty volume or polygon. Cheap
    // early-out so getCost only does the per-edge region test on maps that
    // actually need it.
    static bool HasVolumes(uint32 mapId, std::string_view runWing = {});

    // The largest costMult of any active volume or polygon on `mapId` that
    // contains (x,y,z), or 1.0 when the point lies in no region. Pure (no game
    // state) — unit-testable.
    static float PenaltyAt(uint32 mapId, float x, float y, float z, std::string_view runWing = {});

    // True iff (x,y,z) lies inside some active region on `mapId`. The one place
    // the "penalised" threshold is spelled, so the consumers that ask "is the
    // party standing in a fence?" — the question that decides whether the fence
    // applies to a route at all — can't drift apart from PenaltyAt.
    static bool IsInsideRegion(uint32 mapId, float x, float y, float z, std::string_view runWing = {});

    // The wing token of the wing-tagged row containing (x,y,z) on `mapId`
    // whatever the run wing, or nullptr when the point is in no wing region.
    static char const* WingRegionAt(uint32 mapId, float x, float y, float z);
};

#endif
