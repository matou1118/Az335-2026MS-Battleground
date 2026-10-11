/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "ObjectiveHookRegistry.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

#include "Creature.h"
#include "Group.h"
#include "InstanceScript.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "Timer.h"

#include "Ai/Dungeon/DungeonClear/Data/DungeonBossInfo.h"
#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"
#include "Ai/Dungeon/DungeonClear/DcValueKeys.h"
#include "Ai/Dungeon/DungeonClear/Util/DcCombatFlag.h"
#include "Ai/Dungeon/DungeonClear/Util/DcFlightLeg.h"
#include "Ai/Dungeon/DungeonClear/Util/DcLeaderSignal.h"
#include "Ai/Dungeon/DungeonClear/Util/DcOculusDriverDecision.h"
#include "Ai/Dungeon/DungeonClear/Util/DcOculusFlightDecision.h"
#include "Ai/Dungeon/DungeonClear/Util/DcOculusPlan.h"
#include "Ai/Dungeon/DungeonClear/Util/DcPartyState.h"
#include "Ai/Dungeon/DungeonClear/Util/DcRezRecovery.h"
#include "Ai/Dungeon/DungeonClear/Util/DcRun.h"
#include "Ai/Dungeon/DungeonClear/Util/DcSmartRest.h"
#include "Ai/Dungeon/DungeonClear/Util/DcTargeting.h"
#include "Ai/Dungeon/DungeonClear/Util/DcThrottle.h"

// --- The Oculus (map 578) — the imperative half of the driver ----------------
//
// TWO hooks:
//
//   38  OcDriveAscent — the party plan: muster, fly, land, Eregos
//   39  OcEregosHold  — telemetry while the Eregos objective garrisons
//
// plus the plan memo every member reads (OculusPlanFor).
//
// THE DECISIONS ARE NOT HERE. They are the pure kernel DcOculusDriver::Decide in
// Util/DcOculusDriverDecision.h, tested without a map in t/TestOculus.cpp; this
// file is the census, the memo, the regroup and the telemetry. Nothing here moves
// a unit: each member steers its own drake from the rider rung.
//
// THE RETURN CONTRACT is the module's: Running (Hold / Regroup) claims the tank's
// tick, so the ordinary ladder — Advance above all, which cannot path between
// islands — stays off it; Done yields it to the objective's steps, the at-boss
// pipeline and the combat engine.

namespace
{
    using namespace DcOculus;
    using DcOculusFlight::Vec;

    Vec PosOf(WorldObject const* o)
    {
        return { o->GetPositionX(), o->GetPositionY(), o->GetPositionZ() };
    }

    Creature* FindEregos(Player* bot, InstanceScript* inst)
    {
        if (!bot || !inst)
            return nullptr;
        ObjectGuid const guid = inst->GetGuidData(DATA_EREGOS);
        if (guid.IsEmpty())
            return nullptr;
        Creature* const c = ObjectAccessor::GetCreature(*bot, guid);
        return c && c->IsAlive() ? c : nullptr;
    }

    // The Trial of the Champion's TocPartyReady, for the same reason: a hook has no
    // action to ask EventRestDecision on. Spread is not part of it.
    bool OcPartyReady(Player* owner, AiObjectContext* context)
    {
        if (!owner || !context)
            return true;
        if (DcSmartRest::Enabled(owner))
            return !DcSmartRest::UpdateLatch(owner, context);

        DcPartyState::RestGate const rest = DcPartyState::GetRestGate(owner, context);
        if (rest.minHp <= 0.0f && rest.minMp <= 0.0f)
            return true;
        return DcPartyState::IsPartyReady(owner, rest.minHp, rest.minMp, /*maxSpread*/ 100000.0f);
    }

    // --- the census ---------------------------------------------------------------

    struct Census
    {
        uint32 size = 0;      // alive, same map
        uint32 mounted = 0;   // ...on an Oculus drake
        uint32 landed = 0;    // ...whose drake has landed on dest
        bool   anyEngaged = false;
        bool   unreachableCorpse = false;
        bool   fallen = false;
    };

