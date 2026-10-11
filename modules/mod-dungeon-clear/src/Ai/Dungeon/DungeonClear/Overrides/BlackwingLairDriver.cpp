/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "ObjectiveHookRegistry.h"

#include <cstdint>
#include <list>
#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "CombatManager.h"
#include "Creature.h"
#include "GameObject.h"
#include "Group.h"
#include "InstanceScript.h"
#include "Log.h"
#include "Map.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "Timer.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonBossInfo.h"
#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"
#include "Ai/Dungeon/DungeonClear/Util/DcLeaderSignal.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonClearRouteRegistry.h"
#include "Ai/Dungeon/DungeonClear/Settings/DcSettings.h"
#include "Ai/Dungeon/DungeonClear/Util/DcMovement.h"
#include "Ai/Dungeon/DungeonClear/Util/DcRazorgoreDecision.h"
#include "Ai/Dungeon/DungeonClear/Util/DcSuppressionTransit.h"
#include "Ai/Dungeon/DungeonClear/Util/DcSuppressionTransitDecision.h"
#include "Ai/Dungeon/DungeonClear/Util/DcRun.h"
#include "Ai/Dungeon/DungeonClear/DcValueKeys.h"

// The Blackwing Lair driver — Razorgore's orb and egg run.
//
// ONE hook, and it is a controller in the Black Morass / Violet Hold sense: it
// re-decides from live world state every tick for the whole of phase 1. What
// makes it unlike either of those is WHAT it steers.
//
//   * IT NEVER MOVES THE BOT THAT RUNS IT. The Violet Hold driver's whole job is
//     walking the tank across the arena; this one's is walking a MIND-CONTROLLED
//     BOSS around a room while the bot running it — the raid's main tank — keeps
//     tanking. So every branch here ends in Done (yield the tick): the work is
//     side effects on other units, and claiming the tick would only starve the
//     tank's rotation for the ~135 seconds the egg run takes. Getting that
//     backwards wipes raids, which is the one lesson this module has paid for
//     twice already.
//   * IT DELEGATES ITS OTHER ACTOR. The orb refuses a user with a pet and roots
//     whoever takes it for 90 seconds, so the runner cannot be the tank. The
//     driver ELECTS one and publishes it (DcRunState::razorRunnerGuid, read
//     cross-bot through DcLeaderSignal::GetRazorgoreOrbStation); the runner
//     walks itself to the orb and clicks it on its OWN tick, in
//     DungeonClearRazorgoreOrbAction. Reaching across and poking another bot's
//     MotionMaster from here would fight both that bot's AI and the `bwl`
//     strategy's own repositioning, every tick, for ever.
//
// THE ENCOUNTER'S CLOCKS, all measured, none of them ours to choose:
//   mind control 90s (19832) · charmer lockout 60s (23958) · egg cast 3s and
//   10yd (19873, interrupt-on-move) · 30 eggs · a ~262yd tour of them. Two to
//   three windows, and a rotation of runners because the lockout outlives a
//   single window by design.
//
// WHAT WE DO NOT DO HERE: keep the raid off the boss. Killing Razorgore in phase
// 1 instakills everyone (20038), and that guard is real and DC's own, but it lives
// in the target-selection seam rather than in this driver —
// DcTargetExclusionRegistry, DungeonClearDpsTargetValue and the hold-fire rung,
// which between them bar him from every DPS pick and take him back off anyone who
// already had him. The best thing this driver can do about the risk is what it
// already does: finish phase 1 quickly.
//
// AND WHAT WE NO LONGER DO: walk the raid to the orb platform. Grethok the
// Controller is a boss anchor now (RegisterBlackwingLairRoster), so the ordinary
// pipeline — advance, raid muster, boss standoff, engage — brings the raid up as
// one body and the tank makes the pull. This driver waits for that pull and then
// works the orb; it publishes ONE stamp (razorDrivingMs) which arms the raid's
// egg-run camp from the pull onward, and nothing before it.

namespace
{
    using namespace DcBlackwingLair;

    // Movement point id for the boss's egg-to-egg splines. Arbitrary but stable:
    // it only has to differ from anything the boss's own AI would use, and its
    // AI is not running any movement while charmed (SetCharmedBy idles the
    // MotionMaster and UpdateVictim fails, so UpdateAI early-outs).
    constexpr uint32 EGG_MOVE_POINT_ID = 4690;

    // Don't re-issue the boss's spline more than this often. A MovePoint every
    // tick re-plots the path and the unit travels nowhere; ~1s is well inside the
    // 1-2s a leg between neighbouring eggs takes.
    constexpr uint32 MOVE_REISSUE_MS = 1000;

    // Re-election throttle for the runner, so a comp with nobody eligible logs
    // once a few seconds instead of once a tick.
    constexpr uint32 RUNNER_PICK_MS = 3000;

    // --- world lookups -----------------------------------------------------

    Creature* BwlRazorgore(Player* bot)
    {
        return bot ? bot->FindNearestCreature(NPC_RAZORGORE, ROOM_SCAN, /*alive*/ true) : nullptr;
    }

    GameObject* BwlOrb(Player* bot)
    {
        return bot ? bot->FindNearestGameObject(GO_ORB_OF_DOMINATION, ROOM_SCAN) : nullptr;
    }

    // Every egg still standing. A destroyed egg does not linger in a flagged
    // state to be filtered later: spell_egg_event Uses the goober and pushes its
    // respawn a week out, so it flips to ACTIVE and despawns within ten seconds.
    // Scanning for `isSpawned() && GO_STATE_READY` is therefore an exact count of
    // the work left, and it survives a wipe, a re-entry and the phase flip (which
    // phases the leftovers out) without any bookkeeping of our own.
    void BwlLiveEggs(Creature* razor, std::vector<GameObject*>& out)
    {
        out.clear();
        if (!razor)
            return;
        std::list<GameObject*> found;
        razor->GetGameObjectListWithEntryInGrid(found, GO_BLACK_DRAGON_EGG, ROOM_SCAN);
        for (GameObject* go : found)
            if (go && go->isSpawned() && go->GetGoState() == GO_STATE_READY)
                out.push_back(go);
    }

    bool BwlIsSkipped(DcRunState const& st, ObjectGuid guid)
    {
        for (ObjectGuid const& g : st.razorEggSkipped)
            if (g == guid)
                return true;
        return false;
    }

    // --- the runner --------------------------------------------------------

    // Is this member still a legal orb user RIGHT NOW? The orb script's own three
    // refusals (dead, exhausted, owns a pet) plus "is a bot we can drive".
    // Re-read every tick rather than remembered: a hunter that resummoned its pet
    // and a runner that just took the 60s lockout look identical to a cached flag.
    bool BwlRunnerUsable(Player* p)
    {
        return p && p->IsAlive() && GET_PLAYERBOT_AI(p) && !p->GetPet() &&
               !p->HasAura(SPELL_MIND_EXHAUSTION);
    }

    Player* BwlResolveRunner(Player* leader, DcRunState const& st)
    {
        if (!leader || !st.razorRunnerGuid)
            return nullptr;
        Player* p = ObjectAccessor::FindPlayer(st.razorRunnerGuid);
        if (!p || p->GetMapId() != leader->GetMapId())
            return nullptr;
        return p;
    }

