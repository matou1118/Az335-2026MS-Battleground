/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DungeonClearMultiplier.h"
#include "Ai/Dungeon/DungeonClear/Util/DcRun.h"

#include "Action.h"
#include "FollowActions.h"
#include "InstanceScript.h"
#include "Player.h"
#include "Playerbots.h"
#include "Position.h"
#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"
#include "Ai/Dungeon/DungeonClear/DcPullContext.h"
#include "Ai/Dungeon/DungeonClear/Settings/DcSettings.h"
#include "Ai/Dungeon/DungeonClear/Util/DcBossStandDown.h"
#include "Ai/Dungeon/DungeonClear/Util/DcEscapeLeap.h"
#include "Ai/Dungeon/DungeonClear/Util/DcFlightLeg.h"
#include "Ai/Dungeon/DungeonClear/Util/DcOculusPlan.h"
#include "Ai/Dungeon/DungeonClear/Util/DcSmartRest.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonClearUtil.h"
#include "Ai/Dungeon/DungeonClear/DcValueKeys.h"

// BLACKWING LAIR, the orb runner, and the hardest guard in the module: while this
// bot holds Razorgore's possession, NOTHING it could do is worth doing.
//
// 19832 is a channel on the runner's own body, and the encounter hangs off it —
// break it and a boss the raid must not kill is loose, the runner is locked out of
// the orb for 60s, and the egg run loses a whole window. The orb rung already owns
// the tick and spends it on nothing, but owning the tick is only as strong as the
// rung's relevance, and the relevance band it sits in (62) does not cover
// everything: ACTION_EMERGENCY is 90 and stock `drop target` is 99, so a health
// potion, a healthstone, a flee, or a target drop outranks it and ends the channel
// — and those are exactly the actions a rooted bot with adds on it reaches for.
//
// Zeroing every OTHER action is the only shape that closes that. It costs the
// runner its self-preservation for ninety seconds, which is the same price a human
// raider pays holding the orb: they cannot act either. The raid's job is to keep
// the adds off the ledge (that is what the camp rung is for); the runner's job is
// to stand still.
//
// Free everywhere else — HoldsThePossession rejects on a map compare, and it reads
// this bot's own charm field rather than any cross-bot signal, so no stale stamp
// can drop the guard while the channel is still up.
static float RazorgorePossessionClamp(Player* bot, std::string const& name)
{
    if (!DcBlackwingLair::HoldsThePossession(bot))
        return 1.0f;
    return name == "dungeon clear razorgore orb" ? 1.0f : 0.0f;
}

// KARAZHAN, the chess game: while this bot is held by the game, NOTHING but the
// chess rung is worth doing — the Razorgore-possession shape, for the whole raid.
//
// The rung owns the tick at KzChess (64.25), but stock actions reach higher than
// that — ACTION_EMERGENCY at 90, `drop target` at 99 — and a controller has lost
// Game In Session, so its own rotation, a heal, a potion or a pet command could
// all land on a piece. That is cheating and it breaks the game (H1, H5). So every
// other action is zeroed on every bot the rung holds; the one carve-out is the
// `dc ...` chat commands, so a human can still pause or stop the run.
//
// Free everywhere else, and cheap here: the map compare, then one stamp the rung
// leaves on the bot's own run state (DcKarazhan::ChessHoldsTheBot).
static float KaraChessClamp(Player* bot, PlayerbotAI* botAI, std::string const& name)
{
    if (bot->GetMapId() != DcKarazhan::MAP || !DcKarazhan::ChessHoldsTheBot(bot, botAI))
        return 1.0f;
    if (name == "dungeon clear kz chess" || name.rfind("dc ", 0) == 0)
        return 1.0f;
    return 0.0f;
}

