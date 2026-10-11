/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "Ai/Dungeon/DungeonClear/Data/DungeonClearRouteRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"
#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonRosterBuilders.h"
#include "Ai/Dungeon/DungeonClear/Util/DcTargeting.h"

#include "InstanceScript.h"
#include "Player.h"
#include "SharedDefines.h"

// --- Pit of Saron (map 658) ------------------------------------------------
//
// The declarative half: the roster patch without which this dungeon cannot be
// finished, two event rows and the two authored legs. The imperative half is
// Overrides/PitOfSaronDriver.cpp (hooks 29-30); the arithmetic is the pure kernel
// in Util/DcPosGauntletDecision.h; the numbers all three agree on are namespace
// DcPitOfSaron in DungeonEventTables.h.
//
// READ THE ROSTER PATCH FIRST. Everything else here is downstream of it, and the
// bug it repairs is the quietest kind this module has: not a stall, not a wipe,
// but a run that REPORTS SUCCESS having skipped a third of the dungeon. The basis
// run (tr-20260907-100813-1) "cleared" Pit of Saron in 8m01s at 2/2 bosses. Map
// 658 has THREE. See RegisterPitOfSaronRoster below.
//
// WHAT IS AUTHORED, and why each piece exists:
//
//   A  the roster patch — Scourgelord Tyrannus, plus the one travel objective
//      areatrigger 5633 hangs off, plus two reorders to make room for it;
//   B  two event rows (this file) — the conditional gauntlet driver and the
//      anchored ledge gate;
//   C  two anchor routes (this file) — Krick -> the ledge, and the ledge ->
//      Tyrannus, so A* never re-derives the leg on a rebuild and the gauntlet
//      crossings carry NO_STOP;
//   D  the leader-side hooks (PitOfSaronDriver.cpp).
//
// AND WHAT IS DELIBERATELY *NOT* AUTHORED, so nobody re-derives it:
//
//   * NO DcHazardRegistry ROW FOR THE ICICLES, yet — and note the "for the
//     icicles". The map DOES carry hazard rows now: both Toxic Wastes (69024 from
//     Krick, 70274 from the Plagueborn Horrors) are DcGroundHazard rows, because
//     they are ordinary 4yd persistent-area-aura pools dropped under a random
//     party member and percent-of-max-health damage makes standing in one fatal at
//     any gear level. The icicles are a different question. Twenty-eight Invisible
//     Stalkers (36848) line the tunnel from (954, -117) to (1082, 28) and, while
//     progress is EXACTLY AFTER_TUNNEL_WARN, each casts 69424 every 16-24s to
//     summon a Collapsing Icicle (36847) that lands 69428 + 69426 two and a half
//     seconds later. That is the transient-AoE shape the registry already models —
//     but it is one crossing of one corridor and the emitters stop the instant
//     areatrigger 5633 raises progress to TYRANNUS_INTRO. A hazard row authored
//     blind steers a party away from ground that may be perfectly survivable, and
//     28 rows would be 28 wrong ones if it is. MEASURE FIRST: the question is
//     whether the healer copes, and it is answered by a run, not by this file.
//
//   * NO RoomAggroRegistry ROWS. Neither wave is a room-aggro problem. They are
//     scripted summons that arrive on splines at fixed homes, and the gauntlet
//     driver holds ground for them by name. Wave 1 in particular is a ~15-20s
//     SUMMON CASCADE and not an event: the two Deathbringers land at the top of
//     the ramp passive and unattackable, and the two 4-packs that are the actual
//     ambush are summoned only when each Deathbringer's spline ENDS. The driver
//     holds the party at the foot of the ramp until both packs are hostile —
//     WAVE1_ARMED_QUORUM in DungeonEventTables.h has the whole chain.
//
//   * NO DcNeverTargetRegistry ROW FOR GORKUN'S FALLEN WARRIORS. He summons a
//     36841 every 3 seconds, up to 3 alive, at (1060.95, 102.79, 630.2) for the
//     ENTIRE Tyrannus fight — DoAction(2), which stops the pump, only fires after
//     the boss is dead — and they brawl with the freed slaves 45-60yd behind the
//     boss anchor. It is a real watch item. It is NOT expressible here: that
//     registry is keyed (mapId, entry) with NO position scope, and 36841 is also
//     eight legitimate static spawns in the tunnel the clear has to kill. If a run
//     shows the clear walking backwards into that pack, the answer is a
//     position-scoped exclusion, not a blanket one. Measure before authoring.
//
//   * NO DcCombatPurgeRegistry ROW. Gorkun's pump looks like the population that
//     registry was built for, but it is a combat-BLIND global clock with no
//     reachability guard ([[dc-unreachable-combat-purge]]), and the adds are in
//     mutual combat with friendly NPCs the whole time.
//
//   * NO FightInPlaceRegistry / BossPullbackRegistry ROW FOR TYRANNUS, yet. His
//     leash is brutal (see the LEASH_* block in DungeonEventTables.h) but it is
//     checked against his VICTIM, and Forceful Smash is not a knockback — so the
//     only way to trip it is a straggler being the victim and then leaving. That
//     is a stranded-recovery problem before it is a fight-in-place one. Measure.
//
// AZEROTHCORE FACTS ENCODED HERE, so nobody "fixes" them:
//
//   * The second encounter's KILL CREDIT IS ICK (36476), not Krick (36477).
//     Krick rides Ick; instance_encounters rows 835/836 name 36476.
//   * The instance script NEVER sets DATA_TYRANNUS to IN_PROGRESS. Reset() sets
//     NOT_STARTED and JustDied sets DONE, and there is nothing in between — so
//     "!= NOT_STARTED" on this map means "dead", and any predicate that reads it
//     as "the fight has started" is wrong.
//   * The whole Krick outro is ~85-100s of RP DURING WHICH GATE 1 IS REFUSED.
//     The refusal is silent. See the ARM state in DcPosGauntletDecision.h.
//   * mod-playerbots already ships a `wotlk-pos` strategy for this map, and its
//     TyrannusAction spreads ranged 10yd and does NOTHING ELSE — no Overlord's
//     Brand handling at all. A branded healer that keeps healing heals the boss
//     for 5.5x and the fight becomes unwinnable. That is a mod-playerbots gap on
//     its own branch, NOT a DC one: DC has no seam that suppresses a bot's own
//     heals, and inventing one here would be the wrong layer.

