/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "Log.h"

#include <cstdint>

#include "Creature.h"
#include "InstanceScript.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "SharedDefines.h"

#include "Ai/Dungeon/DungeonClear/Action/DungeonClearActions.h"
#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"
#include "Ai/Dungeon/DungeonClear/DcRunState.h"
#include "Ai/Dungeon/DungeonClear/Trigger/DungeonClearTriggers.h"
#include "Ai/Dungeon/DungeonClear/Util/DcRun.h"
#include "Ai/Dungeon/DungeonClear/Util/DcHorEscapeDecision.h"
#include "Ai/Dungeon/DungeonClear/Util/DcLeaderSignal.h"
#include "Ai/Dungeon/DungeonClear/Util/DcSuppressionTransit.h"
#include "Ai/Dungeon/DungeonClear/Util/DcTargeting.h"
#include "Ai/Dungeon/DungeonClear/Util/DcThrottle.h"

// Halls of Reflection's escape — the PER-BOT half of "stay ahead of him".
//
// The driver (hook 35, Overrides/HallsOfReflectionDriver.cpp) steers the TANK.
// DungeonClearEventDueTrigger is leader-only, so for the whole escape the other
// four bots are in the stock combat engine killing the wall's summons, with
// nothing in the module telling them where to stand. This is that.
//
// AND IT IS NOT A LUXURY, because the encounter is built to pull them backwards.
// Every summon batch is cast AT THE LICH KING — behind the party — and each add
// is given 1000 threat on, and told to AttackStart, the player NEAREST HIM, i.e.
// the rearmost bot. The Risen Witch Doctors then stop at 20yd to cast, which
// leaves them standing on HIS side of the party, so a melee finishing one walks
// toward him by design. Meanwhile stock `flee` / `runaway` / `avoid aoe` and
// FleeAction's MoveAway(target, 5) all answer damage by moving AWAY FROM THE
// THING DEALING IT, which here is exactly backwards.
//
// TWO THINGS THE ENCOUNTER DOES TO A BOT THAT ENDS UP BEHIND OR ON TOP OF HIM:
//
//   * Remorseless Winter (69780 -> 69781) ticks 7068 +/- 863 frost every second
//     to everything within 10 yards. That is the largest sustained number in the
//     dungeon by a wide margin.
//   * Every 2 seconds, each alive player with (p.x - lk.x) + (p.y - lk.y) > 20
//     eats 70653 — 10 000 damage AND A KNOCKBACK, which throws the victim
//     further behind and makes the next check worse. The rule is
//     self-reinforcing, so it has to be pre-empted rather than reacted to; this
//     rung fires at a sixth of that scalar.
//
// WHY THE GENERIC HAZARD VACATE CANNOT SERVE. It retreats RADIALLY — a point
// directly away from the emitter, past its pulse radius — and for a bot that is
// already behind him "away" is further behind, into the zap rule. There is one
// correct direction on this leg and it is FORWARD, so the ring is registered as
// a placement keep-out with no vacateRadius (see DcHazardRegistry) and the
// active move is this, one relevance rung above it.

// TWO REASONS TO MOVE, NOT ONE — and the second is what tp-20260908-000109-1
// was missing. The rung began life as a pressure-relief valve: it armed inside
// LK_PRESSURE_DIST, pushed the bot a few yards, and disarmed. That gets a bot
// off him; it does not get a bot ANYWHERE. When a wall opens the leader runs
// 100-186yd to her next stop and the escape driver walks the tank after her,
// and a follower with only the valve stayed on the adds at the OLD wall,
// oscillating on the edge of the ring as MoveChase pulled it back onto a witch
// doctor standing on his side of the party. Redas, tr-20260908-000117-1: 16.0
// -> 13.2 -> 11.1 -> 15.1 -> 5.9 -> 14.6 -> 9.1yd from him over twenty seconds,
// net displacement nil, while the bear crossed 136yd to stand 2 by itself. The
// three dps died to an untanked Lumbering Abomination, then the healer, then the
// bear met wall 4's nineteen summons alone. Ten runs, ten wipes, all at 5/6.
//
// So the second reason is ADVANCE: the party has moved on and this bot has not.
// It reads the same stand point the driver steers the tank to, and it is a LATCH
// (arm at STAND_LEAVE_LEASH, clear at STAND_LEASH) so a bot that sets off keeps
// the tick, and therefore keeps its spline, for the whole leg. The decision is
// DcHorEscape::DecideFollow; everything here is the glue that feeds it.

namespace
{
    using namespace DcHallsOfReflection;