// HALLS OF REFLECTION, the escape: BACKWARDS IS FATAL, so nothing may move
// backwards.
//
// Stock `flee` and `runaway` both answer damage the same way — put distance
// between the bot and the thing hurting it (FleeAction::Execute is
// MoveAway(target, 5)). Everywhere else in the game that is correct. On this leg
// it is the single worst thing a bot can do: the Lich King walks the party down a
// path that runs -x and -y, so "away from the damage" is BEHIND HIM, and every
// two seconds each player whose (p.x - lk.x) + (p.y - lk.y) exceeds 20 takes
// 10 000 damage plus a knockback that throws them further behind still. A bot
// that flees once is a bot that then keeps being knocked into fleeing again.
//
// So both are zeroed for the whole escape, on every member, in both engines. The
// forward equivalent — DungeonClearHorStayAheadAction at relevance 56 — is what
// actually moves an endangered bot, and it can only own the tick if the stock
// backwards movers are not competing for it.
//
// Scoped as tightly as the harm: map compare first, then the escape's own boss
// state, so this costs one integer compare everywhere else and is inert on map
// 668 itself until the point-of-no-return gossip.
//
// `drop target` is deliberately NOT touched here. It is what lets a bot release
// a Lich King it should never have acquired, and the hold-fire rung depends on
// the release path staying intact.
static bool HorEscapeBackwardsBanned(Player* bot, std::string const& name)
{
    if (name != "flee" && name != "runaway")
        return false;
    if (!bot || bot->GetMapId() != DcHallsOfReflection::MAP_ID)
        return false;

    InstanceScript* inst = bot->GetInstanceScript();
    return inst &&
           inst->GetBossState(DcHallsOfReflection::DATA_LICH_KING) == IN_PROGRESS;
}

// THE OCULUS: no ordinary mount for a bot that has a drake to board or sits on one.
// Stock `check mount state` (timer 1 out of combat, 54 in, and the run-speed packet
// handler) mounts every bot between fights on a map that allows it — and the
// essence's Call spell is refused from a mount, so a mustering party never got a
// single drake. The rider rung steps the bot off (LeaveMount); this keeps stock from
// putting it straight back on for the muster, the settle and the whole flight.
// Both multipliers ask it: the action is registered in both engines.
//
// AND NO STOCK DRAKE STEERING WHILE A DC RUN FLIES THE DRAKES. Stock `wotlk-occ`'s
// master-gated drake actions fire as soon as the bot's playerbots master rides a
// drake — which in a run with a party member as master is every leg:
//   * `occ fly drake` MoveFollows each drake onto the master's at 15yd, dragging it
//     off its lane and off its pad every tick. Live, first flight: 46-185 successful
//     executions per bot in five minutes, the tank's drake pinned 15.7yd from the
//     pad, the landing check flickering, nobody ever dismounting.
//   * `dismount drake` is a bare ExitVehicle the moment the master is on foot —
//     in mid-air for a rider still on its leg, with no parachute in an instance.
// `mount drake` is deliberately NOT here: while a mounted master's
// MountingDrakeMultiplier zeroes everything else on a bot left on foot, it is the
// only action that bot has. The rider rung avoids that state instead (the master
// mounts last and steps off first).
static bool OculusMountBanned(Player* bot, std::string const& name)
{
    bool const mountState = name == "check mount state";
    bool const stockDrake = name == "occ fly drake" || name == "dismount drake";
    if ((!mountState && !stockDrake) || bot->GetMapId() != DcOculus::MAP_ID)
        return false;
    if (mountState && bot->GetVehicle())
        return true;
    DcOculusPlanView const plan = OculusPlanFor(bot);
    if (!plan.valid)
        return false;
    if (stockDrake)
        return true;
    return plan.phase == DcOculusDriver::Phase::Muster || plan.phase == DcOculusDriver::Phase::Fly ||
           plan.phase == DcOculusDriver::Phase::Eregos;
}

// THE RIDER-KILL GUARD, stock half. `occ drake attack` shoots its current target
// or, with none, the FIRST in-combat possible target — and a drake bar spell on a
// creature that cannot fly kills its rider (npc_oculus_drakeAI::SpellHitTarget).
// The rider rung keeps the current target flying-safe; the fallback is the hole,
// and a drake landing under ground fire is exactly when it would pick a Ring-Lord.
// So the action is zeroed whenever the target stock would choose, chosen the same
// way, is not flying-safe.
static bool OculusDrakeAttackUnsafe(Player* bot, PlayerbotAI* botAI, AiObjectContext* context,
                                    std::string const& name)
{
    if (name != "occ drake attack" || bot->GetMapId() != DcOculus::MAP_ID || !DcFlightLeg::OculusDrakeOf(bot))
        return false;
    Unit* target = context->GetValue<Unit*>(DcKey::Stock::CurrentTarget)->Get();
    if (!target)
    {
        for (ObjectGuid const& guid : context->GetValue<GuidVector>(DcKey::Stock::PossibleTargets)->Get())
        {
            Unit* const unit = botAI->GetUnit(guid);
            if (unit && unit->IsInCombat())
            {
                target = unit;
                break;
            }
        }
    }
    return target && !DcFlightLeg::IsFlyingSafe(target);
}

