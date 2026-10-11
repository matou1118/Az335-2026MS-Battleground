/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "ObjectiveHookRegistry.h"

#include <cstdint>
#include <list>
#include <vector>

#include "Creature.h"
#include "InstanceScript.h"
#include "Log.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "ServerFacade.h"
#include "SharedDefines.h"
#include "Timer.h"

#include "Ai/Dungeon/DungeonClear/Data/DungeonBossInfo.h"
#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"
#include "Ai/Dungeon/DungeonClear/DcValueKeys.h"
#include "Ai/Dungeon/DungeonClear/Util/DcCombatFlag.h"
#include "Ai/Dungeon/DungeonClear/Util/DcCosWaveDecision.h"
#include "Ai/Dungeon/DungeonClear/Util/DcMovement.h"
#include "Ai/Dungeon/DungeonClear/Util/DcPartyState.h"
#include "Ai/Dungeon/DungeonClear/Util/DcRun.h"
#include "Ai/Dungeon/DungeonClear/Util/DcSmartRest.h"
#include "Ai/Dungeon/DungeonClear/Util/DcSuppressionTransit.h"
#include "Ai/Dungeon/DungeonClear/Util/DcTargeting.h"
#include "Ai/Dungeon/DungeonClear/Util/DcThrottle.h"

// --- The Culling of Stratholme (map 595) — the imperative half --------------
//
// ONE hook, and it is a controller:
//
//   36  CosDriveWaves — the ten waves, Meathook and Salramm
//
// THE ARITHMETIC IS NOT HERE. It is the pure kernel in
// Util/DcCosWaveDecision.h, unit-tested without a map in
// t/TestCullingOfStratholme.cpp; this file is glue — the grid census, the
// splines, the force-pull, the telemetry — exactly as PitOfSaronDriver.cpp and
// HallsOfReflectionDriver.cpp are glue for theirs.
//
// THE REST OF THIS DUNGEON NEEDS NO HOOK AT ALL, which is unusual enough to state
// plainly. Nothing on map 595 is started by an areatrigger, so there is no packet
// to forge; every gossip on the critical path is reachable through the declarative
// Gossip step or the escort driver's resume branch; and the only item mechanic is
// the five crates, which are the new UseItemAt step. The waves are the single
// thing with no declarative expression, because what they need is a standing
// preference re-decided every tick rather than a sequence.
//
// THE RETURN CONTRACT IS THE MODULE'S THROUGHOUT, and getting it backwards wipes
// parties:
//
//   Running => "I am steering." Claims the tick.
//   Done    => "Nothing to steer." YIELDS the tick (the stepsOwnMovement branch
//              in DcRunEventAction) so the stock combat engine can fight.
//
// On this map the yield matters most inside the wave fights themselves: forty
// trash mobs and two bosses have to be killed by a party whose leader is running
// a rung above the stock combat movers, and every tick the driver claims while it
// has nothing to steer is a tick the tank does not swing. The kernel therefore
// yields on Fight and claims on everything else — and NEVER yields out of combat,
// because on this map a yield out of combat hands the leg to DcRel::Advance (15),
// which walks the party off the wave it is halfway to and onto Market Row.

namespace
{
    using namespace DcCullingOfStratholme;

    // --- the census ---------------------------------------------------------
    //
    // THE spawnId FILTER IS THE WHOLE PROBE. Every wave member is a TempSummon
    // (GetSpawnId() == 0); three of the wave ENTRIES also have static spawns on
    // this map — 103 Risen Zombies and 7 Enraging Ghouls line Fire Street with two
    // Crypt Fiends — and the Stratholme citizens UpdateEntry into Risen Zombies
    // during the city intro. All of those keep a non-zero spawnId.
    //
    // Without the filter the census reads "the wave is up" from the moment Fire
    // Street is inside WAVE_SCAN and the driver walks the party 250 yards into a
    // gauntlet that belongs to the LAST objective of the dungeon, with Arthas and
    // the real wave behind them. That is the one way this driver could lose a run
    // outright, so the filter is first and there is no code path around it.
    struct WaveCensus
    {
        uint32    alive = 0;
        uint32    engaged = 0;   // ...of which already in combat
        float     nearest = -1.0f;
        Creature* target = nullptr;  // the nearest one
    };

