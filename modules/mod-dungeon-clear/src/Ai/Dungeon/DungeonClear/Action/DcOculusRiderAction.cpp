/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include <cmath>

#include "Creature.h"
#include "Group.h"
#include "InstanceScript.h"
#include "Item.h"
#include "Log.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "SharedDefines.h"
#include "Spell.h"
#include "Timer.h"
#include "Vehicle.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include "Ai/Dungeon/DungeonClear/Action/DungeonClearActions.h"
#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"
#include "Ai/Dungeon/DungeonClear/DcRunState.h"
#include "Ai/Dungeon/DungeonClear/DcValueKeys.h"
#include "Ai/Dungeon/DungeonClear/Trigger/DungeonClearTriggers.h"
#include "Ai/Dungeon/DungeonClear/Util/DcCombatFlag.h"
#include "Ai/Dungeon/DungeonClear/Util/DcCoreCompat.h"
#include "Ai/Dungeon/DungeonClear/Util/DcFlightLeg.h"
#include "Ai/Dungeon/DungeonClear/Util/DcFormGate.h"
#include "Ai/Dungeon/DungeonClear/Util/DcMovement.h"
#include "Ai/Dungeon/DungeonClear/Util/DcOculusDriverDecision.h"
#include "Ai/Dungeon/DungeonClear/Util/DcOculusFlightDecision.h"
#include "Ai/Dungeon/DungeonClear/Util/DcOculusPlan.h"
#include "Ai/Dungeon/DungeonClear/Util/DcRun.h"
#include "Ai/Dungeon/DungeonClear/Util/DcTargeting.h"
#include "Ai/Dungeon/DungeonClear/Util/DcThrottle.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonEventExecutor.h"

// The Oculus — the PER-MEMBER half of the ascent: the rider rung.
//
// The driver (hook 38, Overrides/OculusDriver.cpp) decides what the PARTY is
// doing and publishes it as the plan. This rung runs on every member, tank
// included, and does that member's part with its own bot: walk to its colour's
// giver and gossip for the essence, use it and hold still while the drake comes
// down, fly its lane to the pad, land, step off — or hold station on Eregos.
//
// A PLAIN Action, NOT a DcMovementAction. Stock `wotlk-occ`'s OccFlyingMultiplier
// zeroes every MovementAction while a bot sits on a drake, which usefully mutes
// the whole DC walking ladder in the air — and would mute this too. It drives the
// drake's MotionMaster directly (DcFlightLeg), and walks on foot only with a
// plain point move to a giver ten yards away.
//
// IT RETURNS FALSE WHENEVER IT HAS NOTHING TO STEER, including every tick a leg is
// already under way, so stock `occ drake attack` (relevance 15) still gets the
// drake's idle ticks: it is what shoots down the pickets in transit and fights
// Eregos. The decisions are DcOculusRider::Decide (Util/DcOculusDriverDecision.h).

namespace
{
    using namespace DcOculus;
    using DcOculusFlight::Vec;
    using RiderAction = DcOculusRider::Action;

    Vec PosOf(WorldObject const* o)
    {
        return { o->GetPositionX(), o->GetPositionY(), o->GetPositionZ() };
    }

    ColourRow const& MyColour(Player* bot)
    {
        return RowFor(ColourForRole(PlayerbotAI::IsTank(bot), PlayerbotAI::IsHeal(bot)));
    }

    Creature* FindEregos(Player* bot)
    {
        InstanceScript* const inst = DcTargeting::GetInstanceScript(bot);
        if (!inst)
            return nullptr;
        ObjectGuid const guid = inst->GetGuidData(DATA_EREGOS);
        if (guid.IsEmpty())
            return nullptr;
        Creature* const c = ObjectAccessor::GetCreature(*bot, guid);
        return c && c->IsAlive() ? c : nullptr;
    }