    // Build the candidate pool and hand it to the pure elector. The pool is the
    // whole raid on this map, not a sub-group: the orb is one object and the
    // rotation has to draw from everybody who can legally take it.
    // `force` skips the throttle. The throttle exists so a comp with nobody
    // eligible logs once every few seconds instead of once a tick; it must NOT
    // stand between a runner whose possession just broke and its replacement,
    // because those three seconds are three seconds of a freed Razorgore beating
    // on a DPS the raid is forbidden to help by killing him.
    Player* BwlElectRunner(Player* leader, DcRunState& st, bool force = false)
    {
        uint32 const now = getMSTime();
        if (!force && st.razorRunnerPickedMs && now - st.razorRunnerPickedMs < RUNNER_PICK_MS)
            return BwlResolveRunner(leader, st);
        st.razorRunnerPickedMs = now;

        Group* group = leader ? leader->GetGroup() : nullptr;
        if (!group)
            return nullptr;

        std::vector<DcRazorgore::RunnerCandidate> pool;
        std::vector<Player*> members;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* p = ref->GetSource();
            if (!p || p->GetMapId() != leader->GetMapId())
                continue;

            DcRazorgore::RunnerCandidate c;
            c.guidLow   = p->GetGUID().GetCounter();
            c.alive     = p->IsAlive();
            c.isBot     = GET_PLAYERBOT_AI(p) != nullptr;
            c.isLeader  = (p == leader);
            c.isTank    = PlayerbotAI::IsTank(p);
            c.isHealer  = PlayerbotAI::IsHeal(p);
            c.isRanged  = PlayerbotAI::IsRanged(p);
            c.hasPet    = p->GetPet() != nullptr;
            c.exhausted = p->HasAura(SPELL_MIND_EXHAUSTION);
            // Straight-line to the orb, not a path length: the tie-break is
            // bucketed at 10yd and everyone in the pool is in the same chamber, so
            // a pathfinder call per candidate per election would buy nothing.
            c.distToOrb = p->GetExactDist2d(ORB_X, ORB_Y);
            pool.push_back(c);
            members.push_back(p);
        }

        int const pick = DcRazorgore::SelectRunner(pool);
        if (pick < 0)
        {
            // Nobody eligible. Usually transient — every candidate is inside the
            // 60s lockout between windows — but a comp of pet classes makes it
            // permanent, and that is worth saying out loud rather than stalling
            // in silence.
            st.razorRunnerGuid.Clear();
            LOG_WARN("playerbots.dungeonclear",
                     "DungeonClear: Razorgore — no eligible orb runner among {} members "
                     "(needs a live bot with no pet and no Mind Exhaustion)",
                     uint32(members.size()));
            return nullptr;
        }

        Player* runner = members[pick];
        if (st.razorRunnerGuid != runner->GetGUID())
        {
            st.razorRunnerGuid = runner->GetGUID();
            LOG_INFO("playerbots.dungeonclear",
                     "DungeonClear: Razorgore — orb runner is {}", runner->GetName());
        }
        return runner;
    }

    // --- steering the possessed boss ---------------------------------------

    // Nearest live, non-skipped egg to the boss. Greedy nearest-neighbour: the
    // measured tour of all thirty from his spawn is 262yd, which at his 10 yd/s
    // is 26 seconds against 90 seconds of casting — the order is simply not where
    // this encounter's time goes, so the cheapest correct rule wins.
    GameObject* BwlElectEgg(Creature* razor, std::vector<GameObject*> const& eggs,
                            DcRunState const& st)
    {
        GameObject* best = nullptr;
        float bestDist = 0.0f;
        for (GameObject* egg : eggs)
        {
            if (BwlIsSkipped(st, egg->GetGUID()))
                continue;
            float const d = razor->GetExactDist(egg);
            if (!best || d < bestDist)
            {
                best = egg;
                bestDist = d;
            }
        }
        return best;
    }

    void BwlStopBoss(Creature* razor)
    {
        if (!razor)
            return;
        MotionMaster* mm = razor->GetMotionMaster();
        if (mm && mm->GetCurrentMovementGeneratorType() == POINT_MOTION_TYPE)
        {
            // Clear(false) + MoveIdle is the same incantation SetCharmedBy uses to
            // park a freshly possessed creature, and it is what actually removes
            // the generator: StopMoving alone leaves it installed and it re-asserts
            // on the next motion update — the persistent-generator gotcha that
            // walks a "stopped" unit right back out.
            mm->Clear(false);
            mm->MoveIdle();
        }
        razor->StopMoving();
    }

    void BwlMoveBossTo(Creature* razor, GameObject* egg, DcRunState& st)
    {
        uint32 const now = getMSTime();
        bool const sameTarget = st.razorEggGuid == egg->GetGUID();
        bool const travelling = razor->GetMotionMaster() &&
                                razor->GetMotionMaster()->GetCurrentMovementGeneratorType() ==
                                    POINT_MOTION_TYPE;
        if (sameTarget && travelling && now - st.razorMoveIssuedMs < MOVE_REISSUE_MS)
            return;

        st.razorMoveIssuedMs = now;
        // generatePath, and NOT forceDestination. The room has two tiers joined by
        // a ramp, so a straight-line override would happily slide the boss through
        // the wall of the ledge; a generated path that ends short is a stall we can
        // SEE (the no-progress clock below retires the egg) and a wall-clip is not.
        razor->GetMotionMaster()->MovePoint(EGG_MOVE_POINT_ID, egg->GetPositionX(),
                                            egg->GetPositionY(), egg->GetPositionZ(),
                                            FORCED_MOVEMENT_NONE, /*speed*/ 0.0f,
                                            /*orientation*/ 0.0f, /*generatePath*/ true,
                                            /*forceDestination*/ false);
    }

    // Destroy one egg. The polite cast first — a real 3s channel, exactly what a
    // player's possess bar does — and after enough tries on the SAME egg, the same
    // spell TRIGGERED. That is not a different outcome: 19873 carries the whole
    // encounter's bookkeeping in spell_egg_event::HandleOnHit (the instance
    // counter, the boss's line, the goober use, the week-long respawn), and a
    // triggered cast runs the identical script with the identical caster and
    // target. All it skips is the channel.
    //
    // The counter counts ATTEMPTS, not refusals, and that distinction is the whole
    // point of it. 19873's interrupt flags are 31 — movement, pushback, school
    // lock, autoattack and DAMAGE all cancel it — so the failure this has to
    // survive is not a refused cast (which returns a result we can read) but a
    // STARTED one that never finishes: CastSpell says OK, three seconds later
    // something has hit the boss, and the egg is still standing with nothing in
    // the return value to say so. Counting starts catches both shapes; the caller
    // resets the counter when the elected egg changes, so it only ever measures
    // failures against ONE egg.
    void BwlCastEggDestroy(Creature* razor, GameObject* egg, DcRunState& st)
    {
        razor->SetFacingToObject(egg);

        bool const triggered = st.razorEggAttempts >= DcRazorgore::kPoliteCastAttempts;
        SpellCastResult const res = razor->CastSpell(egg, SPELL_DESTROY_EGG, triggered);
        ++st.razorEggAttempts;

        if (res != SPELL_CAST_OK || triggered)
            LOG_DEBUG("playerbots.dungeonclear",
                      "DungeonClear: Razorgore — Destroy Egg on {} -> result {}, attempt {}{}",
                      egg->GetGUID().ToString(), uint32(res), uint32(st.razorEggAttempts),
                      triggered ? " [triggered]" : "");
    }
}