    Census CountParty(Player* owner, uint8 dest)
    {
        Census c;
        auto visit = [&](Player* m)
        {
            if (!m || !m->IsInWorld() || m->GetMapId() != owner->GetMapId())
                return;

            if (!m->IsAlive())
            {
                // A body is where the member died. One that died in the saddle hangs
                // in the air where its drake was — no poly within reach below it,
                // and no rezzer can stand within 20yd of it.
                Vec const body = PosOf(m);
                Vec snap;
                bool const onMesh = DcFlightLeg::SnapOnMesh(m->GetMap(), body.x, body.y, body.z, snap);
                if (!onMesh || body.z < BASEMENT_Z || std::fabs(body.z - snap.z) > LAND_TOLERANCE + 1.0f)
                    c.unreachableCorpse = true;
                return;
            }

            ++c.size;
            if (DcCombatFlag::IsEngaged(m))
                c.anyEngaged = true;

            if (Unit* const base = DcFlightLeg::OculusDrakeOf(m))
            {
                ++c.mounted;
                // A rider's fight is on its drake: the attackers hit the base, and the
                // rider's own combat state never shows it.
                if (base->GetVictim() || !base->getAttackers().empty())
                    c.anyEngaged = true;
                if (dest != SITE_NONE && DcFlightLeg::BaseLanded(base, dest))
                    ++c.landed;
                return;
            }
            if (m->GetVehicle())
                return;

            // On foot and off every poly, or down in the basement: a fall. Stranded
            // recovery is rider-blind by design and cannot bring it back.
            Vec const p = PosOf(m);
            Vec snap;
            if (p.z < BASEMENT_Z || !DcFlightLeg::SnapOnMesh(m->GetMap(), p.x, p.y, p.z, snap))
                c.fallen = true;
        };

        if (Group* const group = owner->GetGroup())
        {
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                visit(ref->GetSource());
        }
        else
            visit(owner);
        return c;
    }

    // --- telemetry ---------------------------------------------------------------------
    //
    // `DcOc ring=<n> site=<name> cc=<k>/10 mounted=<m>/<n> landed=<l> phase=<p> — <state>`
    // on every state change and every TELEMETRY_MS, at INFO so tools/dc_test_run.py
    // sees it by default. The run's whole ascent reads off this one line.
    void Telemetry(Player* owner, DcRunState& st, DcOculusDriver::Inputs const& in,
                   DcOculusDriver::Verdict const& v, uint32 ccCount)
    {
        bool const changed = st.ocDriverState != static_cast<uint8>(v.state);
        if (!changed && st.Throttled(DcThrottle::OcTelemetryLog, TELEMETRY_MS))
            return;
        if (changed)
        {
            st.ocDriverState = static_cast<uint8>(v.state);
            st.ocDriverStateMs = in.nowMs;
            st.ClearThrottle(DcThrottle::OcTelemetryLog);
            st.Throttled(DcThrottle::OcTelemetryLog, TELEMETRY_MS);
        }

        OcSite const& site = SiteRow(v.dest);
        LOG_INFO("playerbots.dungeonclear",
                 "[DC:{}] DcOc ring={} site={} cc={}/{} mounted={}/{} landed={} phase={}{} — {}{}",
                 owner->GetName(), v.dest == SITE_NONE ? 0 : site.ring,
                 v.dest == SITE_NONE ? "-" : site.name, ccCount, CC_TOTAL, in.mounted, in.partySize,
                 in.landed, DcOculusDriver::PhaseName(v.phase), v.hover ? " (hover)" : "",
                 DcOculusDriver::StateName(v.state), in.anyEngaged ? " (in combat)" : "");
    }

    // --- the refresh ---------------------------------------------------------------------