    // Everything both halves of this rung need, resolved once.
    //
    // Deliberately NOT split into a trigger probe and an action probe: the two
    // must agree about which stand point the bot is walking to, and re-deriving
    // it from a live Lich King that has moved between the trigger and the action
    // is how a bot ends up walking at last tick's answer.
    struct StayAheadView
    {
        bool  armed = false;      // the escape is running and he is alive
        bool  travel = false;     // ...and this bot has somewhere to be
        bool  pressured = false;  // ...because he is on top of it / it is behind him
        bool  advancing = false;  // ...because the party has moved on (latched)
        float distToLk = 0.0f;
        float sumMinusLkSum = 0.0f;
        float distToStand = 0.0f;
        uint8 targetStop = 1;
        HorPoint stand{};
    };

    // Resolves the whole rung and STORES THE LATCH, so it must be called exactly
    // once per evaluation and by both halves — the trigger's answer and the
    // action's have to agree about which stand point the bot is walking to, and
    // re-deriving it from a live Lich King that has moved between the two is how
    // a bot ends up walking at last tick's answer.
    StayAheadView Probe(Player* bot, PlayerbotAI* botAI)
    {
        StayAheadView v;
        if (!bot || !botAI || bot->isDead() || bot->GetMapId() != MAP_ID)
            return v;

        InstanceScript* inst = DcTargeting::GetInstanceScript(bot);
        if (!inst || inst->GetBossState(DATA_LICH_KING) != IN_PROGRESS)
            return v;

        Creature* lk = inst->instance->GetCreature(inst->GetGuidData(NPC_LICH_KING));
        if (!lk || !lk->IsAlive())
            return v;

        // WHERE FORWARD IS. Taken from the LEADER, exactly as the driver takes it,
        // through the same pure StopIndexNear — the two must reach the same stand
        // point or the tank and its followers would hold different ground fifty
        // yards apart with a Lich King between them.
        //
        // If she is gone the escape is already lost (the walls open only for her
        // channel), but there is still a right direction, and the last stand point
        // this bot can compute is better than none: fall back to the end of the
        // path, which is forward from everywhere.
        Creature* leader = inst->instance->GetCreature(inst->GetGuidData(NPC_LEADER_PART2));
        uint8 const leaderStop =
            leader && leader->IsAlive()
                ? StopIndexNear(leader->GetPositionX(), leader->GetPositionY())
                : static_cast<uint8>(STOP_COUNT - 1);

        v.armed = true;
        v.targetStop = DcHorEscape::TargetStopFor(leaderStop);
        v.stand = StandPointFor(v.targetStop);

        DcRunState& st = DcRun::Of(botAI);

        DcHorEscape::FollowInputs in;
        in.active = true;
        // NOT ON THE BOT THE DRIVER IS ALREADY STEERING. Hook 35 runs on the
        // dungeon-clear leader and walks it to this same stand point through its
        // own re-issue floor (DcThrottle::HorEscapeIssue). Arming the advance half
        // here as well would put two independent 1500ms floors on one bot, both
        // re-plotting a spline to the same point, which is precisely the
        // double-issue thrash STAND_LEAVE_LEASH was widened to end. The PRESSURE
        // half stays live on the leader — it always has, it costs one extra
        // correction at most, and the driver's own Pressure state wants the same
        // move.
        in.driverSteered = DcLeaderSignal::IsDungeonClearLeader(bot);
        // GATES THE PRESSURE HALF ONLY, because Remorseless Winter is what gates
        // both of the encounter's own rules: for the ~16s before he casts it, and
        // for ever after the fourth wall removes it, standing near him is merely
        // pointless. The ADVANCE half stays live either way — the last 131yd to
        // WP18 is run with Winter already gone and the party still has to make it
        // together.
        in.lkHasWinter = lk->HasAura(SPELL_REMORSELESS_WINTER);
        in.distToLk = bot->GetExactDist(lk);
        in.sumMinusLkSum = (bot->GetPositionX() + bot->GetPositionY()) -
                           (lk->GetPositionX() + lk->GetPositionY());
        in.pressureDist = LK_PRESSURE_DIST;
        in.behindSum = LK_BEHIND_SUM;
        in.distToStand = bot->GetExactDist(v.stand.x, v.stand.y, v.stand.z);
        in.standLeash = STAND_LEASH;
        in.standLeaveLeash = STAND_LEAVE_LEASH;
        in.advancing = st.horFollowAdvancing;

        DcHorEscape::FollowVerdict const fv = DcHorEscape::DecideFollow(in);
        st.horFollowAdvancing = fv.advancing;

        v.travel = fv.travel;
        v.pressured = fv.pressured;
        v.advancing = fv.advancing;
        v.distToLk = in.distToLk;
        v.sumMinusLkSum = in.sumMinusLkSum;
        v.distToStand = in.distToStand;
        return v;
    }
}

