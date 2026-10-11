/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "ObjectiveHookRegistry.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <list>
#include <vector>

#include "Creature.h"
#include "Group.h"
#include "InstanceScript.h"
#include "Log.h"
#include "ObjectGuid.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "ServerFacade.h"
#include "SharedDefines.h"
#include "Timer.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include "Ai/Dungeon/DungeonClear/Data/DungeonBossInfo.h"
#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"
#include "Ai/Dungeon/DungeonClear/DcValueKeys.h"
#include "Ai/Dungeon/DungeonClear/Util/DcCombatFlag.h"
#include "Ai/Dungeon/DungeonClear/Util/DcPosGauntletDecision.h"
#include "Ai/Dungeon/DungeonClear/Util/DcRun.h"
#include "Ai/Dungeon/DungeonClear/Util/DcSuppressionTransit.h"
#include "Ai/Dungeon/DungeonClear/Util/DcTargeting.h"
#include "Ai/Dungeon/DungeonClear/Util/DcThrottle.h"

// --- Pit of Saron (map 658) — the imperative half --------------------------
//
// Two hooks, and only one of them is a controller.
//
//   29  PosDriveGauntlet     — THE CONTROLLER: the three gates and the two waves
//   30  PosEnterTyrannusLedge — gather on the ledge, then walk in and trip AT 5633
//
// THE ARITHMETIC IS NOT HERE. It is the pure kernel in
// Util/DcPosGauntletDecision.h, unit-tested without a map in t/TestPitOfSaron.cpp;
// this file is glue — grid scans, the party walk, the splines, the forged
// packets, the telemetry — exactly as HallsOfLightningDriver.cpp is glue for its
// own kernel.
//
// WHY BOTH HOOKS FORGE PACKETS. A bot never autonomously sends CMSG_AREATRIGGER,
// and FOUR of this dungeon's set pieces are started by one. In a `.dc test` run
// the harness relay already fires all four (DcTestAreaTriggers::Arm picks up any
// trigger with an areatrigger_scripts row and no areatrigger_teleport row, which
// on map 658 is 5578, 5579, 5580, 5589 and 5633), so these forges are what make
// Pit of Saron work for an ORDINARY bot party — and the belt-and-braces that
// stops a run hanging if the relay's own gate ever fails.
//
// AND THE FORGE IS LEVEL-TRIGGERED WHERE THE RELAY IS EDGE-TRIGGERED, which is
// the single most useful property in this file. DcTestAreaTriggers::Tick fires on
// the tick a volume is first occupied and stays quiet until the party has left
// and come back; the driver re-sends the packet EVERY tick it stands in a gate.
// So an early crossing — which npc_pos_tyrannus_eventsAI::SetData refuses in
// silence — costs nothing at all here: the packet simply starts being accepted
// the moment its precondition turns true.
//
// THE RETURN CONTRACT IS BLACKWING LAIR'S throughout, and getting it backwards
// wipes parties:
//
//   Running => "I am steering." Claims the tick.
//   Done    => "Nothing to steer." YIELDS the tick (the stepsOwnMovement branch
//              in DcRunEventAction) so the stock combat engine can fight.
//
// On this map the yield matters most inside the two waves: ten Ymirjar and then
// six (twelve heroic) Wrathbone have to be killed by a party whose leader is
// running a rung above the stock combat movers, and every tick the driver claims
// while it has nothing to steer is a tick the tank does not swing.

namespace
{
    using namespace DcPitOfSaron;

    // Fire the real area trigger from `bot`. Identical to Utgarde Pinnacle's forge
    // and to the Ring of Law's before it, and deliberately so: the core validates
    // that the bot is inside the trigger's own volume and then runs the script,
    // which means a forge from outside the sphere is a harmless no-op rather than
    // a cheat, and the hooks below can call it every tick without bookkeeping.
    void ForgeAreaTrigger(Player* bot, uint32 triggerId)
    {
        if (!bot || !bot->GetSession())
            return;

        WorldPacket p(CMSG_AREATRIGGER);
        p << uint32(triggerId);
        p.rpos(0);
        bot->GetSession()->HandleAreaTriggerOpcode(p);
    }

    // How many living party members on this map are within `radius` of a point,
    // and how many there are in total. One walk of the group for both.
    //
    // LIVING MEMBERS ON THIS MAP ONLY, which is the whole reason this is not a
    // plain distance loop. A corpse-running member is the rez ladder's business
    // and a member who never zoned in must never pin a gate — count either and the
    // quorum becomes unreachable rather than strict.
    struct PosPartyView
    {
        uint32 living = 0;   // leader included
        uint32 near_ = 0;    // ...of which, inside the radius
        float  furthest = 0.0f;
    };