    void RefreshPlan(Player* owner, PlayerbotAI* ownerAI, DcRunState& st, uint32 now)
    {
        st.ocPlanStampMs = now ? now : 1;

        InstanceScript* const inst = DcTargeting::GetInstanceScript(owner);
        if (!inst)
        {
            st.ocPlanPhase = static_cast<uint8>(DcOculusDriver::Phase::Idle);
            st.ocPlanAction = static_cast<uint8>(DcOculusDriver::Action::Yield);
            return;
        }
        AiObjectContext* const ctx = ownerAI->GetAiObjectContext();

        DcOculusDriver::Inputs in;
        in.drakosDone = inst->GetData(DATA_DRAKOS) == STATE_DONE;
        in.eregosDone = inst->GetData(DATA_EREGOS) == STATE_DONE;

        std::optional<DungeonBossInfo> const next =
            ctx->GetValue<std::optional<DungeonBossInfo>>(DcKey::NextDungeonBoss)->Get();
        in.dest = next ? DcOculusDriver::SiteForRow(next->entry) : SITE_NONE;
        in.destHover = in.dest == SITE_R4 && !in.eregosDone;

        Creature* const eregos = FindEregos(owner, inst);
        in.eregosEngaged = eregos && eregos->IsInCombat();

        uint8 const dest = in.dest == SITE_NONE ? SITE_R4 : in.dest;

        in.tankAlive = owner->IsAlive();
        Unit* const tankBase = DcFlightLeg::OculusDrakeOf(owner);
        in.tankMounted = tankBase != nullptr;
        Vec const tankPos = tankBase ? PosOf(tankBase) : PosOf(owner);
        in.tankIsland = DcOculusFlight::IslandOf(tankPos.x, tankPos.y, tankPos.z);
        in.tankLanded = tankBase && !in.destHover && DcFlightLeg::BaseLanded(tankBase, dest);
        if (tankBase && in.destHover)
        {
            OcSite const& r4 = SITES[SITE_R4];
            in.tankAtStage = DcOculusFlight::Dist3d(tankPos, { r4.padX, r4.padY, r4.hoverZ }) <= STAGE_ARRIVE ||
                             (eregos && tankBase->GetExactDist(eregos) <= EREGOS_ENGAGE_RANGE);
        }

        Census const c = CountParty(owner, dest);
        in.partySize = std::max<uint32>(1, c.size);
        in.mounted = c.mounted;
        in.landed = c.landed;
        in.anyEngaged = c.anyEngaged;
        in.unreachableCorpse = c.unreachableCorpse;
        in.fallen = c.fallen;
        // Only asked out of combat and past Drakos — asking refreshes Smart Rest's latch.
        in.partyReady = (in.drakosDone && !c.anyEngaged && c.mounted == 0) ? OcPartyReady(owner, ctx) : true;

        in.nowMs = now;
        in.musterSinceMs = st.ocMusterSinceMs;
        in.landWaitSinceMs = st.ocLandWaitSinceMs;
        in.lastRegroupMs = st.ocLastRegroupMs;

        DcOculusDriver::Verdict const v = DcOculusDriver::Decide(in);

        st.ocMusterSinceMs = v.musterSinceMs;
        st.ocLandWaitSinceMs = v.landWaitSinceMs;

        // A NEW LEG — the riders re-plan from where their drakes are — when the party
        // takes to the air, or when where it is going (or whether it lands there)
        // changes. Fly -> Land is the same leg.
        auto flying = [](uint8 phase)
        {
            DcOculusDriver::Phase const p = static_cast<DcOculusDriver::Phase>(phase);
            return p == DcOculusDriver::Phase::Fly || p == DcOculusDriver::Phase::Land ||
                   p == DcOculusDriver::Phase::Eregos;
        };
        bool const nowFlying = flying(static_cast<uint8>(v.phase));
        if (nowFlying && (!flying(st.ocPlanPhase) || st.ocPlanDest != v.dest || st.ocPlanHover != v.hover))
            ++st.ocPlanLegSeq;

        st.ocPlanPhase = static_cast<uint8>(v.phase);
        st.ocPlanDest = v.dest;
        st.ocPlanHover = v.hover;
        st.ocPlanAction = static_cast<uint8>(v.action);
        st.ocPlanTankOnFootOnDest = in.tankAlive && !in.tankMounted && in.tankIsland == v.dest;
        st.ocPlanEregosEngaged = in.eregosEngaged;

        Telemetry(owner, st, in, v, inst->GetData(DATA_CC_COUNT));

        if (v.musterTimedOut && !st.Throttled(DcThrottle::OcWarn, WARN_THROTTLE_MS))
            LOG_WARN("playerbots.dungeonclear",
                     "[DC:{}] DcOc WARN muster timed out: {}/{} mounted after {}s — lifting off with "
                     "{} if that is at least {}. A member that never mounts is usually one whose Call "
                     "cast was interrupted (the 15s cooldown then passes with no drake).",
                     owner->GetName(), in.mounted, in.partySize, MUSTER_TIMEOUT_MS / 1000, in.mounted,
                     std::min<uint32>(MUSTER_MIN_ON_TIMEOUT, in.partySize));
    }

    // --- hook 38: THE ASCENT ----------------------------------------------------------------
    ObjectiveArriveResult OcDriveAscent(Player* bot, AiObjectContext* /*context*/,
                                        DungeonBossInfo const& /*info*/)
    {
        if (!bot || bot->GetMapId() != MAP_ID)
            return ObjectiveArriveResult::Done;

        DcOculusPlanView const plan = OculusPlanFor(bot);
        if (!plan.valid)
            return ObjectiveArriveResult::Done;

        switch (plan.action)
        {
            case DcOculusDriver::Action::Regroup:
                OculusRegroup(bot);
                return ObjectiveArriveResult::Running;
            case DcOculusDriver::Action::Hold:
                return ObjectiveArriveResult::Running;
            case DcOculusDriver::Action::Yield:
            default:
                return ObjectiveArriveResult::Done;
        }
    }