    // THE RIDER-KILL GUARD. A drake bar spell that is not area-targeted and hits a
    // hostile whose CanFly() is false kills its rider (npc_oculus_drakeAI::
    // SpellHitTarget), and stock `occ drake attack` shoots its current target first
    // with no such check. So a rider never keeps a ground target: it is dropped,
    // and replaced with Eregos when he is fighting, or with the nearest picket that
    // is. (The stock fallback — the first in-combat possible target — is the DC
    // multipliers' half: OculusDrakeAttackUnsafe zeroes the action when it would
    // pick a ground mob.)
    void GuardDrakeTarget(Player* bot, AiObjectContext* context, DcRunState& st, Creature* eregos)
    {
        auto target = context->GetValue<Unit*>(DcKey::Stock::CurrentTarget);
        Unit* const cur = target->Get();
        if (cur && DcFlightLeg::IsFlyingSafe(cur) && cur->IsAlive())
            return;

        if (cur)
        {
            target->Set(nullptr);
            if (!st.Throttled(DcThrottle::OcWarn, WARN_THROTTLE_MS))
                LOG_WARN("playerbots.dungeonclear",
                         "[DC:{}] DcOc WARN dropped ground target {} from a rider: a drake spell on a "
                         "creature that cannot fly kills the rider",
                         bot->GetName(), cur->GetName());
        }

        Unit* replacement = nullptr;
        if (eregos && eregos->IsInCombat())
            replacement = eregos;
        else if (Creature* const picket = bot->FindNearestCreature(NPC_AZURE_RING_GUARDIAN, 60.0f, true))
            if (picket->IsInCombat())
                replacement = picket;
        if (replacement)
            target->Set(replacement);
    }

    // A plain point move on foot, for the walk to a giver on Drakos's ring. The ToC
    // driver's mover, minus the horse.
    bool WalkTo(Player* bot, PlayerbotAI* botAI, DcRunState& st, float x, float y, float z)
    {
        if (!DcMovement::DcMovementAllowed(botAI))
            return false;
        if (st.ThrottledIssue(DcThrottle::OcMoveIssue, x, y, z, /*epsilon*/ 1.0f, MOVE_REISSUE_MS) &&
            bot->isMoving())
            return true;
        DcMovement::ResolveEscortConflict(bot);
        MotionMaster* const mm = bot->GetMotionMaster();
        if (!mm)
            return false;
        mm->Clear();
        mm->MovePoint(/*id*/ 0, x, y, z, FORCED_MOVEMENT_NONE, /*speed*/ 0.0f, /*orientation*/ 0.0f,
                      /*generatePath*/ true, /*forceDestination*/ false);
        return true;
    }

    // Plan this member's leg when the party's leg changes (or after a nudge /
    // pad-centre fallback cleared it), from wherever its drake is now.
    void EnsureLeg(Player* bot, Unit* base, DcRunState& st, DcOculusPlanView const& plan, uint32 lane,
                   uint32 now)
    {
        uint8 const kind = plan.hover ? 2 : 1;
        if (st.ocLegCount && st.ocLegSeq == plan.legSeq && st.ocLegDest == plan.dest && st.ocLegPhase == kind)
            return;

        bool const padCentre = st.ocLegSeq == plan.legSeq && st.ocLegDest == plan.dest && st.ocLegPadCentre;
        Vec const start = PosOf(base);
        uint8 const src = DcOculusFlight::IslandOf(start.x, start.y, start.z);
        Map const* const map = bot->GetMap();
        auto const los = [map](Vec const& a, Vec const& b) { return DcFlightLeg::ChordClear(map, a, b); };
        DcOculusFlight::LegPlan const leg =
            DcOculusFlight::PlanLeg(start, src, plan.dest, padCentre ? 0 : lane, !plan.hover, los);

        st.ocLegSeq = plan.legSeq;
        st.ocLegDest = plan.dest;
        st.ocLegPhase = kind;
        st.ocLegPadCentre = padCentre;
        st.ocLegCount = leg.count;
        st.ocLegCursor = 0;
        for (uint8 i = 0; i < leg.count; ++i)
        {
            st.ocLegWp[i * 3] = leg.wp[i].x;
            st.ocLegWp[i * 3 + 1] = leg.wp[i].y;
            st.ocLegWp[i * 3 + 2] = leg.wp[i].z;
        }
        st.ocLegArrivedMs = 0;
        st.ocProgressMs = now ? now : 1;
        st.ocProgressX = start.x;
        st.ocProgressY = start.y;
        st.ocProgressZ = start.z;
        st.ocStallReissued = false;
        st.ClearThrottle(DcThrottle::OcMoveIssue);

        OcSite const& d = SiteRow(plan.dest);
        LOG_INFO("playerbots.dungeonclear",
                 "[DC:{}] DcOc leg {} -> {} lane {}{}: {} waypoint(s), cruise {:.1f}{}",
                 bot->GetName(), src == SITE_NONE ? "air" : SiteRow(src).name, d.name, padCentre ? 0 : lane,
                 plan.hover ? " (hover)" : "", leg.count, leg.count > 1 ? leg.wp[leg.count - 2].z : start.z,
                 leg.viaColumn ? " via the column" : "");
        if (leg.blocked && !st.Throttled(DcThrottle::OcWarn, WARN_THROTTLE_MS))
            LOG_WARN("playerbots.dungeonclear",
                     "[DC:{}] DcOc WARN leg to {} has a chord that fails the static-VMAP LOS check (from "
                     "({:.1f}, {:.1f}, {:.1f}), lane {}) — flying it anyway; the site table's hover/column "
                     "for this site needs re-probing if the drake wedges",
                     bot->GetName(), d.name, start.x, start.y, start.z, lane);
    }