    WaveCensus CosCensus(Player* bot)
    {
        WaveCensus c;
        if (!bot)
            return c;

        std::list<Creature*> found;
        bot->GetCreatureListWithEntryInGrid(found, CosWaveEntries(), WAVE_SCAN);

        for (Creature* cr : found)
        {
            if (!cr || !cr->IsAlive())
                continue;
            if (cr->GetSpawnId() != 0)
                continue;  // a STATIC Fire Street zombie / a transformed citizen

            ++c.alive;
            if (cr->IsInCombat())
                ++c.engaged;

            float const d = bot->GetExactDist(cr);
            if (c.nearest < 0.0f || d < c.nearest)
            {
                c.nearest = d;
                c.target = cr;
            }
        }
        return c;
    }

    // --- has the party recovered enough to walk into the next 4-pack? -------
    //
    // EventRestDecision's legacy branch, lifted to a free function because a hook
    // has a Player* and a context and not an action. Spread is deliberately NOT
    // part of it (effectively-infinite maxSpread): cohesion on this phase is the
    // travel leg's business, and folding it in here would report "needs rest" for
    // a party that is merely strung out behind the tank.
    //
    // Returns TRUE when there is nothing to wait for — which is also the answer
    // when no rest target is configured at all, and that case is load-bearing: the
    // floors are 0/0 under a phantom combat flag, where nobody CAN eat or drink, so
    // a set-piece must not stop for a rest that can never happen.
    bool CosPartyRecovered(Player* bot, AiObjectContext* context)
    {
        if (!bot || !context)
            return true;

        if (DcSmartRest::Enabled(bot))
        {
            // Smart Rest owns the whole decision. Its party-wide hysteresis latch
            // is normally refreshed by the between-pulls gate, which is dormant
            // inside a set-piece — so refresh it here, idempotently within a tick,
            // exactly as the event rest decision does.
            return !DcSmartRest::UpdateLatch(bot, context);
        }

        DcPartyState::RestGate const rest = DcPartyState::GetRestGate(bot, context);
        if (rest.minHp <= 0.0f && rest.minMp <= 0.0f)
            return true;
        return DcPartyState::IsPartyReady(bot, rest.minHp, rest.minMp,
                                          /*maxSpread*/ 100000.0f);
    }

    // --- breaking a standoff ------------------------------------------------
    //
    // The two halves of a pull, and neither substitutes for the other. Lifted in
    // shape from PitOfSaronDriver's PosForcePull/PosEngageTarget (and VioletHold's
    // before it), which solve the same "hostile standing there pointing at
    // nothing" problem — and deliberately NOT fixed by loosening the shared engage
    // path's target filters, whose strictness is load-bearing everywhere else.
    //
    // On THIS map it is insurance rather than a mechanism: the wave mobs are
    // ordinary REACT_AGGRESSIVE trash with ~20yd detection and the tank's walk-in
    // is a relocation, so their own aggro check should fire every time. It costs
    // nothing when it does.

    // Make the CREATURE attack the BOT.
    bool CosForcePull(Player* bot, Creature* c)
    {
        if (!bot || !c || !c->IsAlive() || c->IsInCombat())
            return false;

        // A mob that despawn-reset itself would settle straight back into its home
        // idle after the pull; clear that first.
        if (c->IsInEvadeMode())
            c->ClearUnitState(UNIT_STATE_EVADE);

        c->EngageWithTarget(bot);
        if (c->AI())
            c->AI()->AttackStart(bot);
        return true;
    }

    // Make the BOT attack the CREATURE. A player does not auto-retaliate, so the
    // force-pull alone leaves the tank standing there being hit; something has to
    // call Player::Attack, and the tank has to commit to THIS mob rather than
    // whatever stock targeting next re-picks.
    void CosEngageTarget(Player* bot, AiObjectContext* context, Creature* target)
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

    // The per-tick telemetry line, throttled. Without it a failed wave phase says
    // nothing about WHICH mechanism is biting: "the census is empty" reads exactly
    // like "the party cannot path to Elders' Square" in a log that only shows a
    // party standing in a street.
    void CosWaveLog(Player* bot, DcRunState& st, DcCosWaves::Verdict const& v,
                    DcCosWaves::Inputs const& in)
    {
        if (st.Throttled(DcThrottle::CosWaveLog, TELEMETRY_MS))
            return;

        LOG_DEBUG("playerbots.dungeonclear",
                  "[DC:{}] CoS waves — progress {}, {} — {} live wave mob(s) ({} engaged), "
                  "nearest {:.1f}yd, party {}, {} -> {}",
                  bot->GetName(), in.progress, DcCosWaves::StateName(v.state),
                  in.waveAlive, in.waveEngaged ? "yes" : "no", in.nearestWaveDist,
                  in.partyInCombat ? "in combat" : "out of combat",
                  in.partyRecovered ? "recovered" : "NOT recovered",
                  v.travel      ? "walking to the wave"
                  : v.pullWave  ? "pulling it"
                  : v.yieldTick ? "yielding — fight"
                                : "holding");
    }