float DungeonClearMultiplier::GetValue(Action* action)
{
    if (!action || !botAI || !bot)
        return 1.0f;

    std::string const& name = action->getName();

    if (float const clamp = RazorgorePossessionClamp(bot, name); clamp != 1.0f)
        return clamp;

    if (float const clamp = KaraChessClamp(bot, botAI, name); clamp != 1.0f)
        return clamp;

    // Halls of Reflection's escape: no backwards movement, ever. See
    // HorEscapeBackwardsBanned above.
    if (HorEscapeBackwardsBanned(bot, name))
        return 0.0f;

    if (OculusMountBanned(bot, name))
        return 0.0f;
    if (OculusDrakeAttackUnsafe(bot, botAI, context, name))
        return 0.0f;

    // Rest-target cap. Applies to EVERY bot in an active DC run — the leader tank
    // AND its followers — so the whole group stops eating/drinking at the group's
    // chosen target (DungeonClear.RestHealthPct / RestManaPct). Followers never
    // set `enabled`, so we gate on the cross-bot "dungeon clear party tank" value
    // (non-null only while the leader's clear runs and is unpaused) rather than
    // the per-bot enabled flag used below. The matching
    // DungeonClearNeeds{Eat,Drink} triggers raise the floor (drink/eat up to the
    // target even above the stock playerbots stop); this caps the ceiling so a
    // target BELOW the stock stop is honoured too. 0 = inherit, no cap.
    if (name == "food" || name == "drink")
    {
        if (Player* tank = AI_VALUE(Player*, DcKey::PartyTank))
        {
            // Smart Rest hysteresis: between rests eating/drinking is FULLY
            // suppressed (the stock medium-health/high-mana triggers included)
            // so the party keeps pushing; during a latched rest it runs
            // uncapped so everyone tops to 100. Humans have no multiplier and
            // can always eat/drink; while paused PartyTank is null, so stock
            // rest resumes for the duration of the pause.
            if (DcSettings::GetBool(bot, "SmartRest"))
                return DcSmartRest::IsLatched(tank) ? 1.0f : 0.0f;

            bool const isDrink = (name == "drink");
            uint32 const target =
                DcSettings::GetUInt(bot, isDrink ? "RestManaPct" : "RestHealthPct");
            if (target > 0)
            {
                float const pct =
                    isDrink ? bot->GetPowerPct(POWER_MANA) : bot->GetHealthPct();
                if (pct >= static_cast<float>(target))
                    return 0.0f;
            }
        }
    }

    // The stock anti-stack shuffle ("move out of collision", NonCombatStrategy
    // relevance 2) fights DC's positioning and can livelock a follower: the
    // follow-tank/camp-hold pin is a FIXED point, so when another unit stands
    // inside ContactDistance the shuffle hops the bot ~followDistance away in a
    // random direction, the persistent MoveFollow generator walks it straight
    // back onto the pin, and the two alternate forever — a rapid two-steps-out/
    // two-steps-back dance. The bot never stands still long enough to sit, so
    // it can't drink/eat, and the leader's between-pulls rest gate then stalls
    // the whole run on its mana. DC's own positioning decides where everyone
    // stands; drop the shuffle for every member of an active run (same
    // cross-bot gate as the rest cap above).
    if (name == "move out of collision" &&
        AI_VALUE(Player*, DcKey::PartyTank))
        return 0.0f;

    // Blink / Disengage leap a fixed distance straight away from the bot's target
    // and land it in whatever pack is behind — see DcEscapeLeap. Every member of an
    // active run, same cross-bot gate as above.
    if (DcEscapeLeap::IsBanned(name) && AI_VALUE(Player*, DcKey::PartyTank))
        return 0.0f;

    // Wander-style autonomous navigation (grind / rpg / travel). This is what
    // drives the bot around the world on its own.
    bool const isWander =
        name.find("grind") != std::string::npos ||
        name.find("rpg") != std::string::npos ||
        name == "travel" || name.find("travel ") != std::string::npos;

    // Proactive engagement: the bot's OWN target-picking that walks it to a mob
    // and pulls (the grind strategy fires "attack anything"; the pull strategy
    // fires "pull start"/"pull action"/"reach pull"). These run in the non-combat
    // engine BEFORE combat starts, so they bypass DC's triggers entirely — which
    // is exactly how a paused/parked tank still strolls THROUGH a navmesh-passable
    // door to a pack on the far side (the pack only aggros once the tank arrives,
    // so it never shows as combat). DC drives all engagement through its own
    // engage-trash/engage-boss actions, so the stock pickers must stay suppressed
    // whenever DC owns the bot — active OR paused. Reactive defense (a mob that
    // actually aggros the bot) is combat-engine and untouched here.
    bool const isProactiveEngage =
        name == "attack anything" ||
        name == "move random" ||
        name == "pull action" || name == "pull start" || name == "reach pull";

    // FOLLOWER in advanced-pull camp-hold. Followers never set `enabled`, so this
    // sits above the enabled gate below. In pull mode the party holds and
    // leapfrogs camp-to-camp (DungeonClearHoldAtCampAction); when it is parked the
    // hold action YIELDS the tick so the bot can rest/loot at camp — but that
    // would also let stock follow-master / wander / self-pull drag it off toward
    // the scouting tank. Suppress exactly those for a camp-held follower; food /
    // drink / loot / reactive combat fall through untouched so the party still
    // recovers. Only resolve the leader for an action we might actually suppress,
    // so the per-tick leader lookup stays off the hot path.
    if (isWander || isProactiveEngage || dynamic_cast<FollowAction*>(action))
    {
        Position camp;
        bool passive = false;
        if (DcLeaderSignal::GetLeaderCampHold(bot, camp, passive))
            return 0.0f;
    }

    // Stock group-target pickers while the camp hold is PASSIVE (the tank is
    // tagging or dragging). "dps assist" / "tank assist" sit at relevance 50 in the
    // non-combat engine — above hold-at-camp (28), where +passive does not apply —
    // so they target the tank's attacker and flip the follower onto the combat
    // engine before the hold can pin it. On a planned pull the +passive already on
    // the combat engine caught that; on a scout aggro nothing did, and the casters
    // opened on the pack at its spawn (tr-20260923-171623-1). The off-tank's
    // "tank assist" is the same picker and ran it 27yd off camp. Only while
    // passive: between pulls and at Engage the party must still defend itself.
    if (name == "dps assist" || name == "tank assist")
    {
        Position camp;
        bool passive = false;
        if (DcLeaderSignal::GetLeaderCampHold(bot, camp, passive) && passive)
            return 0.0f;
    }

    // Stock follow-master (FollowAction, relevance ~1) points a bot at its MASTER —
    // the human party leader — NOT the dungeon-clear tank. While a DC run is active
    // DC owns 100% of positioning: follow-tank (rel 25) trails the tank out of
    // combat, and the assist/regroup rungs drive the party into the tank's fight in
    // combat. The failure this closes: when BOTH legitimately stand down for a tick
    // — a follower flagged into combat (follow-tank bails on IsInCombat) during the
    // brief threat-lead before the assist releases, with the pulled pack dragged out
    // of its line of sight so stock combat has no visible target — the ONLY surviving
    // movement action is stock follow-master, and it walks the follower to the HUMAN
    // instead of the tank (the reported "dps run to me, not the tank" bug). Worse,
    // FollowAction installs a PERSISTENT MoveFollow generator, so once it grabs a
    // follower it keeps dragging it to the master even as the assist flickers back
    // on. Suppress it for EVERY member of an active run (cross-bot PartyTank gate —
    // followers never set `enabled`, and this sits above the enabled/paused gates
    // like the camp-hold and rest-cap blocks; PartyTank resolves the leader only
    // while the clear runs unpaused, and to the tank itself for the leader — which
    // subsumes the tank-rubberband guard that used to live in the active branch).
    // DC's own follow-tank redirect is a DcMovementAction, not a FollowAction, so
    // this never touches it. When paused/off PartyTank is null and stock follow
    // resumes (a paused run hands positioning back to the player).
    if (dynamic_cast<FollowAction*>(action) && AI_VALUE(Player*, DcKey::PartyTank))
        return 0.0f;

    bool const enabled = DcRun::Of(context).enabled;
    bool const paused = DcRun::Of(context).paused;

    // DC off: fully stock behavior, nothing suppressed.
    if (!enabled)
        return 1.0f;

    // Paused is a HOLD, not a hand-back to stock AI. This used to return 1.0
    // (full stock), which let the tank grind/pull off on its own — that's how a
    // paused tank walked straight THROUGH a door the player paused at to deal
    // with. Keep autonomous wandering AND proactive engagement suppressed so a
    // paused tank simply holds and follows the party like any member (follow is
    // left at 1.0, unlike the active branch below) until the player resumes.
    if (paused)
        return (isWander || isProactiveEngage) ? 0.0f : 1.0f;

    // --- Active (enabled && !paused) ------------------------------------------
    // (Stock follow-master is already suppressed above for every active-run member,
    // tank included — the tank must never rubberband back toward its master when
    // Advance parks at the party-spread limit, and followers must never drift to the
    // human mid-fight. That guard is the cross-bot PartyTank block above the enabled
    // gate, because followers never set `enabled` and would return early here.)
    //
    // Suppress wander AND the stock proactive-engagement pickers while DC is
    // active — DC owns engagement via its own engage-trash/engage-boss actions.
    // Anything else (loot, food, drink, reactive combat, our own dungeon-clear
    // actions, and follow for non-tanks) is untouched.
    if (isWander || isProactiveEngage)
        return 0.0f;
    return 1.0f;
}