    DcOculusFlight::LegPlan StoredLeg(DcRunState const& st)
    {
        DcOculusFlight::LegPlan p;
        p.count = std::min<uint8>(st.ocLegCount, DcOculusFlight::kMaxWaypoints);
        for (uint8 i = 0; i < p.count; ++i)
            p.wp[i] = { st.ocLegWp[i * 3], st.ocLegWp[i * 3 + 1], st.ocLegWp[i * 3 + 2] };
        return p;
    }

    void LogVerdict(Player* bot, DcRunState& st, DcOculusRider::Inputs const& in, RiderAction a,
                    ColourRow const& colour, uint32 lane)
    {
        if (st.ocRiderAction == static_cast<uint8>(a))
            return;
        st.ocRiderAction = static_cast<uint8>(a);
        if (a == RiderAction::None)
            return;
        LOG_INFO("playerbots.dungeonclear",
                 "[DC:{}] DcOc rider {} (lane {}, phase {}, site {}) — {}",
                 bot->GetName(), colour.name, lane, DcOculusDriver::PhaseName(in.phase),
                 in.dest == SITE_NONE ? "-" : SiteRow(in.dest).name, DcOculusRider::ActionName(a));
    }
}

bool DungeonClearOculusRiderTrigger::IsActive()
{
    // Map first: registered in both engines on every bot, and everywhere outside
    // The Oculus this must cost one integer compare.
    if (!bot || bot->GetMapId() != DcOculus::MAP_ID || !bot->IsAlive())
        return false;
    if (bot->GetVehicle())
        return true;
    DcOculusPlanView const plan = OculusPlanFor(bot);
    return plan.valid &&
           (plan.phase != DcOculusDriver::Phase::Idle || plan.action == DcOculusDriver::Action::Regroup);
}

