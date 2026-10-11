/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DcStrandedRecovery.h"

#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonBossInfo.h"
#include "Ai/Dungeon/DungeonClear/DcPullContext.h"
#include "Ai/Dungeon/DungeonClear/DcValueKeys.h"
#include "Ai/Dungeon/DungeonClear/Settings/DcSettings.h"
#include "Ai/Dungeon/DungeonClear/Util/DcBossStandDown.h"
#include "Ai/Dungeon/DungeonClear/Util/DcCombatFlag.h"
#include "Ai/Dungeon/DungeonClear/Util/DcLeaderSignal.h"
#include "Ai/Dungeon/DungeonClear/Util/DcPartyState.h"
#include "Ai/Dungeon/DungeonClear/Util/DcPullPlanner.h"
#include "Ai/Dungeon/DungeonClear/Util/DcRun.h"
#include "Ai/Dungeon/DungeonClear/Util/DcRunProgress.h"
#include "Ai/Dungeon/DungeonClear/Util/DcStatusPublisher.h"
#include "Ai/Dungeon/DungeonClear/Util/DcStrandedDecision.h"
#include "Ai/Dungeon/DungeonClear/Util/DcTargeting.h"

#include <cmath>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "AiObjectContext.h"
#include "Group.h"
#include "InstanceScript.h"
#include "Log.h"
#include "MotionMaster.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "Timer.h"

namespace
{
    // Walk the leader's same-map group into kernel rows and report whether the
    // party is FIGHTING. Recover re-walks the live group itself, so a plain value
    // snapshot is all the kernel needs here.
    //
    // Engagement, NOT the combat flag. This failsafe exists for exactly one
    // situation — a member stuck out of range forever while the tank waits on it —
    // and the raw flag disabled it in the one case that produced that situation
    // most reliably: a hostile area aura flags the whole party in with nothing
    // aggroed, so `partyEngaged` read true every tick, which both re-armed the
    // progress clock and made Decide() early-out. The failsafe could never fire
    // (Arcatraz heroic, tr-20260801-194932-20: followers parked 30yd back for 15
    // minutes, run killed by the no-progress watchdog). A real fight still blocks
    // the teleport and still counts as progress — that is what engagement means.
    void BuildSnapshot(Player* anchor, std::vector<DcStrandedDecision::Member>& out,
                       bool& partyEngaged)
    {
        partyEngaged = DcCombatFlag::AnyPartyEngagement(anchor);

        Group* group = anchor->GetGroup();
        if (!group)
            return;

        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (!member || !member->IsInWorld())
                continue;
            // A rider is never rescued (see Recover), so it is never a stray
            // either — otherwise Decide would ask for a rescue Recover refuses,
            // and ask again every tick.
            if (member != anchor && member->GetVehicle())
                continue;

            DcStrandedDecision::Member m;
            m.isBot = GET_PLAYERBOT_AI(member) != nullptr;
            m.isAlive = member->IsAlive();
            m.onMap = member->GetMapId() == anchor->GetMapId();
            m.isTank = member == anchor;
            m.distToTank = anchor->GetDistance(member);
            out.push_back(m);
        }
    }

    // Who runs this failsafe's tick, whose run state it reads, and the point the
    // strays are gathered on. One resolver so Evaluate and Recover can never
    // disagree about any of the three.
    //
    // HEALTHY RUN: the elected leader tank, gathering on itself. Byte-for-byte the
    // old behaviour.
    //
    // TANK DEAD: FindLeaderTank elects only among ALIVE tank bots, so in a 5-man it
    // returns nullptr the instant the tank dies — and this failsafe, gated on
    // IsDungeonClearLeader, switched itself off in the one state it exists for.
    // Gundrak tp-20260830-231921-1: five runs wiped on Slad'ran, the survivors sat
    // 122yd from the corpse for ten minutes each, and every one died to the 600s
    // no-progress watchdog with the rescue never once evaluated. FindTerminalDriver
    // is the module's existing answer to the same shape (see its header) and is
    // reused verbatim here, so every member computes the same driver and exactly
    // one fires.
    //
    // The anchor is then the run OWNER'S CORPSE, not the driver. That is the whole
    // point: the survivors' only way out is a rez, a rez is cast at the body, and a
    // rescue that gathers them anywhere else leaves the run exactly as stuck. It
    // also means the driver may be a stray itself — the sole survivor 122yd out is
    // both — so Recover moves the anchor's group, never "everyone but me".
    // The rescue's out-of-range threshold, matched to the gate the tank's
    // advance is ACTUALLY enforcing rather than to the raw setting. See
    // DcStrandedDecision::RescueSpread for the dead band this closes and why the
    // tank-anchored test is load-bearing. Resolved once per tick by Evaluate and
    // again by Recover; both go through this one body so they can never disagree
    // about who is stranded.
    float RescueSpread(Player* bot)
    {
        DcPartyState::SpreadGate const gate = DcPartyState::LeaderGate(bot);
        return DcStrandedDecision::RescueSpread(
            DcSettings::GetFloat(bot, "PartyMaxSpread"), gate.maxSpread,
            /*gateIsTankAnchored*/ gate.anchor == nullptr);
    }

    struct Driver
    {
        Player* clockOwner = nullptr;  // whose tick runs this
        Player* runOwner   = nullptr;  // whose DcRunState IS this run
        Player* anchor     = nullptr;  // where the strays are gathered
    };

    Driver ResolveDriver(Player* bot)
    {
        Driver d;
        if (!bot)
            return d;

        d.runOwner = DcLeaderSignal::FindRunOwner(bot);
        if (!d.runOwner)
            return d;

        if (Player* leader = DcLeaderSignal::FindLeaderTank(bot))
        {
            d.clockOwner = leader;
            d.anchor = leader;
            return d;
        }

        d.clockOwner = DcLeaderSignal::FindTerminalDriver(bot);
        d.anchor = d.runOwner;
        return d;
    }
}