    // --- hook 36: RUN THE TEN WAVES — the controller ------------------------
    ObjectiveArriveResult CosDriveWaves(Player* bot, AiObjectContext* context,
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
        uint32 const progress = inst->GetData(DATA_ARTHAS_EVENT);

        // --- the snapshot ---------------------------------------------------
        //
        // The census is the only expensive probe and it is skipped entirely
        // outside the window: the predicate already gates the event, but this hook
        // is also reachable from the executor's own Drive and the cost of a 320yd
        // grid walk is not worth paying to answer a question the counter settles.
        bool const inWindow = progress == PROGRESS_FINISHED_CITY_INTRO ||
                              progress == PROGRESS_KILLED_MEATHOOK;
        WaveCensus const wave = inWindow ? CosCensus(bot) : WaveCensus{};

        DcCosWaves::Inputs in;
        in.progress = progress;
        in.progressWavesStart = PROGRESS_FINISHED_CITY_INTRO;
        in.progressMeathook = PROGRESS_KILLED_MEATHOOK;
        in.waveAlive = wave.alive;
        in.nearestWaveDist = wave.nearest;
        in.waveEngaged = wave.engaged > 0;
        in.partyInCombat = DcCombatFlag::AnyPartyEngagement(bot);
        in.engageRange = WAVE_ENGAGE_RANGE;
        in.restSafeDist = WAVE_REST_SAFE_DIST;
        in.partyRecovered = inWindow ? CosPartyRecovered(bot, context) : true;
        in.nowMs = now;
        in.standoffMs = WAVE_STANDOFF_MS;
        in.standoffSinceMs = st.cosWaveStandoffMs;
        in.restBudgetMs = WAVE_REST_BUDGET_MS;
        in.restSinceMs = st.cosWaveRestMs;
        in.restSpentMs = st.cosWaveRestSpentMs;
        in.state = st.cosWaveState;
        in.stateSinceMs = st.cosWaveStateMs;

        DcCosWaves::Verdict const v = DcCosWaves::Decide(in);

        // Store the clocks back BEFORE acting, so the stored state and the state it
        // names can never disagree even for one tick.
        st.cosWaveStandoffMs = v.standoffSinceMs;
        st.cosWaveRestMs = v.restSinceMs;
        st.cosWaveRestSpentMs = v.restSpentMs;

        if (v.complete)
        {
            // Out of the window — Salramm is dead, or the city intro has not
            // finished. Drop the block for the ClearPosGauntlet reason: the event is
            // Repeatable, and a party that re-enters the window holding a spent
            // standoff clock would open its pull budget on a silence that ended
            // minutes ago.
            st.ClearCos();
            return ObjectiveArriveResult::Done;
        }

        CosWaveLog(bot, st, v, in);

        // ONE LINE WHEN THE REST HOLD GIVES UP, and it names why. "The party
        // stopped resting because it is full" and "the party stopped resting
        // because it never will be" are the same observation from outside — a party
        // that starts walking — and only the second one is a problem worth chasing.
        if (v.restBudgetExpired)
            LOG_WARN("playerbots.dungeonclear",
                     "[DC:{}] CoS waves — the phase's {}ms rest budget is spent with the "
                     "party still short; walking to the wave, and not resting again until "
                     "it actually recovers. Somebody cannot reach the rest target (no "
                     "water, a member the rez ladder has not reached, or a rest target "
                     "above what drinking delivers — a mana-using TANK is the usual one).",
                     bot->GetName(), WAVE_REST_BUDGET_MS);

        // --- announce the transitions worth one line each -------------------
        if (st.cosWaveState != v.storeState)
        {
            st.cosWaveState = v.storeState;
            st.cosWaveStateMs = v.stateSinceMs;
            LOG_INFO("playerbots.dungeonclear",
                     "[DC:{}] CoS waves — {} (instance progress {}, {} live wave mob(s), "
                     "nearest {:.1f}yd)",
                     bot->GetName(), DcCosWaves::StateName(v.state), progress,
                     in.waveAlive, in.nearestWaveDist);
        }
        else
        {
            st.cosWaveStateMs = v.stateSinceMs;
        }

        // --- act -------------------------------------------------------------

        // THE STANDOFF BREAKER. A live wave mob has stood inside engage range with
        // nobody in combat for the whole budget, so its own aggro check is never
        // going to come. Start the fight; one pull normally cascades through the
        // rest of the 4-pack, and if it does not the next tick's census picks the
        // next one.
        if (v.pullWave && wave.target)
        {
            LOG_INFO("playerbots.dungeonclear",
                     "[DC:{}] CoS waves — {} live wave mob(s), nearest {} at {:.1f}yd, and "
                     "nobody in combat after {}ms. Pulling it.",
                     bot->GetName(), wave.alive, wave.target->GetName(), wave.nearest,
                     WAVE_STANDOFF_MS);

            CosForcePull(bot, wave.target);
            CosEngageTarget(bot, context, wave.target);
            return ObjectiveArriveResult::Running;
        }

        // WALK AT THE MOB, not at a point near it, and not at an authored cluster
        // centroid. Two reasons, and the second is the expensive one:
        //
        //   * the mobs are summoned at a scripted spot and go REACT_AGGRESSIVE in
        //     place, with no movement script — so one the party never physically
        //     reaches is a wave that never dies and a counter that never moves;
        //   * the geometric centre of the four clusters has NO navmesh at city
        //     height (probed: only map 595's flat z = 0.14 sheet), and this map is
        //     one of the twelve where a destination with no navmesh poly resolves
        //     to that sheet — a ~130 yard sink. A live summon's own position is
        //     standable by construction, which no hand-authored point is.
        //
        // Through DcTransit::TravelTo (LongRangePathfinder + a re-issue floor)
        // because the legs run to 264 yards: a bare MovePoint is capped at 74
        // smoothed points and truncates SILENTLY well before that.
        //
        // TravelTo returns false when the leader is already inside the leash —
        // which the kernel has just told us it is not, since WAVE_TRAVEL_LEASH is
        // well under WAVE_ENGAGE_RANGE — so a false here means the spline could not
        // be issued at all, and the honest answer is to yield rather than claim a
        // tick we did nothing with. Halls of Lightning's contract, verbatim.
        if (v.travel && wave.target)
        {
            return DcTransit::TravelTo(bot, botAI, wave.target->GetPositionX(),
                                       wave.target->GetPositionY(),
                                       wave.target->GetPositionZ(), WAVE_TRAVEL_LEASH)
                       ? ObjectiveArriveResult::Running
                       : ObjectiveArriveResult::Done;
        }

        // YIELDING — the kernel says the party should be fighting, which is the
        // only state on this map where handing the tick back is right.
        if (v.yieldTick)
            return ObjectiveArriveResult::Done;

        // HOLDING. Either the census is empty (the 20 seconds before wave 1, the 10
        // seconds after an Arthas death, or the instant between a wave's last death
        // and the next summon) or the party is resting out of reach of a live one.
        //
        // It also catches the one case the branches above can fall through: a travel
        // or pull verdict whose target died between the census and here. Standing
        // still for a tick is the right answer to that too, and the alternative —
        // claiming the tick having issued nothing — is how a driver goes quietly
        // inert.
        //
        // A soft stop rather than no movement at all: the party may have been
        // walking to a mob that has just died, and a Rest hold in particular has to
        // actually STOP so the followers can sit down. Soft (not Hold) because Hold
        // would also cancel an escort glide, which this phase never has but the
        // followers' own ladders might.
        DcMovement::StopBot(bot, DcMovement::Stop::Soft);
        return ObjectiveArriveResult::Running;
    }
}

// Id 36. The Violet Hold's are 15-19, Blackwing Lair's 20-21, Halls of Stone's
// 22-23, Halls of Lightning's 24, Utgarde Pinnacle's 25-28, Pit of Saron's 29-30
// and Halls of Reflection's 31-35; ids are ONE FLAT SPACE across every dungeon,
// and AddHook LOG_ERRORs a collision rather than silently dropping one.
void RegisterCullingOfStratholmeHooks(ObjectiveHookRegistry::HookTable& out)
{
    using namespace DcCullingOfStratholme;

    ObjectiveHookRegistry::AddHook(out, HOOK_COS_WAVES, &CosDriveWaves);
}