namespace
{
    using namespace DcPitOfSaron;

    // --- event 1's gate: the Ymirjar gauntlet is ours ----------------------
    //
    // DUE for exactly the window between Ick's death and the tunnel warn, and the
    // four probes are ordered cheapest-first because this runs on every COMBAT
    // tick of the DC leader on map 658 — and from the moment gate 1 is accepted
    // the party is in essentially unbroken combat until wave 2 is down.
    //
    //   1. the map;
    //   2. both earlier bosses DONE. This is not redundant with the progress
    //      check: npc_pos_tyrannus_eventsAI::SetData bails on it BEFORE it looks
    //      at which id it was handed, so it is the orchestrator's own first gate
    //      and belongs in ours;
    //   3. the progress counter in [FINISHED_KRICK_SCENE, AFTER_WARN_2]. False
    //      before Ick dies, and false the moment the tunnel warn lands — so the
    //      driver owns exactly the three gates and the two waves, and nothing
    //      else on the map;
    //   4. Tyrannus not already down (a re-entered instance walks this corridor
    //      with nothing to do).
    //
    // THIS IS A STRONGER NEAR-GATE THAN A DISTANCE CHECK, which is what
    // t/TestEventRegistry.cpp's IsNearGatedConditionalWhitelisted row records.
    // DATA_INSTANCE_PROGRESS is MONOTONIC and is raised by exactly two things: the
    // death of Ick, and a player standing inside one of two areatrigger spheres
    // 34.5 and 38.1yd across. The counter cannot read 3 or 4 with the party
    // anywhere else on the map.
    //
    // NOT GATED ON COMBAT, for the Halls of Lightning reason: the ARM hold starts
    // the instant Ick dies, while the party is still out of combat, and a
    // predicate that waited for combat would arm the driver only after the clear
    // had already walked the party into the ambush trigger.
    bool PosGauntletDue(Player* bot, AiObjectContext* /*context*/)
    {
        if (!bot || bot->GetMapId() != MAP_ID)
            return false;

        InstanceScript* inst = DcTargeting::GetInstanceScript(bot);
        if (!inst)
            return false;

        if (inst->GetData(DATA_GARFROST) != DONE || inst->GetData(DATA_ICK) != DONE)
            return false;

        uint32 const progress = inst->GetData(DATA_INSTANCE_PROGRESS);
        if (progress < PROGRESS_FINISHED_KRICK_SCENE || progress > PROGRESS_AFTER_WARN_2)
            return false;

        return inst->GetData(DATA_TYRANNUS) != DONE;
    }
}

