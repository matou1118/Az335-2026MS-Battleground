/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DcHazard.h"

#include "DynamicObject.h"
#include "GameObject.h"
#include "Playerbots.h"
#include "Ai/Dungeon/DungeonClear/DcValueKeys.h"
#include "Ai/Dungeon/DungeonClear/Data/DcHazardRegistry.h"

#include <cmath>
#include <limits>

namespace
{
    // Resolve the bot's value context, or nullptr. The sampler needs it.
    AiObjectContext* ContextOf(Player* bot)
    {
        if (!bot)
            return nullptr;
        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        return botAI ? botAI->GetAiObjectContext() : nullptr;
    }

    // Is (px,py,pz) inside `h`'s DANGER band? Same-floor, and within the row's own
    // pulse + holdBand — so a bot already parked past the rim still reads clear
    // and one that has drifted back toward it does not. Non-vacate rows (and dead
    // creature emitters, which have stopped pulsing) are never in danger.
    bool InDangerBand(DcHazard::LiveHazard const& h, float px, float py, float pz)
    {
        if (h.vacateRadius <= 0.0f || !h.alive)
            return false;

        if (std::fabs(h.z - pz) > h.zBand)
            return false;

        float const reach = h.vacateRadius + h.holdBand;
        float const dx = px - h.x, dy = py - h.y;
        return dx * dx + dy * dy <= reach * reach;
    }
}

DcHazard::LiveSet DcHazard::Sample(Player* bot)
{
    LiveSet live;
    if (!bot)
        return live;

    // Cheap registry early-outs before anything touches game state. Each kind is
    // gated on its own bool, not on HasAnyHazard, so a map that registers only a
    // pool (Scholomance) or only a trap (the Shattered Halls) never pulls — and
    // therefore never recalculates — the two values it has no rows for.
    uint32 const mapId = bot->GetMapId();
    bool const wantEmitters = DcHazardRegistry::HasEmitters(mapId);
    bool const wantGround   = DcHazardRegistry::HasGroundHazards(mapId);
    bool const wantTraps    = DcHazardRegistry::HasTrapHazards(mapId);
    if (!wantEmitters && !wantGround && !wantTraps)
        return live;

    AiObjectContext* ctx = ContextOf(bot);
    if (!ctx)
        return live;

    // Order matters and is the registry's: creature emitters, then ground pools,
    // then traps. NearestVacate keeps the FIRST strictly-nearest centre it sees,
    // so a different order could re-elect a different emitter on a tie.
    if (wantEmitters)
    {
        GuidVector const& hazards = ctx->GetValue<GuidVector>(DcKey::Hazards)->Get();
        live.reserve(live.size() + hazards.size());
        for (ObjectGuid guid : hazards)
        {
            Unit* u = ObjectAccessor::GetUnit(*bot, guid);
            if (!u)
                continue;

            // Re-check the registry on the resolved unit rather than trusting the
            // cached set: the value can be up to 500ms stale, and a bot that has
            // changed map in that window would otherwise measure against an
            // emitter from the instance it just left.
            DcHazardEmitter const* e = DcHazardRegistry::Find(u->GetMapId(), u->GetEntry());
            if (!e || u->GetMapId() != mapId)
                continue;

            live.push_back(LiveHazard{ u->GetPositionX(), u->GetPositionY(), u->GetPositionZ(),
                                       e->radius, e->zBand,
                                       e->vacateRadius, e->holdBand, e->retreatSlack,
                                       u->IsAlive() });
        }
    }

    if (wantGround)
    {
        // A persistent area aura is not a unit, so these guids resolve through
        // GetDynamicObject — ObjectAccessor::GetUnit on one returns nullptr and
        // the pool would read as clean ground.
        //
        // Ground pools have no liveness flag to test: a pool exists until its
        // duration expires and the DynamicObject is removed, so resolving the
        // guid IS the liveness check.
        GuidVector const& pools = ctx->GetValue<GuidVector>(DcKey::GroundHazards)->Get();
        live.reserve(live.size() + pools.size());
        for (ObjectGuid guid : pools)
        {
            DynamicObject* d = ObjectAccessor::GetDynamicObject(*bot, guid);
            if (!d)
                continue;

            DcGroundHazard const* g = DcHazardRegistry::FindGround(d->GetMapId(), d->GetSpellId());
            if (!g || d->GetMapId() != mapId)
                continue;

            live.push_back(LiveHazard{ d->GetPositionX(), d->GetPositionY(), d->GetPositionZ(),
                                       g->radius, g->zBand,
                                       g->vacateRadius, g->holdBand, g->retreatSlack,
                                       true });
        }
    }

    if (wantTraps)
    {
        // A GAMEOBJECT_TYPE_TRAP is neither a unit nor a dynamic object: BOTH of
        // the resolvers above return nullptr on one of these guids and the fire
        // would read as clean ground. As with a pool, the guid going dead IS how
        // expiry is observed — a Blaze is removed when its 60s duration runs out.
        // Its loot state is deliberately not consulted: a trap cycles READY ->
        // ACTIVATED -> READY every couple of seconds, and "mid-cast right now" is
        // not the difference between safe and unsafe ground.
        GuidVector const& traps = ctx->GetValue<GuidVector>(DcKey::TrapHazards)->Get();
        live.reserve(live.size() + traps.size());
        for (ObjectGuid guid : traps)
        {
            GameObject* go = ObjectAccessor::GetGameObject(*bot, guid);
            if (!go)
                continue;

            DcTrapHazard const* t = DcHazardRegistry::FindTrap(go->GetMapId(), go->GetEntry());
            if (!t || go->GetMapId() != mapId)
                continue;

            live.push_back(LiveHazard{ go->GetPositionX(), go->GetPositionY(), go->GetPositionZ(),
                                       t->radius, t->zBand,
                                       t->vacateRadius, t->holdBand, t->retreatSlack,
                                       true });
        }
    }

    return live;
}

