/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCFLIGHTLEG_H
#define _PLAYERBOT_DCFLIGHTLEG_H

#include "Define.h"

#include "Ai/Dungeon/DungeonClear/Util/DcOculusFlightDecision.h"

class Map;
class Player;
class Unit;
struct DcRunState;

// The IMPERATIVE half of an Oculus flight leg: the drake mover, the landing probe
// and the chord check. The decisions are DcOculusFlightDecision.h.
//
// WHY THE DRAKE AND NOT THE BOT. On a vehicle the bot is the passenger: its
// position is the seat's, and a generator on its own MotionMaster walks nothing
// (the Trial of the Champion lesson). A player-controlled vehicle base honours
// MotionMaster::MovePoint — the root from AddPassenger lands on the passenger, not
// the base — and the drake's CanFly() is unconditionally true, so with
// generatePath=false, forceDestination=true and UNIT_STATE_IGNORE_PATHFINDING the
// spline is a straight flying chord to the exact point.
namespace DcFlightLeg
{
    // One of the three Oculus drakes (Ruby / Amber / Emerald).
    bool IsOculusDrake(Unit const* u);

    // The drake `p` rides, or nullptr.
    Unit* OculusDrakeOf(Player const* p);

    // One of FLYING_SAFE_ENTRIES and able to fly: the only hostiles a drake bar
    // spell may hit without killing its rider.
    bool IsFlyingSafe(Unit const* u);

    // A live hostile that is NOT flying-safe — a construct, a Ring-Lord — on the
    // drake or on its rider. A picket alone is a fight the saddle answers; this is
    // one it cannot.
    bool HasGroundAttacker(Player const* bot, Unit const* base);

    // Once per drake (latched on its GUID): the packet-only client-control hand-off
    // zone_borean_tundra uses "to make the waypoints function", pathfinding off on
    // the base, and a log of the fly flags it arrived with.
    void PrepareBase(Player* bot, Unit* base, DcRunState& st);

    enum class Move : uint8
    {
        Refused,   // paused run, no MotionMaster
        UnderWay,  // the same point is already being flown to
        Issued,    // a new MovePoint went out this tick
    };

    // Point-move the drake to (x, y, z). Re-issues the same point only after
    // MOVE_REISSUE_MS, or at once if the drake has stopped short of it. Writes the
    // fly flags (FLYING included, which selects MOVE_FLIGHT speed) on every issue.
    Move MoveBase(Player* bot, Unit* base, DcRunState& st, float x, float y, float z);

    // Nearest navmesh point within SNAP_RADIUS x SNAP_VERT.
    bool SnapOnMesh(Map const* map, float x, float y, float z, DcOculusFlight::Vec& out);

    // DcOculusFlight::Landed on the drake's live position.
    bool BaseLanded(Unit const* base, uint8 destSite);

    // Static-VMAP line of sight: true = the chord is clear.
    bool ChordClear(Map const* map, DcOculusFlight::Vec const& a, DcOculusFlight::Vec const& b);
}

#endif  // _PLAYERBOT_DCFLIGHTLEG_H
