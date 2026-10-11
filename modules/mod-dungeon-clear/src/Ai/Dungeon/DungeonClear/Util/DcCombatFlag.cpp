/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DcCombatFlag.h"

#include "Ai/Dungeon/DungeonClear/DcApproachState.h"
#include "Ai/Dungeon/DungeonClear/DcValueKeys.h"
#include "Ai/Dungeon/DungeonClear/Util/DcEngageGeometry.h"
#include "Ai/Dungeon/DungeonClear/Util/DcTickMemo.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonClearMath.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonClearTuning.h"

#include "AiObjectContext.h"
#include "Value.h"
#include "CombatManager.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "Group.h"
#include "Map.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "Timer.h"

namespace DcCombatFlag
{
    bool IsEngaged(Player* p)
    {
        if (!p || !p->IsAlive() || !p->IsInWorld())
            return false;

        // Range-qualified on both sides (see DC_ENGAGEMENT_RADIUS). A combat
        // reference survives the geometry that made it unusable, so "something is
        // on my attacker list" is not the same question as "I am in a fight" the
        // moment anything crosses a navmesh break. 3D distance, not 2D: the
        // Azjol-Nerub case that produced this is 366yd of it VERTICAL.
        //
        // SQUARED distance, and IsInWorld before GetMap, both because of where
        // this sits. It is the module's hottest predicate — every follower rung
        // asks it every tick, AnyPartyEngagement fans it across the group, and a
        // tank in an AoE pack carries a double-digit attacker set — so the sqrt
        // is not worth paying and neither is a needless map compare. GetMap()
        // ASSERTs on a null map, and the attacker set holds raw pointers we now
        // dereference (the old `.empty()` test never did), so nothing here may
        // touch a unit on its way out of the world.
        Map const* const map = p->GetMap();
        constexpr float radiusSq = DC_ENGAGEMENT_RADIUS * DC_ENGAGEMENT_RADIUS;

        // EVADING IS NOT FIGHTING, and this is the second qualifier the radius
        // could not supply. A creature in evade mode is on its way out of combat
        // and back to its spawn: it will not swing, it cannot be tanked, and
        // nothing the party does resolves it. It is exactly the shape that holds
        // the flag with no fight behind it — and unlike the stranding case the
        // radius was added for, it happens WELL INSIDE 100yd, so the radius never
        // sees it.
        //
        // The case that produced this: BWL's Vaelastrasz->staging hall runs 24.3yd
        // directly under the upper suppression room, and 24% of that route is
        // inside the 3D aggro radius of the trash standing on the floor above.
        // Those mobs aggro through the floor, cannot path down, and evade where
        // they stand at 100% HP — `first contact: Blackwing Technician at 25.0yd,
        // 0.0yd from its spawn` is the fingerprint, the mob never moved. On
        // tp-20260828-175353-1 one member with twelve such attackers made
        // AnyPartyEngagement true for the whole party, MayDrive false for the
        // leader, and every MayDrive-gated rung — Advance included — inert. All
        // five raids stopped there permanently.
        //
        // The SAME evade question ScanCombatHolders asks of the same units
        // (CombatManager::IsInEvadeMode, not Creature's UNIT_STATE_EVADE) — and the
        // same one DcDiagSnapshot prints as EVADING in the teardown dump. Two
        // predicates about "is this holder real" disagreeing on what evading means
        // is how a triage ends up reading a blame table that contradicts the gate
        // it is trying to explain.
        //
        // Cheap enough for the module's hottest predicate: a reference read and an
        // integer compare, kept behind the distance test. No pathfind, unlike
        // ScanCombatHolders' reachability walk — this answers a strictly narrower
        // question and pays a strictly smaller price for it.
        auto const inFight = [p, map](Unit const* other)
        {
            if (!other || !other->IsInWorld() || other->GetMap() != map)
                return false;
            if (p->GetExactDistSq(other) > radiusSq)
                return false;
            return !other->GetCombatManager().IsInEvadeMode();
        };

        if (inFight(p->GetVictim()))
            return true;

        for (Unit const* const attacker : p->getAttackers())
            if (inFight(attacker))
                return true;

        return false;
    }

    namespace
    {
        // The caller's own per-tick memo, or nullptr for a human/context-less
        // read. Same 50ms within-tick contract as every DcTickMemo consumer:
        // several rungs ask these party-level predicates each tick, and at raid
        // sizes an unmemoised answer is a fresh full-group walk every time.
        DcTickMemo* MemoFor(Player* bot)
        {
            PlayerbotAI* ai = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
            AiObjectContext* ctx = ai ? ai->GetAiObjectContext() : nullptr;
            if (!ctx)
                return nullptr;
            DcTickMemo& m = ctx->GetValue<DcTickMemo&>(DcKey::TickMemo)->Get();
            m.EnsureFresh(getMSTime());
            return &m;
        }
    }