void RegisterPitOfSaronEvents(std::vector<DungeonEvent>& out)
{
    using namespace DcPitOfSaron;

    // (1) RUN THE YMIRJAR GAUNTLET — the conditional driver.
    //
    // ONE Custom step, for the Blackwing Lair / Halls of Lightning reason: what
    // this leg needs is a standing PREFERENCE re-decided every tick — hold for the
    // outro, or walk into a gate, or stand and fight a wave — not a sequence. A
    // step list can only say "do these in order and block on each", and every one
    // of the six states can be interrupted at any point by a wave that has not
    // finished spawning or a gate that is still being refused.
    //
    // DRIVES IN COMBAT — the load-bearing flag. The ordinary conditional rung
    // stands down on IsInCombat(), and from the tick gate 1 is accepted the party
    // is fighting ten Ymirjar, then six (twelve heroic) Wrathbone, with the gaps
    // between them measured in seconds. A rung that only ran between fights would
    // get its ticks before the first gate and essentially never again — and the
    // thing it exists to do (walk into the NEXT gate once the wave is dead) is
    // precisely what has to happen while the counter is still 3 or 4.
    //
    // STEPS OWN MOVEMENT — the driver delivers the leader to each gate on its own
    // long-range spline, and the at-objective hold runs BEFORE Drive: without
    // this, last tick's glide is cancelled before the hook can see it and the
    // party creeps one tick at a time while every log line reports a healthy
    // spline issue. It is also what makes a Done RETURN YIELD THE TICK, which is
    // the load-bearing half on this map: the driver is idle for most of both
    // waves, and those are exactly the ticks four bots and a tank need in order to
    // kill ten Ymirjar.
    //
    // OWNS THE PULL — zero advanced pulls across the gauntlet. The pull's Idle
    // branch answers unplanned aggro by walking a fresh camp BACK along the route
    // until it finds ground clear of hostiles, and on this leg "backward" is not
    // merely slow: it drags the party back through a gate sphere it has already
    // spent. That is harmless for the DRIVER's own forge (level-triggered, see the
    // kernel) but it is not harmless for the run — a camp dragged 80yd west of
    // gate 2 while wave 1 is still up means the wave is fought at the far end of a
    // corridor from the gate that ends it, and a camp dragged out of wave 2's
    // volume means the driver's own "is the wave dead" probe starts reading empty.
    //
    // REPEATABLE — the gauntlet is not a thing that completes once. The condition
    // going false (the tunnel warn lands, or Tyrannus dies) is the only "done",
    // and expressing completion that way rather than as a latch is what makes a
    // wipe-and-retry re-arm cleanly in whatever state the instance really is.
    //
    // PERSISTENT — the step list must not be rewound by the combat gaps, and on
    // this leg a "gap" is one Ymirjar dying.
    //
    // NOT Optional. The gauntlet cannot be skipped: without the three gates the
    // orchestrator never reaches AFTER_TUNNEL_WARN, at_tyrannus_event_starter
    // refuses, Gorkun is never summoned and Tyrannus is never attackable. A quiet
    // skip here costs the last boss; stalling names the problem for the human, who
    // can `dc skip` if they disagree.
    //
    // PanelAfterBoss(Ick), and NEVER PanelBeforeBoss(Tyrannus), which is what it
    // visually wants and must not have: panelGatesBossEntry also keys
    // DcTargeting::HasPendingSummonEvent, which reads an unlatched gating event as
    // "this boss must still be SUMMONED" and suppresses the dynamic pull within
    // 80yd of him — and a REPEATABLE event is never latched, so the suppression
    // would be permanent ([[dc-panelbeforeboss-repeatable-permanent-hold]]). Ick
    // is the anchor immediately before this leg, so the row lands in the same
    // place on the panel with none of that meaning.
    out.push_back(
        EventBuilder(MAP_ID, EVENT_GAUNTLET, "Run the Ymirjar gauntlet")
            .Conditional(&PosGauntletDue)
            .Repeatable()
            .Persistent()
            .OwnsThePull()
            .DrivesInCombat()
            .StepsOwnMovement()
            .PanelAfterBoss(NPC_ICK)
            .Custom(HOOK_POS_GAUNTLET)
                .Timeout(GAUNTLET_TIMEOUT_MS)
            .Build());

    // (2) TYRANNUS'S LEDGE — the anchored gate into the last encounter.
    //
    // A ONE-WAY DOOR, and this event's job is to make crossing it a DELIBERATE act
    // taken with the party formed up, rather than something the first bot down the
    // tunnel does on its own.
    //
    // Tripping areatrigger 5633 summons Gorkun/Martin at (1069.49, 88.99, 631.5)
    // and his CONSTRUCTOR immediately makes Tyrannus SetImmuneToPC(false),
    // REACT_AGGRESSIVE and AttackStart(nearest player within 100yd) — but Reset()
    // has already set UNIT_FLAG_NON_ATTACKABLE, so for the next ~38 seconds the
    // party is in a boss fight it cannot damage. And for every tick of those 38
    // seconds boss_tyrannusAI::UpdateAI checks that ITS VICTIM is within 100yd of
    // TSDistCheckPos: a straggler that is the nearest player and then walks back
    // down the tunnel FULL-HEALS the boss, evades him, despawns Gorkun AND
    // Rimefang, and the encounter can only be re-entered after Rimefang respawns.
    // That is what the gather in hook 30 is for.
    //
    // STEPS OWN MOVEMENT because the hook owns the walk-in. The objective anchor
    // is deliberately OUTSIDE the trigger sphere — 73yd from its centre, 22yd
    // clear — so that arriving does not fire it; the hook gathers there and then
    // walks the leader the ~30yd in. Without the flag the per-tick hold cancels
    // that glide the tick after it is issued.
    //
    // PERSISTENT because the hold spans a gather, a walk and a scripted summon
    // during which Gorkun's own Fallen Warrior pump starts putting bots in and out
    // of combat — any of which would rewind a non-persistent step list to step 0
    // and re-walk the party to the ledge it is already standing past.
    //
    // NO OwnsThePull() HERE, and it is worth saying why rather than leaving the
    // absence to look like an oversight. The flag is CONDITIONAL-ONLY: the anchored
    // path infers the stand-down from Persistent() alone
    // (IsPersistentAnchoredEventActive), and FindDueConditionalEvent — the only
    // thing that reads ownsThePull — never sees an anchored row. Setting it here
    // would be a silent no-op, which is exactly what TestEventRegistry's
    // PullOwningEventsAreVetted turns red.
    //
    // AND NO DrivesInCombat, which is a real limitation and not an oversight
    // either. An anchored event is driven off DcRel::AtObjective (30), a
    // non-combat rung; there is no anchored combat rung to set the flag on.
    // The consequence is that the hook stops being ticked the moment the trigger
    // fires and the party is pulled into the intro — which is why the hook hands
    // the encounter over at exactly that point (see PosEnterTyrannusLedge) rather
    // than trying to hold through 38 seconds of combat it would not be called
    // during.
    out.push_back(
        EventBuilder(MAP_ID, EVENT_TYRANNUS_LEDGE, "Tyrannus's ledge")
            .Anchored(ORDER_LEDGE)
            .Persistent()
            .StepsOwnMovement()
            .Custom(HOOK_POS_TYRANNUS_LEDGE)
                .Timeout(LEDGE_TIMEOUT_MS)
            .Build());
}

