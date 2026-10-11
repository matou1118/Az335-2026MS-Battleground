/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCOCULUSFLIGHTDECISION_H
#define _PLAYERBOT_DCOCULUSFLIGHTDECISION_H

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "Define.h"

#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"

// PURE kernel for one Oculus flight leg — everything the rider rung decides about
// WHERE its drake goes, lifted out of the world so it can be tested without a map.
// The glue (the base mover, the LOS query, the snap) lives in Util/DcFlightLeg.cpp.
//
// NOTHING ELSE IN THE MODULE CAN FLY. Advance, the long-range pathfinder, the
// follower, stuck detection and the rez walk all assume a walking bot on a
// connected mesh. Rather than teach that ladder to fly, a leg is its own
// primitive: a short list of straight chords on the drake itself, planned once
// when the leg starts —
//
//   rise    straight up from where the drake stands to the cruise altitude;
//           or, when something is overhead (Ring 3 above the central ring, the
//           Ring 4 floor above Urom's arena), a short lift, a slide to the
//           destination's COLUMN — an open vertical, the central shaft by
//           default — and the climb there;
//   cruise  across at max(source hover, destination hover), on this rider's lane;
//   final   straight down onto its landing point LAND_HOVER above the pad, or
//           level at the destination's hover altitude for a site the party only
//           hovers over (Eregos's).
//
// Every chord is LOS-checked when the leg is planned and a blocked one is
// REPORTED (LegPlan::blocked) rather than silently trusted: the site table only
// knows the geometry it was probed against.
namespace DcOculusFlight
{
    // Not M_PI: MSVC only defines the M_* macros under _USE_MATH_DEFINES.
    constexpr float kPi = 3.14159265358979f;

    struct Vec
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    inline float Dist2d(Vec const& a, Vec const& b) { return std::hypot(a.x - b.x, a.y - b.y); }