// The hook. Ordered by the pure kernel (DcRazorgore::Decide); everything below is
// the glue that turns one Step into world side effects.
//
// ALWAYS returns Done. Done means "nothing to steer this tick" and yields, and
// that is right on every branch here because the driver never moves the bot it
// runs on — the tank has an add wave to hold and needs its rotation. The event is
// Repeatable, so a Done simply re-arms it for the next tick.
static ObjectiveArriveResult DriveRazorgoreOrb(Player* bot, AiObjectContext* context,
                                               DungeonBossInfo const& /*info*/)
{
    if (!bot || !context || bot->GetMapId() != DcBlackwingLair::MAP_ID)
        return ObjectiveArriveResult::Done;

    using namespace DcBlackwingLair;
    DcRunState& st = DcRun::Of(context);

    Creature* razor = BwlRazorgore(bot);
    std::vector<GameObject*> eggs;
    BwlLiveEggs(razor, eggs);

    InstanceScript* inst = bot->GetInstanceScript();

    DcRazorgore::View v;
    v.eventDone      = inst && inst->GetData(DATA_EGG_EVENT) == DONE;
    v.bossAlive      = razor != nullptr;
    v.eggsRemaining  = static_cast<uint32>(eggs.size());

    // ADOPT WHOEVER ACTUALLY HOLDS HIM. The driver's election is a preference;
    // the charm is a fact. They come apart in the one case that matters — a
    // possession that broke early, an election that has since moved on, and a
    // second bot about to click an orb on a boss somebody else is already
    // driving — and every time they do, the world is right and the election is
    // wrong. So the charmer is re-published as the runner before anything is
    // decided, and the two can never disagree for more than a tick.
    if (razor && razor->IsCharmed())
    {
        ObjectGuid const charmer = razor->GetCharmerGUID();
        if (charmer.IsPlayer() && charmer != st.razorRunnerGuid)
        {
            Player* holder = ObjectAccessor::FindPlayer(charmer);
            if (holder && GET_PLAYERBOT_AI(holder) && holder->GetMapId() == bot->GetMapId())
            {
                LOG_INFO("playerbots.dungeonclear",
                         "DungeonClear: Razorgore — adopting {} as the orb runner "
                         "(it holds the possession)", holder->GetName());
                st.razorRunnerGuid = holder->GetGUID();
            }
        }
    }

    Player* runner = BwlResolveRunner(bot, st);
    v.bossCharmed = razor && runner && razor->IsCharmed() &&
                    razor->GetCharmerGUID() == runner->GetGUID();
    v.bossCasting = razor && razor->HasUnitState(UNIT_STATE_CASTING);
    // Pulling the guard pack pulls the boss: creature_formations makes Razorgore
    // a member of Grethok's formation with groupAI 7 (full mutual assist), so the
    // tank's tag on Grethok aggros him from 77yd on the next tick. Either flag is
    // "the pull has landed" as far as the kernel is concerned.
    v.bossEngaged = razor && razor->IsInCombat();

    // The platform. Only asked while the boss is NOT charmed — mid-possession the
    // guards are long dead and the scan is pure cost, and a stray respawn must
    // never abort a live egg run.
    //
    // Out of the chamber the answer is "held and unpulled", not "clear". The event
    // is due from 200yd out (the leader may still be on the approach) and the SAFE
    // reading of an unscanned platform is the one that moves nobody: everything
    // the kernel does with a live body starts on the far side of that answer.
    bool const nearChamber = bot->GetExactDist2d(ORB_X, ORB_Y) <= GUARD_CLEAR_RANGE;
    OrbGuardState const guards =
        (!v.bossCharmed && nearChamber) ? DcBlackwingLair::OrbGuards(bot) : OrbGuardState{};
    v.orbGuardsAlive   = !v.bossCharmed && (!nearChamber || guards.alive);
    v.orbGuardsEngaged = guards.engaged;

    // THE ANCHOR LATCH, and the one thing here that talks to the clear rather than
    // to the encounter. Grethok's roster row borrows Razorgore's kill-bit, which
    // covers everything up to the moment the raid actually kills the boss — but a
    // phase-1 wipe leaves a Grethok who is dead, decayed, and will never respawn,
    // and a candidate row for a creature that is not on the map stalls the clear
    // ("not spawned. Use 'dc skip'"). An empty platform IS his completion, so say
    // so once and let the roster advance to Razorgore on its own.
    //
    // Only on a tick that actually SCANNED the platform: mid-possession the scan
    // is skipped (it is pure cost with a charm up) and `guards` reads its default
    // all-false, which is not the same fact.
    if (nearChamber && !v.bossCharmed && !guards.alive && !v.eventDone)
    {
        auto& cleared =
            context->GetValue<std::unordered_set<uint32>&>(DcKey::ClearedAnchors)->Get();
        if (cleared.insert(NPC_GRETHOK_THE_CONTROLLER).second)
            LOG_INFO("playerbots.dungeonclear",
                     "DungeonClear: Razorgore — the orb platform is clear; "
                     "Grethok's anchor is done");
    }

    v.haveRunner     = runner && runner->IsAlive() && GET_PLAYERBOT_AI(runner);
    v.runnerAtOrb    = runner && runner->GetExactDist2d(ORB_X, ORB_Y) <= ORB_STATION_RADIUS;
    // The orb script's own refusals ONLY. "Mid-cast" used to be in here and was a
    // bug with a signature: a DPS bot is casting most ticks, so the elected runner
    // read unusable, the FSM asked for another, and the rotation cycled a new
    // runner every three seconds for the whole window without a single click
    // landing. A cast in flight is interrupted at the orb instead — see
    // DungeonClearRazorgoreOrbAction.
    v.runnerCanClick = BwlRunnerUsable(runner);

    GameObject* egg = nullptr;
    if (v.bossCharmed)
    {
        egg = BwlElectEgg(razor, eggs, st);
        v.haveEgg = egg != nullptr;
        if (egg)
            v.bossToEgg = razor->GetExactDist(egg);
    }

    // One line per 3s carrying every number the hook decided on. A run that goes
    // wrong is diagnosed from this: eggs standing still with a charm up means the
    // casts are not landing; "NeedRunner" forever means the comp has nobody legal;
    // a charm that never appears means the click is being swallowed.
    DcRazorgore::Step const step = DcRazorgore::Decide(v);
    {
        if (!st.Throttled(DcThrottle::RazorgoreLog, 3000))
        {
            LOG_DEBUG("playerbots.dungeonclear",
                      "DungeonClear: Razorgore — step {}, eggs {}/{}, guards {}, charmed {}, "
                      "runner {} (atOrb {}, canClick {}), egg {:.1f}yd, skipped {}",
                      uint32(step), v.eggsRemaining, EGG_COUNT,
                      v.orbGuardsAlive
                          ? ((v.orbGuardsEngaged || v.bossEngaged) ? "true/pulled" : "true/unpulled")
                          : "false",
                      v.bossCharmed,
                      runner ? runner->GetName() : "none", v.runnerAtOrb, v.runnerCanClick,
                      v.haveEgg ? v.bossToEgg : -1.0f, uint32(st.razorEggSkipped.size()));
        }
    }

    // Publish the ONE stamp the raid's positioning reads cross-bot: "the egg run
    // owns this fight". It arms the camp rung (the raid holds the floor below the
    // ledge, between the adds and the rooted runner) and keeps the elected
    // runner's own rung alive.
    //
    // Stamped from the PULL onward — never before it. Up to the tag, positioning
    // belongs to the clear: the advance is walking the raid to Grethok's anchor
    // and the muster is topping it off, and a camp rung armed underneath that
    // would fight the pipeline for every bot. WaitPull is exactly "the tank has
    // not pulled yet", so it is the one step that publishes nothing; Done is the
    // other, so the camp releases the instant phase 1 ends without a completion
    // test of its own.
    if (step != DcRazorgore::Step::Done && step != DcRazorgore::Step::WaitPull)
        st.razorDrivingMs = getMSTime();

    switch (step)
    {
        case DcRazorgore::Step::Done:
            // Phase 1 is over (or the boss is gone). Drop every scrap of state so a
            // re-entry, a wipe, or the SECOND time this instance is run starts
            // clean — the encounter soft-resets itself on a phase-1 death and we
            // must not come back holding a stale runner or a stale skip list.
            st.ClearRazorgore();
            return ObjectiveArriveResult::Done;

        case DcRazorgore::Step::Idle:
            // Every remaining egg is on the skip list. Clear it and take another
            // pass: a skip is a "not now" (a partial path, a cast that would not
            // land), never a "never", and the eggs that stalled early are often
            // reachable from where the boss has since ended up.
            if (!st.razorEggSkipped.empty())
            {
                LOG_INFO("playerbots.dungeonclear",
                         "DungeonClear: Razorgore — retrying {} skipped egg(s)",
                         uint32(st.razorEggSkipped.size()));
                st.razorEggSkipped.clear();
                st.razorEggGuid.Clear();
            }
            return ObjectiveArriveResult::Done;

        case DcRazorgore::Step::WaitPull:
            // The platform is held and nobody has pulled it. This is the tank's
            // tick, not ours: Grethok is boss anchor #0 and the ordinary pipeline
            // is walking the raid to him, mustering it and taking the standoff.
            // Deliberately NOTHING here — not even an election, whose only effect
            // before the pull would be to arm a runner's rung and send one DPS bot
            // up the ramp ahead of its raid.
            return ObjectiveArriveResult::Done;

        case DcRazorgore::Step::NeedRunner:
            // FORCE the election whenever a runner exists but can no longer take
            // the orb. That is the broken-possession path — its Mind Exhaustion is
            // up for the next sixty seconds and Razorgore is on it — and the
            // three-second throttle is a debounce for an empty candidate pool, not
            // a reason to leave the raid without a runner while that plays out.
            BwlElectRunner(bot, st, /*force*/ v.haveRunner && !v.runnerCanClick);
            return ObjectiveArriveResult::Done;

        case DcRazorgore::Step::StageRunner:
            // The runner walks itself — see DungeonClearRazorgoreOrbAction, which
            // reads the published GUID through DcLeaderSignal. Nothing to do here
            // but keep the election fresh in case it died on the way.
            return ObjectiveArriveResult::Done;

        case DcRazorgore::Step::ClickOrb:
        {
            GameObject* orb = BwlOrb(bot);
            if (!orb)
                return ObjectiveArriveResult::Done;
            // GameObject::Use dispatches to go_orb_of_domination::GossipHello
            // before it ever looks at the goober type, so this IS the player's
            // click: the script sets the boss's charmer, has it attack the runner,
            // and casts 19832 from the runner. We do not fake any of that.
            //
            // The runner does the clicking on its own tick (it is the one that
            // knows it has arrived and is not mid-cast); this branch exists for the
            // case where the runner's own rung is not the thing that got here —
            // it is idempotent, because a second click while already charmed is
            // refused by the script's own DONE / exhaustion / pet gates.
            LOG_INFO("playerbots.dungeonclear",
                     "DungeonClear: Razorgore — {} takes the Orb of Domination",
                     runner->GetName());
            orb->Use(runner);
            return ObjectiveArriveResult::Done;
        }

        case DcRazorgore::Step::WaitCast:
            return ObjectiveArriveResult::Done;

        case DcRazorgore::Step::MoveBoss:
        {
            // No-progress clock on the ELECTED EGG, not on the run: the failure this
            // catches is one egg the boss cannot reach (a path that ends short of
            // the ledge), and the answer is to spend the window on the other
            // twenty-nine rather than to stall the encounter on this one.
            uint32 const now = getMSTime();
            if (st.razorEggGuid != egg->GetGUID())
            {
                st.razorEggGuid     = egg->GetGUID();
                st.razorEggElectedMs = now;
                st.razorEggBestDist = v.bossToEgg;
                st.razorEggAttempts = 0;
            }
            else if (v.bossToEgg < st.razorEggBestDist - DcRazorgore::kRepathEpsilon)
            {
                st.razorEggBestDist = v.bossToEgg;
                st.razorEggElectedMs = now;
            }
            else if (now - st.razorEggElectedMs > DcRazorgore::kEggStallMs)
            {
                LOG_INFO("playerbots.dungeonclear",
                         "DungeonClear: Razorgore — egg {} unreachable ({:.1f}yd, no progress "
                         "in {}ms) -> skipping for this pass",
                         egg->GetGUID().ToString(), v.bossToEgg, DcRazorgore::kEggStallMs);
                st.razorEggSkipped.push_back(egg->GetGUID());
                st.razorEggGuid.Clear();
                return ObjectiveArriveResult::Done;
            }

            BwlMoveBossTo(razor, egg, st);
            return ObjectiveArriveResult::Done;
        }

        case DcRazorgore::Step::CastEgg:
        {
            // Stop FIRST. 19873 carries interrupt flags 31 — any movement during
            // the 3s cast eats it — and a unit that is still sliding out of its
            // last spline is moving even when it looks parked.
            BwlStopBoss(razor);

            uint32 const now = getMSTime();
            if (st.razorEggGuid != egg->GetGUID())
            {
                st.razorEggGuid      = egg->GetGUID();
                st.razorEggElectedMs = now;
                st.razorEggBestDist  = v.bossToEgg;
                st.razorEggAttempts  = 0;
            }

            BwlCastEggDestroy(razor, egg, st);

            // In range, stationary, and still standing after both the polite and
            // the triggered attempts have had their fifteen seconds. Park it and
            // spend the window on the other twenty-nine.
            if (now - st.razorEggElectedMs > DcRazorgore::kEggStallMs)
            {
                LOG_WARN("playerbots.dungeonclear",
                         "DungeonClear: Razorgore — egg {} survived {} cast attempt(s) "
                         "-> skipping for this pass",
                         egg->GetGUID().ToString(), uint32(st.razorEggAttempts));
                st.razorEggSkipped.push_back(egg->GetGUID());
                st.razorEggGuid.Clear();
            }
            return ObjectiveArriveResult::Done;
        }
    }

    return ObjectiveArriveResult::Done;
}


