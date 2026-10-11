/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "TestRun/DcDiagSnapshot.h"

#include <algorithm>
#include <list>
#include <optional>
#include <unordered_set>
#include <vector>

#include "CombatManager.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "Group.h"
#include "InstanceScript.h"
#include "Map.h"
#include "Player.h"
#include "SharedDefines.h"
#include "Timer.h"
#include "Vehicle.h"

#include "PlayerbotAI.h"
#include "Playerbots.h"

#include "Ai/Dungeon/DungeonClear/Data/DungeonBossInfo.h"
#include "Ai/Dungeon/DungeonClear/DcApproachState.h"
#include "Ai/Dungeon/DungeonClear/DcPullContext.h"
#include "Ai/Dungeon/DungeonClear/DcRunState.h"
#include "Ai/Dungeon/DungeonClear/DcValueKeys.h"
#include "Ai/Dungeon/DungeonClear/Util/ChunkedPathfinder.h"
#include "Ai/Dungeon/DungeonClear/Util/DcAnchorDone.h"
#include "Ai/Dungeon/DungeonClear/Util/DcEngageGeometry.h"
#include "Ai/Dungeon/DungeonClear/Util/DcRun.h"
#include "Ai/Dungeon/DungeonClear/Util/DcStatusPublisher.h"
#include "Ai/Dungeon/DungeonClear/Util/DcTargeting.h"
#include "Ai/Dungeon/DungeonClear/Util/DcTickMemo.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonClearMath.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonEventExecutor.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonPathFollower.h"
#include "Ai/Dungeon/DungeonClear/Util/NavmeshSnap.h"
#include "TestRun/DcTestRunRecord.h"

namespace
{
    // Split the publisher's tab-separated STATUS frame. Same shape the run job
    // already parses (DcTestRunJob::OnStatusPayload) — reused rather than
    // re-deriving the state ladder, so the snapshot and the status timeline can
    // never disagree about what state the run was in.
    std::vector<std::string> SplitStatus(std::string const& payload)
    {
        std::vector<std::string> parts;
        std::size_t from = 0;
        while (from <= payload.size())
        {
            std::size_t const tab = payload.find('\t', from);
            if (tab == std::string::npos)
            {
                parts.push_back(payload.substr(from));
                break;
            }
            parts.push_back(payload.substr(from, tab - from));
            from = tab + 1;
        }
        return parts;
    }

    std::uint32_t PowerPct(Player* p, Powers power)
    {
        std::uint32_t const maxPower = p->GetMaxPower(power);
        if (!maxPower)
            return 0;
        return static_cast<std::uint32_t>((static_cast<std::uint64_t>(p->GetPower(power)) * 100) / maxPower);
    }

    // The floor column: narrow, and tall enough to reach the street from a unit
    // a dozen yards under it. NavmeshSnap's default 10yd would have missed the
    // deeper of the two King's Square tanks (12.4yd down). The nearest poly in
    // z wins, so a unit genuinely on a lower floor still finds its own floor.
    constexpr float kFloorColumnRadius = 3.0f;
    constexpr float kFloorColumnHalfHeight = 40.0f;
    // Ask the ground from just above the unit, so one standing ON a floor finds
    // that floor rather than whatever is below it.
    constexpr float kGroundProbeLift = 2.0f;

    DcDiag::FloorProbe ProbeFloor(WorldObject const* at)
    {
        DcDiag::FloorProbe f;
        Map* const map = at ? at->FindMap() : nullptr;
        if (!map)
            return f;
        float const x = at->GetPositionX();
        float const y = at->GetPositionY();
        float const z = at->GetPositionZ();
        f.probed = true;
        NavmeshSnap::Result const mesh =
            NavmeshSnap::Snap(map, x, y, z, kFloorColumnRadius, kFloorColumnHalfHeight);
        f.meshOk = mesh.ok;
        f.meshZ = mesh.z;
        f.groundZ = at->GetMapHeight(x, y, z + kGroundProbeLift);
        f.gridZ = map->GetGridHeight(x, y);
        return f;
    }

    // At most this many holders are described per member. A bot held by a whole
    // pack does not need every row to explain itself, and each row costs a
    // pathfind (IsReachable); the untruncated total is reported separately as
    // holderRefCount so a cap can never read as "that was all of them".
    constexpr std::size_t kMaxHoldersPerMember = 8;