bool DungeonClearOculusRiderAction::Execute(Event /*event*/)
{
    if (!bot || !botAI || bot->GetMapId() != MAP_ID || !bot->IsAlive())
        return false;

    DcOculusPlanView const plan = OculusPlanFor(bot);
    if (!plan.valid)
        return false;
    if (plan.action == DcOculusDriver::Action::Regroup && OculusRegroup(bot))
        return true;

    DcRunState& st = DcRun::Of(context);
    uint32 const now = getMSTime();
    ColourRow const& colour = MyColour(bot);
    uint32 const lane = OculusLaneOf(bot);
    Unit* const base = DcFlightLeg::OculusDrakeOf(bot);
    Creature* const eregos = FindEregos(bot);

    DcOculusRider::Inputs in;
    in.phase = plan.phase;
    in.dest = plan.dest;
    in.planFresh = true;
    in.isTank = plan.owner == bot;
    in.alive = true;
    in.engaged = DcCombatFlag::IsEngaged(bot);
    in.onVehicle = bot->GetVehicle() != nullptr;
    in.onMount = bot->IsMounted();

    // The playerbots master relation, for stock MountingDrakeMultiplier (see the
    // kernel's Inputs). GetMaster() is whatever stock reads, so ask it the same way.
    if (Player* const master = botAI->GetMaster())
        in.masterMounted = master != bot && master->GetVehicleBase() != nullptr;
    in.masterWaitSinceMs = st.ocMasterWaitSinceMs;
    if (Group* const group = bot->GetGroup())
    {
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* const m = ref->GetSource();
            if (!m || m == bot || !m->IsInWorld() || m->GetMapId() != bot->GetMapId())
                continue;
            if (PlayerbotAI* const mAI = GET_PLAYERBOT_AI(m))
                if (mAI->GetMaster() == bot)
                    in.mastersOthers = true;
            if (!m->IsAlive())
                continue;
            ++in.othersAlive;
            if (m->GetVehicle())
                ++in.othersMounted;
        }
    }
    in.nowMs = now;

    Vec const self = PosOf(bot);
    in.island = DcOculusFlight::IslandOf(self.x, self.y, self.z);

    in.eregosPresent = eregos != nullptr;
    in.eregosAttackable = eregos && !eregos->HasUnitFlag(UNIT_FLAG_NON_ATTACKABLE);
    in.eregosEngaged = eregos && eregos->IsInCombat();

    Creature* giver = nullptr;
    Vec station{};
    DcOculusFlight::Step step;

    if (!in.onVehicle)
    {
        // Off the drake: whatever leg this member had is over. The rung's own
        // dismount clears ocBaseGuid, so a guid still set here is an exit nobody
        // asked for — the drake died or despawned, or something ejected the rider.
        // Live (the central ring): a rider lost its drake mid-leg and died on foot
        // under a mounted master, and nothing said why.
        if (!st.ocBaseGuid.IsEmpty())
        {
            Creature* const drake = ObjectAccessor::GetCreature(*bot, st.ocBaseGuid);
            Vec snap;
            bool const onMesh = DcFlightLeg::SnapOnMesh(bot->GetMap(), self.x, self.y, self.z, snap);
            LOG_WARN("playerbots.dungeonclear",
                     "[DC:{}] DcOc WARN off the drake without a dismount at ({:.1f}, {:.1f}, {:.1f}) "
                     "aboveMesh={:.1f} (-1 = off mesh) phase={} site={} drake={} drakeHp={}% hp={}% attackers={}",
                     bot->GetName(), self.x, self.y, self.z, onMesh ? self.z - snap.z : -1.0f,
                     DcOculusDriver::PhaseName(plan.phase), plan.dest == SITE_NONE ? "-" : SiteRow(plan.dest).name,
                     !drake ? "gone" : (drake->IsAlive() ? "alive" : "dead"),
                     drake ? static_cast<uint32>(drake->GetHealthPct()) : 0,
                     static_cast<uint32>(bot->GetHealthPct()), bot->getAttackers().size());
        }
        if (st.ocLegCount || !st.ocBaseGuid.IsEmpty())
        {
            st.ClearOcLeg();
            st.ocBaseGuid.Clear();
        }
        in.hasEssence = bot->HasItemCount(colour.essence, 1);
        in.essenceOnCooldown = bot->HasSpellCooldown(colour.callSpell);
        in.mountIssuedMs = st.ocMountIssuedMs;
        in.essenceWaitSinceMs = st.ocEssenceWaitSinceMs;
        if (!in.hasEssence && in.island == SITE_R1_GIVERS)
        {
            giver = bot->FindNearestCreature(colour.giver, GIVER_SCAN, /*alive*/ true);
            in.giverPresent = giver != nullptr;
            in.giverFlagged = giver && giver->HasNpcFlag(UNIT_NPC_FLAG_GOSSIP);
            in.giverDist = giver ? bot->GetExactDist(giver) : -1.0f;
            in.postDist = bot->GetExactDist(colour.postX, colour.postY, colour.postZ);
        }
    }
    else
    {
        st.ocMountIssuedMs = 0;
        if (base)
        {
            in.onOcDrake = true;
            VehicleSeatEntry const* const seat = bot->GetVehicle()->GetSeatForPassenger(bot);
            in.seatCanControl = seat && seat->CanControl();
            in.groundAttacker = DcFlightLeg::HasGroundAttacker(bot, base);
            in.baseInCombat = base->GetVictim() != nullptr || !base->getAttackers().empty() || in.groundAttacker;
            in.channeling = base->GetCurrentSpell(CURRENT_CHANNELED_SPELL) != nullptr;
            in.tankOnFootOnDest = plan.tankOnFootOnDest;
            Vec const b = PosOf(base);

            if (eregos)
            {
                in.nearEregos = base->GetExactDist(eregos) <= EREGOS_ENGAGE_RANGE;
                float const range = (in.isTank && !in.eregosEngaged) ? EREGOS_PULL_RANGE : EREGOS_STATION_RANGE;
                station = DcOculusFlight::EregosStation(PosOf(eregos), lane, eregos->HasAura(SPELL_PLANAR_SHIFT),
                                                        range);
                in.stationError = DcOculusFlight::Dist3d(b, station);
            }

            bool const flyingPhase = plan.phase == DcOculusDriver::Phase::Fly ||
                                     plan.phase == DcOculusDriver::Phase::Land ||
                                     plan.phase == DcOculusDriver::Phase::Eregos;
            if (flyingPhase && plan.dest != SITE_NONE)
            {
                EnsureLeg(bot, base, st, plan, lane, now);
                DcOculusFlight::LegPlan const leg = StoredLeg(st);
                step = DcOculusFlight::Advance(b, leg, st.ocLegCursor);
                st.ocLegCursor = step.cursor;
                in.legArrived = step.arrived;

                // Progress on the BASE. A drake holding for a picket is not stalled.
                float const moved = DcOculusFlight::Dist3d(b, { st.ocProgressX, st.ocProgressY, st.ocProgressZ });
                if (in.baseInCombat || in.legArrived || !st.ocProgressMs || moved >= LEG_PROGRESS_YD)
                {
                    st.ocProgressMs = now ? now : 1;
                    st.ocProgressX = b.x;
                    st.ocProgressY = b.y;
                    st.ocProgressZ = b.z;
                    if (moved >= LEG_PROGRESS_YD)
                        st.ocStallReissued = false;
                }
                in.legStalled = DcOculusFlight::Stalled(now, st.ocProgressMs);
                in.stallReissued = st.ocStallReissued;
                in.landed = !plan.hover && DcFlightLeg::BaseLanded(base, plan.dest);
                in.atPadCentre = lane == 0 || st.ocLegPadCentre;
                in.arrivedSinceMs = st.ocLegArrivedMs;
            }

            GuardDrakeTarget(bot, context, st, eregos);
        }
    }

    DcOculusRider::Verdict const v = DcOculusRider::Decide(in);
    st.ocLegArrivedMs = in.legArrived ? v.arrivedSinceMs : 0;
    st.ocMasterWaitSinceMs = v.masterWaitSinceMs;
    st.ocEssenceWaitSinceMs = v.essenceWaitSinceMs;
    LogVerdict(bot, st, in, v.action, colour, lane);

    // A picket has the drake mid-leg: stop and hover where it is, so `occ drake
    // attack` casts from a still base (CastVehicleSpell cancels a cast on a moving
    // one), and resume the leg once the picket is dead. A ground attacker never
    // stops a drake (DcOculusRider::HoldsForPicket).
    if (base && DcOculusRider::HoldsForPicket(in) && base->isMoving() && v.action == RiderAction::None &&
        !in.eregosEngaged)
        DcFlightLeg::MoveBase(bot, base, st, base->GetPositionX(), base->GetPositionY(), base->GetPositionZ());

    switch (v.action)
    {
        case RiderAction::WalkToGiver:
        {
            if (!giver)
                return WalkTo(bot, botAI, st, colour.postX, colour.postY, colour.postZ);
            float dx = bot->GetPositionX() - giver->GetPositionX();
            float dy = bot->GetPositionY() - giver->GetPositionY();
            float const d = std::hypot(dx, dy);
            if (d > 0.1f)
            {
                dx /= d;
                dy /= d;
            }
            else
            {
                dx = 1.0f;
                dy = 0.0f;
            }
            return WalkTo(bot, botAI, st, giver->GetPositionX() + dx * GIVER_STANDOFF,
                          giver->GetPositionY() + dy * GIVER_STANDOFF, giver->GetPositionZ());
        }

        case RiderAction::Gossip:
        {
            if (!giver)
                return false;
            if (st.Throttled(DcThrottle::OcGossip, GOSSIP_RETRY_MS))
                return true;
            if (bot->isMoving())
                DcMovement::StopBot(bot, DcMovement::Stop::Hold);
            bool const sent = DungeonEventExecutor::SelectGossip(bot, giver, colour.gossipOption);
            LOG_INFO("playerbots.dungeonclear",
                     "[DC:{}] DcOc essence colour={} giver={} option={} {} ({:.1f}yd){}",
                     bot->GetName(), colour.name, giver->GetName(), colour.gossipOption,
                     sent ? "selected" : "could not select", in.giverDist,
                     bot->HasItemCount(colour.essence, 1) ? " — essence in the bags" : "");
            return true;
        }

        case RiderAction::FabricateEssence:
            bot->AddItem(colour.essence, 1);
            if (in.island == SITE_R1_GIVERS)
                LOG_WARN("playerbots.dungeonclear",
                         "[DC:{}] DcOc WARN essence fabricated: {} has had no {} essence for {}s on Drakos's "
                         "ring (giver {}, {:.1f}yd, gossip flag {}; post {:.1f}yd) — handing over item {}",
                         bot->GetName(), bot->GetName(), colour.name, GIVER_WAIT_MS / 1000,
                         in.giverPresent ? "in scan" : "NOT in scan", in.giverDist, in.giverFlagged ? 1 : 0,
                         in.postDist, colour.essence);
            else
                LOG_WARN("playerbots.dungeonclear",
                         "[DC:{}] DcOc WARN essence fabricated: {} has no {} essence and is not on Drakos's ring "
                         "(the givers never leave it) — handing over item {}",
                         bot->GetName(), bot->GetName(), colour.name, colour.essence);
            return true;

        case RiderAction::LeaveMount:
            // The mount aura is the mount (stock CheckMountStateAction steps off the
            // same way); removing it runs Unit::Dismount. Stock `check mount state`
            // is zeroed for this bot while it has a drake to board (the multipliers'
            // OculusMountBanned), so nothing puts it straight back on.
            bot->RemoveAurasByType(SPELL_AURA_MOUNTED);
            if (bot->isMoving())
                DcMovement::StopBot(bot, DcMovement::Stop::Hold);
            LOG_INFO("playerbots.dungeonclear",
                     "[DC:{}] DcOc dismount own mount before the {} essence (phase {}){}",
                     bot->GetName(), colour.name, DcOculusDriver::PhaseName(plan.phase),
                     bot->IsMounted() ? " — STILL MOUNTED" : "");
            return true;

        case RiderAction::Mount:
        {
            Item* const item = bot->GetItemByEntry(colour.essence);
            if (!item)
                return false;
            if (bot->isMoving())
                DcMovement::StopBot(bot, DcMovement::Stop::Hold);
            // The kernel only proposes Mount on foot, but a mount can land between
            // the census and here; the Call is refused from one either way.
            if (bot->IsMounted())
                bot->RemoveAurasByType(SPELL_AURA_MOUNTED);
            DcFormGate::DropBlockingForm(bot, item);
            SpellCastTargets targets;
            targets.SetUnitTarget(bot);
            bot->CastItemUseSpell(item, targets, 0, 0);
            st.ocMountIssuedMs = now ? now : 1;
            LOG_INFO("playerbots.dungeonclear",
                     "[DC:{}] DcOc mount colour={} phase={} site={} lane={}",
                     bot->GetName(), colour.name, DcOculusDriver::PhaseName(plan.phase),
                     plan.dest == SITE_NONE ? "-" : SiteRow(plan.dest).name, lane);
            return true;
        }

        case RiderAction::SettleHold:
            if (bot->isMoving())
                DcMovement::StopBot(bot, DcMovement::Stop::Hold);
            return true;

        case RiderAction::Fly:
            if (!base || step.arrived)
                return false;
            return DcFlightLeg::MoveBase(bot, base, st, step.target.x, step.target.y, step.target.z) ==
                   DcFlightLeg::Move::Issued;

        case RiderAction::Reissue:
            if (!base || step.arrived)
                return false;
            st.ocStallReissued = true;
            st.ocProgressMs = now ? now : 1;
            st.ClearThrottle(DcThrottle::OcMoveIssue);
            LOG_WARN("playerbots.dungeonclear",
                     "[DC:{}] DcOc WARN leg stalled: the drake has not moved {:.0f}yd in {}s — re-issuing "
                     "({:.1f}, {:.1f}, {:.1f})",
                     bot->GetName(), LEG_PROGRESS_YD, LEG_STALL_MS / 1000, step.target.x, step.target.y,
                     step.target.z);
            return DcFlightLeg::MoveBase(bot, base, st, step.target.x, step.target.y, step.target.z) ==
                   DcFlightLeg::Move::Issued;

        case RiderAction::Nudge:
            if (!base)
                return false;
            // Never dismount in the air. Climb clear of whatever holds the drake and
            // re-plan the leg from there next tick.
            st.ocLegCount = 0;
            st.ocStallReissued = false;
            st.ocProgressMs = now ? now : 1;
            st.ClearThrottle(DcThrottle::OcMoveIssue);
            LOG_WARN("playerbots.dungeonclear",
                     "[DC:{}] DcOc WARN leg still stalled after a re-issue and not over mesh — climbing 10yd "
                     "and re-planning (never dismounting in the air)",
                     bot->GetName());
            return DcFlightLeg::MoveBase(bot, base, st, base->GetPositionX(), base->GetPositionY(),
                                         base->GetPositionZ() + 10.0f) == DcFlightLeg::Move::Issued;

        case RiderAction::FlyPadCentre:
            st.ocLegCount = 0;
            st.ocLegPadCentre = true;
            LOG_INFO("playerbots.dungeonclear",
                     "[DC:{}] DcOc land: lane {} has not passed the landing check in {}s — landing on the pad "
                     "centre instead",
                     bot->GetName(), lane, PAD_FALLBACK_MS / 1000);
            return true;

        case RiderAction::Dismount:
        {
            if (!base)
                return false;
            Vec snap;
            bool const onMesh =
                DcFlightLeg::SnapOnMesh(base->GetMap(), base->GetPositionX(), base->GetPositionY(),
                                        base->GetPositionZ(), snap);
            WorldPacket packet;
            bot->GetSession()->HandleRequestVehicleExit(packet);
            float gap = -1.0f;
            // A clientless bot does not fall: set it down on the snapped floor it
            // was hovering LAND_HOVER above, rather than leave it standing in air.
            if (!bot->GetVehicle() && onMesh && DcCoreCompat::IsClientless(bot->GetSession()))
            {
                gap = bot->GetPositionZ() - snap.z;
                if (gap > 0.5f)
                    bot->NearTeleportTo(snap.x, snap.y, snap.z + 0.1f, bot->GetOrientation());
            }
            st.ClearOcLeg();
            st.ocBaseGuid.Clear();
            LOG_INFO("playerbots.dungeonclear",
                     "[DC:{}] DcOc dismount site={} landed=1 gap={:.1f}{}",
                     bot->GetName(), plan.dest == SITE_NONE ? "-" : SiteRow(plan.dest).name, gap,
                     bot->GetVehicle() ? " (STILL MOUNTED — the exit was refused)" : "");
            return true;
        }

        case RiderAction::PullEregos:
            if (!base || !eregos)
                return false;
            context->GetValue<Unit*>(DcKey::Stock::CurrentTarget)->Set(eregos);
            bot->SetSelection(eregos->GetGUID());
            DcFlightLeg::MoveBase(bot, base, st, station.x, station.y, station.z);
            return false;  // `occ drake attack` opens on him this tick

        case RiderAction::Station:
            if (!base)
                return false;
            return DcFlightLeg::MoveBase(bot, base, st, station.x, station.y, station.z) ==
                   DcFlightLeg::Move::Issued;

        case RiderAction::WaitForGiver:
        case RiderAction::None:
        default:
            return false;
    }
}