namespace DcStrandedRecovery
{
    bool Enabled(Player* bot)
    {
        return DcSettings::GetBool(bot, "StrandedRecovery");
    }

    bool Evaluate(Player* bot)
    {
        if (!bot || bot->isDead())
            return false;

        // Single clock owner, alive tank or dead one (see ResolveDriver). A no-op
        // on every other bot.
        Driver const driver = ResolveDriver(bot);
        if (!driver.runOwner || !driver.anchor || driver.clockOwner != bot)
            return false;

        PlayerbotAI* leaderAI = GET_PLAYERBOT_AI(driver.runOwner);
        if (!leaderAI)
            return false;

        DcRunState& run = DcRun::Of(leaderAI);
        if (!run.enabled || !Enabled(bot))
            return false;

        uint32 const now = getMSTime();

        // A pause is an intentional hold, not a stall: keep the clock fresh so a
        // long Wait-at-Boss / door pause never makes the run look frozen on resume.
        if (run.paused)
        {
            DcRunProgress::Stamp(run.progress, now);
            return false;
        }

        // Raid boss stand-down: a live encounter is the playerbots strategy's
        // fight — a rescue teleport mid-encounter would yank fighters out of
        // position (and engagement alone can't be trusted through submerge /
        // phase-flip signal gaps). Same clock treatment as a pause, so the
        // failsafe re-arms clean when the fight ends.
        if (DcBossStandDown::IsActive(bot))
        {
            DcRunProgress::Stamp(run.progress, now);
            return false;
        }

        // Karazhan's chess game: ten minutes with no kill, no anchor and the raid
        // parked on a sideline far from the tank is the game, not a strand — and a
        // rescue teleport would pull controllers off their pieces. Stamped, so the
        // failsafe re-arms clean when the game ends.
        if (DcKarazhan::ChessIsOn(bot))
        {
            DcRunProgress::Stamp(run.progress, now);
            return false;
        }

        std::vector<DcStrandedDecision::Member> members;
        bool partyEngaged = false;
        BuildSnapshot(driver.anchor, members, partyEngaged);

        // A FIGHT re-arms the clock wholesale — a fight is progress, so neither a
        // long boss fight nor a between-pulls skirmish ever burns the budget. A
        // bare combat FLAG with nothing fighting is not progress and must not
        // re-arm it; see BuildSnapshot. Otherwise fall to the closing-distance /
        // encounter detector.
        bool const progressed =
            partyEngaged || DcRunProgress::Detect(driver.runOwner, leaderAI, run.progress);
        if (progressed || run.progress.stampMs == 0)
            DcRunProgress::Stamp(run.progress, now);

        DcStrandedDecision::Inputs in;
        in.enabled = true;
        in.nowMs = now;
        in.lastProgressMs = run.progress.stampMs;
        in.noProgressTimeoutMs = DcSettings::GetUInt(bot, "StrandedRecoveryNoProgressSecs") * 1000;
        in.partyEngaged = partyEngaged;
        in.maxSpread = RescueSpread(bot);

        return DcStrandedDecision::Decide(in, members).recover;
    }