bool DcHazard::PointIsHot(LiveSet const& live, float x, float y, float z)
{
    for (LiveHazard const& h : live)
        if (DcHazardRegistry::PointInCylinder(h.radius, h.zBand, h.x, h.y, h.z, x, y, z))
            return true;
    return false;
}

bool DcHazard::SegmentIsHot(LiveSet const& live, float ax, float ay, float az,
                            float bx, float by, float bz)
{
    for (LiveHazard const& h : live)
        if (DcHazardRegistry::SegmentClipsCylinder(h.radius, h.zBand, h.x, h.y, h.z,
                                                   ax, ay, az, bx, by, bz))
            return true;
    return false;
}

bool DcHazard::PointIsInVacateBand(LiveSet const& live, float x, float y, float z)
{
    for (LiveHazard const& h : live)
        if (InDangerBand(h, x, y, z))
            return true;
    return false;
}

DcHazard::VacateEmitter DcHazard::NearestVacate(LiveSet const& live, float px, float py, float pz)
{
    VacateEmitter best;
    float bestDistSq = std::numeric_limits<float>::max();

    for (LiveHazard const& h : live)
    {
        if (!InDangerBand(h, px, py, pz))
            continue;

        float const dx = px - h.x, dy = py - h.y;
        float const distSq = dx * dx + dy * dy;
        if (distSq >= bestDistSq)
            continue;

        bestDistSq = distSq;
        best.ok = true;
        best.x = h.x;
        best.y = h.y;
        best.z = h.z;
        best.pulseRadius = h.vacateRadius;
        best.retreatSlack = h.retreatSlack;
    }

    return best;
}

// ---- the Player* entry points ----------------------------------------------
//
// One sample, one answer. A caller that asks about several points in the same
// tick should sample once itself and use the LiveSet forms above.

bool DcHazard::PointIsHot(Player* bot, float x, float y, float z)
{
    if (!bot)
        return false;
    return PointIsHot(Sample(bot), x, y, z);
}

bool DcHazard::SegmentIsHot(Player* bot, float ax, float ay, float az,
                            float bx, float by, float bz)
{
    if (!bot)
        return false;
    return SegmentIsHot(Sample(bot), ax, ay, az, bx, by, bz);
}

bool DcHazard::LegIsHot(Player* bot, float x, float y, float z)
{
    if (!bot)
        return false;
    return SegmentIsHot(bot,
                        bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(),
                        x, y, z);
}

DcHazard::VacateEmitter DcHazard::NearestVacate(Player* bot)
{
    if (!bot)
        return VacateEmitter{};
    return NearestVacate(Sample(bot),
                         bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
}

bool DcHazard::PointIsInVacateBand(Player* bot, float x, float y, float z)
{
    if (!bot)
        return false;
    return PointIsInVacateBand(Sample(bot), x, y, z);
}