// --- the Suppression Rooms transit ----------------------------------------
//
// Hook 21, and the only thing on this map that MOVES THE LEADER WHILE IT IS
// ENGAGED. Everything above drives a possessed boss around a room the raid is
// already standing in; this drives the raid itself across 342yd of gauntlet that
// the ordinary clear cannot cross at all.
//
// WHY IT CANNOT. Three facts, and the design is just their consequence:
//
//   1. DcCombatFlag::MayDrive is false for as long as anything in the party is
//      engaged, and Advance is registered ONLY in the non-combat engine. With 100
//      whelps inside 20yd of the route line on a 30s respawn, AnyPartyEngagement
//      is true essentially forever — so the clear has no driver. Not a slow leg: a
//      stopped one.
//   2. Clearing it is arithmetically impossible. 160 whelps / 30s = 5.3 spawns a
//      second. A 40-bot raid can out-DPS that; it cannot out-TRAVEL it.
//   3. 95% of the route lies inside an armed Suppression Device's 20yd bubble, so
//      the 54-second walk is a 268-second one until they are disarmed — and over
//      268 seconds the route-adjacent whelps come back NINE times.
//
// So this is a TRANSIT: cross it in one body with the brakes off, and stand still
// only for the three things worth standing still for (the pack, the elites, the
// disarm rung's tick). What it deliberately does NOT do is clear, detour, or
// teleport past — the twenty elites on this leg are 600s respawns and six of them
// hold the only ramp between the rooms.
//
// THE RETURN CONTRACT is the Black Morass one, and getting it backwards wipes
// raids:
//
//   Running => "I am steering the raid right now." Claims the tick, which is what
//              lets it take the tank off whatever it is fighting and walk it down
//              the route.
//   Done    => "Nothing to steer." YIELDS the tick (the stepsOwnMovement branch in
//              DcRunEventAction) so the stock combat engine can fight: pick a
//              target, swing, cast, hold threat.
//
// EVERY HOLD YIELDS. That is not an optimisation — a hold is precisely a tick on
// which the raid should be fighting rather than walking, and a driver at
// relevance 61 that returns Running through a two-minute Taskmaster fight is a
// raid with no rotation for two minutes.