// --- the roster: the boss that reports success by not existing -------------
//
// ONE MakeBossWithBit row is the single highest-value change on this map, and the
// failure it repairs is worth understanding exactly.
//
// BossSpawnIndex::Build is a two-step join. Step 1 walks DungeonEncounter.dbc x
// instance_encounters keeping ENCOUNTER_CREDIT_KILL_CREATURE rows and builds
// creditEntry -> {encounterIndex, name}. Step 2 walks the `creature` SPAWN table
// and emits a DungeonBossInfo for every spawn whose entry matches — and the
// boss's world position comes from that spawn row.
//
//     instance_encounters: (837, 0, 36658, 0)  and  (838, 0, 36658, 0)
//     creature:            <nothing, on any map>
//
// Scourgelord Tyrannus has NO SPAWN ROW ANYWHERE. He is created at runtime as
// Rimefang's VEHICLE ACCESSORY (vehicle_template_accessory 36661 -> 36658), so
// step 2 matches nothing, no row is emitted, and the derived roster is two bosses
// for a three-boss dungeon.
//
// THE OBSERVABLE SYMPTOM IS NOT A STALL. encounterMask 0x3 against a two-row
// roster reads "all bosses cleared", so the run does not fail — it SUCCEEDS,
// reports 2/2, and stops at the top of the Krick outro with a third of the
// dungeon unwalked. tr-20260907-100813-1 is that run, in 8m01s.
//
// THIS IS THE GUNDRAK SHAPE, not the Utgarde Pinnacle one. Svala's credit entry
// is a runtime UpdateEntry TARGET (the creature spawns under a different entry and
// transforms); Tyrannus never has a spawn row under any entry at all. The
// distinguishing test either way is simply whether `creature` contains the credit
// entry, and MakeBossWithBit is the escape hatch for both.
//
// THE COMPLETION BIT IS SAFE. Bit 2 is a plain ENCOUNTER_CREDIT_KILL_CREATURE row
// on both difficulties (DungeonEncounter.dbc 837 and 838, encounterIndex 2 on
// each), so KillRewarder -> Map::UpdateEncounterState sets it from
// _victim->GetEntry() at death time exactly as it does for the other two. Only
// the DERIVATION failed; the credit path is untouched.
//
// AND THE ANCHOR IS HIS EXIT POSITION, NOT HIS SPAWN. See the TYRANNUS_* block in
// DungeonEventTables.h: he rides Rimefang 14.7yd in the air until DoAction(1)
// MoveJump()s him down, and NavmeshSnap's vertical extent is a FIXED 10, so an
// anchor at the riding position would fail to snap and the row would be DROPPED AT
// LOAD — the same silent short roster, one layer down.
void RegisterPitOfSaronRoster(std::vector<BossRosterPatch>& t)
{
    using namespace DcRoster;
    using namespace DcPitOfSaron;

    // ONE Any-gated patch serves both difficulties: the three DBC bits are 0/1/2
    // on normal AND heroic with no shift, unlike Gundrak and the Nexus whose
    // heroic-only additions needed their own gated patch. Heroic changes the
    // gauntlet (wave 2 goes from six mobs to twelve and adds a second spawn group
    // 68yd further north) but not the roster.
    BossRosterPatch p;
    p.mapId = MAP_ID;

    p.add = {
        // The boss the derivation drops.
        MakeBossWithBit(NPC_TYRANNUS, MAP_ID, "Scourgelord Tyrannus",
                        TYRANNUS_X, TYRANNUS_Y, TYRANNUS_Z,
                        /*encounterIndex*/ BIT_TYRANNUS,
                        /*orderOverride*/ ORDER_TYRANNUS),

        // The ledge. An objective's encounterIndex is an ordering hint only — it
        // carries no kill-bit and NextDungeonBossValue never tests the completion
        // mask for one — so it stays 0 and the clear orders by orderOverride.
        //
        // It exists because the thing it anchors cannot be reached by walking to a
        // boss: areatrigger 5633 is a 51.5yd sphere with no creature in it, and the
        // boss it starts is fourteen yards in the air and non-attackable until it
        // has been tripped. The anchor is deliberately 22yd OUTSIDE that sphere,
        // so ARRIVING here is not the same act as ENTERING the encounter.
        MakeObjective(OBJ(1), /*encounterIndex*/ 0, MAP_ID, "Tyrannus's ledge",
                      LEDGE_X, LEDGE_Y, LEDGE_Z,
                      LEDGE_ARRIVE, /*gateEntry*/ 0, /*hook*/ 0,
                      /*eventId*/ EVENT_TYRANNUS_LEDGE,
                      /*orderOverride*/ ORDER_LEDGE),
    };

    // The two derived bosses onto the same 1..4 scale. Their relative order is
    // unchanged from what their DBC bits already gave them; the reorder exists
    // only to make an integer slot for the objective between Ick and Tyrannus.
    p.reorder = {
        { NPC_GARFROST, ORDER_GARFROST },
        { NPC_ICK,      ORDER_KRICK    },
    };

    t.push_back(std::move(p));
}