    // Name every unit holding `member` in combat, and record — per holder — each
    // input the phantom-combat hatch weighs. Deliberately a MIRROR of
    // DungeonClearTriggers.cpp's HasLegitimateCombatHolder rather than a call
    // into it: that function answers one bool for the whole member and discards
    // the reasoning, and the reasoning is the entire point of this record. If
    // the guards there change, change them here too — a divergence shows up as
    // a snapshot that says "phantom" about a member the hatch never fires on,
    // which is the most misleading thing this file could print.
    //
    // Both ref maps are walked. A PvE-empty / PvP-held bot is exactly the
    // "opaque combat" case the hatch treats as legitimate-by-default, and
    // without the PvP rows the record would show a flagged member with no
    // holders at all — indistinguishable from a genuine flag/ref desync.
    void CaptureCombatHolders(Player* member, DcDiag::MemberSnapshot& m)
    {
        Map* const map = member->GetMap();
        m.attackerCount = static_cast<std::uint32_t>(member->getAttackers().size());

        bool anyLegitimatePvEHolder = false;
        auto describe = [&](Unit* other, bool suppressed, bool pvp)
        {
            if (!other)
                return;
            ++m.holderRefCount;

            // The verdict is computed for EVERY ref, cap or no cap — otherwise a
            // truncated list could report "no legitimate holder" about a member
            // the hatch is correctly leaving alone.
            bool const alive = other->IsAlive();
            bool const sameMap = other->GetMap() == map;
            bool const evading = other->GetCombatManager().IsInEvadeMode();
            bool const reachChecked = alive && sameMap && !evading;
            bool const reachable =
                reachChecked &&
                DcEngageGeometry::IsReachable(member, other->GetPositionX(),
                                              other->GetPositionY(),
                                              other->GetPositionZ());
            // A holder whose own AI forbids it attacking this member can never
            // resolve the reference, however reachable it is — see the matching
            // guard in HasLegitimateCombatHolder.
            Creature* const asCreature = other->ToCreature();
            bool const canAttackMe =
                !asCreature || !asCreature->AI() || asCreature->AI()->CanAIAttack(member);
            // A TRIGGER holder can never be attacked back, so it can never end the
            // fight — the matching guard in ScanCombatHolders. Recorded as its own
            // field rather than folded silently into `legitimate` because this is
            // precisely the row a reader has to be able to see: before the guard
            // existed the teardown printed `Flame Breath Trigger (Skadi) 0.0yd
            // 100% reach LEGIT` and the wedge looked like a healthy fight.
            bool const trigger =
                DungeonClearMath::IsUnresolvableCombatHolder(asCreature != nullptr,
                                                             asCreature && asCreature->IsTrigger());
            bool const legitimate = reachable && canAttackMe && !trigger;
            if (DcDiag::IsLegitimatePvECombatHolder(pvp, legitimate))
                anyLegitimatePvEHolder = true;

            if (m.combatHolders.size() >= kMaxHoldersPerMember)
                return;

            DcDiag::CombatHolderSnapshot h;
            h.name = other->GetName();
            h.entry = other->GetEntry();
            h.guid = other->GetGUID().GetRawValue();
            h.isCreature = other->IsCreature();
            h.pvp = pvp;
            h.alive = alive;
            h.sameMap = sameMap;
            h.evading = evading;
            h.suppressed = suppressed;
            h.reachable = reachable;
            h.reachChecked = reachChecked;
            h.dist = sameMap ? member->GetDistance(other) : -1.f;
            h.healthPct = static_cast<std::uint32_t>(other->GetHealthPct());
            h.x = other->GetPositionX();
            h.y = other->GetPositionY();
            h.z = other->GetPositionZ();
            if (sameMap)
                h.floor = ProbeFloor(other);
            if (Unit* hv = other->GetVictim())
                h.victim = hv->GetName();
            // One of the hatch's guards since S1476, and the reason it became one:
            // a boss whose CanAIAttack gates on geometry (Selin Fireheart's is
            // `X > 216`) reads as alive, non-evading and path-reachable while being
            // permanently unable to touch a party camped outside that plane — a real
            // ref behind a fight that physically cannot happen. This field spent that
            // whole time printing the answer next to a LEGITIMATE verdict
            // (tr-20260803-211838-7, 334s wedged) before it was wired into one.
            h.canAttackMe = canAttackMe;
            h.trigger = trigger;
            // Scripted-out-of-the-fight state. See the header: a boss playing a
            // defeat sequence is alive, reachable and attack-permitted, and the
            // row said exactly that for eleven seconds while a run was thrown away.
            h.immune = other->IsImmuneToAll();
            h.passive = asCreature && asCreature->GetReactState() == REACT_PASSIVE;
            h.atOneHp = other->GetHealth() == 1;
            h.legitimate = legitimate;
            m.combatHolders.push_back(std::move(h));
        };

        CombatManager const& cm = member->GetCombatManager();
        for (auto const& kv : cm.GetPvECombatRefs())
            if (CombatReference* const ref = kv.second)
                describe(ref->GetOther(member), ref->IsSuppressedFor(member), /*pvp*/ false);
        for (auto const& kv : cm.GetPvPCombatRefs())
            if (CombatReference* const ref = kv.second)
                describe(ref->GetOther(member), ref->IsSuppressedFor(member), /*pvp*/ true);

        // No refs at all is the hatch's "opaque/forced combat, leave it alone"
        // branch — legitimate by default, so it must NOT read as phantom here.
        bool const hasLegitimateHolder = DcDiag::HasLegitimatePvECombatHolder(
            !cm.GetPvECombatRefs().empty(), anyLegitimatePvEHolder);
        // The verdict itself goes through the SAME kernel the trigger uses, so
        // the snapshot cannot drift from the hatch on the one field a reader
        // will trust it on. Only the per-holder legitimacy above is mirrored by
        // hand, and only because the trigger throws that detail away.
        m.phantomCombat = DungeonClearMath::IsPhantomCombat(
            /*inCombat*/ true, m.attackerCount > 0, !m.victim.empty(), hasLegitimateHolder);
    }

    // Wrap-safe "how long ago", clamped: a zero stamp means "never", not "forever".
    std::uint32_t SinceMs(std::uint32_t stampMs)
    {
        if (!stampMs)
            return 0;
        return getMSTimeDiff(stampMs, getMSTime());
    }

    // The escortee search. The spawn-id store holds every DB-spawned creature on
    // the map whatever its state and whoever is near it, so it is the one list a
    // grid-visibility problem cannot hide a copy from; the grid search around the
    // tank adds the summons the store never holds, and records which copies a
    // grid search CAN see — the same kind of search the driver makes.
    constexpr float kEscorteeGridScanYd = 500.0f;
    constexpr std::size_t kMaxEscorteeCopies = 4;

    // Mirror of DriveEscortCreature's resolve radius (DcEngageActions.cpp),
    // default included. Change the two together.
    constexpr float kEscortDefaultSearchYd = 80.0f;

    DungeonEvent const* ActiveEscortEvent(Player* tank, AiObjectContext* context,
                                          DungeonEventProgress const*& progOut)
    {
        for (char const* key : { DcKey::EventProgress, DcKey::ConditionalEventProgress })
        {
            DungeonEventProgress const& p = context->GetValue<DungeonEventProgress&>(key)->Get();
            if (!p.eventId)
                continue;
            if (p.instanceId && p.instanceId != tank->GetInstanceId())
                continue;
            DungeonEvent const* ev = DungeonEventRegistry::Find(tank->GetMapId(), p.eventId);
            if (!ev || p.stepIndex >= ev->steps.size())
                continue;
            if (ev->steps[p.stepIndex].kind != EventStepKind::EscortCreature)
                continue;
            progOut = &p;
            return ev;
        }
        return nullptr;
    }