    void Recover(Player* bot)
    {
        if (!bot || bot->isDead())
            return;

        Driver const driver = ResolveDriver(bot);
        if (!driver.runOwner || !driver.anchor || driver.clockOwner != bot)
            return;

        Player* const leader = driver.anchor;
        PlayerbotAI* leaderAI = GET_PLAYERBOT_AI(driver.runOwner);
        if (!leaderAI)
            return;
        Group* group = leader->GetGroup();
        if (!group)
            return;

        float const maxSpread = RescueSpread(bot);
        float const lx = leader->GetPositionX();
        float const ly = leader->GetPositionY();
        float const lz = leader->GetPositionZ();

        uint32 moved = 0;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            // Never the anchor itself. When the tank is alive that is the old
            // `member == leader` skip; when it is a corpse the anchor is already
            // where everyone is being sent, and the DRIVER is not excluded — it is
            // routinely one of the strays.
            if (!member || member == leader)
                continue;
            if (!member->IsInWorld() || !member->IsAlive())
                continue;                       // dead members are the rez recovery's job
            if (member->GetMapId() != leader->GetMapId())
                continue;
            if (!GET_PLAYERBOT_AI(member))       // bots only, never a human
                continue;
            // Never a rider. NearTeleportTo ejects a passenger and leaves the
            // vehicle where it stood — on Trial of the Champion that is a horse at
            // the arena wall and a lance-armed bot on foot in the middle of a
            // joust, which is a strand this recovery would be CAUSING.
            if (member->GetVehicle())
                continue;
            float const strandedDist = leader->GetDistance(member);
            if (strandedDist <= maxSpread)
                continue;                       // in range — not stranded

            // Fan the strays out a little around the tank so they don't stack on
            // one point, and drop any stale follow spline that would otherwise drag
            // them straight back toward wherever they were stuck.
            float const angle = leader->GetOrientation() + static_cast<float>(moved) * 0.7f;
            float const off = 1.5f + 0.5f * static_cast<float>(moved);
            float const tx = lx + std::cos(angle) * off;
            float const ty = ly + std::sin(angle) * off;

            member->GetMotionMaster()->Clear();
            member->NearTeleportTo(tx, ty, lz, member->GetOrientation(),
                                   /*casting*/ false, /*vehicle*/ false, /*withPet*/ true);
            ++moved;

            LOG_INFO("playerbots.dungeonclear",
                     "[DC:{}] stranded-recovery: no progress past the timeout with {} out of "
                     "range ({:.0f}yd > {:.0f}yd) -> teleported to the {}",
                     leader->GetName(), member->GetName(), strandedDist, maxSpread,
                     leader->isDead() ? "tank's corpse (driver " + bot->GetName() + ")"
                                      : "tank");
        }

        if (moved == 0)
            return;

        // Make the rescue STICK. In pull mode the followers are pinned by
        // hold-at-camp, so the moment they land next to the tank they walk right
        // back to the camp — and if that camp is what stranded them (stale behind a
        // driver that isn't advance, or on ground they can't path back onto) the
        // teleport is undone within seconds and the same freeze repeats every
        // timeout, forever. Observed live in heroic Old Hillsbrad: five rescues, all
        // reversed. Re-anchor the camp onto walked ground behind the tank, unless a
        // pull maneuver is in flight (there the camp is the drag destination and
        // must not move under the tank's feet).
        //
        // "In flight" means a HOLDING phase (Forming/Advancing/Returning) — the legs
        // where the party is pinned passive and the tank is hauling a pack home. It
        // does NOT mean Engage: by then the drag is over and the party has been
        // released, so the camp is free to move. The first cut of this guard tested
        // `phase == Idle`, which skipped the re-anchor for the whole Engage phase —
        // and a phase orphaned at Engage is exactly the state that strands a party
        // (heroic Old Hillsbrad tr-20260801-174432-3: nine rescues, every one of
        // them reversed because this block never ran). A failsafe must not stand
        // down in the state it exists for.
        AiObjectContext* ctx = leaderAI->GetAiObjectContext();
        DcPullContext& pull = ctx->GetValue<DcPullContext&>(DcKey::PullContext)->Get();
        if (!DcLeaderSignal::IsPullPhaseHolding(static_cast<uint32>(pull.phase)) &&
            pull.HasCamp())
        {
            float const setback = DcSettings::GetFloat(bot, "PullSetback");
            float const maxDrag = DcSettings::GetFloat(bot, "PullMaxDrag");
            std::optional<Position> const trail =
                DcPullPlanner::ComputeTrailCamp(leaderAI, setback, maxDrag);
            pull.camp = trail ? *trail : leader->GetPosition();
            LOG_INFO("playerbots.dungeonclear",
                     "[DC:{}] stranded-recovery: camp re-anchored to ({:.1f},{:.1f},{:.1f}) so "
                     "the rescued members aren't sent back to the old one",
                     leader->GetName(), pull.camp.GetPositionX(), pull.camp.GetPositionY(),
                     pull.camp.GetPositionZ());
        }

        // Re-arm the clock: the strays are back at the tank but nothing else has
        // changed, so without this the very next tick would read as stale again and
        // re-fire. Give the run a fresh window for follow-tank + advance to resume
        // real progress. Re-seed the closing-distance mark from the tank's current
        // spot too, so a re-measure isn't fooled by the old best.
        DcRunState& run = DcRun::Of(leaderAI);
        DcRunProgress::Stamp(run.progress, getMSTime());
        run.progress.bestDist = -1.0f;

        DcStatusPublisher::SendAddonMessage(
            leaderAI,
            "CHAT\tRescued " + std::to_string(moved) +
                (moved == 1 ? " stuck party member." : " stuck party members."));
    }
}
