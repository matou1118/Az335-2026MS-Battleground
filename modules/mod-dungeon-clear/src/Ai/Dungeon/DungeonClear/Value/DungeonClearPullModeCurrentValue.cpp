/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DungeonClearPullModeCurrentValue.h"

#include "Creature.h"
#include "Playerbots.h"
#include "Ai/Dungeon/DungeonClear/Data/FightInPlaceRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/ScriptedPullRegistry.h"
#include "Ai/Dungeon/DungeonClear/DcPullContext.h"
#include "Ai/Dungeon/DungeonClear/Util/DcBossStandDown.h"
#include "Ai/Dungeon/DungeonClear/Util/DcLeaderSignal.h"
#include "Ai/Dungeon/DungeonClear/Util/DcNoStopZone.h"
#include "Ai/Dungeon/DungeonClear/Util/DcTargeting.h"
#include "Ai/Dungeon/DungeonClear/Util/DcTickMemo.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonClearUtil.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonEventExecutor.h"
#include "Ai/Dungeon/DungeonClear/DcValueKeys.h"

bool DungeonClearPullModeCurrentValue::Calculate()
{
    DcPullContext& pull = context->GetValue<DcPullContext&>(DcKey::PullContext)->Get();

    // While a PERSISTENT anchored event drives (ZulFarrak's temple), the event
    // owns the tank: force the EFFECTIVE pull mode Off so the whole dynamic/
    // advanced pull system stands down as one — no camp-drag kiting the tank off
    // the waves, no scout-lag stranding the party up-ramp (scout-lag reads the
    // pull setting directly; see DcLeaderSignal::IsLeaderDynamicScouting, gated the
    // same way). The tank engages directly and tanks in place; the party follows
    // close and the leader-fight assist brings it in. This is the single switch
    // that replaces per-mechanic suppressions — the event needs exactly "pull Off"
    // behaviour. The stored pull-setting preference is untouched (the addon status
    // still shows it, and it resumes the instant the event completes).
    //
    // CLEAR THE STANDING VERDICT, don't just stop reporting it. This return skips
    // DcPullPlanner::UpdateDynamicPullMode below — and that function is the only
    // writer of the Dynamic verdict, so a bare `return false` freezes whatever
    // `decision` happened to be latched on the tick the event started rather than
    // standing it down. `decision == PatrolHold` is the one that bites: the pull
    // trigger keeps its rung live on that code by design (pull mode reads off, but
    // the tank must still hold at commit range while it waits a patrol out), so the
    // pull action re-planted the tank at DcRel::Pull (35) every tick, above
    // DcRel::AtObjective (30), and the event never got another tick to drive its own
    // steps. The patrol-wait timeout cannot break the tie either — ShouldWaitForPatrol
    // is only evaluated inside the governor we just skipped.
    //
    // Live: tr-20260817-100413-43/44/45, all three stalled identically in Shattered
    // Halls. The tank latched "patrol-contended" on the Shattered Hand Champion pack
    // (17671) at the assassin hallway's mouth — contended by the very stealthed
    // Assassins (17695) the sweep event exists to kill — one second before the sweep
    // event's step 0 completed and armed this stand-down. `decision` then read
    // PatrolHold for 913 seconds, the sweep never advanced past step 0, and the run
    // failed the 600s no-progress watchdog with the party standing in the hallway.
    //
    // ANCHORED IS ONLY HALF OF IT. The predicate below also covers a CONDITIONAL
    // event whose row carries `ownsThePull` — BWL's Suppression Rooms crossing.
    // The anchored-only read could never see that one: it resolves the event from
    // DcKey::NextDungeonBoss and requires an Objective anchor, while a conditional
    // event drives BETWEEN anchors with the next BOSS still sitting in that value
    // (live: the whole crossing ran with NextDungeonBoss = Broodlord, kind boss).
    // So the stand-down never armed, the advanced pull's Idle branch answered every
    // whelp aggro with a fresh camp walked back along the route, and the tank ran
    // the gauntlet backwards 16-102yd at a time — nine legs in four minutes on
    // tr-20260828-142623-4. See DungeonEvent::ownsThePull.
    //
    // A NO_STOP ROUTE LEG STANDS THE PULL DOWN THE SAME WAY, and it wants exactly
    // this behaviour rather than a variant of it — no camp, no drag, no LOS-break
    // setback, no scout-lag stranding, the stored setting untouched and handed back
    // on the way out. So it shares the branch and the latch below rather than
    // growing a third one.
    //
    // What it buys: some stretches of an authored route are ground to CROSS, not
    // ground to camp, for reasons the pull planner cannot see. BWL's
    // Vaelastrasz->staging hall runs 24.3yd under the upper suppression room and
    // is inside that floor's 3D aggro radius for 24% of its length. Left to plan,
    // the pull system answers the legitimate pack at the FAR end of that hall with
    // `safe-camp: ranged attacker -> requiring LOS break, maxDrag extended to
    // 60yd`, drags the camp BACKWARD into the middle of the overhead band, and
    // parks the raid there to fight, loot and rest. On tp-20260828-175353-1 that is
    // where all five raids ended their run. See DcNoStopZone.
    //
    // A LIVE RAID ENCOUNTER STANDS THE PULL DOWN THE SAME WAY, and this is the
    // branch it belongs in rather than a gate of its own — "the raid is fighting
    // its boss" wants precisely what the two above want: no camp, no drag, no
    // scout-lag, the stored setting untouched and handed back on the way out.
    //
    // DcBossStandDown is the run's non-interference contract: while an encounter
    // is live the fight belongs to mod-playerbots' raid strategies and DC goes
    // inert. It was only ever wired into DungeonClearCombatMultiplier, which
    // covers the COMBAT engine — and the pull pipeline is a NON-combat rung whose
    // Idle branch gates on the tank's OWN combat flag. A tank that has dropped
    // combat mid-encounter (the boss is on somebody else, which on a 40-man is
    // most of the fight) therefore walked straight through the contract and
    // started a fresh trash pull on top of the raid's boss.
    //
    // What that cost, live on Firemaw (tr-20260829-204120-2, 21:01:40, 39s into
    // the encounter): "dynamic verdict for pack 12459: ADVANCED", a camp
    // published at (-7615.7,-1061.1,449.2) — 102yd back down the Broodlord
    // corridor and around its bend from the boss — and then 32 bots on one tick
    // logging "advanced-pull: held passive at camp". `+passive` is a STOCK
    // strategy flip (DcFollowerLifecycle::ApplyFollowerPassive), so the combat
    // multiplier's stand-down could not undo it: the ranged simply stopped
    // attacking. Three such pins in a 160s fight, plus 2023 camp-recall ticks
    // dragging the raid backwards for the rest of it. Reported as "ranged dps got
    // stuck around a corner out of line of sight and did not dps the boss".
    //
    // Lowering the LATCHED bool is what actually fixes it, which is why this
    // shares the branch instead of vetoing at the pull trigger: the camp hold,
    // the party-spread gate and ReapStrandedPassives all read that bool, so the
    // followers are released and un-passived on the next world update. A veto at
    // the trigger alone would have stopped the NEXT pull and left the raid pinned
    // and passive at the one already standing.
    if (DungeonEventExecutor::IsPullOwningEventDriving(bot, context) ||
        DcNoStopZone::IsInNoStopZone(bot, context) ||
        DcBossStandDown::IsActive(bot))
    {
        pull.ClearDynamicVerdict();
        // LOWER THE LATCHED BOOL TOO, not just the effective value. Returning
        // false here skips UpdateDynamicPullMode, which is the only writer of
        // DcKey::PullMode — so whatever the governor had latched when the event
        // armed stays latched for the event's whole duration. That bool is what
        // the follower camp-hold reads (DcLeaderSignal::GetLeaderPullInfo /
        // GetLeaderCampHold, DcPartyState), so arming mid-Advanced-pull would pin
        // the party at a stale camp while the tank fights the event alone. Latched
        // as `eventForced` so the handback below can never clobber a player choice,
        // exactly like scriptedForced one clause down.
        if (!pull.eventForced)
        {
            pull.eventForced = true;
            context->GetValue<bool>(DcKey::PullMode)->Set(false);
            DcLeaderSignal::SetLeaderDazeImmunity(bot, false);
        }
        return false;
    }
    if (pull.eventForced)
    {
        pull.eventForced = false;
        // Same handback as the scripted stage's: Off/On go back in lock-step with
        // the setting, Dynamic is left for the governor call at the bottom of this
        // function to re-decide from the pack in front of the tank.
        uint32 const setting = context->GetValue<uint32>(DcKey::PullSetting)->Get();
        if (setting != 2u)
        {
            bool const active = (setting == 1u);
            context->GetValue<bool>(DcKey::PullMode)->Set(active);
            DcLeaderSignal::SetLeaderDazeImmunity(bot, active);
        }
    }

    // SCRIPTED PULL STAGE (ScriptedPullRegistry) — the mirror image of the override
    // above: force the pull system ON for the plan's duration, whatever the player's
    // pull setting says. A plan is not a tactical preference. Selin's guard packs
    // cannot be fought where they stand at all, so "pull Off" there would mean the
    // walk-in engage takes the party into the room and onto the boss — the wipe the
    // whole plan exists to avoid. Same reasoning (and the same "the registry row IS
    // the decision" rule) as a BossPullbackRegistry drag.
    //
    // Raising the LATCHED bool rather than only reporting true here is deliberate:
    // the follower camp-hold (DcLeaderSignal::GetLeaderPullInfo / GetLeaderCampHold)
    // and the combat drag-back trigger all read `dungeon clear pull mode` directly,
    // so a plan that only moved the effective value would drag a pack home to a camp
    // nobody was holding. `scriptedForced` remembers that WE raised it, so the
    // handback below can never clobber a setting the player chose.
    //
    // Leader-only, like the Dynamic governor: a follower's own copy of the bool
    // drives nothing, and writing it there would just add churn.
    bool const isLeader = DcLeaderSignal::IsDungeonClearLeader(bot);
    if (isLeader && DcTickMemoAccess::ScriptedStage(bot, context) != nullptr)
    {
        if (!pull.scriptedForced)
        {
            pull.scriptedForced = true;
            context->GetValue<bool>(DcKey::PullMode)->Set(true);
            // The drag-back runs the tank home back-turned; the pull session's daze
            // immunity is what keeps a hit from behind from turning that into a
            // crawl. Armed with the bool everywhere else, so arm it here too.
            DcLeaderSignal::SetLeaderDazeImmunity(bot, true);
        }
        return true;
    }
    if (pull.scriptedForced)
    {
        pull.scriptedForced = false;
        // Hand the bool back to the player's preference. Off (0) / On (1) keep it in
        // lock-step with the setting exactly as ApplyPullSetting does; Dynamic (2) is
        // the governor's to own, so leave both the bool and the immunity to the
        // UpdateDynamicPullMode call immediately below.
        uint32 const setting = context->GetValue<uint32>(DcKey::PullSetting)->Get();
        if (setting != 2u)
        {
            bool const active = (setting == 1u);
            context->GetValue<bool>(DcKey::PullMode)->Set(active);
            DcLeaderSignal::SetLeaderDazeImmunity(bot, active);
        }
    }

    // FIGHT-IN-PLACE ROOM (FightInPlaceRegistry) — force the mode Off, same single
    // switch as the anchored-event override above and for the same reason: the room
    // needs exactly "pull Off" behaviour, and half-applying it is what has been
    // costing us.
    //
    // The registry's rule already lived in DungeonClearPullTrigger's Idle branch —
    // "this target is in a fight-in-place room, defer to the walk-in engage". But
    // that trigger is only half the pull system. The other half is the COMBAT-side
    // maneuver, whose Idle branch retreats to a fresh camp on ANY unplanned aggro
    // and never consulted the registry at all, so the two halves disagreed by one
    // second (tr-20260803-205519-1):
    //
    //   21:01:08  pull trigger: target 24689 is in a fight-in-place room -> defer
    //             to walk-in engage
    //   21:01:09  advanced-pull: unplanned aggro while scouting -> fresh camp
    //             (189.6,3.8,-2.8) drag 27.9yd, party converges
    //
    // The walk-in engage did its job and took the tank in after Selin's centre pair;
    // aggro landed; the maneuver hauled it 28yd straight back out to X=189 — thirty
    // yards below the CanAIAttack plane this registry exists to keep the fight above
    // — and then the route re-formed and walked back in. That in-out-in shuffle is
    // what the player sees, and no per-target veto can fix it, because by the time
    // the maneuver runs the question is no longer "may I pull this target" but "may
    // I retreat at all".
    //
    // Answered at the mode instead, so the pull trigger, the drag-back maneuver, the
    // follower camp-hold and the scout-lag stand down together. Two ways in, because
    // the answer has to survive the walk-in: the TARGET test covers the approach from
    // out in the corridor (it is the same read the trigger vetoes on, one rung
    // earlier), and the BOT test covers the tank once it is inside, where the pull
    // target read goes quiet and nothing may be dragged out regardless of what it is.
    //
    // Below the scripted-stage clause deliberately: a stage IS the authored exception
    // to this room's rule, and it has already returned true above.
    if (FightInPlaceRegistry::IsNoPullZone(bot->GetMapId(), bot->GetPositionX(),
                                           bot->GetPositionY()))
        return false;
    if (Unit* trash = DcTargeting::GetPullTarget(botAI))
    {
        // Judged from a creature's HOME position, never from where it is standing
        // this instant. A room mob that has already run out to meet the tank is
        // still a room mob — and it is precisely then, mid-aggro with the tank a few
        // yards short of the doorway, that both live-position reads go false at once
        // and the drag-back would slip through.
        Creature const* c = trash->ToCreature();
        Position const at = c ? c->GetHomePosition() : trash->GetPosition();
        if (FightInPlaceRegistry::IsNoPullZone(trash->GetMapId(), at.GetPositionX(),
                                               at.GetPositionY()))
            return false;
    }

    // Refresh the Dynamic (pull setting == 2) verdict for THIS tick, then report
    // the behavioural bool. UpdateDynamicPullMode is a no-op for Off/On (where
    // DcPullAction owns the bool) and internally throttles the expensive
    // classification, so running it on every read is cheap and idempotent.
    DcPullPlanner::UpdateDynamicPullMode(botAI, context);
    return AI_VALUE(bool, DcKey::PullMode);
}