    void CaptureEscort(Player* tank, AiObjectContext* context, DcDiag::EscortSnapshot& e)
    {
        DungeonEventProgress const* prog = nullptr;
        DungeonEvent const* ev = ActiveEscortEvent(tank, context, prog);
        if (!ev || !prog)
            return;

        EventStep const& step = ev->steps[prog->stepIndex];
        e.active = true;
        e.eventId = ev->id;
        e.eventName = ev->name;
        e.stepIndex = prog->stepIndex;
        e.entry = step.creatureEntry;
        e.searchRadius = step.radius > 0.0f ? step.radius : kEscortDefaultSearchYd;
        if (Creature* seen = tank->FindNearestCreature(e.entry, e.searchRadius, /*alive*/ true))
        {
            e.driverSees = true;
            e.driverSeesGuid = seen->GetGUID().GetRawValue();
        }
        e.deadAirSeen = prog->escortProgressMs != 0;
        e.deadAirMs = SinceMs(prog->escortProgressMs);
        e.instanceDataId = step.instanceDataId;
        e.instanceDataMin = step.instanceDataMin;
        if (step.instanceDataId >= 0)
            if (InstanceScript* inst = DcTargeting::GetInstanceScript(tank))
                e.instanceData = inst->GetData(static_cast<uint32>(step.instanceDataId));

        Map* const map = tank->GetMap();
        std::vector<Creature*> found;
        std::unordered_set<std::uint64_t> known;
        for (auto const& kv : map->GetCreatureBySpawnIdStore())
        {
            Creature* c = kv.second;
            if (c && c->GetEntry() == e.entry && known.insert(c->GetGUID().GetRawValue()).second)
                found.push_back(c);
        }
        std::list<Creature*> nearby;
        tank->GetCreatureListWithEntryInGrid(nearby, e.entry, kEscorteeGridScanYd);
        std::unordered_set<std::uint64_t> gridSeen;
        for (Creature* c : nearby)
        {
            if (!c)
                continue;
            gridSeen.insert(c->GetGUID().GetRawValue());
            if (known.insert(c->GetGUID().GetRawValue()).second)
                found.push_back(c);
        }

        e.copyCount = static_cast<std::uint32_t>(found.size());
        std::sort(found.begin(), found.end(), [tank](Creature* a, Creature* b)
                  { return tank->GetExactDist(a) < tank->GetExactDist(b); });
        for (Creature* c : found)
        {
            if (e.copies.size() >= kMaxEscorteeCopies)
                break;
            DcDiag::EscorteeCopy copy;
            copy.guid = c->GetGUID().GetRawValue();
            copy.spawnId = static_cast<std::uint32_t>(c->GetSpawnId());
            copy.alive = c->IsAlive();
            copy.deathState = static_cast<std::uint32_t>(c->getDeathState());
            copy.inWorld = c->IsInWorld();
            copy.x = c->GetPositionX();
            copy.y = c->GetPositionY();
            copy.z = c->GetPositionZ();
            Position const& home = c->GetHomePosition();
            copy.homeX = home.GetPositionX();
            copy.homeY = home.GetPositionY();
            copy.homeZ = home.GetPositionZ();
            copy.distToTank = tank->GetExactDist(c);
            copy.moving = c->isMoving();
            copy.inCombat = c->IsInCombat();
            copy.evading = c->IsInEvadeMode();
            copy.gossipFlag = c->HasNpcFlag(UNIT_NPC_FLAG_GOSSIP);
            copy.faction = c->GetFaction();
            copy.updateNeeded = c->IsUpdateNeeded();
            copy.gridLoaded = map->IsGridLoaded(copy.x, copy.y);
            copy.seenByGridScan = gridSeen.count(copy.guid) != 0;
            e.copies.push_back(copy);
        }
    }

    void AppendFloor(std::ostringstream& s, DcDiag::FloorProbe const& f)
    {
        if (!f.probed)
            return;
        s << ",\"floor\":{\"meshOk\":" << (f.meshOk ? "true" : "false")
          << ",\"meshZ\":" << f.meshZ
          << ",\"groundZ\":" << f.groundZ
          << ",\"gridZ\":" << f.gridZ << '}';
    }

    void AppendEscaped(std::ostringstream& s, std::string const& v)
    {
        s << '"' << DcTestRunRecord::EscapeJson(v) << '"';
    }

    void AppendBool(std::ostringstream& s, char const* key, bool v)
    {
        s << ",\"" << key << "\":" << (v ? "true" : "false");
    }

    // DcStrategyGate keeps these file-static in its own TU, so they are
    // duplicated rather than shared. Kept adjacent to that comment so a rename
    // there is caught here: a wrong name silently reports "no DC strategy" on
    // every member, which reads as a real (and very misleading) finding.
    char const* const kDcNonCombatStrategy = "dungeon clear";
    char const* const kDcCombatStrategy    = "dungeon clear combat";
}

namespace DcDiag
{
    bool IsLegitimatePvECombatHolder(bool isPvp, bool holderIsLegitimate)
    {
        return !isPvp && holderIsLegitimate;
    }

    bool HasLegitimatePvECombatHolder(bool hasPvERefs, bool anyLegitimatePvEHolder)
    {
        return !hasPvERefs || anyLegitimatePvEHolder;
    }

    bool IsUnderMesh(FloorProbe const& floor, float z)
    {
        return floor.probed && floor.meshOk && z < floor.meshZ - kUnderMeshYd;
    }

