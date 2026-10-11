/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCFORMATION_H
#define _PLAYERBOT_DCFORMATION_H

#include <algorithm>
#include <cmath>

#include "Define.h"

// Shared formation geometry for parking a party/raid around an anchor (camp
// fan, follow fan, post-drop scatter). Pure arithmetic, header-only, kernel
// tested.
//
// The 5-man fans hardcoded a 1-2yd circle, which is exactly right for 4
// followers and a mosh pit for 39: the circumference must GROW with the
// population or everyone stacks. The primitives here keep the fan deterministic
// per bot (seeded by GUID counter — MoveTo dedups an unchanging destination, so
// each bot glides to one spot and parks) while sizing the ring from how many
// bodies actually stand on it, and optionally splitting roles onto concentric
// rings (melee in, ranged next, healers out) so a raid camp reads as a
// formation instead of a blob.
namespace DcFormation
{
    // Arc length each member needs on its ring. ~2.5yd keeps neighbors outside
    // the stock anti-stack shuffle's contact distance without spreading a small
    // party wider than the old fixed fan.
    inline constexpr float kSlotSpacingYd = 2.5f;

    // Radial gap between consecutive role rings.
    inline constexpr float kRingGapYd = 3.0f;

    // The golden angle (radians): consecutive seeds land maximally apart, so
    // any subset of bots fans evenly without coordination.
    inline constexpr float kGoldenAngle = 2.39996323f;

    // Radius of one ring holding `count` members at kSlotSpacingYd spacing,
    // never below `baseRadius` (the small-party floor — 5-man fans keep their
    // familiar tight circle through it).
    inline float RingRadius(uint32 count, float baseRadius)
    {
        float const need =
            static_cast<float>(count) * kSlotSpacingYd / (2.0f * 3.14159265f);
        return std::max(baseRadius, need);
    }

    // The three concentric role rings for the given role census. Melee hug the
    // anchor; ranged ring opens outside the melee ring; healers outside that.
    // Each ring is sized for ITS population, then pushed outside its inner
    // neighbor by kRingGapYd.
    struct RoleRings
    {
        float melee;
        float ranged;
        float healer;
    };

    inline RoleRings ComputeRoleRings(uint32 meleeCount, uint32 rangedCount,
                                      uint32 healerCount, float baseRadius = 1.5f)
    {
        RoleRings r{};
        r.melee  = RingRadius(meleeCount, baseRadius);
        r.ranged = std::max(RingRadius(rangedCount, baseRadius), r.melee + kRingGapYd);
        r.healer = std::max(RingRadius(healerCount, baseRadius), r.ranged + kRingGapYd);
        return r;
    }

    // Deterministic slot offset on a ring: golden-angle direction from the
    // seed, ring radius plus up to 1yd of seeded variance so a ring reads as a
    // loose arc, not a drill formation.
    struct Offset
    {
        float dx;
        float dy;
    };

    inline Offset SlotOffset(uint32 seed, float ringRadius)
    {
        float const angle = static_cast<float>(seed) * kGoldenAngle;
        float const radius =
            ringRadius + static_cast<float>(seed % 101) / 100.0f;  // [+0, +1]
        return { radius * std::cos(angle), radius * std::sin(angle) };
    }

    // Follow-fan angle for a raid: the member's SUBGROUP picks one of eight
    // ring segments behind/around the tank, and the seed fans within it — a
    // 40-bot raid trails as eight short arcs instead of one conga line whose
    // tail laps the corridor. `subGroup` is Player::GetSubGroup() (0..7).
    inline float RaidFollowAngle(uint8 subGroup, uint32 seed)
    {
        constexpr float kTwoPi = 2.0f * 3.14159265f;
        constexpr float kSegment = kTwoPi / 8.0f;
        float const center = static_cast<float>(subGroup % 8) * kSegment;
        // Seeded fan within ±kSegment/2 of the segment center.
        float const within =
            (static_cast<float>(seed % 101) / 100.0f - 0.5f) * kSegment;
        float angle = std::fmod(center + within, kTwoPi);
        if (angle < 0.0f)
            angle += kTwoPi;
        return angle;
    }
}

#endif  // _PLAYERBOT_DCFORMATION_H