    bool AnyPartyEngagement(Player* bot)
    {
        if (!bot)
            return false;

        DcTickMemo* const memo = MemoFor(bot);
        if (memo && memo->partyEngagement >= 0)
            return memo->partyEngagement == 1;

        auto const compute = [&]()
        {
            if (IsEngaged(bot))
                return true;
            Group* group = bot->GetGroup();
            if (!group)
                return false;
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->GetSource();
                if (member && member != bot && member->GetMapId() == bot->GetMapId() &&
                    IsEngaged(member))
                    return true;
            }
            return false;
        };
        bool const engaged = compute();
        if (memo)
            memo->partyEngagement = engaged ? 1 : 0;
        return engaged;
    }

    bool AnyPartyCombatFlag(Player* bot)
    {
        if (!bot)
            return false;

        DcTickMemo* const memo = MemoFor(bot);
        if (memo && memo->partyCombatFlag >= 0)
            return memo->partyCombatFlag == 1;

        auto const compute = [&]()
        {
            if (bot->IsInCombat())
                return true;
            Group* group = bot->GetGroup();
            if (!group)
                return false;
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->GetSource();
                if (member && member != bot && member->IsAlive() &&
                    member->GetMapId() == bot->GetMapId() && member->IsInCombat())
                    return true;
            }
            return false;
        };
        bool const flagged = compute();
        if (memo)
            memo->partyCombatFlag = flagged ? 1 : 0;
        return flagged;
    }

    HolderScan ScanCombatHolders(Player* p)
    {
        HolderScan scan;
        if (!p)
            return scan;

        auto const& refs = p->GetCombatManager().GetPvECombatRefs();
        if (refs.empty())
        {
            scan.opaque = true;
            return scan;
        }

        Map* const map = p->GetMap();
        for (auto const& kv : refs)
        {
            CombatReference* const ref = kv.second;
            if (!ref)
                continue;
            Unit* const other = ref->GetOther(p);
            if (!other || !other->IsAlive() || other->GetMap() != map)
                continue;
            if (other->GetCombatManager().IsInEvadeMode())
                continue;  // holder is bailing home -> not a real threat
            // RANGE FIRST, and it is not just an optimisation. DcEngageGeometry::
            // IsReachable delegates to the CHUNKED pathfinder, which by design
            // accepts any path with forward progress so the tank can walk a boss
            // route farther than PathGenerator's ~296yd single-call cap. Handed a
            // holder on the far side of a one-way relocation it therefore answers
            // "reachable" — tr-20260818-223003-8's teardown reads `Skittering
            // Swarmer(32593) 346.9yd 100% reachable -> LEGITIMATE` about a mob
            // 350yd overhead through solid rock, and that verdict is what left the
            // phantom-combat hatch inert while the party sat wedged for eleven
            // minutes. Bound it at the same DC_ENGAGEMENT_RADIUS IsEngaged uses:
            // one sanity radius for "a combat reference has outlived the geometry
            // it was made in", asked the same way on both sides of the module.
            // Cheap, too — this runs before the per-reference pathfind.
            if (p->GetExactDistSq(other) > DC_ENGAGEMENT_RADIUS * DC_ENGAGEMENT_RADIUS)
                continue;  // left behind by geometry -> not a fight, whatever the mesh says
            // A holder nothing in the party can ever ATTACK is not a fight at all —
            // see DungeonClearMath::IsUnresolvableCombatHolder for why this is the
            // trigger flag and not the selectable one. Placed ahead of the pathfind
            // deliberately: it is a creature-template flag read, and the
            // reachability answer for a trigger parked on top of us is always
            // "yes", so asking that first only pays for a pathfind to reach the
            // wrong verdict.
            Creature* const holder = other->ToCreature();
            if (DungeonClearMath::IsUnresolvableCombatHolder(holder != nullptr,
                                                            holder && holder->IsTrigger()))
                continue;  // unkillable script helper -> a flag with no way out
            if (!DcEngageGeometry::IsReachable(p, other->GetPositionX(),
                                               other->GetPositionY(), other->GetPositionZ()))
                continue;  // unreachable -> the phantom holder
            if (holder && holder->AI() && !holder->AI()->CanAIAttack(p))
                continue;  // its own script forbids it touching us -> phantom too
            // Keep scanning so nearestDist is the CLOSEST such holder: every caller
            // asks a distance question of whichever holder is most nearly on top of
            // us, not of whichever the map happened to enumerate first.
            float const dist = p->GetExactDist(other);
            if (!scan.found || dist < scan.nearestDist)
                scan.nearestDist = dist;
            scan.found = true;
        }
        return scan;
    }

    bool IsHeldByLiveEnemy(Player* p, float radius)
    {
        // Cheap reads first, in the order that short-circuits most ticks: the
        // scan below costs a pathfind per combat reference.
        if (!p || !p->IsAlive() || !p->IsInCombat())
            return false;
        if (IsEngaged(p))
            return true;
        HolderScan const scan = ScanCombatHolders(p);
        // `opaque` deliberately does NOT count. The hatch reads it as "leave this
        // alone", but here the question is "is there something to fight", and a
        // flag with no reference behind it names nothing that could be fought.
        return scan.found && scan.nearestDist <= radius;
    }

    bool AnyPartyHeldByLiveEnemy(Player* bot, float radius)
    {
        if (!bot)
            return false;

        // Memoised per tick — this is the expensive one (a pathfind per combat
        // reference per member). The radius is part of the key: a read with a
        // different radius recomputes directly (rare; every hot site passes
        // DC_FIGHT_HOLDER_RADIUS).
        DcTickMemo* const memo = MemoFor(bot);
        if (memo && memo->partyHeldByLiveEnemy >= 0 && memo->partyHeldRadius == radius)
            return memo->partyHeldByLiveEnemy == 1;

        auto const compute = [&]()
        {
            if (IsHeldByLiveEnemy(bot, radius))
                return true;
            Group* group = bot->GetGroup();
            if (!group)
                return false;
            // Cap the per-tick cost at raid scale: past this many members
            // scanned without finding a holder, answer TRUE — the conservative
            // direction for every caller (the rez release and the phantom
            // hatch both treat "held" as "do not act yet"), and 12 members is
            // already far beyond what a real all-clear needs to prove in one
            // tick; the next tick re-asks with a fresh memo.
            constexpr int kMaxMemberScans = 12;
            int scanned = 0;
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->GetSource();
                if (!member || member == bot || member->GetMapId() != bot->GetMapId())
                    continue;
                if (++scanned > kMaxMemberScans)
                    return true;
                if (IsHeldByLiveEnemy(member, radius))
                    return true;
            }
            return false;
        };
        bool const held = compute();
        if (memo)
        {
            memo->partyHeldByLiveEnemy = held ? 1 : 0;
            memo->partyHeldRadius = radius;
        }
        return held;
    }

    bool MayDrive(Player* bot, AiObjectContext* context)
    {
        if (!bot || !context)
            return false;
        DcApproachState& appr = context->GetValue<DcApproachState&>(DcKey::ApproachState)->Get();
        return DungeonClearMath::MayDriveWhileFlagged(
            bot->IsInCombat(), AnyPartyEngagement(bot), getMSTime(),
            DC_FLAGGED_NO_ENGAGE_GRACE_MS, appr.flaggedNoEngageSinceMs);
    }

    bool MayDriveEvent(Player* bot, AiObjectContext* context, bool drivesInCombat)
    {
        if (!bot || !context)
            return false;
        DcApproachState& appr = context->GetValue<DcApproachState&>(DcKey::ApproachState)->Get();
        return DungeonClearMath::EventDueGateOpen(
            drivesInCombat, bot->IsInCombat(), AnyPartyEngagement(bot), getMSTime(),
            DC_FLAGGED_NO_ENGAGE_GRACE_MS, appr.flaggedNoEngageSinceMs);
    }

    bool IsPhantomFlag(Player* bot, AiObjectContext* context)
    {
        if (!bot || !context)
            return false;
        // PARTY-wide flag, unlike MayDrive's own-flag test: the between-pulls gate
        // waits for every member to top up, so one member an aura holds in combat
        // is enough to make the wait unsatisfiable — and the tank commonly drops
        // combat a second or two before its followers do.
        //
        // The same kernel and the same grace, on its own latch (see
        // DcApproachState). The grace is what stops this firing in the window at
        // the end of every ordinary fight, where the party is still flagged and
        // nothing is engaged any more — waiving the floors there would send the
        // tank to the next pull instead of drinking. Feeding the flag THROUGH the
        // kernel (rather than early-returning on it) is deliberate: an unflagged
        // tick must clear the streak, or a later phantom flag would inherit a
        // stale timestamp and skip its grace entirely.
        DcApproachState& appr = context->GetValue<DcApproachState&>(DcKey::ApproachState)->Get();
        bool const flagged = AnyPartyCombatFlag(bot);
        return DungeonClearMath::MayDriveWhileFlagged(
                   flagged, AnyPartyEngagement(bot), getMSTime(),
                   DC_FLAGGED_NO_ENGAGE_GRACE_MS, appr.partyFlaggedNoEngageSinceMs) &&
               flagged;
    }
}