    Snapshot Capture(Player* tank, char const* capturedAt)
    {
        Snapshot snap;
        snap.capturedAt = capturedAt ? capturedAt : "";

        PlayerbotAI* tankAI = tank ? GET_PLAYERBOT_AI(tank) : nullptr;
        if (!tank || !tankAI || !tank->IsInWorld())
            return snap;

        AiObjectContext* context = tankAI->GetAiObjectContext();
        if (!context)
            return snap;

        snap.valid = true;

        // --- run switches -------------------------------------------------
        DcRunState const& rs = DcRun::Of(context);
        snap.enabled = rs.enabled;
        snap.paused = rs.paused;
        snap.pauseReason = rs.pauseReason;
        snap.pausedAtDoor = !rs.pausedDoor.IsEmpty();
        snap.selectedBossEntry = rs.selectedBossEntry;
        snap.smartRestLatched = rs.smartRestLatched;

        // --- state-machine position ---------------------------------------
        snap.phase = AI_VALUE(std::string&, DcKey::Phase);
        snap.stallReason = AI_VALUE(std::string&, DcKey::StallReason);

        std::vector<std::string> const status = SplitStatus(DcStatusPublisher::BuildStatusPayload(tankAI));
        if (status.size() >= 8 && status[0] == "STATUS")
        {
            snap.stateStr = status[6];
            snap.detail = status[7];
        }

        // --- world / tank -------------------------------------------------
        snap.mapId = tank->GetMapId();
        snap.instanceId = tank->GetInstanceId();
        snap.tankX = tank->GetPositionX();
        snap.tankY = tank->GetPositionY();
        snap.tankZ = tank->GetPositionZ();
        snap.tankInCombat = tank->IsInCombat();
        snap.tankMoving = tank->isMoving();
        if (Unit* victim = tank->GetVictim())
            snap.tankVictim = victim->GetName();

        InstanceScript* inst = DcTargeting::GetInstanceScript(tank);
        snap.completedEncounterMask = inst ? inst->GetCompletedEncounterMask() : 0u;

        std::unordered_set<std::uint32_t> const& cleared =
            AI_VALUE(std::unordered_set<uint32>&, DcKey::ClearedAnchors);
        std::unordered_set<std::uint32_t> const& skipped =
            AI_VALUE(std::unordered_set<uint32>&, DcKey::Skipped);
        std::unordered_set<std::uint32_t> const& seen =
            AI_VALUE(std::unordered_set<uint32>&, DcKey::SeenBosses);
        snap.clearedAnchors = static_cast<std::uint32_t>(cleared.size());
        snap.skippedCount = static_cast<std::uint32_t>(skipped.size());

        // --- heartbeat ----------------------------------------------------
        // Taken here rather than mirrored out of the member loop below, which a
        // solo tank (no group) never enters.
        std::uint32_t const tankTickStamp = DcTickHeartbeat::LastMs(context);
        snap.dcTickSeen = tankTickStamp != 0;
        snap.dcTickAgeMs = SinceMs(tankTickStamp);

        // --- target -------------------------------------------------------
        snap.stickyBoss = AI_VALUE(uint32, DcKey::StickyBoss);
        std::optional<DungeonBossInfo> const next =
            context->GetValue<std::optional<DungeonBossInfo>>(DcKey::NextDungeonBoss)->Get();
        if (next.has_value())
        {
            snap.nextBossEntry = next->entry;
            snap.nextBossName = next->name;
            if (next->mapId == snap.mapId)
                snap.distToTarget = tank->GetDistance(next->x, next->y, next->z);
        }

        DcApproachState const& appr = AI_VALUE(DcApproachState&, DcKey::ApproachState);
        snap.committedTargetEntry = appr.lastTargetEntry;
        // The cache key of the built route — i.e. the boss the current path
        // actually leads to, which is NOT the same notion as lastTargetEntry
        // (that one is the committed target, the same notion as StickyBoss).
        snap.approachTargetEntry = appr.longPathTargetEntry;

        // Three independent notions of "the boss we are going to" that must
        // agree: the committed target, the recomputed target, and the one the
        // built route actually leads to. A disagreement here is the signature
        // of the run chasing a target it has no path for — worth flagging
        // explicitly rather than making a reader diff three numbers.
        snap.targetMismatch =
            (snap.nextBossEntry && snap.stickyBoss && snap.nextBossEntry != snap.stickyBoss) ||
            (snap.nextBossEntry && snap.committedTargetEntry &&
             snap.nextBossEntry != snap.committedTargetEntry) ||
            (snap.nextBossEntry && snap.approachTargetEntry &&
             snap.nextBossEntry != snap.approachTargetEntry);

        // --- route --------------------------------------------------------
        ChunkedPathfinder::Result const& path =
            AI_VALUE(ChunkedPathfinder::Result&, DcKey::LongPath);
        snap.pathReachable = path.reachable;
        snap.pathComplete = path.complete;
        snap.pathStartFarFromPoly = path.startFarFromPoly;
        snap.pathFailureReason = path.failureReason;
        snap.pathSegments = static_cast<std::uint32_t>(path.segments.size());

        DungeonFollowerState const& follower =
            AI_VALUE(DungeonFollowerState&, DcKey::FollowerState);
        snap.segmentIdx = follower.segmentIdx;
        snap.pointIdx = follower.pointIdx;
        snap.offPathTicks = follower.offPathTicks;
        // RouteDeviation returns 0 when the cursor is past the path end, which
        // is indistinguishable from "dead on the corridor" — and a cursor past
        // the end is exactly the path-ends-short livelock. Only record a
        // deviation when there is a real point to measure against.
        if (!path.segments.empty() &&
            DungeonPathFollower::CurrentPoint(path, follower).has_value())
            snap.routeDeviation = DungeonPathFollower::RouteDeviation(tank, path, follower);
        else
            snap.cursorPastPathEnd = !path.segments.empty();

        // --- wedge watchdogs ----------------------------------------------
        snap.routeGlideStuck = appr.routeGlideWatch.stuckTicks;
        snap.doorWalkInStuck = appr.doorWalkInWatch.stuckTicks;
        snap.pursuitStuck = appr.pursuitWatch.stuckTicks;
        snap.finalApproachStuck = appr.finalApproachWatch.stuckTicks;
        snap.stuckCount = appr.stuckCount;
        snap.rebuildAttempts = appr.rebuildAttempts;
        snap.resnapAttempts = appr.resnapAttempts;
        snap.partyNotReadyTicks = appr.partyNotReadyTicks;

        snap.doorStalled = !appr.doorStallGuid.IsEmpty();
        snap.doorStalledForMs = snap.doorStalled ? SinceMs(appr.doorStallSinceMs) : 0u;

        // --- pull ---------------------------------------------------------
        DcPullContext const& pull = AI_VALUE(DcPullContext&, DcKey::PullContext);
        snap.pullSetting = AI_VALUE(uint32, DcKey::PullSetting);
        snap.pullPhase = static_cast<std::uint32_t>(pull.phase);
        snap.pullDecision = static_cast<std::uint32_t>(pull.decision);
        snap.pullPhaseForMs = SinceMs(pull.phaseSince);
        snap.pullFizzleCount = pull.fizzleCount;
        snap.pullHasCamp = pull.HasCamp();

        // --- party --------------------------------------------------------
        // GetFirstMember walks the whole raid, which is what we want: a member
        // that fell out of the tank's sub-group still matters to the report.
        if (Group* group = tank->GetGroup())
        {
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->GetSource();
                if (!member)
                    continue;

                MemberSnapshot m;
                m.name = member->GetName();
                m.guid = member->GetGUID().GetRawValue();
                m.level = member->GetLevel();
                m.online = true;
                m.isBot = GET_PLAYERBOT_AI(member) != nullptr;
                m.mapId = member->GetMapId();
                m.x = member->GetPositionX();
                m.y = member->GetPositionY();
                m.z = member->GetPositionZ();
                m.distToTank = (m.mapId == snap.mapId) ? tank->GetDistance(member) : -1.f;
                m.floor = ProbeFloor(member);
                if (Vehicle* const vehicle = member->GetVehicle())
                {
                    m.onVehicle = true;
                    if (Unit* const base = vehicle->GetBase())
                    {
                        m.baseEntry = base->GetEntry();
                        m.baseX = base->GetPositionX();
                        m.baseY = base->GetPositionY();
                        m.baseZ = base->GetPositionZ();
                    }
                    VehicleSeatEntry const* const seat = vehicle->GetSeatForPassenger(member);
                    m.seatCanControl = seat && seat->CanControl();
                }
                m.alive = member->IsAlive();
                m.healthPct = static_cast<std::uint32_t>(member->GetHealthPct());
                if (member->getPowerType() == POWER_MANA)
                    m.manaPct = PowerPct(member, POWER_MANA);
                m.inCombat = member->IsInCombat();
                if (Unit* victim = member->GetVictim())
                    m.victim = victim->GetName();

                if (PlayerbotAI* memberAI = GET_PLAYERBOT_AI(member))
                {
                    m.dcStrategy = memberAI->HasStrategy(kDcNonCombatStrategy, BOT_STATE_NON_COMBAT);
                    m.dcCombatStrategy = memberAI->HasStrategy(kDcCombatStrategy, BOT_STATE_COMBAT);
                    m.botState = memberAI->GetState() == BOT_STATE_COMBAT ? "combat" : "noncombat";

                    // Only ask a bot that actually carries a DC ladder. Reading
                    // the value lazily creates it, and creating DC values on a
                    // bot that has never run DC would be both meaningless and a
                    // write this read-only capture has no business making.
                    if (m.dcStrategy || m.dcCombatStrategy)
                    {
                        std::uint32_t const stamp =
                            DcTickHeartbeat::LastMs(memberAI->GetAiObjectContext());
                        m.dcTickSeen = stamp != 0;
                        m.dcTickAgeMs = SinceMs(stamp);
                    }
                }

                // Only for members actually flagged: each holder row costs a
                // pathfind, and a member out of combat has nothing to blame.
                if (m.inCombat)
                    CaptureCombatHolders(member, m);

                if (m.alive)
                    ++snap.aliveCount;
                if (m.inCombat)
                    ++snap.inCombatCount;
                snap.members.push_back(std::move(m));
            }

            // Members that logged out / were never resolved never appear above;
            // recording them as offline rows is the difference between "the
            // healer died" and "the healer is not in the world any more".
            for (Group::MemberSlot const& slot : group->GetMemberSlots())
            {
                bool found = false;
                for (MemberSnapshot const& m : snap.members)
                    if (m.guid == slot.guid.GetRawValue())
                    {
                        found = true;
                        break;
                    }
                if (found)
                    continue;

                // Group::MemberSlot carries no level — an offline row reports
                // level 0, which the reader should treat as "unknown", not "1".
                MemberSnapshot m;
                m.name = slot.name;
                m.guid = slot.guid.GetRawValue();
                m.online = false;
                m.distToTank = -1.f;
                ++snap.offlineCount;
                snap.members.push_back(std::move(m));
            }
        }
        snap.partySize = static_cast<std::uint32_t>(snap.members.size());