namespace
{
    // The authored route, as the kernel's plain-arithmetic view of it — sliced at
    // the staging point, because everything downstream of here counts from there:
    // the kernel's gather gate is `cursorIndex == 0`, the arm pins the cursor to 0,
    // ResolveCursor clamps against a stored index in the same space, and the
    // completion test is "the LAST anchor". Hand the driver the unsliced row and
    // every one of those means something different.
    //
    // It NOW LIVES IN DcTransit::Route(), shared with the pack rung, which needs
    // the same polyline to hold on (DcTransit::HoldPoint). Keeping it private to
    // this file is how the two halves came to disagree about where the corridor
    // runs: the driver walked the authored anchors and every follower walked a
    // straight chord across them, through a hole in the mesh.
    using BwlTransitRoute = DcTransit::RouteView;

    // A TAIL slice: the crossing ends AT Broodlord, so the row simply stops
    // there. (Halls of Lightning takes a middle slice for the opposite reason —
    // its party still has a hairpin and a second ramp to walk afterwards.)
    BwlTransitRoute const* TransitRoute()
    {
        return DcTransit::Route(MAP_ID, NPC_BROODLORD_LASHLAYER, TRANSIT_STAGE_ANCHOR_INDEX,
                                SIZE_MAX);
    }

    // How far the FURTHEST living member is from the CURSOR, and how many are
    // inside the gather radius OF THE CURSOR. One walk of the group for both,
    // because both are wanted on the same ticks and a second walk of a 40-member
    // raid every tick buys nothing.
    //
    // BOTH AGAINST THE CURSOR, and that is a fix rather than a tidy-up. The gather
    // quorum used to be measured against the LEADER while the rung that positions
    // the raid (DungeonClearTransitPackTrigger) holds it on a ring around the
    // CURSOR. During the gather the cursor is pinned to the staging point and the
    // leader keeps walking, so the two reference points drift apart and the gate
    // asks a question the rung is not answering. Live, tr-20260830-125018-2: the
    // raid stood PERFECTLY formed — `0 of 24 outside, trail 21.5yd` of a 25yd
    // leash — while the gate read `2/25 near` for nineteen consecutive ticks and
    // then timed out and crossed with two members. The leader was 6.3yd past
    // staging on the room side and the raid 21.5yd back on the approach side, so
    // the distances ADDED to ~28yd.
    //
    // Living members ON THIS MAP only: a corpse-running member is the rez ladder's
    // business, and a member who never zoned in must not pin the crossing.
    struct BwlPackView
    {
        float trailDist = -1.0f;  // <0 = the leader is alone
        uint32 living = 0;        // living members on this map, leader included
        uint32 nearCursor = 0;    // ...of which, inside the gather radius OF THE CURSOR
        bool topped = true;       // ...and everybody is at the pre-boss bars

        // The pack hold's own counts, measured against the CURSOR rather than
        // against the leader — the pack forms on the leg ahead, not on the tank.
        uint32 followers = 0;     // living members on this map, leader EXCLUDED
        uint32 outsideLeash = 0;  // ...of which, further than the pack leash from it
    };

    // TOPPED uses the RAID MUSTER's bars, not Smart Rest's release bars, and
    // deliberately: the muster runs this same test either side of this leg (at
    // Vaelastrasz and at Broodlord), and one definition of "the raid is ready" for
    // the whole raid layer is worth more than a second one tuned three points
    // differently. Bots are held to 99% because they can always eat; humans to a
    // loose 90/80 because a muster must never deadlock on a player standing where
    // they mean to stand.
    constexpr float BWL_BOT_HP_BAR = 99.0f;
    constexpr float BWL_BOT_MP_BAR = 99.0f;
    constexpr float BWL_HUMAN_HP_BAR = 90.0f;
    constexpr float BWL_HUMAN_MP_BAR = 80.0f;