// --- the two designed legs, in walking order -------------------------------
//
// DERIVED, NOT DRAWN. Every anchor below is a point on the corridor
// LongRangePathfinder itself returns for that leg against the live map-658
// mmtiles, decimated to ~16yd — the [[dc-navharness-prints-the-route]] method, and
// the reason t/TestPitOfSaronRouteProbe prints the polylines it routed: an mmaps
// regen that moves a corridor is re-authored the same way rather than by hand.
//
// WHY THEY EXIST. Not because the mesh defeats the pathfinder — both legs route
// cleanly — but for the Blackwing Lair / Halls of Lightning reason plus one this
// map has on its own:
//
//   * THE GAUNTLET NEEDS NO_STOP. The advanced pull's Idle branch answers
//     unplanned aggro by stamping a fresh camp BEHIND the tank and dragging it
//     there, and on this leg "behind" means back through a gate sphere and out of
//     the volume the driver measures wave 2 in. The event row carries
//     OwnsThePull for the window it is due; the NO_STOP flags carry the same
//     intent for the ticks it is not.
//   * THE ICICLE TUNNEL IS NOT GROUND TO CAMP ON. While progress is exactly
//     AFTER_TUNNEL_WARN, twenty-eight emitters drop AoE along its whole length on
//     a 16-24s cycle. It is crossed once; it is not held.
//
// Each row is keyed on the entry of its DESTINATION and starts where the party
// will be standing when that leg begins, because DungeonPathFollower::SeedCursor
// projects the bot onto the row from its own position and a row that starts
// somewhere else snaps the cursor to the far end
// ([[dc-anchor-route-must-cover-where-the-party-stands]]).
void RegisterPitOfSaronRoute()
{
    using namespace DcRoster;
    using namespace DcPitOfSaron;

    // --- Leg A: Krick's arena to Tyrannus's ledge -------------------------
    //
    // 743yd, 54 anchors, and the whole gauntlet plus the icicle tunnel in one row
    // — from the ground the party is standing on when Ick dies to the ledge it
    // forms up on before the last encounter.
    //
    // IT ROUTES THROUGH THE THREE GATE STAND POINTS ON PURPOSE, and that costs
    // about 60yd of detour it would be a mistake to optimise away. The corridor
    // A* actually prefers between Krick and the ledge climbs the ramp at y ~ 76
    // and never comes closer than 33.2yd to areatrigger 5578's centre — 1.3yd
    // inside a 34.54yd sphere, which is less margin than the arrival leash. A
    // party walking THAT line would step in and out of the trigger with no way to
    // be sure it was ever inside it. Anchor 10 sits 3.6yd from the centre.
    //
    // NO_STOP RUNS 10..48, and the flag covers the leg LEAVING an anchor, so that
    // is the gate-1 crossing through to the far end of the icicle tunnel. Two
    // reasons, one after the other along the same row:
    //
    //   * ACROSS THE GAUNTLET (10..31) the pull's Idle branch would drag a camp
    //     BACKWARD out of the 80yd volume the driver measures wave 2 in — at
    //     which point "is the wave dead" starts reading empty with mobs still up
    //     — and back through a gate sphere the run has already spent.
    //   * ACROSS THE TUNNEL (31..48) twenty-eight emitters drop AoE along the
    //     whole length on a 16-24s cycle while progress is exactly
    //     AFTER_TUNNEL_WARN. The static trash in there is a 3600s respawn and IS
    //     worth killing — NO_STOP is not a combat suppression, only a planning
    //     one, so the party still fights what reaches it — but no camp on that
    //     ground is ground the party can hold.
    //
    // It RELEASES at anchor 48, which is the last one carrying the flag inside the
    // emitter field (the furthest emitter stands at (1071, 71, 631); anchor 48 is
    // at (1068.5, 45.2, 631.4) and anchor 49, the first unflagged one, at
    // (1066.9, 61.2, 632.6)). The approach to the ledge is ordinary ground.
    //
    // THE STEEPEST STEPS ARE REAL RAMP, not a shortcut. 16 -> 18 climbs 11.6yd in
    // 11.9yd of ground and 32 -> 33 climbs 8.7yd in 12.0; both are straight in xy
    // and follow the corridor vertex for vertex. Do not "smooth" them — the whole
    // leg climbs 125yd and the mesh does it in ramps. What they must NOT do is
    // span a CREST, which is a different failure and the reason for the three
    // anchors marked "crest" below — see CREST ANCHORS.
    //
    // CREST ANCHORS (12, 17) — THE ESCORT SPLINE IS LINEAR, SO A CHORD OVER A
    // CONVEX RISE PUTS THE BOT INSIDE THE HILL.
    //
    // An anchored route is not pathfound. StridedPathfinder's anchor fast-path
    // snaps each anchor to the mesh and returns, one polyline point per segment,
    // before the primary A* producer ever runs — and DcActionShared then flies the
    // window as a single MoveSplinePath, "linear, wall-safe, no per-point stops".
    // So between two anchors the bot travels a straight line in three dimensions.
    // Nothing consults DcRouteFilter / DcNavPenaltyRegistry on this leg; a fence
    // row cannot bend it, because no search happens for a fence to bias.
    //
    // That is fine on a concave or straight run — the chord rides above the floor
    // and the bot walks it. Over a CONVEX rise it is not: the chord cuts the crest
    // and the bot is dragged through the rock. Measured against the map's own
    // mmtiles, with each anchor at the Z THE RUNTIME SNAPS IT TO (which is what
    // flies, and is up to 2.6yd below the authored Z here), three segments of this
    // leg did that:
    //
    //     the shoulder at (848.3, 79.3)   3.14yd under the surface
    //    11 -> 13 (old 7 -> 8)            1.27yd under at (874.8, 53.8)
    //    16 -> 18 (old 11 -> 12)          1.88yd under at (925.8, 66.7)
    //
    // THE FIRST NO LONGER HAS AN ANCHOR, because the leg that cut it is gone: the
    // ARM hold moved south onto the flat plain (see STAGE_* in DungeonEventTables)
    // and the party no longer traverses that shoulder at all. It is listed here so
    // that anyone re-deriving this route knows the ground is bad and does not
    // route back over it.
    //
    // Each fix is ONE anchor placed ON THE MESH at the crest the chord was cutting,
    // which drops the worst penetration to 0.05 / 0.28yd. They are inserted, never
    // substituted: every original anchor keeps its x/y, so the corridor this leg
    // was decimated from is unchanged and only the sag between points is gone.
    // Both carry NO_STOP because they fall inside the 10..48 span.
    //
    // With the southern hold in place the worst penetration anywhere between Ick
    // and gate 1 is 0.09yd, against 3.14yd over the old shoulder line.
    //
    // Authored Z is left as it was on the original anchors even where it is up to
    // 2.6yd off the mesh, because the runtime snap makes it cosmetic — but note
    // that any future measurement of this route has to snap first or it will read
    // the sag 1-3yd shallower than it really is.
    DungeonClearRouteRegistry::Register(
        MAP_ID, DUNGEON_DIFFICULTY_NORMAL, OBJ(1),
        {
            // --- Krick's arena, and the walk out of it ------------------------
            { ICK_X,     ICK_Y,    ICK_Z    },   //  0  where Ick dies
            {  846.00f,  111.00f, 510.10f },     //  1
            {  841.62f,   99.87f, 510.89f },     //  2
            // --- west onto the flat southern plain, then straight down it -----
            // x830 is dead flat (z 510.2..510.5) from y92 to y44 and threads the
            // obstacle field at y79..99 — the raised rock at (833..839, 87..93)
            // is east of the lane, the meshed blocks at x850/854/856 further east
            // again. This is the ground that replaces the old shoulder traverse.
            {  830.00f,   92.00f, 510.22f },     //  3
            {  830.00f,   76.00f, 510.42f },     //  4
            {  830.00f,   60.00f, 510.50f },     //  5
            {  830.00f,   44.00f, 510.30f },     //  6
            {  833.00f,   32.00f, 510.36f },     //  7
            // --- the ARM hold: where the ~85-100s Krick outro is waited out ---
            { STAGE_X,   STAGE_Y,  STAGE_Z  },   //  8  the ARM hold
            // --- north-east back up to gate 1, lined up with the ramp ---------
            {  848.20f,   32.50f, 511.42f },     //  9
            // --- GATE 1: dead centre of areatrigger 5578 ---------------------
            { GATE_1_X,  GATE_1_Y, GATE_1_Z, /*doorGoEntry*/ 0,
              ToFlag(AnchorFlag::NO_STOP) },     // 10
            // --- the ambush ramp: wave 1 arrives across these anchors --------
            {  871.83f,   52.31f, 523.03f, 0, ToFlag(AnchorFlag::NO_STOP) },  // 11
            {  874.76f,   53.73f, 523.98f, 0, ToFlag(AnchorFlag::NO_STOP) },  // 12  crest
            {  882.64f,   57.54f, 530.37f, 0, ToFlag(AnchorFlag::NO_STOP) },  // 13
            {  893.44f,   62.76f, 537.77f, 0, ToFlag(AnchorFlag::NO_STOP) },  // 14
            {  904.98f,   65.45f, 545.44f, 0, ToFlag(AnchorFlag::NO_STOP) },  // 15
            {  916.90f,   66.88f, 554.18f, 0, ToFlag(AnchorFlag::NO_STOP) },  // 16
            {  925.78f,   66.67f, 562.74f, 0, ToFlag(AnchorFlag::NO_STOP) },  // 17  crest
            {  928.83f,   66.60f, 565.79f, 0, ToFlag(AnchorFlag::NO_STOP) },  // 18
            {  940.67f,   64.61f, 567.51f, 0, ToFlag(AnchorFlag::NO_STOP) },  // 19
            // --- GATE 2: inside areatrigger 5579, 16.2yd from its centre -----
            { GATE_2_X,  GATE_2_Y, GATE_2_Z, 0, ToFlag(AnchorFlag::NO_STOP) },  // 20
            // --- north-west down the long shelf to the wave-2 ground ---------
            {  947.29f,   47.28f, 569.48f, 0, ToFlag(AnchorFlag::NO_STOP) },  // 21
            {  945.29f,   31.41f, 573.17f, 0, ToFlag(AnchorFlag::NO_STOP) },  // 22
            {  943.30f,   15.53f, 575.75f, 0, ToFlag(AnchorFlag::NO_STOP) },  // 23
            {  941.30f,   -0.34f, 579.57f, 0, ToFlag(AnchorFlag::NO_STOP) },  // 24
            {  939.81f,  -12.25f, 584.37f, 0, ToFlag(AnchorFlag::NO_STOP) },  // 25
            {  938.35f,  -28.15f, 592.05f, 0, ToFlag(AnchorFlag::NO_STOP) },  // 26  <- wave 2
            {  938.49f,  -44.15f, 592.86f, 0, ToFlag(AnchorFlag::NO_STOP) },  // 27  <- wave 2
            {  938.63f,  -60.15f, 592.87f, 0, ToFlag(AnchorFlag::NO_STOP) },  // 28  <- wave 2
            // Anchors 28-29 pass the Ice Wall (GO 201885 at (932.3, -80.7,
            // 591.7)) at 13.2yd — outside DungeonClearBlockingDoorValue's 12yd
            // same-floor band, and it is open by now regardless. NOT flagged
            // DOOR_AHEAD: the instance script opens it the moment Garfrost and Ick
            // are both DONE, which is a precondition of this whole leg.
            {  943.70f,  -75.05f, 593.08f, 0, ToFlag(AnchorFlag::NO_STOP) },  // 29
            {  950.41f,  -89.57f, 595.89f, 0, ToFlag(AnchorFlag::NO_STOP) },  // 30
            {  958.05f, -103.59f, 595.82f, 0, ToFlag(AnchorFlag::NO_STOP) },  // 31
            // --- GATE 3: inside areatrigger 5580, 6.1yd from its centre ------
            // The tightest of the three spheres (20.51yd) and the one where the
            // stand point's margin matters most.
            { GATE_3_X,  GATE_3_Y, GATE_3_Z, 0, ToFlag(AnchorFlag::NO_STOP) },  // 32
            // --- the icicle tunnel: 28 emitters, 16-24s each, one crossing ----
            {  980.99f, -113.61f, 600.40f, 0, ToFlag(AnchorFlag::NO_STOP) },  // 33
            {  989.74f, -121.81f, 609.11f, 0, ToFlag(AnchorFlag::NO_STOP) },  // 34
            { 1000.31f, -127.14f, 616.53f, 0, ToFlag(AnchorFlag::NO_STOP) },  // 35
            { 1011.68f, -126.07f, 622.22f, 0, ToFlag(AnchorFlag::NO_STOP) },  // 36
            { 1025.06f, -118.08f, 626.15f, 0, ToFlag(AnchorFlag::NO_STOP) },  // 37
            { 1036.33f, -106.73f, 629.12f, 0, ToFlag(AnchorFlag::NO_STOP) },  // 38
            { 1042.45f,  -92.23f, 632.59f, 0, ToFlag(AnchorFlag::NO_STOP) },  // 39
            { 1047.10f,  -76.92f, 633.29f, 0, ToFlag(AnchorFlag::NO_STOP) },  // 40
            { 1053.78f,  -62.56f, 634.06f, 0, ToFlag(AnchorFlag::NO_STOP) },  // 41
            { 1062.48f,  -49.13f, 634.58f, 0, ToFlag(AnchorFlag::NO_STOP) },  // 42
            { 1067.50f,  -34.44f, 634.53f, 0, ToFlag(AnchorFlag::NO_STOP) },  // 43
            { 1068.84f,  -18.50f, 634.13f, 0, ToFlag(AnchorFlag::NO_STOP) },  // 44
            { 1070.18f,   -2.55f, 635.31f, 0, ToFlag(AnchorFlag::NO_STOP) },  // 45
            { 1071.52f,   13.39f, 635.43f, 0, ToFlag(AnchorFlag::NO_STOP) },  // 46
            { 1069.99f,   29.32f, 631.71f, 0, ToFlag(AnchorFlag::NO_STOP) },  // 47
            { 1068.46f,   45.24f, 631.35f, 0, ToFlag(AnchorFlag::NO_STOP) },  // 48  last emitter
            // --- out of the tunnel and round to the ledge --------------------
            { 1066.94f,   61.17f, 632.56f },     // 49
            { 1063.35f,   76.72f, 632.35f },     // 50
            { 1058.81f,   92.06f, 631.52f },     // 51
            { 1054.00f,  107.32f, 629.34f },     // 52
            { LEDGE_X,   LEDGE_Y, LEDGE_Z },     // 53  the gather point
        });

    // --- Leg B: the ledge to Tyrannus -------------------------------------
    //
    // 49yd of flat arena floor, and the only leg on this map where the ORDER of
    // the anchors carries meaning beyond the walk. Anchor 2 IS the driver's arena
    // stand point — the place hook 30 walks the leader to in order to be inside
    // areatrigger 5633's 51.5yd sphere — so the ordinary clear and the hook take
    // the same line into the encounter.
    //
    // Every anchor is within 40yd of TSDistCheckPos and dead level with it, which
    // is the boss's own leash box (100yd / +/-20yd, checked against his VICTIM
    // every tick). Nothing on this leg can evade him.
    DungeonClearRouteRegistry::Register(
        MAP_ID, DUNGEON_DIFFICULTY_NORMAL, NPC_TYRANNUS,
        {
            { LEDGE_X,    LEDGE_Y,    LEDGE_Z    },  // 0  the gather point
            { 1039.39f,   131.97f,   628.70f     },  // 1
            { ARENA_X,    ARENA_Y,    ARENA_Z    },  // 2  inside areatrigger 5633
            { TYRANNUS_X, TYRANNUS_Y, TYRANNUS_Z },  // 3  his exitPos / home
        });
}
