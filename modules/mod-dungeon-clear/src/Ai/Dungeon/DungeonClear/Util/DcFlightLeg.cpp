/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "Ai/Dungeon/DungeonClear/Util/DcFlightLeg.h"

#include "Log.h"
#include "Map.h"
#include "ModelIgnoreFlags.h"
#include "MotionMaster.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "Unit.h"

#include "Ai/Dungeon/DungeonClear/DcRunState.h"
#include "Ai/Dungeon/DungeonClear/Util/DcMovement.h"
#include "Ai/Dungeon/DungeonClear/Util/DcThrottle.h"
#include "Ai/Dungeon/DungeonClear/Util/NavmeshSnap.h"

namespace DcFlightLeg
{
    using namespace DcOculus;
    using DcOculusFlight::Vec;

    bool IsOculusDrake(Unit const* u)
    {
        if (!u)
            return false;
        uint32 const e = u->GetEntry();
        return e == NPC_RUBY_DRAKE || e == NPC_AMBER_DRAKE || e == NPC_EMERALD_DRAKE;
    }

    Unit* OculusDrakeOf(Player const* p)
    {
        if (!p)
            return nullptr;
        Unit* const base = p->GetVehicleBase();
        return IsOculusDrake(base) ? base : nullptr;
    }

    bool IsFlyingSafe(Unit const* u)
    {
        if (!u || !u->CanFly())
            return false;
        for (uint32 e : FLYING_SAFE_ENTRIES)
            if (u->GetEntry() == e)
                return true;
        return false;
    }

    bool HasGroundAttacker(Player const* bot, Unit const* base)
    {
        auto const ground = [](Unit const* u) { return u && u->IsAlive() && !IsFlyingSafe(u); };
        if (base)
        {
            if (ground(base->GetVictim()))
                return true;
            for (Unit const* a : base->getAttackers())
                if (ground(a))
                    return true;
        }
        if (bot)
            for (Unit const* a : bot->getAttackers())
                if (ground(a))
                    return true;
        return false;
    }

    // MoveSplineInit reads both off m_movementInfo: CAN_FLY | DISABLE_GRAVITY makes
    // the chord a flying spline, and FLYING is what MovementInfo::GetSpeedType maps
    // to MOVE_FLIGHT — without it the drake flies at MOVE_RUN. Unit::SetCharmedBy
    // adds FLYING when a player takes the reins ("fixes speed"), and the drake's own
    // MovementInform clears DISABLE_GRAVITY when it lands next to its summoner.
    constexpr uint32 FLIGHT_FLAGS = MOVEMENTFLAG_CAN_FLY | MOVEMENTFLAG_DISABLE_GRAVITY | MOVEMENTFLAG_FLYING;

    void PrepareBase(Player* bot, Unit* base, DcRunState& st)
    {
        if (!bot || !base || st.ocBaseGuid == base->GetGUID())
            return;
        st.ocBaseGuid = base->GetGUID();

        bot->SetClientControl(base, /*allowMove*/ false, /*packetOnly*/ true);
        base->AddUnitState(UNIT_STATE_IGNORE_PATHFINDING);

        uint32 const flags = base->GetUnitMovementFlags();
        LOG_INFO("playerbots.dungeonclear",
                 "[DC:{}] DcOc base prepared: {} entry {} canFly={} disableGravity={} flying={} CanFly()={}{}",
                 bot->GetName(), base->GetName(), base->GetEntry(), (flags & MOVEMENTFLAG_CAN_FLY) ? 1 : 0,
                 (flags & MOVEMENTFLAG_DISABLE_GRAVITY) ? 1 : 0, (flags & MOVEMENTFLAG_FLYING) ? 1 : 0,
                 base->CanFly() ? 1 : 0, (flags & FLIGHT_FLAGS) != FLIGHT_FLAGS ? " (fly flags written)" : "");
    }

    Move MoveBase(Player* bot, Unit* base, DcRunState& st, float x, float y, float z)
    {
        if (!bot || !base)
            return Move::Refused;
        if (PlayerbotAI* ai = GET_PLAYERBOT_AI(bot))
            if (!DcMovement::DcMovementAllowed(ai))
                return Move::Refused;

        PrepareBase(bot, base, st);

        bool const throttled =
            st.ThrottledIssue(DcThrottle::OcMoveIssue, x, y, z, /*epsilon*/ 1.0f, MOVE_REISSUE_MS);
        if (throttled && base->isMoving())
            return Move::UnderWay;

        MotionMaster* const mm = base->GetMotionMaster();
        if (!mm)
            return Move::Refused;
        mm->Clear();
        // Every issue, not once per drake: a rider with a real client (self-bot mode)
        // still sends movement for its drake whenever no spline is running, and
        // HandleMoverRelocation copies those flags over the base's — FLYING dropped.
        // SetCanFly on a client-controlled unit waits for an ack no bot sends, so the
        // flags are written directly.
        base->AddUnitMovementFlag(FLIGHT_FLAGS);
        mm->MovePoint(/*id*/ 0, x, y, z, FORCED_MOVEMENT_NONE, /*speed*/ 0.0f, /*orientation*/ 0.0f,
                      /*generatePath*/ false, /*forceDestination*/ true);
        return Move::Issued;
    }

    bool SnapOnMesh(Map const* map, float x, float y, float z, Vec& out)
    {
        if (!map)
            return false;
        NavmeshSnap::Result const r = NavmeshSnap::Snap(map, x, y, z, SNAP_RADIUS, SNAP_VERT);
        if (!r.ok)
            return false;
        out = { r.x, r.y, r.z };
        return true;
    }

    bool BaseLanded(Unit const* base, uint8 destSite)
    {
        if (!base)
            return false;
        Vec const b{ base->GetPositionX(), base->GetPositionY(), base->GetPositionZ() };
        Vec snap;
        bool const ok = SnapOnMesh(base->GetMap(), b.x, b.y, b.z, snap);
        return DcOculusFlight::Landed(b, ok, snap, destSite);
    }

    bool ChordClear(Map const* map, Vec const& a, Vec const& b)
    {
        if (!map)
            return true;
        return map->isInLineOfSight(a.x, a.y, a.z, b.x, b.y, b.z, PHASEMASK_NORMAL, LINEOFSIGHT_CHECK_VMAP,
                                    VMAP::ModelIgnoreFlags::Nothing);
    }
}