    BwlPackView BwlPack(Player* bot, float cursorX, float cursorY, float cursorZ,
                        float gatherRadius, float packLeash)
    {
        BwlPackView v;
        if (!bot)
            return v;

        v.living = 1;
        v.nearCursor = bot->GetExactDist(cursorX, cursorY, cursorZ) <= gatherRadius ? 1 : 0;

        Group* group = bot->GetGroup();
        if (!group)
            return v;

        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (!member || !member->IsAlive() || member->GetMapId() != bot->GetMapId())
                continue;

            bool const isBot = GET_PLAYERBOT_AI(member) != nullptr;
            if (member->GetHealthPct() < (isBot ? BWL_BOT_HP_BAR : BWL_HUMAN_HP_BAR))
                v.topped = false;
            if (member->GetMaxPower(POWER_MANA) > 0 &&
                member->GetPowerPct(POWER_MANA) < (isBot ? BWL_BOT_MP_BAR : BWL_HUMAN_MP_BAR))
                v.topped = false;

            if (member == bot)
                continue;

            ++v.living;
            float const d = member->GetExactDist(cursorX, cursorY, cursorZ);
            if (d <= gatherRadius)
                ++v.nearCursor;

            if (d > v.trailDist)
                v.trailDist = d;

            ++v.followers;
            if (packLeash > 0.0f && d > packLeash)
                ++v.outsideLeash;
        }
        return v;
    }

    // Nearest LIVE elite worth stopping for, or -1. Taskmasters and Hatchers only:
    // the whelps are not a reason to stand still (there is no number of them the
    // party can finish) and DcNeverTargetRegistry has already taken them out of
    // the clear's pickers.
    float BwlNearestElite(Player* bot)
    {
        if (!bot)
            return -1.0f;

        static std::vector<uint32> const kElites = { NPC_BLACKWING_TASKMASTER,
                                                     NPC_DEATH_TALON_HATCHER };
        std::list<Creature*> found;
        bot->GetCreatureListWithEntryInGrid(found, kElites, TRANSIT_SCAN);

        float best = -1.0f;
        for (Creature* c : found)
        {
            if (!c || !c->IsAlive())
                continue;
            float const d = bot->GetExactDist(c);
            if (best < 0.0f || d < best)
                best = d;
        }
        return best;
    }

    // What the raid is still TOWING: live creatures holding somebody in combat
    // from further than TRANSIT_AGGRO_SHED_DIST away from everyone they hold.
    //
    // A GRID SCAN CANNOT ANSWER THIS, which is why it walks combat references
    // instead of reusing BwlNearestElite's shape. The whole point of the set is
    // that it is somewhere else — 132.6-151.6yd away and still standing on its
    // spawn, in tr-20260830-142049-6 — so any radius wide enough to see it is
    // wide enough to sweep two rooms of things that are not holding anybody.
    //
    // MEASURED FROM THE MEMBER IT ACTUALLY HOLDS, not from the leader, and the
    // difference is a false positive that would hold the raid for its whole
    // budget every crossing: a straggler 50yd behind the leader fighting a whelp
    // at arm's length is a mob 55yd from the leader and 2yd from the fight. So a
    // creature counts only when it is remote from EVERY member on its reference
    // list, which is the honest reading of "nobody is fighting this thing".
    //
    // DEDUPED BY GUID for the same reason the count is a count: seven members
    // held by one Technician is one thing to shed, and reporting it as seven
    // would make the log line unreadable in exactly the situation it exists for.
    //
    // Evade is skipped — a holder already walking home is the case the route
    // comment always assumed and is nothing to wait on. No pathfind anywhere:
    // unlike DcCombatFlag::ScanCombatHolders this asks a pure distance question,
    // and the caller only asks it at the staging point.
    struct BwlRemoteAggro
    {
        bool        valid = false;
        uint32      holders = 0;
        float       farthest = 0.0f;
        std::string worst;  // the farthest one's name, for the hold's log line
    };

    BwlRemoteAggro BwlRemoteHolders(Player* leader)
    {
        BwlRemoteAggro out;
        Map* const map = leader ? leader->GetMap() : nullptr;
        if (!leader || !map)
            return out;
        out.valid = true;

        // guid -> (closest member holding it, its name)
        std::unordered_map<ObjectGuid, std::pair<float, std::string>> nearest;

        auto const walk = [&](Player* member)
        {
            if (!member || !member->IsAlive() || member->GetMap() != map)
                return;
            for (auto const& kv : member->GetCombatManager().GetPvECombatRefs())
            {
                CombatReference* const ref = kv.second;
                if (!ref)
                    continue;
                Unit* const other = ref->GetOther(member);
                if (!other || !other->IsAlive() || other->GetMap() != map)
                    continue;
                if (!other->ToCreature())
                    continue;
                if (other->GetCombatManager().IsInEvadeMode())
                    continue;  // bailing home -> already shed

                float const d = member->GetExactDist(other);
                auto const it = nearest.find(other->GetGUID());
                if (it == nearest.end())
                    nearest.emplace(other->GetGUID(),
                                    std::make_pair(d, std::string(other->GetName())));
                else if (d < it->second.first)
                    it->second.first = d;
            }
        };

        if (Group* const group = leader->GetGroup())
            for (GroupReference* r = group->GetFirstMember(); r; r = r->next())
                walk(r->GetSource());
        else
            walk(leader);

        for (auto const& kv : nearest)
        {
            if (kv.second.first <= TRANSIT_AGGRO_SHED_DIST)
                continue;
            ++out.holders;
            if (kv.second.first > out.farthest)
            {
                out.farthest = kv.second.first;
                out.worst = kv.second.second;
            }
        }
        return out;
    }

    // Nearest ARMED Suppression Device, or -1. GO_STATE_READY is the armed state —
    // go_suppression_device pulses 22247 every 5s while it holds it, and the bot
    // disarm rung turns it off with SetGoState(GO_STATE_ACTIVE). A device turned
    // off that way never re-arms, so a device that reads ACTIVE is done with for
    // the rest of the run and is not a reason to stop.
    float BwlNearestArmedDevice(Player* bot)
    {
        if (!bot)
            return -1.0f;

        std::list<GameObject*> found;
        bot->GetGameObjectListWithEntryInGrid(found, GO_SUPPRESSION_DEVICE, TRANSIT_SCAN);

        float best = -1.0f;
        for (GameObject* go : found)
        {
            if (!go || !go->isSpawned() || go->GetGoState() != GO_STATE_READY)
                continue;
            float const d = bot->GetExactDist(go);
            if (best < 0.0f || d < best)
                best = d;
        }
        return best;
    }

    // The per-tick telemetry line, throttled. Without this a failed run tells you
    // nothing about WHICH of the four mechanisms is still biting — the whole point
    // of measuring the leg was to be able to tell "the pack never formed" from
    // "the devices were never disarmed" from "the driver never got a tick".
    void BwlTransitLog(Player* bot, DcRunState& st, DcSuppressionTransit::Inputs const& in,
                       DcSuppressionTransit::Verdict const& v, BwlPackView const& pack,
                       uint32 anchorCount, BwlRemoteAggro const& remote)
    {
        // Throttle state on the bot's own run state — see Util/DcThrottle.h.
        if (st.Throttled(DcThrottle::TransitLog, TRANSIT_TELEMETRY_MS))
            return;

        LOG_DEBUG("playerbots.dungeonclear",
                  "[DC:{}] BWL transit — cursor {}/{} ({:.1f}yd), pack {}/{} near the cursor{}, "
                  "{} of {} outside, trail {:.1f}yd (leash {:.0f}), elite {:.1f}yd, "
                  "armed device {:.1f}yd, towing {} -> {}{}",
                  bot->GetName(), in.cursorIndex, anchorCount ? anchorCount - 1 : 0, in.distToCursor,
                  pack.nearCursor, pack.living, pack.topped ? "" : ", NOT TOPPED",
                  in.packOutside, in.packLiving,
                  in.trailDist, in.packLeash,
                  in.nearestEliteDist, in.nearestArmedDeviceDist,
                  remote.valid ? std::to_string(remote.holders) : std::string("n/a"),
                  v.complete ? "AT THE STANDOFF"
                             : (v.advance ? "advancing" : DcSuppressionTransit::HoldName(v.hold)),
                  v.timedOut ? " (WATCHDOG)" : "");
    }
}