bool DungeonClearHorStayAheadTrigger::IsActive()
{
    // Map first: this is registered in both engines on every bot, and everywhere
    // outside Halls of Reflection it must cost one integer compare.
    if (!bot || bot->isDead() || bot->GetMapId() != DcHallsOfReflection::MAP_ID)
        return false;

    // Cannot step forward while rooted or stunned — and the Lumbering
    // Abomination's Vomit Spray and the Raging Ghouls' leap both come with
    // control. Do not claim the tick from something that might break it.
    if (bot->HasUnitState(UNIT_STATE_STUNNED | UNIT_STATE_FLEEING |
                          UNIT_STATE_CONFUSED | UNIT_STATE_ROOT))
        return false;

    StayAheadView const v = Probe(bot, botAI);
    return v.armed && v.travel;
}

bool DungeonClearHorStayAheadAction::Execute(Event /*event*/)
{
    if (!bot || !botAI)
        return false;

    StayAheadView const v = Probe(bot, botAI);
    if (!v.armed || !v.travel)
        return false;  // raced clear between the trigger and here

    // ALREADY THERE is decided in the kernel (DecideFollow returns travel=false
    // inside standLeash whichever reason armed it): a bot on the stand point that
    // still reads pressured has had the Lich King walk up to IT, which is the
    // ordinary state of a wall that is taking a while, and re-issuing a move to
    // where it is standing would only tear down its melee approach.

    // Re-issue floor on the destination. The stand point only changes when the
    // leader reaches her next stop, so without this the rung would re-plot the
    // same spline every tick of a wall fight — and TravelTo has no
    // "already moving" guard by design (in combat a bot is essentially always
    // moving under MoveChase, and such a guard would make the whole thing a
    // no-op).
    //
    // AND IT CLAIMS THE TICK WHILE IT SUPPRESSES, exactly as the driver's own
    // floor does (HorDriveEscape's HorEscapeIssue branch returns Running rather
    // than yielding). Returning false here was the bug that lost
    // tp-20260907-232556-1: the floor is 1500ms and a bot thinks two to four
    // times inside it, so on every suppressed tick the engine fell through to
    // the combat rungs, which issue MoveChase — and MoveChase cancels the
    // forward spline this rung had just laid down. The bot then holds station on
    // a witch doctor standing on the LICH KING'S side of the party while he
    // walks into it at 1.45yd/s.
    //
    // The trace is unambiguous. Isun, tr-20260907-232601-10, the rung firing
    // every 1.5-2s throughout: 16.0 -> 15.8 -> 9.7 -> 7.5 -> 3.4yd from him over
    // nine seconds — his own walking speed into a bot that never moved — then
    // +4.4 and +28.9 on the behind-scalar as the Zap knocked it past him. Across
    // the ten runs, 45% of consecutive samples ended CLOSER to him than the one
    // before, and 27 of the 37 bots that ever fired this rung finished it above
    // the +20 zap line.
    //
    // Claiming a tick costs nothing here by construction: the rung arms only when
    // the bot has somewhere else to be, and both reasons are worth more per
    // second (7068 frost, or 10 000 and a knockback, or the whole party splitting
    // across a 136yd leg) than any swing the yield would have bought.
    DcRunState& st = DcRun::Of(botAI);
    if (st.ThrottledIssue(DcThrottle::HorStayAheadIssue, v.stand.x, v.stand.y, v.stand.z,
                          /*epsilon*/ 2.0f, /*windowMs*/ 1500))
        return true;

    // FORWARD, THROUGH THE LONG-RANGE FUNNEL. `forcePath` is unconditional: the
    // legs between stand points run 100-176yd, a bare MovePoint truncates
    // silently past ~30, and TravelTo's own 30yd gate reads the STRAIGHT-LINE
    // distance — which on the stretch from stop 1 to stop 2 understates a
    // corridor that bends twice.
    if (!DcTransit::TravelTo(bot, botAI, v.stand.x, v.stand.y, v.stand.z,
                             DcHallsOfReflection::STAND_LEASH, /*forcePath*/ true))
    {
        // A FAILED ISSUE STILL CLAIMS THE TICK WHILE ADVANCING, for the reason
        // the driver gives at the same branch: the leg is 100-186yd, the path
        // comes back off DcPathWorker a tick or two later, and every tick handed
        // to the combat engine in between is one MoveChase spends walking the bot
        // back to the add it is meant to be leaving. Under PRESSURE alone the
        // trade goes the other way — that correction is a few yards, the bot is
        // in a fight it can contribute to, and a healer that claimed every tick
        // of a stuck path would stop healing.
        return v.advancing;
    }

    LOG_DEBUG("playerbots.dungeonclear",
              "[DC:{}] HoR escape — {} to stand {}, {:.1f}yd away ({:.1f}yd from the Lich "
              "King, {:+.1f} on the behind-scalar; his ring is {:.0f}yd and the zap lands "
              "at +20)",
              bot->GetName(),
              v.pressured ? (v.advancing ? "stepping forward and moving up"
                                         : "stepping forward")
                          : "moving up with the party",
              v.targetStop, v.distToStand, v.distToLk, v.sumMinusLkSum, LK_PRESSURE_DIST);
    return true;
}