        // --- roster -------------------------------------------------------
        // Completion is derived exactly as NextDungeonBossValue derives it —
        // all three paths — and doneVia records WHICH one fired. That is the
        // field that distinguishes a boss the watchdog can see from one it
        // cannot: only "mask" and "anchor" completions reset the no-progress
        // timer, so a roster full of "bossState" kills explains a no_progress
        // verdict on a run that was actually going fine.
        std::vector<DungeonBossInfo> const& bosses =
            AI_VALUE(std::vector<DungeonBossInfo>, DcKey::DungeonBosses);
        for (DungeonBossInfo const& info : bosses)
        {
            BossSnapshot b;
            b.entry = info.entry;
            b.orderKey = BossOrderKey(info);
            b.name = info.name;
            b.kind = (info.kind == DungeonAnchorKind::Boss) ? "boss" : "objective";
            b.encounterIndex = static_cast<std::int32_t>(info.encounterIndex);
            b.x = info.x;
            b.y = info.y;
            b.z = info.z;
            b.isTarget = (snap.nextBossEntry && info.entry == snap.nextBossEntry);
            b.isSticky = (snap.stickyBoss && info.entry == snap.stickyBoss);

            bool done = false;
            if (cleared.count(info.entry))
            {
                done = true;
                b.doneVia = "anchor";
            }
            else if (info.kind == DungeonAnchorKind::Boss && info.encounterIndex < 32 &&
                     (snap.completedEncounterMask & (1u << info.encounterIndex)))
            {
                done = true;
                b.doneVia = "mask";
            }
            else if (DcAnchorDoneVia const via = DcAnchorDoneByInstance(info, inst);
                     via != DcAnchorDoneVia::None)
            {
                done = true;
                b.doneVia = via == DcAnchorDoneVia::BossState ? "bossState" : "instanceData";
            }

            // Same ladder DcBossesAction paints the panel rows with, so the
            // report and the in-game boss list can never disagree.
            if (done)
                b.status = "dead";
            else if (skipped.count(info.entry))
                b.status = "skipped";
            else if (DcTargeting::FindLiveCreatureOnMap(tank, info.entry))
                b.status = "alive";
            else if (DcTargeting::IsCreaturePresentOnMap(tank, info.entry))
                b.status = "dead";  // corpse on the map, completion not latched yet
            else if (seen.count(info.entry))
                b.status = "missing";
            else
                b.status = "alive";  // never seen: grid most likely not loaded

            snap.roster.push_back(std::move(b));
        }

        CaptureEscort(tank, context, snap.escort);