    PosPartyView PosParty(Player* bot, float x, float y, float z, float radius)
    {
        PosPartyView v;
        if (!bot)
            return v;

        v.living = 1;
        v.near_ = bot->GetExactDist(x, y, z) <= radius ? 1 : 0;

        Group* group = bot->GetGroup();
        if (!group)
            return v;

        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (!member || member == bot || !member->IsAlive() ||
                member->GetMapId() != bot->GetMapId())
                continue;

            ++v.living;
            float const d = member->GetExactDist(x, y, z);
            if (d <= radius)
                ++v.near_;
            if (d > v.furthest)
                v.furthest = d;
        }
        return v;
    }

    // --- the wave probes ---------------------------------------------------
    //
    // TWO SHAPES, because the two waves are not equally identifiable.
    //
    // WAVE 1 is probed by ENTRY ALONE. None of 36892 / 36840 / 36893 has a static
    // spawn anywhere on map 658, so any live one within the scan IS a wave-1 mob
    // and no volume is needed — which is the right answer as well as the cheap
    // one, because wave 1's ten mobs are scattered over 70yd of ramp between two
    // home positions and any volume tight enough to be meaningful would miss one.
    //
    // WAVE 2 needs a VOLUME. 36841 Fallen Warrior has EIGHT static spawns in the
    // icicle tunnel and Gorkun pumps more of it behind the boss for the whole last
    // encounter; a bare aliveness probe would read "wave 2 is still up" from the
    // moment the tunnel is in scan range and the driver would never walk to gate 3.
    // See the WAVE2_* block in DungeonEventTables.h for the margins.
    struct WaveScan
    {
        uint32   alive = 0;
        uint32   armed = 0;   // ...of which alive AND attackable AND aggressive
        float    nearest = -1.0f;
        Creature* target = nullptr;
    };

    // Is this wave mob a real threat yet, or is it still inside its summon
    // window? BOTH HALVES ARE LOAD-BEARING AND THEY ARE NOT REDUNDANT:
    //
    //   * the two Deathbringers (36892) are held by pit_of_saron.cpp with
    //     REACT_PASSIVE **and** UNIT_FLAG_NON_ATTACKABLE, both cleared together in
    //     events 35/36;
    //   * the eight escorts (36840 / 36893) are held by `smart_scripts` with the
    //     react state ALONE — no unit flag is ever set on them, so a flag test on
    //     its own would read all eight as armed the instant they are summoned.
    //
    // A creature already in combat is armed by definition, whatever it was a
    // moment ago.
    bool PosWaveMobArmed(Creature* c)
    {
        return c->IsInCombat() || (!c->HasUnitFlag(UNIT_FLAG_NON_ATTACKABLE) &&
                                   c->GetReactState() == REACT_AGGRESSIVE);
    }

    bool InWave2Volume(Creature* c)
    {
        float const dx = c->GetPositionX() - WAVE2_X;
        float const dy = c->GetPositionY() - WAVE2_Y;
        if (dx * dx + dy * dy > WAVE2_RADIUS * WAVE2_RADIUS)
            return false;
        return std::fabs(c->GetPositionZ() - WAVE2_Z) <= WAVE2_ZBAND;
    }

    WaveScan ScanWave(Player* bot, uint32 progress)
    {
        WaveScan s;
        if (!bot)
            return s;

        static std::vector<uint32> const kWave1 = { NPC_YMIRJAR_DEATHBRINGER,
                                                    NPC_YMIRJAR_WRATHBRINGER,
                                                    NPC_YMIRJAR_FLAMEBEARER };
        static std::vector<uint32> const kWave2 = { NPC_FALLEN_WARRIOR,
                                                    NPC_WRATHBONE_COLDWRAITH };

        bool const wave2 = progress >= PROGRESS_AFTER_WARN_2;

        std::list<Creature*> found;
        bot->GetCreatureListWithEntryInGrid(found, wave2 ? kWave2 : kWave1, WAVE_SCAN);

        for (Creature* c : found)
        {
            if (!c || !c->IsAlive())
                continue;
            if (wave2 && !InWave2Volume(c))
                continue;

            ++s.alive;
            if (PosWaveMobArmed(c))
                ++s.armed;
            float const d = bot->GetExactDist(c);
            if (s.nearest < 0.0f || d < s.nearest)
            {
                s.nearest = d;
                s.target = c;
            }
        }
        return s;
    }

    // Is the orchestrator alive and standing where gate 1 needs it?
    //
    // Read through the instance's GUID store rather than a grid scan, for the
    // Utgarde Pinnacle reason: 36794 flies a 400yd arc across three floors during
    // the outro, and any scan radius honest enough to be called a room scan reads
    // "not there" for most of it. GetGuidData(DATA_TYRANNUS_EVENT_GUID) answers
    // from anywhere, for free.
    //
    // THE 3.0 IS THE CORE'S OWN NUMBER, not a tolerance of ours:
    // npc_pos_tyrannus_eventsAI::SetData(1, 1) returns without a word if
    // `me->GetExactDist(&PTSTyrannusWaitPos1) > 3.0f`. Widening it here would arm
    // the gate before the SetData would accept it, and the refusal is silent.
    bool PosOrchestratorInPlace(InstanceScript* inst)
    {
        if (!inst)
            return false;

        Creature* rp = inst->instance->GetCreature(inst->GetGuidData(DATA_TYRANNUS_EVENT_GUID));
        return rp && rp->IsAlive() &&
               rp->GetExactDist(RP_WAIT_X, RP_WAIT_Y, RP_WAIT_Z) <= RP_ARRIVE_DIST;
    }

    // Which areatrigger a phase is gated on, and the PROBED, on-mesh point the
    // driver stands on to forge it.
    //
    // NOT the DBC centre. A trigger centre is a point in a sphere, not a point on
    // the floor: 5578's is 34.5yd across and sits over the ramp the party climbs,
    // and 5580's is on the tunnel mouth's slope. t/TestPitOfSaronRouteProbe
    // asserts each stand point is on the navmesh AND — with room for the arrival
    // leash — inside its own sphere, because a forge from outside is a SILENT
    // no-op.
    struct GateSpec
    {
        uint32 trigger = 0;
        float  x = 0.0f, y = 0.0f, z = 0.0f;
    };

    // --- breaking a wave standoff ------------------------------------------
    //
    // The two halves of a pull, and neither is a substitute for the other. Both
    // are needed here for the reason spelled out at the WAVE_STANDOFF_MS budget:
    // the wave's own aggro is a single relocation-driven look taken while it is
    // still REACT_PASSIVE, so once the party and the mob are both at rest nothing
    // in the world will ever start this fight. The driver has to.
    //
    // Lifted verbatim in shape from VioletHoldDriver's VhForcePull/VhEngageTarget,
    // which solve the same "hostile standing there pointing at nothing" problem;
    // deliberately NOT fixed by loosening the shared engage path's target filters,
    // whose strictness is load-bearing in every other dungeon.

    // Make the CREATURE attack the BOT.
    bool PosForcePull(Player* bot, Creature* c)
    {
        if (!bot || !c || !c->IsAlive() || c->IsInCombat())
            return false;

        // A wave mob that despawn-reset itself would settle straight back into its
        // home idle after the pull; clear that first.
        if (c->IsInEvadeMode())
            c->ClearUnitState(UNIT_STATE_EVADE);

        c->EngageWithTarget(bot);
        if (c->AI())
            c->AI()->AttackStart(bot);
        return true;
    }

    // Make the BOT attack the CREATURE. A player does not auto-retaliate, so
    // PosForcePull alone leaves the tank standing there being hit; something has
    // to call Player::Attack, and the tank has to commit to THIS mob rather than
    // whatever stock targeting next re-picks.
    void PosEngageTarget(Player* bot, AiObjectContext* context, Creature* target)
    {
        if (!bot || !target || !target->IsAlive())
            return;

        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        if (!botAI)
            return;

        if (bot->GetVictim() != target)
        {
            bot->SetSelection(target->GetGUID());
            if (!bot->HasInArc(CAST_ANGLE_IN_FRONT, target))
                ServerFacade::instance().SetFacingTo(bot, target);
            if (context)
                context->GetValue<Unit*>(DcKey::Stock::CurrentTarget)->Set(target);
            bot->Attack(target, botAI->IsMelee(bot));
        }

        // FLIP THE BOT ONTO THE COMBAT ENGINE. Engine transitions are
        // action-driven, not derived from bot->IsInCombat(): a bot force-attacked
        // without the flip sits on the NON-combat engine, where no class rotation
        // exists to run, and melees the mob down one white hit at a time with no
        // threat abilities. Outside the victim guard on purpose — gating it on the
        // target CHANGE would latch that broken state permanently.
        if (botAI->GetState() != BOT_STATE_COMBAT)
        {
            botAI->ChangeEngine(BOT_STATE_COMBAT);
            botAI->SetNextCheckDelay(sPlayerbotAIConfig.reactDelay);
        }
    }

    // The per-tick telemetry line, throttled. Without it a failed gauntlet says
    // nothing about WHICH mechanism is still biting: "the orchestrator never
    // arrived" reads exactly like "wave 1 never died" in a log that only shows a
    // party standing in a corridor.
    void PosGauntletLog(Player* bot, DcRunState& st, DcPosGauntlet::Verdict const& v,
                        DcPosGauntlet::Inputs const& in, PosPartyView const& party)
    {
        if (st.Throttled(DcThrottle::PosGauntletLog, TELEMETRY_MS))
            return;

        LOG_DEBUG("playerbots.dungeonclear",
                  "[DC:{}] PoS gauntlet — progress {}, {} — gate {:.1f}yd, party {}/{} "
                  "(quorum {}), wave {} alive / {} armed, nearest {:.1f}yd -> {}{}",
                  bot->GetName(), in.progress, DcPosGauntlet::StateName(v.state),
                  in.distToGate, party.near_, party.living, v.quorumMet ? "met" : "SHORT",
                  in.waveAlive, in.waveArmed, in.nearestWaveDist,
                  v.walkToStage   ? "walking to the staging point"
                  : v.walkToGate  ? "walking into the gate"
                  : v.waveArming  ? "holding — the wave is not armed yet"
                  : v.engageWave  ? "closing on a wave mob"
                  : v.pullWave    ? "pulling a silent wave mob"
                  : v.forge       ? "forging"
                  : v.yieldTick   ? "yielding — fight"
                                  : "holding",
                  v.stalled ? " (STALLED)" : "");
    }

    // --- hook 29: RUN THE YMIRJAR GAUNTLET — the controller ----------------
    ObjectiveArriveResult PosDriveGauntlet(Player* bot, AiObjectContext* context,
                                           DungeonBossInfo const& /*info*/)
    {
        if (!bot || !context || bot->GetMapId() != MAP_ID)
            return ObjectiveArriveResult::Done;

        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        if (!botAI)
            return ObjectiveArriveResult::Done;

        InstanceScript* inst = DcTargeting::GetInstanceScript(bot);
        if (!inst)
            return ObjectiveArriveResult::Done;

        DcRunState& st = DcRun::Of(context);
        uint32 const now = getMSTime();
        uint32 const progress = inst->GetData(DATA_INSTANCE_PROGRESS);

        // --- the snapshot ---------------------------------------------------
        //
        // The wave scan is the only expensive probe here and it is skipped
        // entirely outside the two wave phases: at progress 2 there is no wave and
        // never has been, so a grid scan would be pure cost.
        WaveScan const wave =
            progress >= PROGRESS_AFTER_WARN_1 ? ScanWave(bot, progress) : WaveScan{};

        DcPosGauntlet::Inputs in;
        in.progress = progress;
        in.bossesDone = inst->GetData(DATA_GARFROST) == DONE && inst->GetData(DATA_ICK) == DONE;
        in.tyrannusDone = inst->GetData(DATA_TYRANNUS) == DONE;
        in.rpInPlace = progress == PROGRESS_FINISHED_KRICK_SCENE
                           ? PosOrchestratorInPlace(inst)
                           : true;
        in.waveAlive = wave.alive;
        in.waveArmed = wave.armed;
        in.nearestWaveDist = wave.nearest;
        in.waveEngageRange = WAVE_ENGAGE_RANGE;
        in.partyInCombat = DcCombatFlag::AnyPartyEngagement(bot);
        in.distToStage = bot->GetExactDist(STAGE_X, STAGE_Y, STAGE_Z);
        in.stageLeash = STAGE_LEASH;
        in.gatherQuorum = GATHER_QUORUM;
        in.nowMs = now;
        in.phase = st.posGauntletPhase;
        in.phaseSinceMs = st.posGauntletPhaseMs;
        in.gateHoldSinceMs = st.posGateHoldMs;
        in.waveHoldSinceMs = st.posWaveHoldMs;
        in.stallReported = st.posGateStallReported;
        in.waveArmedLatched = st.posWaveArmedLatched;
        in.waveGraceMs = WAVE_SPAWN_GRACE_MS;
        in.waveStandoffMs = WAVE_STANDOFF_MS;
        in.gateStallMs = GATE_STALL_MS;

        // WAVE 1 ONLY. Wave 2's mobs are summoned in one go by event 60 and ride a
        // single spline to a fixed home with no scripted passive window; a quorum
        // there would be a way to hang on a wave that is already coming. Zero is
        // the kernel's "no arming gate".
        in.waveArmedQuorum =
            progress == PROGRESS_AFTER_WARN_1 ? WAVE1_ARMED_QUORUM : 0u;
        in.waveArmMs = WAVE1_ARM_MS;

        // The gate geometry has to be filled in before Decide, and which gate it
        // is depends on the phase — which the kernel derives from the progress
        // counter alone. So resolve it the same way here rather than running
        // Decide twice: phase 2 is gate 1, phase 3 gate 2, phase 4 gate 3, and the
        // Arm/Wave sub-states simply do not use the numbers.
        GateSpec const gate =
            progress == PROGRESS_FINISHED_KRICK_SCENE
                ? GateSpec{ AREATRIGGER_WARN_1, GATE_1_X, GATE_1_Y, GATE_1_Z }
            : progress == PROGRESS_AFTER_WARN_1
                ? GateSpec{ AREATRIGGER_WARN_2, GATE_2_X, GATE_2_Y, GATE_2_Z }
                : GateSpec{ AREATRIGGER_TUNNEL, GATE_3_X, GATE_3_Y, GATE_3_Z };

        PosPartyView const party = PosParty(bot, gate.x, gate.y, gate.z, GATHER_RADIUS);
        in.living = party.living;
        in.nearGate = party.near_;
        in.distToGate = bot->GetExactDist(gate.x, gate.y, gate.z);
        in.gateLeash = GATE_LEASH;

        DcPosGauntlet::Verdict const v = DcPosGauntlet::Decide(in);

        // Store the clocks back BEFORE acting, so the index and the state it names
        // can never disagree even for one tick.
        st.posGauntletPhase = static_cast<uint8>(v.phase);
        st.posGauntletPhaseMs = v.phaseSinceMs;
        st.posGateHoldMs = v.gateHoldSinceMs;
        st.posWaveHoldMs = v.waveHoldSinceMs;
        st.posGateStallReported = v.stallReported;
        st.posWaveArmedLatched = v.waveArmedLatched;

        PosGauntletLog(bot, st, v, in, party);

        // --- announce the transitions worth one line each -------------------
        if (st.posGauntletState != static_cast<uint8>(v.state))
        {
            st.posGauntletState = static_cast<uint8>(v.state);
            LOG_INFO("playerbots.dungeonclear",
                     "[DC:{}] PoS gauntlet — {} (instance progress {}, {} of {} party members "
                     "at the gate)",
                     bot->GetName(), DcPosGauntlet::StateName(v.state), progress,
                     party.near_, party.living);
        }

        // ONE LINE WHEN THE ARMING HOLD RELEASES, and it names WHY. The two exits
        // are not equally good news: a quorum that armed is the encounter working,
        // and a budget that expired is a wedged summon cascade the party is now
        // walking into anyway. In a log that only shows a party starting to move,
        // those are indistinguishable.
        if (!in.waveArmedLatched && v.waveArmedLatched && in.waveArmedQuorum)
        {
            LOG_INFO("playerbots.dungeonclear",
                     "[DC:{}] PoS gauntlet — wave 1 arming hold released: {} of {} wave mobs "
                     "are alive, attackable and aggressive ({}). Closing on the ambush.",
                     bot->GetName(), in.waveArmed, WAVE1_ARMED_QUORUM,
                     in.waveArmed >= WAVE1_ARMED_QUORUM ? "both 4-packs are up"
                     : in.partyInCombat                 ? "the party is already in combat"
                                                        : "BUDGET EXPIRED — the summon "
                                                          "cascade never finished");
        }

        if (v.reportStall)
            LOG_WARN("playerbots.dungeonclear",
                     "[DC:{}] PoS gauntlet — areatrigger {} has been forged from inside its own "
                     "sphere for {}ms and the instance progress is still {}. The orchestrator "
                     "(36794) is dead or wedged, or a wave mob despawned instead of dying and "
                     "left killsLeft stuck. Still forging; the event timeout will stall the run.",
                     bot->GetName(), gate.trigger, GATE_STALL_MS, progress);

        if (v.complete)
        {
            // The tunnel warn has landed (or Tyrannus is down). Drop the block for
            // the ClearTransit reason: the event is Repeatable, and a party that
            // somehow re-enters the window holding a spent phase clock would open
            // its wave-summon grace on ground where the wave has already been
            // killed.
            st.ClearPosGauntlet();
            return ObjectiveArriveResult::Done;
        }

        // --- act -------------------------------------------------------------
        // The ARM walk is the one that CLAIMS THE TICK EVEN WHEN IT FAILS, and the
        // asymmetry with the two below is deliberate. This is a hold, not a
        // transit: yielding hands the leg to DcRel::Advance (15), which walks the
        // party the 88yd into gate 1's sphere and stands them in an ambush trigger
        // that will refuse them for a minute and a half. A leader that cannot be
        // splined to the staging point should stand where it is, not be handed to
        // the thing the hold exists to stop.
        if (v.walkToStage)
        {
            DcTransit::TravelTo(bot, botAI, STAGE_X, STAGE_Y, STAGE_Z, STAGE_LEASH);
            return ObjectiveArriveResult::Running;
        }

        // WALKING, both here and below, THROUGH LongRangePathfinder: a bare
        // MovePoint truncates silently past ~30yd and both of these legs are
        // longer than that (the wave-1 kill zone to gate 2 is ~80yd, and a wave
        // mob parked at its home can be 60yd off).
        //
        // TravelTo returns false when the leader is already inside the leash,
        // which the kernel has just told us it is not — so a false here means the
        // spline could not be issued at all, and the honest answer is to yield
        // rather than claim a tick we did nothing with. Halls of Lightning's
        // contract, verbatim.
        if (v.walkToGate)
        {
            return DcTransit::TravelTo(bot, botAI, gate.x, gate.y, gate.z, GATE_LEASH)
                       ? ObjectiveArriveResult::Running
                       : ObjectiveArriveResult::Done;
        }

        // THE STANDOFF BREAKER. The kernel has watched a live wave mob stand in
        // reach saying nothing for WAVE_STANDOFF_MS with the party out of combat —
        // long past the 3.5s emerge window — so the wave is armed and simply never
        // looked again. Start the fight; one pull cascades through the rest of the
        // wave on its own, because every wave-1 entry carries a
        // `call for help` (radius 10) smart_scripts row.
        if (v.pullWave && wave.target)
        {
            LOG_INFO("playerbots.dungeonclear",
                     "[DC:{}] PoS gauntlet — {} live wave mob(s), nearest {} at {:.1f}yd, and "
                     "nobody in combat after {}ms. The wave emerged passive and never took "
                     "another look; pulling it.",
                     bot->GetName(), wave.alive, wave.target->GetName(), wave.nearest,
                     WAVE_STANDOFF_MS);

            PosForcePull(bot, wave.target);
            PosEngageTarget(bot, context, wave.target);
            return ObjectiveArriveResult::Running;
        }

        if (v.engageWave && wave.target)
        {
            // Walk AT the mob rather than at a point near it: both waves contain
            // creatures parked at a scripted home position that go REACT_AGGRESSIVE
            // in place, and killsLeft only moves on a DEATH. A wave mob nobody ever
            // reaches is a gauntlet that never opens.
            return DcTransit::TravelTo(bot, botAI, wave.target->GetPositionX(),
                                       wave.target->GetPositionY(),
                                       wave.target->GetPositionZ(), WAVE_ENGAGE_RANGE)
                       ? ObjectiveArriveResult::Running
                       : ObjectiveArriveResult::Done;
        }

        if (v.forge)
        {
            ForgeAreaTrigger(bot, gate.trigger);

            // Log the fire that TOOK, not every fire: while a precondition is
            // unmet this runs every tick, and one line per accepted gate is the
            // whole signal.
            if (inst->GetData(DATA_INSTANCE_PROGRESS) != progress)
                LOG_INFO("playerbots.dungeonclear",
                         "[DC:{}] PoS gauntlet — areatrigger {} accepted, instance progress "
                         "{} -> {} ({} of {} party members on the gate, furthest {:.1f}yd)",
                         bot->GetName(), gate.trigger, progress,
                         inst->GetData(DATA_INSTANCE_PROGRESS), party.near_, party.living,
                         party.furthest);
            return ObjectiveArriveResult::Running;
        }

        // HOLDING or YIELDING. The kernel has decided which; a yield is exactly a
        // tick on which the party should be fighting rather than walking, and a
        // hold is a tick on which nothing should move the party at all.
        //
        // Deliberately no StopBot on the hold: killing the spline every tick of a
        // wave fight is a stop packet every tick, and the leader is being held by
        // combat anyway.
        return v.yieldTick ? ObjectiveArriveResult::Done : ObjectiveArriveResult::Running;
    }

    // The ledge hook's per-state line, throttled on the same budget as the
    // gauntlet's. Prints the two tests that decide which of the hook's three
    // states it is in — the gather quorum and the distance left to the walk-in
    // target — so "held short of the sphere" can never again be indistinguishable
    // from "walked at it and the spline did not land".
    void PosLedgeLog(Player* bot, AiObjectContext* context, char const* state,
                     PosPartyView const& party, float toArena)
    {
        DcRunState& st = DcRun::Of(context);
        if (st.Throttled(DcThrottle::PosLedgeLog, TELEMETRY_MS))
            return;

        LOG_DEBUG("playerbots.dungeonclear",
                  "[DC:{}] PoS ledge — {}: party {}/{} within {:.0f}yd of the ledge "
                  "(quorum {:.0f}%, furthest {:.1f}yd), {:.1f}yd to the walk-in point "
                  "(leash {:.0f})",
                  bot->GetName(), state, party.near_, party.living, GATHER_RADIUS,
                  GATHER_QUORUM * 100.0f, party.furthest, toArena, ARENA_LEASH);
    }

    // --- hook 30: TYRANNUS'S LEDGE -----------------------------------------
    //
    // Gather outside the sphere, then walk in and trip it. Three states, no
    // kernel: the decisions are a quorum test and two distance tests, and lifting
    // those out of the world would cost more in indirection than it returns.
    //
    // THE ORDER MATTERS AND THE GEOMETRY ENFORCES IT. The objective anchor is 73yd
    // from areatrigger 5633's centre — 22yd outside its 51.5yd sphere — so
    // ARRIVING cannot fire it. That is the whole reason the anchor is not simply
    // put on the trigger: a party that trips this while strung out down the tunnel
    // starts a 38-second scripted intro during which Tyrannus's VICTIM is
    // leash-checked at 100yd from TSDistCheckPos every tick, and the victim is
    // whoever happened to be nearest when Gorkun's constructor ran.
    ObjectiveArriveResult PosEnterTyrannusLedge(Player* bot, AiObjectContext* context,
                                                DungeonBossInfo const& /*info*/)
    {
        if (!bot || !context || bot->GetMapId() != MAP_ID)
            return ObjectiveArriveResult::Done;

        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        if (!botAI)
            return ObjectiveArriveResult::Done;

        InstanceScript* inst = DcTargeting::GetInstanceScript(bot);
        if (!inst)
            return ObjectiveArriveResult::Running;  // not in the instance yet

        // He is already down — a re-entered instance, or a retry after the boss
        // anchor finished the job. Nothing to gate.
        if (inst->GetData(DATA_TYRANNUS) == DONE)
            return ObjectiveArriveResult::Done;

        // THE RECEIPT IS GORKUN, not the progress counter, and the difference is
        // the one recovery path this encounter has.
        //
        // at_tyrannus_event_starter raises progress to TYRANNUS_INTRO in the same
        // branch that summons him, so the two normally move together — but
        // boss_tyrannusAI::EnterEvadeMode despawns Gorkun and CLEARS
        // DATA_MARTIN_OR_GORKUN_GUID while leaving the (monotonic) counter at 6.
        // Gating the hand-over on the counter would therefore latch this objective
        // on a failed attempt and leave nothing able to re-trip the trigger;
        // gating it on Gorkun's existence means an evade that happens while this
        // hook still owns the tick simply re-fires.
        //
        // AFTER the hand-over there is no such recovery, and that is a known gap
        // rather than an oversight: an anchored event has no combat-side rung
        // (DcRel::AtObjective is non-combat only), the intro puts the whole party
        // in combat within a tick of the trigger, and the primary defence against
        // ever needing the recovery is the gather below.
        if (!inst->GetGuidData(DATA_MARTIN_OR_GORKUN_GUID).IsEmpty())
        {
            LOG_INFO("playerbots.dungeonclear",
                     "[DC:{}] PoS — Tyrannus's intro is running (Gorkun summoned, instance "
                     "progress {}); handing the encounter to the boss anchor",
                     bot->GetName(), inst->GetData(DATA_INSTANCE_PROGRESS));
            return ObjectiveArriveResult::Done;
        }

        // --- 1. gather on the ledge -----------------------------------------
        //
        // THE QUORUM IS THREE OF FOUR FOLLOWERS, not four of four: one bot that
        // cannot path in must never hold the other three at a door, and stranded
        // recovery (relevance 42) sits above this whole ladder and owns that
        // member. Dead members do not count against it — PosParty counts the
        // LIVING — so the gate opens as soon as the party is up, and the rez
        // ladder, not this hook, is what gets it there.
        PosPartyView const party = PosParty(bot, LEDGE_X, LEDGE_Y, LEDGE_Z, GATHER_RADIUS);
        bool const quorum =
            party.living <= 1 ||
            static_cast<float>(party.near_) >=
                static_cast<float>(party.living) * GATHER_QUORUM;

        if (!quorum)
        {
            // Hold the leader ON the ledge while they come in. TravelTo is a no-op
            // once it is inside the leash, so this is a re-walk trigger for a
            // leader shoved off the anchor and nothing else.
            //
            // ...which is exactly why this branch MUST say so. Both Running returns
            // in this hook used to be silent, and a leader already on the anchor
            // makes this one issue no movement at all — so a quorum-starved hold and
            // a walk-in whose spline never landed are the SAME observation from
            // outside: a tank parked on the anchor, not moving, until the step
            // timeout stalls the run. That ambiguity is what tp-20260907-140341-2
            // could not be triaged past (six runs, tank pinned to LEDGE_* within
            // 0.1yd, 250s from arrival to failure, and nothing in the log to
            // separate the two). Name the state and the numbers that decide it.
            PosLedgeLog(bot, context, "gathering", party,
                        bot->GetExactDist(ARENA_X, ARENA_Y, ARENA_Z));
            DcTransit::TravelTo(bot, botAI, LEDGE_X, LEDGE_Y, LEDGE_Z, LEDGE_ARRIVE);
            return ObjectiveArriveResult::Running;
        }

        // --- 2. walk into the sphere ----------------------------------------
        //
        // A FORGE FROM THE LEDGE WOULD BE A NO-OP. HandleAreaTriggerOpcode
        // re-tests Player::IsInAreaTriggerRadius, and for a radius trigger that is
        // a 3D sphere test against the DBC centre — so the leader has to be
        // physically within 51.5yd of (1006.86, 178.96, 628.16), and the ledge is
        // 73yd away. ARENA_* is the probed on-mesh point it walks to.
        float const toArena = bot->GetExactDist(ARENA_X, ARENA_Y, ARENA_Z);
        if (toArena > ARENA_LEASH)
        {
            PosLedgeLog(bot, context, "walking in", party, toArena);
            DcTransit::TravelTo(bot, botAI, ARENA_X, ARENA_Y, ARENA_Z, ARENA_LEASH);
            return ObjectiveArriveResult::Running;
        }

        // --- 3. trip it -------------------------------------------------------
        ForgeAreaTrigger(bot, AREATRIGGER_TYRANNUS);

        if (!inst->GetGuidData(DATA_MARTIN_OR_GORKUN_GUID).IsEmpty())
        {
            LOG_INFO("playerbots.dungeonclear",
                     "[DC:{}] PoS — Tyrannus's encounter started by forged areatrigger {} with "
                     "{}/{} of the party on the ledge (furthest {:.1f}yd)",
                     bot->GetName(), AREATRIGGER_TYRANNUS, party.near_, party.living,
                     party.furthest);
            return ObjectiveArriveResult::Done;
        }

        return ObjectiveArriveResult::Running;
    }
}

// Ids 29-30. The Violet Hold's are 15-19, Blackwing Lair's 20-21, Halls of
// Stone's 22-23, Halls of Lightning's 24 and Utgarde Pinnacle's 25-28; ids are ONE
// FLAT SPACE across every dungeon, and AddHook LOG_ERRORs a collision rather than
// silently dropping one.
void RegisterPitOfSaronHooks(ObjectiveHookRegistry::HookTable& out)
{
    using namespace DcPitOfSaron;

    ObjectiveHookRegistry::AddHook(out, HOOK_POS_GAUNTLET,       &PosDriveGauntlet);
    ObjectiveHookRegistry::AddHook(out, HOOK_POS_TYRANNUS_LEDGE, &PosEnterTyrannusLedge);
}