    // --- hook 39: the Eregos hold's telemetry ----------------------------------------------
    //
    // Runs while OBJ(8)'s garrison holds on the Ring 4 floor, i.e. from the landing
    // after the kill until the slot reads DONE — and, if the party ever walks onto
    // the floor with him alive, for the fight too. The result is ignored.
    ObjectiveArriveResult OcEregosHold(Player* bot, AiObjectContext* context, DungeonBossInfo const& /*info*/)
    {
        if (!bot || !context || bot->GetMapId() != MAP_ID)
            return ObjectiveArriveResult::Done;
        DcRunState& st = DcRun::Of(context);
        if (st.Throttled(DcThrottle::OcEregosLog, TELEMETRY_MS))
            return ObjectiveArriveResult::Done;

        InstanceScript* const inst = DcTargeting::GetInstanceScript(bot);
        Creature* const eregos = FindEregos(bot, inst);
        LOG_INFO("playerbots.dungeonclear",
                 "[DC:{}] DcOc eregos hold: state={} alive={} engaged={} hp={}% planarShift={}",
                 bot->GetName(), inst ? inst->GetData(DATA_EREGOS) : 0, eregos ? 1 : 0,
                 eregos && eregos->IsInCombat() ? 1 : 0, eregos ? static_cast<uint32>(eregos->GetHealthPct()) : 0,
                 eregos && eregos->HasAura(SPELL_PLANAR_SHIFT) ? 1 : 0);
        return ObjectiveArriveResult::Done;
    }
}

DcOculusPlanView OculusPlanFor(Player* bot)
{
    DcOculusPlanView view;
    if (!bot || bot->GetMapId() != MAP_ID)
        return view;

    Player* const owner = DcLeaderSignal::FindRunOwner(bot);
    PlayerbotAI* const ownerAI = owner ? GET_PLAYERBOT_AI(owner) : nullptr;
    if (!ownerAI || owner->GetMapId() != MAP_ID)
        return view;

    DcRunState& st = DcRun::Of(ownerAI);
    if (st.paused)
        return view;

    uint32 const now = getMSTime();
    if (!st.ocPlanStampMs || now - st.ocPlanStampMs >= PLAN_MEMO_MS)
        RefreshPlan(owner, ownerAI, st, now);

    view.valid = true;
    view.phase = static_cast<DcOculusDriver::Phase>(st.ocPlanPhase);
    view.action = static_cast<DcOculusDriver::Action>(st.ocPlanAction);
    view.dest = st.ocPlanDest;
    view.hover = st.ocPlanHover;
    view.legSeq = st.ocPlanLegSeq;
    view.tankOnFootOnDest = st.ocPlanTankOnFootOnDest;
    view.eregosEngaged = st.ocPlanEregosEngaged;
    view.owner = owner;
    return view;
}

uint32 OculusLaneOf(Player* bot)
{
    if (!bot)
        return 0;
    Player* const owner = DcLeaderSignal::FindRunOwner(bot);
    if (!owner || owner == bot)
        return 0;
    Group* const group = bot->GetGroup();
    if (!group)
        return 1;

    std::vector<uint64> guids;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* const m = ref->GetSource();
        if (!m || m == owner || m->GetMapId() != bot->GetMapId())
            continue;
        guids.push_back(m->GetGUID().GetRawValue());
    }
    std::sort(guids.begin(), guids.end());
    auto const it = std::find(guids.begin(), guids.end(), bot->GetGUID().GetRawValue());
    return it == guids.end() ? 1 : static_cast<uint32>(it - guids.begin()) + 1;
}

bool OculusRegroup(Player* bot)
{
    Player* const owner = bot ? DcLeaderSignal::FindRunOwner(bot) : nullptr;
    PlayerbotAI* const ownerAI = owner ? GET_PLAYERBOT_AI(owner) : nullptr;
    if (!ownerAI)
        return false;

    DcRunState& st = DcRun::Of(ownerAI);
    uint32 const now = getMSTime();
    if (st.ocLastRegroupMs && now - st.ocLastRegroupMs < REGROUP_COOLDOWN_MS)
        return false;
    st.ocLastRegroupMs = now ? now : 1;

    LOG_WARN("playerbots.dungeonclear",
             "[DC:{}] DcOc WARN regroup: a member cannot be reached on foot (a body in the air or off "
             "the mesh, or a member standing off it) — reviving the party at the portal landing. Dead "
             "bosses stay dead and the construct count is saved; the ascent resumes from Drakos's ring.",
             owner->GetName());
    return DcRezRecovery::RegroupAtEntrance(owner);
}

// Ids 38 and 39. Trial of the Champion's is 37; ids are ONE FLAT SPACE across every
// dungeon, and AddHook LOG_ERRORs a collision rather than silently dropping one.
void RegisterOculusHooks(ObjectiveHookRegistry::HookTable& out)
{
    ObjectiveHookRegistry::AddHook(out, DcOculus::HOOK_OC_DRIVER, &OcDriveAscent);
    ObjectiveHookRegistry::AddHook(out, DcOculus::HOOK_OC_EREGOS, &OcEregosHold);
}