        return snap;
    }

    void AppendJson(std::ostringstream& s, Snapshot const& snap)
    {
        s << "{\"valid\":" << (snap.valid ? "true" : "false") << ",\"capturedAt\":";
        AppendEscaped(s, snap.capturedAt);
        if (!snap.valid)
        {
            s << '}';
            return;
        }

        AppendBool(s, "enabled", snap.enabled);
        AppendBool(s, "paused", snap.paused);
        s << ",\"pauseReason\":";
        AppendEscaped(s, snap.pauseReason);
        AppendBool(s, "pausedAtDoor", snap.pausedAtDoor);
        s << ",\"selectedBossEntry\":" << snap.selectedBossEntry;
        AppendBool(s, "smartRestLatched", snap.smartRestLatched);

        s << ",\"phase\":";
        AppendEscaped(s, snap.phase);
        s << ",\"state\":";
        AppendEscaped(s, snap.stateStr);
        s << ",\"detail\":";
        AppendEscaped(s, snap.detail);
        s << ",\"stallReason\":";
        AppendEscaped(s, snap.stallReason);

        s << ",\"target\":{\"sticky\":" << snap.stickyBoss
          << ",\"nextEntry\":" << snap.nextBossEntry
          << ",\"nextName\":";
        AppendEscaped(s, snap.nextBossName);
        s << ",\"committedEntry\":" << snap.committedTargetEntry
          << ",\"approachEntry\":" << snap.approachTargetEntry
          << ",\"distance\":" << snap.distToTarget
          << ",\"mismatch\":" << (snap.targetMismatch ? "true" : "false") << '}';

        s << ",\"route\":{\"reachable\":" << (snap.pathReachable ? "true" : "false")
          << ",\"complete\":" << (snap.pathComplete ? "true" : "false")
          << ",\"startFarFromPoly\":" << (snap.pathStartFarFromPoly ? "true" : "false")
          << ",\"failureReason\":";
        AppendEscaped(s, snap.pathFailureReason);
        s << ",\"segments\":" << snap.pathSegments
          << ",\"segmentIdx\":" << snap.segmentIdx
          << ",\"pointIdx\":" << snap.pointIdx
          << ",\"offPathTicks\":" << snap.offPathTicks
          << ",\"deviation\":" << snap.routeDeviation
          << ",\"cursorPastPathEnd\":" << (snap.cursorPastPathEnd ? "true" : "false") << '}';

        s << ",\"watchdogs\":{\"routeGlide\":" << snap.routeGlideStuck
          << ",\"doorWalkIn\":" << snap.doorWalkInStuck
          << ",\"pursuit\":" << snap.pursuitStuck
          << ",\"finalApproach\":" << snap.finalApproachStuck
          << ",\"stuckCount\":" << snap.stuckCount
          << ",\"rebuildAttempts\":" << snap.rebuildAttempts
          << ",\"resnapAttempts\":" << snap.resnapAttempts
          << ",\"partyNotReadyTicks\":" << snap.partyNotReadyTicks
          << ",\"doorStalled\":" << (snap.doorStalled ? "true" : "false")
          << ",\"doorStalledForMs\":" << snap.doorStalledForMs << '}';

        s << ",\"pull\":{\"setting\":" << snap.pullSetting
          << ",\"phase\":" << snap.pullPhase
          << ",\"decision\":" << snap.pullDecision
          << ",\"phaseForMs\":" << snap.pullPhaseForMs
          << ",\"fizzleCount\":" << snap.pullFizzleCount
          << ",\"hasCamp\":" << (snap.pullHasCamp ? "true" : "false") << '}';

        s << ",\"world\":{\"map\":" << snap.mapId
          << ",\"instance\":" << snap.instanceId
          << ",\"x\":" << snap.tankX << ",\"y\":" << snap.tankY << ",\"z\":" << snap.tankZ
          << ",\"inCombat\":" << (snap.tankInCombat ? "true" : "false")
          << ",\"moving\":" << (snap.tankMoving ? "true" : "false")
          << ",\"victim\":";
        AppendEscaped(s, snap.tankVictim);
        s << ",\"completedEncounterMask\":" << snap.completedEncounterMask
          << ",\"clearedAnchors\":" << snap.clearedAnchors
          << ",\"skipped\":" << snap.skippedCount
          // -1 == the DC ladder has never run for this bot; otherwise ms since.
          << ",\"dcTickAgeMs\":"
          << (snap.dcTickSeen ? static_cast<std::int64_t>(snap.dcTickAgeMs) : -1)
          << '}';

        s << ",\"party\":{\"size\":" << snap.partySize
          << ",\"alive\":" << snap.aliveCount
          << ",\"offline\":" << snap.offlineCount
          << ",\"inCombat\":" << snap.inCombatCount
          << ",\"members\":[";
        for (std::size_t i = 0; i < snap.members.size(); ++i)
        {
            MemberSnapshot const& m = snap.members[i];
            if (i)
                s << ',';
            s << "{\"name\":";
            AppendEscaped(s, m.name);
            s << ",\"guid\":" << m.guid
              << ",\"level\":" << m.level
              << ",\"bot\":" << (m.isBot ? "true" : "false")
              << ",\"online\":" << (m.online ? "true" : "false")
              << ",\"map\":" << m.mapId
              << ",\"x\":" << m.x << ",\"y\":" << m.y << ",\"z\":" << m.z
              << ",\"distToTank\":" << m.distToTank
              << ",\"alive\":" << (m.alive ? "true" : "false")
              << ",\"hp\":" << m.healthPct
              << ",\"mp\":" << m.manaPct
              << ",\"inCombat\":" << (m.inCombat ? "true" : "false")
              << ",\"victim\":";
            AppendEscaped(s, m.victim);
            AppendFloor(s, m.floor);
            s << ",\"dcStrategy\":" << (m.dcStrategy ? "true" : "false")
              << ",\"dcCombatStrategy\":" << (m.dcCombatStrategy ? "true" : "false")
              << ",\"dcTickAgeMs\":"
              << (m.dcTickSeen ? static_cast<std::int64_t>(m.dcTickAgeMs) : -1)
              << ",\"botState\":";
            AppendEscaped(s, m.botState);
            // Emitted only for a rider, so every existing reader sees the same row.
            if (m.onVehicle)
                s << ",\"onVehicle\":true,\"baseEntry\":" << m.baseEntry
                  << ",\"baseX\":" << m.baseX << ",\"baseY\":" << m.baseY << ",\"baseZ\":" << m.baseZ
                  << ",\"seatCanControl\":" << (m.seatCanControl ? "true" : "false");
            // Emitted only for a flagged member, so an absent block means "was
            // not in combat" rather than "nothing was holding it" — the two
            // read identically if every member carries an empty array.
            if (m.inCombat)
            {
                s << ",\"attackers\":" << m.attackerCount
                  << ",\"holderRefs\":" << m.holderRefCount;
                AppendBool(s, "phantomCombat", m.phantomCombat);
                s << ",\"combatHolders\":[";
                for (std::size_t h = 0; h < m.combatHolders.size(); ++h)
                {
                    CombatHolderSnapshot const& c = m.combatHolders[h];
                    if (h)
                        s << ',';
                    s << "{\"name\":";
                    AppendEscaped(s, c.name);
                    s << ",\"entry\":" << c.entry
                      << ",\"guid\":" << c.guid
                      << ",\"dist\":" << c.dist
                      << ",\"hp\":" << c.healthPct
                      << ",\"x\":" << c.x << ",\"y\":" << c.y << ",\"z\":" << c.z;
                    AppendBool(s, "creature", c.isCreature);
                    AppendBool(s, "pvp", c.pvp);
                    AppendBool(s, "alive", c.alive);
                    AppendBool(s, "sameMap", c.sameMap);
                    AppendBool(s, "evading", c.evading);
                    AppendBool(s, "suppressed", c.suppressed);
                    AppendBool(s, "reachable", c.reachable);
                    AppendBool(s, "reachChecked", c.reachChecked);
                    AppendBool(s, "canAttackMe", c.canAttackMe);
                    AppendBool(s, "trigger", c.trigger);
                    AppendBool(s, "immune", c.immune);
                    AppendBool(s, "passive", c.passive);
                    AppendBool(s, "atOneHp", c.atOneHp);
                    AppendBool(s, "legitimate", c.legitimate);
                    AppendFloor(s, c.floor);
                    s << ",\"victim\":";
                    AppendEscaped(s, c.victim);
                    s << '}';
                }
                s << ']';
            }
            s << '}';
        }
        s << "]}";

        s << ",\"roster\":[";
        for (std::size_t i = 0; i < snap.roster.size(); ++i)
        {
            BossSnapshot const& b = snap.roster[i];
            if (i)
                s << ',';
            s << "{\"entry\":" << b.entry
              << ",\"order\":" << b.orderKey
              << ",\"name\":";
            AppendEscaped(s, b.name);
            s << ",\"kind\":";
            AppendEscaped(s, b.kind);
            s << ",\"status\":";
            AppendEscaped(s, b.status);
            s << ",\"doneVia\":";
            AppendEscaped(s, b.doneVia);
            s << ",\"encounterIndex\":" << b.encounterIndex
              << ",\"x\":" << b.x << ",\"y\":" << b.y << ",\"z\":" << b.z
              << ",\"isTarget\":" << (b.isTarget ? "true" : "false")
              << ",\"isSticky\":" << (b.isSticky ? "true" : "false")
              << '}';
        }
        s << ']';

        // Only while an escort step is active: an absent key means "no escort
        // running", which an always-present empty block would blur.
        if (snap.escort.active)
        {
            EscortSnapshot const& e = snap.escort;
            s << ",\"escort\":{\"eventId\":" << e.eventId << ",\"eventName\":";
            AppendEscaped(s, e.eventName);
            s << ",\"stepIndex\":" << e.stepIndex
              << ",\"entry\":" << e.entry
              << ",\"searchRadius\":" << e.searchRadius;
            AppendBool(s, "driverSees", e.driverSees);
            s << ",\"driverSeesGuid\":" << e.driverSeesGuid
              // -1 == the dead-air clock has never been stamped.
              << ",\"deadAirMs\":" << (e.deadAirSeen ? static_cast<std::int64_t>(e.deadAirMs) : -1)
              << ",\"instanceDataId\":" << e.instanceDataId
              << ",\"instanceData\":" << e.instanceData
              << ",\"instanceDataMin\":" << e.instanceDataMin
              << ",\"copyCount\":" << e.copyCount
              << ",\"copies\":[";
            for (std::size_t i = 0; i < e.copies.size(); ++i)
            {
                EscorteeCopy const& c = e.copies[i];
                if (i)
                    s << ',';
                s << "{\"guid\":" << c.guid
                  << ",\"spawnId\":" << c.spawnId
                  << ",\"deathState\":" << c.deathState
                  << ",\"x\":" << c.x << ",\"y\":" << c.y << ",\"z\":" << c.z
                  << ",\"homeX\":" << c.homeX << ",\"homeY\":" << c.homeY << ",\"homeZ\":" << c.homeZ
                  << ",\"distToTank\":" << c.distToTank
                  << ",\"faction\":" << c.faction;
                AppendBool(s, "alive", c.alive);
                AppendBool(s, "inWorld", c.inWorld);
                AppendBool(s, "moving", c.moving);
                AppendBool(s, "inCombat", c.inCombat);
                AppendBool(s, "evading", c.evading);
                AppendBool(s, "gossipFlag", c.gossipFlag);
                AppendBool(s, "updateNeeded", c.updateNeeded);
                AppendBool(s, "gridLoaded", c.gridLoaded);
                AppendBool(s, "seenByGridScan", c.seenByGridScan);
                s << '}';
            }
            s << "]}";
        }
        s << '}';
    }

    std::string Summarize(Snapshot const& snap)
    {
        if (!snap.valid)
            return "diag: tank unresolvable at capture time";

        std::ostringstream s;
        s.precision(1);
        s << std::fixed;
        s << "state=" << (snap.stateStr.empty() ? "?" : snap.stateStr)
          << " phase=" << (snap.phase.empty() ? "-" : snap.phase)
          << " boss=" << (snap.nextBossName.empty() ? "none" : snap.nextBossName)
          << "(" << snap.nextBossEntry << ")"
          << " dist=" << snap.distToTarget
          << " pos=" << snap.mapId << ":" << snap.tankX << "," << snap.tankY << "," << snap.tankZ
          << " party=" << snap.aliveCount << "/" << snap.partySize << " alive"
          << (snap.offlineCount ? " offline=" + std::to_string(snap.offlineCount) : "")
          << " combat=" << snap.inCombatCount
          << " route=" << (snap.pathReachable ? "ok" : "UNREACHABLE")
          << "/" << snap.pathSegments << "seg"
          << " dev=" << snap.routeDeviation
          << " stuck=" << snap.routeGlideStuck << "/" << snap.pursuitStuck
          << "/" << snap.finalApproachStuck
          << " resnaps=" << snap.resnapAttempts;
        // Only when it is load-bearing. A DC ladder is evaluated every AI tick
        // (>=100ms apart), so a healthy age is hundreds of milliseconds; the
        // threshold sits far past any legitimate scheduling hiccup so this line
        // never fires on a busy world. When it DOES fire, the companion count is
        // the whole diagnosis: a stale tank beside live followers is a bot that
        // stopped being updated, while every member stale together is the run
        // (or the server) having stopped, which is a different bug entirely.
        constexpr std::uint32_t kTickStaleMs = 5000;
        if (!snap.dcTickSeen || snap.dcTickAgeMs >= kTickStaleMs)
        {
            std::uint32_t live = 0, dcMembers = 0;
            for (MemberSnapshot const& m : snap.members)
            {
                if (!m.dcStrategy && !m.dcCombatStrategy)
                    continue;
                ++dcMembers;
                if (m.dcTickSeen && m.dcTickAgeMs < kTickStaleMs)
                    ++live;
            }
            if (!snap.dcTickSeen)
                s << " DC-TICK-NEVER";
            else
                s << " DC-TICK-STALE " << (snap.dcTickAgeMs / 1000) << "s";
            s << " (ticking " << live << "/" << dcMembers << ")";
        }
        if (snap.doorStalled)
            s << " DOOR-STALLED " << (snap.doorStalledForMs / 1000) << "s";
        if (snap.targetMismatch)
            s << " TARGET-MISMATCH sticky=" << snap.stickyBoss
              << " approach=" << snap.approachTargetEntry;
        if (!snap.pathFailureReason.empty())
            s << " pathFail=\"" << snap.pathFailureReason << "\"";
        if (!snap.stallReason.empty())
            s << " stall=\"" << snap.stallReason << "\"";
        // Only when it is load-bearing. A member flagged with nothing on it, or
        // flagged while sitting on the non-combat engine, is the shape of every
        // stuck-in-combat freeze this harness has recorded — worth a token on
        // the one line people actually grep, not just in the JSON.
        std::uint32_t phantom = 0, offEngine = 0;
        for (MemberSnapshot const& m : snap.members)
        {
            if (!m.inCombat)
                continue;
            if (m.phantomCombat)
                ++phantom;
            if (m.botState == "noncombat")
                ++offEngine;
        }
        if (phantom)
            s << " PHANTOM-COMBAT=" << phantom;
        if (offEngine)
            s << " FLAGGED-OFF-COMBAT-ENGINE=" << offEngine;
        std::uint32_t underMesh = 0;
        std::uint32_t riders = 0;
        for (MemberSnapshot const& m : snap.members)
        {
            if (m.onVehicle)
                ++riders;  // airborne by design: its column's mesh is the ring below
            else if (m.online && IsUnderMesh(m.floor, m.z))
                ++underMesh;
        }
        if (underMesh)
            s << " UNDER-MESH=" << underMesh;
        if (riders)
            s << " RIDERS=" << riders;
        // The escort driver's own view disagreeing with the map is the question
        // the Town Hall stall left open; say which way it disagrees.
        if (snap.escort.active && !snap.escort.driverSees)
        {
            s << " ESCORTEE-UNSEEN copies=" << snap.escort.copyCount;
            if (!snap.escort.copies.empty())
            {
                EscorteeCopy const& c = snap.escort.copies.front();
                s << " nearest=" << c.distToTank << "yd"
                  << (c.alive ? " alive" : " DEAD")
                  << (c.updateNeeded ? "" : " NOT-UPDATED")
                  << (c.seenByGridScan ? "" : " GRID-BLIND");
            }
        }
        return s.str();
    }

    std::string SummarizeCombat(Snapshot const& snap)
    {
        if (!snap.valid)
            return "combat blame: tank unresolvable at capture time";
        // Derived from the member rows, NOT from inCombatCount. The counter is a
        // separate field, and if the two ever disagree this function would
        // report "nobody is flagged" while printing nothing about members that
        // plainly are — a confident denial is worse than no line at all.
        bool const anyFlagged =
            std::any_of(snap.members.begin(), snap.members.end(),
                        [](MemberSnapshot const& m) { return m.inCombat; });
        if (!anyFlagged)
            return "combat blame: nobody in the party is flagged in combat";

        std::ostringstream s;
        s.precision(1);
        s << std::fixed;
        bool first = true;
        for (MemberSnapshot const& m : snap.members)
        {
            if (!m.inCombat)
                continue;
            if (!first)
                s << " | ";
            first = false;
            s << m.name
              << (m.onVehicle ? " ON-VEHICLE" : (IsUnderMesh(m.floor, m.z) ? " UNDER-MESH" : ""))
              << " [engine=" << (m.botState.empty() ? "?" : m.botState)
              << " attackers=" << m.attackerCount
              << " victim=" << (m.victim.empty() ? "-" : m.victim)
              << (m.phantomCombat ? " PHANTOM" : "") << "] held by ";
            if (m.combatHolders.empty())
            {
                // Distinguish the two ways a flagged member can have nothing to
                // show: no refs at all (the hatch's opaque case) from refs that
                // exist but were all past the cap (impossible today, but the
                // reader should never have to assume which).
                s << (m.holderRefCount ? "(all rows truncated)" : "NOTHING (no combat refs)");
                continue;
            }
            for (std::size_t i = 0; i < m.combatHolders.size(); ++i)
            {
                CombatHolderSnapshot const& c = m.combatHolders[i];
                if (i)
                    s << ", ";
                s << c.name << "(" << c.entry << ")"
                  << " " << c.dist << "yd " << c.healthPct << "%"
                  << (c.alive ? "" : " DEAD")
                  << (c.sameMap ? "" : " OTHER-MAP")
                  << (c.evading ? " EVADING" : "")
                  << (c.suppressed ? " SUPPRESSED" : "")
                  << (c.immune ? " IMMUNE" : "")
                  << (c.passive ? " PASSIVE" : "")
                  << (c.atOneHp ? " AT-1-HP" : "")
                  << (!c.reachChecked ? " path-not-tested"
                                      : (c.reachable ? " reachable" : " UNREACHABLE"))
                  << (c.canAttackMe ? "" : " CANNOT-ATTACK-ME")
                  << (c.trigger ? " TRIGGER" : "")
                  << (IsUnderMesh(c.floor, c.z) ? " UNDER-MESH" : "")
                  << (c.legitimate ? " -> LEGITIMATE" : " -> phantom");
                if (!c.victim.empty())
                    s << " fighting=" << c.victim;
            }
            if (m.holderRefCount > m.combatHolders.size())
                s << " (+" << (m.holderRefCount - m.combatHolders.size()) << " more refs)";
        }
        return s.str();
    }
}