ObjectiveArriveResult DriveSuppressionTransit(Player* bot, AiObjectContext* context,
                                              DungeonBossInfo const& /*info*/)
{
    if (!bot || !context || bot->GetMapId() != MAP_ID)
        return ObjectiveArriveResult::Done;

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        return ObjectiveArriveResult::Done;

    DcRunState& st = DcRun::Of(context);

    // THE MASTER SWITCH, and the rollback. Off, this hook is a no-op that stops
    // publishing — within two AI ticks the pack rung goes inert with it and the
    // leg behaves exactly as it did before any of this existed.
    if (!DcSettings::GetBool(bot, "SuppressionTransit"))
    {
        st.ClearTransit();
        return ObjectiveArriveResult::Done;
    }

    BwlTransitRoute const* const rt = TransitRoute();
    if (!rt)
    {
        // No authored route: there is nothing to run a cursor down, and inventing
        // one across this geometry is how a raid ends up in a wall. Yield and let
        // the ordinary ladder have the leg back.
        st.ClearTransit();
        return ObjectiveArriveResult::Done;
    }

    uint32 const now = getMSTime();
    std::vector<WaypointHint> const& hints = rt->hints;
    std::vector<DcSuppressionTransit::Anchor> const& route = rt->anchors;

    uint32 const anchorCount = static_cast<uint32>(route.size());
    uint32 const lastAnchor = anchorCount - 1;

    // --- arm the crossing --------------------------------------------------
    if (!st.transitArmedMs)
    {
        st.transitArmedMs = now;
        st.transitCursorIndex = 0;
        st.transitHoldSinceMs = 0;
        st.transitHoldReason = 0;
        st.transitHoldTimedOut = false;

        // YOU CANNOT GATHER AT A POINT YOU HAVE ALREADY PASSED, and DISTANCE IS
        // NOT THE TEST FOR THAT — position ALONG THE ROUTE is.
        //
        // This asked `toStage > TRANSIT_STAGE_SKIP_DIST` alone, and it is a test
        // that cannot tell "37yd short of staging" from "37yd past it". In
        // tp-20260828-121941-1 all five raids armed at 37yd, every one of them
        // INSIDE the lower room, and every one latched `gathered = false` — which
        // pins the cursor to anchor 0, a point BEHIND the raid, for the rest of
        // the run. The gather gate then cannot open (it wants `reached`, and a
        // raid walking deeper into the room never gets it), so the pack hold
        // measured twenty-four members against that behind-them anchor and held
        // forever: runs 1, 3 and 4 spent 100% of their transit at `cursor 0/19`
        // with the distance drifting 37 -> 65yd. Three of five never left the
        // entrance.
        //
        // The projection answers the actual question. Past anchor 1 the leader is
        // in the gauntlet, whatever the straight-line distance says.
        float const toStage = bot->GetExactDist(TRANSIT_STAGE_X, TRANSIT_STAGE_Y, TRANSIT_STAGE_Z);
        uint32 const armCursor = DcSuppressionTransit::ResolveCursor(
            route, bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(),
            /*stored*/ 1, TRANSIT_CURSOR_RESYNC_DIST);

        st.transitGathered = toStage > TRANSIT_STAGE_SKIP_DIST || armCursor > 1;
        if (st.transitGathered)
            st.transitCursorIndex = armCursor;

        LOG_INFO("playerbots.dungeonclear",
                 "[DC:{}] BWL transit — crossing the Suppression Rooms ({} anchors, "
                 "{:.0f}yd to staging, projected anchor {}){}",
                 bot->GetName(), anchorCount, toStage, armCursor,
                 st.transitGathered ? " — already past the staging point, skipping the gather"
                                    : "");
    }

    // --- where are we ------------------------------------------------------
    //
    // The cursor is the leader's projection onto the route, monotone-clamped
    // against what we stored (see ResolveCursor) — EXCEPT while the gather gate is
    // still shut, where it is pinned to the staging anchor so the pack forms on
    // the point the raid is standing on rather than on a leg twelve yards into the
    // room. The pin is only ever correct while the raid is actually AT staging.
    uint32 const projected = DcSuppressionTransit::ResolveCursor(
        route, bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(),
        std::max<uint32>(st.transitCursorIndex, 1), TRANSIT_CURSOR_RESYNC_DIST);

    // ...and the pin is re-checked EVERY TICK, not only at the arm. A leader can
    // be shoved into the gauntlet after arming — dragged by a fight, carried by a
    // spline already in flight — and the arm-time answer would keep the cursor
    // parked on staging behind it for the rest of the run. Same test as the arm's:
    // past anchor 1 means the gather has nothing left to gather at.
    if (!st.transitGathered && projected > 1)
    {
        st.transitGathered = true;
        LOG_INFO("playerbots.dungeonclear",
                 "[DC:{}] BWL transit — the raid is already {} anchor(s) into the "
                 "gauntlet; abandoning the gather and crossing from here",
                 bot->GetName(), projected);
    }

    uint32 cursor = st.transitGathered ? projected : 0u;
    if (cursor > lastAnchor)
        cursor = lastAnchor;

    WaypointHint const& target = hints[cursor];

    // --- the snapshot ------------------------------------------------------
    float const packLeash = DcSettings::GetFloat(bot, "TransitPackLeash");

    // THE GATHER RADIUS HAS A FLOOR, and the floor is the ring the pack rung
    // actually parks the raid on. Both of these are runtime-tunable and were
    // tunable into direct contradiction: the rung walks a member to
    // `packLeash - TRANSIT_PACK_HOLD_MARGIN` of the cursor and then refuses to
    // move it again once it is within TRANSIT_PACK_ARRIVE_LEASH of that point, so
    // the raid comes to rest in a band ending at 23yd on the defaults — and the
    // gate then asked for 20. Nobody is ever inside it, so the gather can only end
    // by watchdog, which is precisely what tr-20260830-125018-2 did (2 of 25, 60s
    // burnt, tank crossed alone).
    //
    // Clamped here rather than in the settings registry because it is a
    // RELATIONSHIP between two settings, and the registry validates one row at a
    // time. A larger authored radius is still honoured; only an impossible one is
    // raised.
    float const gatherRadius =
        std::max(DcSettings::GetFloat(bot, "TransitGatherRadius"),
                 DcSuppressionTransit::GatherRadiusFloor(packLeash, TRANSIT_PACK_HOLD_MARGIN,
                                                         TRANSIT_PACK_ARRIVE_LEASH));
    BwlPackView const pack =
        BwlPack(bot, target.x, target.y, target.z, gatherRadius, packLeash);

    float const quorum = DcSettings::GetFloat(bot, "TransitGatherQuorum");

    DcSuppressionTransit::Inputs in;
    in.cursorIndex = cursor;
    in.anchorCount = anchorCount;
    in.distToCursor = bot->GetExactDist(target.x, target.y, target.z);
    in.arriveRadius = target.arriveRadius;
    in.gathered = st.transitGathered;
    in.quorumMet = pack.living <= 1 ||
                   static_cast<float>(pack.nearCursor) >= static_cast<float>(pack.living) * quorum;
    in.topped = pack.topped;
    in.trailDist = pack.trailDist;
    in.packLeash = packLeash;
    in.packOutside = pack.outsideLeash;
    in.packLiving = pack.followers;
    in.packQuorum = quorum;
    in.nearestEliteDist = BwlNearestElite(bot);
    in.eliteHoldRadius = DcSettings::GetFloat(bot, "TransitEliteHoldRadius");
    in.nearestArmedDeviceDist = BwlNearestArmedDevice(bot);
    in.disarmHoldRadius = DcSettings::GetFloat(bot, "TransitDisarmHoldRadius");
    in.nowMs = now;
    in.armedSinceMs = st.transitArmedMs;
    in.holdSinceMs = st.transitHoldSinceMs;
    in.holdReason = st.transitHoldReason;
    in.gatherTimeoutMs = DcSettings::GetUInt(bot, "TransitGatherTimeoutMs");
    in.packHoldTimeoutMs = TRANSIT_PACK_HOLD_TIMEOUT_MS;
    in.eliteHoldTimeoutMs = TRANSIT_ELITE_HOLD_TIMEOUT_MS;
    in.disarmHoldTimeoutMs = TRANSIT_DISARM_HOLD_TIMEOUT_MS;

    // Measured every tick and REPORTED ONLY. There is no hold behind this number
    // any more: the gate that used to read it was written on the theory that the
    // aggro is picked up on the approach and could be shed at the staging point,
    // and tr-20260830-152617-2 and -5 both falsified that — the raid reaches
    // staging with `towing 0` and 25 of 25 formed up on merit, and the count only
    // climbs at cursor 9, the foot of the Taskmaster ramp. The gate went with the
    // theory; the instrument that killed it stays.
    //
    // Affordable because it is arithmetic. Twenty-five members by up to forty
    // references is ~1000 distance compares and a hash insert each, with NO
    // pathfind anywhere. The driver runs on one bot.
    BwlRemoteAggro const remote = BwlRemoteHolders(bot);

    DcSuppressionTransit::Verdict const v = DcSuppressionTransit::Decide(in);

    BwlTransitLog(bot, st, in, v, pack, anchorCount, remote);

    // --- act ---------------------------------------------------------------
    if (v.openGatherGate)
    {
        st.transitGathered = true;
        LOG_INFO("playerbots.dungeonclear",
                 "[DC:{}] BWL transit — {} of {} raid members formed up at the staging "
                 "point{} — crossing",
                 bot->GetName(), pack.nearCursor, pack.living,
                 v.timedOut ? " (GATHER TIMEOUT — going with what we have)" : "");
    }

    if (v.complete)
    {
        // The standoff. Kill the glide first: the event sets StepsOwnMovement, so
        // nothing else cancels it, and deciding "arrived" while an escort spline is
        // still in flight carries the tank straight on past Broodlord (the Black
        // Morass lesson, measured on map 269).
        DcMovement::ResolveEscortConflict(bot);
        LOG_INFO("playerbots.dungeonclear",
                 "[DC:{}] BWL transit — the raid is at the Broodlord standoff "
                 "({} member(s) alive on the map)",
                 bot->GetName(), pack.living);
        st.ClearTransit();
        return ObjectiveArriveResult::Done;
    }

    // ONCE PER RELEASE, not once per tick. A watchdog release now LATCHES (the
    // verdict keeps reporting the hold with its original clock so the leg keeps
    // walking), so `v.timedOut` stays true for as long as the condition it gave up
    // on persists — which for a wedged straggler is minutes of identical warnings.
    bool const watchdogFired = v.timedOut && !v.openGatherGate;
    if (watchdogFired && !st.transitHoldTimedOut)
        LOG_WARN("playerbots.dungeonclear",
                 "[DC:{}] BWL transit — watchdog released a '{}' hold at anchor {} "
                 "(trail {:.1f}yd, elite {:.1f}yd, armed device {:.1f}yd, towing {}) "
                 "-> walking on",
                 bot->GetName(),
                 DcSuppressionTransit::HoldName(
                     static_cast<DcSuppressionTransit::Hold>(st.transitHoldReason)),
                 cursor, in.trailDist, in.nearestEliteDist, in.nearestArmedDeviceDist,
                 remote.valid ? std::to_string(remote.holders) : std::string("n/a"));


    st.transitHoldTimedOut = watchdogFired;
    st.transitHoldSinceMs = v.holdSinceMs;
    st.transitHoldReason = static_cast<uint8>(v.hold);

    // The cursor bump happens HERE, before publication, so the index and the
    // position it names can never disagree even for one tick — a follower that
    // read a stale pair would form up on the leg the leader has just finished.
    if (v.cursorAdvance && cursor < lastAnchor)
        ++cursor;
    WaypointHint const& aim = hints[cursor];

    // PUBLISH, on every branch that is not completion. The pack rung reads this
    // cross-bot and a hold is still a tick on which the raid should be forming up
    // on this anchor — publishing only when walking would release the pack exactly
    // when the column most needs to close.
    st.transitCursorIndex = cursor;
    st.transitCursorX = aim.x;
    st.transitCursorY = aim.y;
    st.transitCursorZ = aim.z;
    st.transitDrivingMs = now;

    if (!v.advance)
    {
        // HOLDING. Issue no movement and YIELD — a hold is exactly a tick on which
        // the raid should be fighting rather than walking, and the pack rung does
        // any walking the column still needs. Deliberately no StopBot: killing the
        // spline every tick of a two-minute elite fight is a stop packet every
        // tick, and the bot is being held by combat anyway.
        return ObjectiveArriveResult::Done;
    }

    // WALKING. Through LongRangePathfinder, because a bare MovePoint truncates
    // silently past ~30yd and a straggling leader's catch-up leg is longer than
    // the authored 24 (the Black Morass lesson again). DcTransit::TravelTo returns
    // false when the leader is already inside the arrive radius, which the kernel
    // has already told us it is not — so a false here means the route could not be
    // issued at all, and the honest answer is to yield rather than claim a tick we
    // did nothing with.
    if (!DcTransit::TravelTo(bot, botAI, aim.x, aim.y, aim.z, aim.arriveRadius))
        return ObjectiveArriveResult::Done;

    return ObjectiveArriveResult::Running;
}

// Ids 20 and 21. The Violet Hold's are 15-19; ids are one flat space across every
// dungeon, and AddHook LOG_ERRORs a collision rather than silently dropping one.
void RegisterBlackwingLairHooks(ObjectiveHookRegistry::HookTable& out)
{
    ObjectiveHookRegistry::AddHook(out, DcBlackwingLair::HOOK_RAZORGORE_ORB,
                                   &DriveRazorgoreOrb);
    ObjectiveHookRegistry::AddHook(out, DcBlackwingLair::HOOK_SUPPRESSION_TRANSIT,
                                   &DriveSuppressionTransit);
}