float DungeonClearCombatMultiplier::GetValue(Action* action)
{
    if (!action || !botAI || !bot)
        return 1.0f;

    std::string const& name = action->getName();

    // The possession clamp, first and unconditional — see its definition above.
    // The runner is IN COMBAT for most of its window (the adds are on it), so the
    // combat engine is where this actually has to bite.
    if (float const clamp = RazorgorePossessionClamp(bot, name); clamp != 1.0f)
        return clamp;

    // The chess clamp, for the same reason and in the same place: a controller is
    // combat-flagged by its piece's fight half the time.
    if (float const clamp = KaraChessClamp(bot, botAI, name); clamp != 1.0f)
        return clamp;

    // Halls of Reflection's escape: no backwards movement, ever. ABOVE the
    // isDcAction fast path below, because `flee` and `runaway` are stock actions
    // and that path returns 1.0 for everything that is not a DC action or
    // `drop target`. See HorEscapeBackwardsBanned above.
    if (HorEscapeBackwardsBanned(bot, name))
        return 0.0f;

    // Above the fast path for the same reason: `check mount state` and `occ drake
    // attack` are stock actions.
    if (OculusMountBanned(bot, name))
        return 0.0f;
    if (OculusDrakeAttackUnsafe(bot, botAI, context, name))
        return 0.0f;

    // RAID BOSS STAND-DOWN — the one shared check that makes every DC combat
    // trigger node inert during a raid encounter, instead of a copy in each of
    // the ~12 triggers. While DcBossStandDown reads active, a "dungeon clear *"
    // combat action is zeroed unless it is on the exemption list (the playerbots
    // raid strategy owns the fight, ACTION_RAID=60+). The list, and the reason
    // each name is on it, live with the pure classifier in DcBossStandDown.h.
    // `drop target` classifies as Defer and falls through to the suppressor
    // below — during an encounter as much as outside one, because the
    // out-of-LOS assist and that suppression are one mechanism in two halves.
    // Order of tests: the prefix compare is paid only after the cheap
    // exact-compare fast path fails, and IsActive itself is raid-gated +
    // leader-memoised, so dungeon runs pay one Map::IsRaid() at most.
    bool const isDcAction = name.compare(0, 13, "dungeon clear") == 0;
    if (!isDcAction && name != "drop target")
        return 1.0f;
    if (DcBossStandDown::IsActive(bot))
    {
        switch (DcBossStandDown::ClassifyAction(name, isDcAction))
        {
            case DcBossStandDown::ActionVerdict::Stock:
                return 1.0f;
            case DcBossStandDown::ActionVerdict::Inert:
                return 0.0f;
            case DcBossStandDown::ActionVerdict::Defer:
                break;  // `drop target` -> the suppressor below decides
        }
    }

    // Touch EXACTLY ONE other combat action. Fast-path everything else so a
    // fight's full action list pays only the compares above per tick — the
    // combat engine otherwise stays fully stock.
    if (name != "drop target")
        return 1.0f;

    // Drop-target ping-pong guard. Engine transitions are action-driven: the stock
    // "drop target" (CombatStrategy, relevance 99) fires whenever the current target
    // is "invalid", and InvalidTargetValue treats OUT-OF-LINE-OF-SIGHT as invalid
    // (AttackersValue::IsValidTarget -> IsWithinLOSInMap). It then leaves the combat
    // engine. So when the flip-early party-assist seeds the tank's mob and flips a
    // follower into the combat engine to close on it (DungeonClearAssistCampAction),
    // drop target (99) out-ranks reach spell/melee (20) every tick and bounces the
    // bot straight back to the non-combat engine before it can move — the 1-tick
    // engine ping-pong that froze the party mid-fight (observed live: 1679 flip
    // attempts, 0 engages). Suppress drop target ONLY for that transient case: an
    // active tank-fight assist, a non-healer, and a current target that is alive,
    // same-map and attackable but merely out of LOS (still being closed on). A dead /
    // despawned / truly-invalid target is NOT out-of-LOS-only, so it still drops
    // normally and the bot moves on or leaves combat cleanly.
    // SECOND CASE, same mechanism one role over: the LEADER mid-drag. An LOS-break
    // pull (ComputeSafeCamp's ranged branch) walks the camp back along the trail
    // until the tank can no longer SEE the mob it just tagged — that is the entire
    // point of it. But "can no longer see it" is precisely InvalidTargetValue's
    // out-of-LOS clause, so the maneuver's own success condition fires drop target
    // on the tank the moment it rounds the corner, and the tank leaves the combat
    // engine. `dungeon clear pull maneuver` is a COMBAT-engine trigger, so the pull
    // FSM stops being ticked at all: it freezes in Returning with its return-leg
    // watchdog — the thing whose whole job is to fall out to "fight in place" —
    // unreachable, because that watchdog is evaluated inside the action that no
    // longer runs. Nothing times out and the run deadlocks until the overall budget.
    //
    // Live (tp-20260815-162044-2, Deadmines workshop, 3 of 5 runs): the tank tagged
    // a Goblin Engineer (622), dragged 21.8yd to an LOS-break camp, and went silent
    // — no log line of any kind for the remaining 130-215s, phase stuck at Returning
    // (forMs 130368 / 163495 / 215454), party parked passive at camp, everyone at
    // 100% HP. The engineers never came either: their SmartAI does
    // SET_COMBAT_MOVE(0) in the 5-30yd band (smart_scripts 622 id 1), so breaking
    // LOS does not bring them — UNIT_STATE_NO_COMBAT_MOVEMENT means there is no
    // chase generator left to notice. Both sides of the fight stood still.
    //
    // Deliberately gated THREE ways so this cannot touch an ordinary pull:
    //   - losPull: stamped at commit only when ComputeSafeCamp took its ranged
    //     branch and walked the camp back to break LOS on purpose. Every other
    //     pull keeps the mob glued to the tank and in sight the whole way home, so
    //     it never reaches the out-of-LOS test below and is bit-for-bit unchanged.
    //   - leader-only: a follower mid-drag is the OTHER case above, already handled.
    //   - HOLDING phases only (Forming/Advancing/Returning). At Engage the maneuver
    //     is over and an out-of-LOS target drops normally, as in any other fight.
    bool losBreakDrag = false;
    if (DcLeaderSignal::IsDungeonClearLeader(bot))
    {
        DcPullContext const& pull = AI_VALUE(DcPullContext&, DcKey::PullContext);
        losBreakDrag = pull.losPull &&
                       (pull.phase == DcPullPhase::Forming ||
                        pull.phase == DcPullPhase::Advancing ||
                        pull.phase == DcPullPhase::Returning);
    }

    if (!losBreakDrag &&
        (PlayerbotAI::IsHeal(bot) || !DcLeaderSignal::IsLeaderFightAssistWanted(bot)))
        return 1.0f;

    Unit* tgt = AI_VALUE(Unit*, DcKey::Stock::CurrentTarget);
    if (tgt && tgt->IsAlive() && tgt->GetMapId() == bot->GetMapId() &&
        bot->IsValidAttackTarget(tgt) && !bot->IsWithinLOSInMap(tgt))
        return 0.0f;

    return 1.0f;
}