    inline float Dist3d(Vec const& a, Vec const& b)
    {
        float const dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    // The island (site) a point stands on, or SITE_NONE — in the air, in the
    // basement, or anywhere the site table does not describe. First match wins;
    // the only overlapping circles (Drakos's ring and the entrance floor, 149yd
    // apart with radii 55 and 110) overlap over a gap with no mesh in it.
    inline uint8 IslandOf(float x, float y, float z)
    {
        using namespace DcOculus;
        for (uint8 s = 0; s < SITE_COUNT; ++s)
        {
            OcSite const& r = SITES[s];
            if (std::fabs(z - r.padZ) <= r.islandZBand &&
                std::hypot(x - r.islandX, y - r.islandY) <= r.islandRadius)
                return s;
        }
        return SITE_NONE;
    }

    // Where rider `lane` sets down on `site`. Lane 0 — the tank, and every rider's
    // last-resort point — is the pad itself; the others sit on a circle of the
    // site's landRadius around it, 72 degrees apart, so five drakes never share a
    // point and none of them comes down on the rim. A sixth and later rider (not
    // a five-man shape, but the kernel must not stack them) takes a tighter
    // circle rotated half a step.
    inline Vec PadLanePoint(uint8 site, uint32 lane)
    {
        using namespace DcOculus;
        OcSite const& r = SiteRow(site);
        if (lane == 0)
            return { r.padX, r.padY, r.padZ };

        uint32 const ring = (lane - 1) / 5;
        float const step = 2.0f * kPi / 5.0f;
        float const ang = static_cast<float>((lane - 1) % 5) * step + static_cast<float>(ring) * step * 0.5f;
        float const rad = r.landRadius / static_cast<float>(1 + ring);
        return { r.padX + std::cos(ang) * rad, r.padY + std::sin(ang) * rad, r.padZ };
    }

    // The cruise lane: rider `lane` flies displaced perpendicular to its chord,
    // alternating sides at LANE_SPACING steps, and LANE_Z higher per lane. Lane 0
    // flies the chord itself.
    inline Vec LaneOffset(Vec const& from, Vec const& to, uint32 lane)
    {
        using namespace DcOculus;
        if (lane == 0)
            return {};
        float dx = to.x - from.x, dy = to.y - from.y;
        float const len = std::hypot(dx, dy);
        float px = 1.0f, py = 0.0f;
        if (len > 0.1f)
        {
            px = -dy / len;
            py = dx / len;
        }
        float const side = (lane % 2) ? 1.0f : -1.0f;
        float const mag = LANE_SPACING * static_cast<float>((lane + 1) / 2);
        return { px * side * mag, py * side * mag, LANE_Z * static_cast<float>(lane) };
    }

    constexpr uint8 kMaxWaypoints = 6;

    struct LegPlan
    {
        Vec   wp[kMaxWaypoints]{};
        uint8 count = 0;
        bool  blocked = false;     // some chord failed the LOS check when planned
        bool  viaColumn = false;   // the straight rise was blocked; the leg climbs at the column
    };

    // Plan a leg from `start` (the drake, where it hovers now) to `destSite`.
    //
    // `srcSite` is the island the rider stood on when the leg began, or SITE_NONE
    // when it was already in the air; `landing` is false for a hover-only site.
    // `los(a, b)` answers "is the straight chord a->b clear" — the static VMAP in
    // the glue, a fake in the tests.
    template <class LosFn>
    inline LegPlan PlanLeg(Vec const& start, uint8 srcSite, uint8 destSite, uint32 lane, bool landing,
                           LosFn const& los)
    {
        using namespace DcOculus;
        OcSite const& d = SiteRow(destSite);

        float const srcHover = srcSite < SITE_COUNT ? SITES[srcSite].hoverZ : start.z + LIFT_MIN;
        float const cruise = std::max({ srcHover, d.hoverZ, start.z }) + LANE_Z * static_cast<float>(lane);

        LegPlan p;
        auto push = [&p](Vec const& v)
        {
            if (p.count < kMaxWaypoints)
                p.wp[p.count++] = v;
        };
        auto check = [&p, &los](Vec const& a, Vec const& b)
        {
            if (!los(a, b))
                p.blocked = true;
        };

        Vec const pad = PadLanePoint(destSite, lane);
        Vec cur = start;

        Vec const rise{ start.x, start.y, cruise };
        if (los(cur, rise))
        {
            push(rise);
            cur = rise;
        }
        else
        {
            p.viaColumn = true;
            Vec const lift{ start.x, start.y, start.z + COLUMN_SLIDE_Z };
            Vec const slide{ d.columnX, d.columnY, lift.z };
            Vec const climb{ d.columnX, d.columnY, cruise };
            check(cur, lift);
            push(lift);
            check(lift, slide);
            push(slide);
            check(slide, climb);
            push(climb);
            cur = climb;
        }

        Vec const off = LaneOffset(cur, pad, lane);
        Vec const cruiseEnd{ pad.x + off.x, pad.y + off.y, cruise };
        check(cur, cruiseEnd);
        push(cruiseEnd);

        Vec const last = landing ? Vec{ pad.x, pad.y, pad.z + LAND_HOVER }
                                 : Vec{ pad.x, pad.y, d.hoverZ + LANE_Z * static_cast<float>(lane) };
        check(cruiseEnd, last);
        push(last);
        return p;
    }

    // Advance the cursor past every waypoint the drake is already at, and name the
    // one to fly to next. `arrived` once the last one is reached.
    struct Step
    {
        bool  arrived = false;
        uint8 cursor = 0;
        Vec   target{};
    };

    inline Step Advance(Vec const& base, LegPlan const& plan, uint8 cursor)
    {
        using namespace DcOculus;
        Step s;
        s.cursor = cursor;
        while (s.cursor < plan.count && Dist3d(base, plan.wp[s.cursor]) <= LEG_ARRIVE)
            ++s.cursor;
        if (plan.count == 0 || s.cursor >= plan.count)
        {
            s.arrived = true;
            s.cursor = plan.count;
            if (plan.count)
                s.target = plan.wp[plan.count - 1];
            return s;
        }
        s.target = plan.wp[s.cursor];
        return s;
    }

    // Progress is measured on the BASE — a passenger's own isMoving() is false for
    // the whole ride, so the module's walking stuck detection is blind up here.
    // `progressSinceMs` is when the drake last moved LEG_PROGRESS_YD.
    inline bool Stalled(uint32 nowMs, uint32 progressSinceMs)
    {
        return progressSinceMs && nowMs - progressSinceMs >= DcOculus::LEG_STALL_MS;
    }

    // THE ONLY DISMOUNT GATE. The drake must hover within LAND_TOLERANCE above a
    // snapped navmesh point, within LAND_SNAP_2D beside it, and that point must be
    // on the destination island. Anything else is air: a clientless bot stepping
    // off there stands in it until its next move clamps it to whatever is below —
    // usually the basement, 250yd down.
    inline bool Landed(Vec const& base, bool snapOk, Vec const& snap, uint8 destSite)
    {
        using namespace DcOculus;
        if (!snapOk || destSite >= SITE_COUNT)
            return false;
        float const dz = base.z - snap.z;
        if (dz < -0.5f || dz > LAND_TOLERANCE)
            return false;
        if (Dist2d(base, snap) > LAND_SNAP_2D)
            return false;
        return IslandOf(snap.x, snap.y, snap.z) == destSite;
    }

    // Rider `lane`'s station on Eregos: EREGOS_STATION_RANGE out on its own
    // bearing, the bearings EREGOS_LANE_DEG apart and centred on the direction the
    // party arrived from (the Ring 4 pad), at his altitude staggered by lane.
    // Planar Shift (heroic) pushes every station PLANAR_SCATTER_OUT further out and
    // PLANAR_SCATTER_UP higher — the anomalies chase at their own speed, and
    // distance is the only defence.
    inline Vec EregosStation(Vec const& eregos, uint32 lane, bool planarShift, float range)
    {
        using namespace DcOculus;
        OcSite const& r4 = SITES[SITE_R4];
        float const base = std::atan2(r4.padY - eregos.y, r4.padX - eregos.x);
        // 0, +1, -1, +2, -2 ... so the lanes fan out either side of the approach.
        int32 const signedIdx = (lane % 2) ? static_cast<int32>((lane + 1) / 2) : -static_cast<int32>(lane / 2);
        float const idx = static_cast<float>(signedIdx);
        float const bearing = base + idx * EREGOS_LANE_DEG * kPi / 180.0f;
        float const dist = range + (planarShift ? PLANAR_SCATTER_OUT : 0.0f);
        float const dz = ((lane % 2) ? 1.0f : -1.0f) * LANE_Z * static_cast<float>((lane + 1) / 2) +
                         (planarShift ? PLANAR_SCATTER_UP : 0.0f);
        return { eregos.x + std::cos(bearing) * dist, eregos.y + std::sin(bearing) * dist, eregos.z + dz };
    }
}

#endif  // _PLAYERBOT_DCOCULUSFLIGHTDECISION_H
