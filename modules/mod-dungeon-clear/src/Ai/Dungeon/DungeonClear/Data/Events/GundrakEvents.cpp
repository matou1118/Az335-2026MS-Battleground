/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"
#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonRosterBuilders.h"

#include "Creature.h"
#include "Player.h"
#include "Playerbots.h"
#include "SharedDefines.h"
#include "UnitDefines.h"

// --- Gundrak (map 604) ----------------------------------------------------
//
// Gundrak could not be cleared at all before this file, and it could not be
// cleared by pathfinding alone even with the obvious content authored. FOUR
// separate run-ending defects stack, and each one is only observable once the
// one before it is fixed:
//
//   1. TWO OF THE FIVE BOSSES NEVER ENTER THE DERIVED ROSTER. The Drakkari
//      Colossus and Eck are credited to entries (29573, 29932) that have no
//      creature spawn, so BossSpawnIndex never visits them. The missing Colossus
//      is the worse of the two: he never gets an anchor, so the party never walks
//      to his wing, so he never dies, so HIS ALTAR IS NEVER CLICKABLE and the
//      bridge to the last boss never forms. The run hard-stalled at 2/4.
//   2. THE COLOSSUS CANNOT BE ATTACKED. He spawns NON_ATTACKABLE with
//      MoveInLineOfSight stubbed out; the only thing that starts the fight is a
//      player hitting one of five inert summoned Living Mojos ringing him.
//   3. THREE ALTAR CLICKS GATE THE LAST BOSS, and each altar is NOT_SELECTABLE
//      until its own boss is dead.
//   4. GAL'DARAH'S ENTIRE WING IS A DISCONNECTED NAVMESH COMPONENT. The bridge is
//      a DB-spawned GameObject, so it is not in the mmaps and never will be.
//      Clicking all three altars produces a bridge the bots still cannot walk on.
//
// 1-3 are ordinary authoring (a roster patch and five events). 4 needs the
// sanctioned blunt instrument, a TeleportParty gated on the bridge having
// actually formed.
//
// --- the progression spine -----------------------------------------------
//
//   enter (1891.8, 832.2, 176.7)
//     +-- Slad'ran  29304          -> altar 192518 clickable -> CLICK
//     +-- Colossus  29307          -> altar 192520 clickable -> CLICK
//     +-- Moorabi   29305          -> altar 192519 clickable -> CLICK
//     |                              3/3 statues READY -> +5s -> BRIDGE
//     +-- [HEROIC] Eck door opens on Moorabi -> kill the dweller FORMATION -> Eck
//     +-- CROSS THE GAP (teleport — the bridge is not navmesh)
//     +-- Gal'darah 29306 (his room SEALS during the fight)
//
// The server-side chain, from instance_gundrak.cpp and the altars' SmartAI:
// SetBossState(DONE) clears GO_FLAG_NOT_SELECTABLE on that boss's altar; a click
// fires SMART_EVENT_GOSSIP_HELLO (filter 0, so a plain GameObject::Use reaches
// it) -> a timed actionlist -> SMART_ACTION_SET_INST_DATA -> SetData flips that
// altar's STATUE to GO_STATE_READY; when all three are READY a 5s timer fires and
// Update() drives the four statues AND the collision block to
// GO_STATE_ACTIVE_ALTERNATIVE. That is the bridge dropping.
//
// --- map 604 is an 88-201 YARD TRAPDOOR ----------------------------------
//
// Every one of map 604's 25 .map tiles is MAP_HEIGHT_NO_HEIGHT with
// gridHeight = 0.0, so a missed VMAP raycast makes Map::GetHeight return 0.0 as
// real ground. Its walkable surfaces run z 75 to z 201. This is the Azjol-Nerub
// sink mechanism on a map that is about to be run, and it is why EVERY coordinate
// in this file is the COLUMN-PROBED navmesh floor rather than the GO's or
// creature's own Z, and why the probe asks for the surface NEAREST the intended Z
// rather than the highest in the column (at the hub centre the highest surface is
// an unreachable 195.55 roof scrap, 77yd above the bridge deck).
//
// Two altars detonate that trap directly: their own columns have NO walkable
// surface at all. See the altar section below.
//
// --- what is NOT here, and why -------------------------------------------
//
//  * No DcNeverTargetRegistry row. The obvious candidate, Slad'ran Summon Target
//    (29682), already carries unit_flags NOT_SELECTABLE and flags_extra
//    TRIGGER|CIVILIAN, so IsPossibleTarget rejects it without help.
//  * No FightInPlace / ScriptedPull / BossPullback rows: no Gundrak boss
//    overrides CheckInRoom, and the Colossus is a scripted START, not a scripted
//    pull sequence. There IS now a RoomAggroRegistry row for Slad'ran (29304,
//    radius 75) — this bullet used to claim no Gundrak boss had a room-aggro
//    pre-clear problem, and live runs disproved it: his summon targets sit 29-73yd
//    south, inside the snake trash, so both add waves spawn in the packs and
//    Poison Nova shoves the party outward into them. See RoomAggroRegistry.cpp for
//    the measured room and why the member whitelist is load-bearing.
//  * No DcNavPenaltyRegistry fence. A fence taxes the router away from mesh it
//    should not use; Gundrak's problem is mesh that is MISSING, which no fence can
//    add. The voids are avoided by not authoring anchors in them.
//  * No playerbots change for the live `wotlk-gd` strategy, which zeroes every
//    DcMovementAction for a 3.5s window in Slad'ran's fight and a 6s window in
//    Gal'darah's. Both are gated on FindTargetValue, which walks the bot's own
//    threat list rather than a proximity scan, so neither can touch the APPROACH
//    to a boss room — this is a fight-quality question, not a navigation blocker.
//    Measure it in a run before touching a file in another module.
//  * No DcEventDoorRegistry row. All six Gundrak doors are instance-script
//    GO-state territory and none should ever be bot-opened. Whether any of them
//    (or the collision block sitting on the island) reads as a corridor blocker is
//    a question for a real run; an unnecessary row here is as much a bug as a
//    missing one.

// One file-scope using-directive, the VioletHoldEvents.cpp idiom: every number
// below is a DcGundrak constant, and qualifying each one would bury the shape of
// the events under the prefix. (A using-directive inside the anonymous namespace
// would only be in effect inside it — the appenders below are at file scope.)
using namespace DcGundrak;

namespace
{
    bool GdColossusFrozen(Player* bot, AiObjectContext* context);
}

// --- the Colossus activation predicate (event 2) --------------------------
//
// DUE for exactly the window the mojo pull is needed and no longer: a live
// Colossus within COLOSSUS_SCAN of the bot that is still carrying
// UNIT_FLAG_NON_ATTACKABLE.
//
// That flag is the encounter's own phase bit and it is unambiguous in every
// direction, which is what makes a Conditional event the right shape here rather
// than an anchored objective:
//
//   * Reset() SETS it on a fresh spawn, together with the five mojo summons — so
//     the predicate is true exactly when there is a mojo to pull.
//   * The fight CLEARS it, so the predicate goes false the moment the boss is
//     attackable and the ordinary at-boss engage takes over.
//   * After an EVADE, Reset() takes its IsInEvadeMode() branch, which REMOVES the
//     flag and summons no mojos at all (BossAI::_Reset() has already despawned
//     them). So on every retry the predicate reads false and the event simply
//     never fires — which is exactly right, because post-evade the boss is
//     directly pullable and there is nothing to hit. An anchored Optional step
//     would have to burn a timeout to discover the same thing.
//
// Note the flag is NON_ATTACKABLE specifically, not NOT_SELECTABLE: the boss sets
// NOT_SELECTABLE on itself at 51% and 2% health while the Drakkari Elemental is
// out, and reading that one would re-fire the event in the middle of the fight.
//
// The proximity term is what keeps this near-gated for
// ConditionalEventsWithoutArrivalStepAreProximityVetted: the event's lone step is
// a KillCreatureEngage, which is not an arrival step, so without a distance term
// the event could latch complete with the tank anywhere on the map.
// FindNearestCreature is a grid scan FROM THE BOT, so 40yd means "the leader is in
// the Colossus's arena". A wider scan would also be wrong on the merits — it
// would fire while the party was still fighting the west corridor's trash and
// send the tank at a mojo early.
namespace
{
    bool GdColossusFrozen(Player* bot, AiObjectContext* /*context*/)
    {
        Creature* colossus = bot->FindNearestCreature(COLOSSUS, COLOSSUS_SCAN, /*alive*/ true);
        if (!colossus)
            return false;
        return colossus->HasUnitFlag(UNIT_FLAG_NON_ATTACKABLE);
    }
}

void RegisterGundrakEvents(std::vector<DungeonEvent>& out)
{
    // --- (1) (3) (4) the three altar clicks -------------------------------
    //
    // PLAIN UseGO, NOT .ReportUse(). The altars are gameobject_template type 1
    // buttons whose smart_scripts row is SMART_EVENT_GOSSIP_HELLO (64) with
    // event_param1 (the reportUse filter) = 0, i.e. "always execute". A plain
    // GameObject::Use() reaches it through AI()->GossipHello(player, false). This
    // is the Nexus Containment Sphere case, not the BWL Chromaggus-lever case.
    //
    // A VERIFICATION HOLD after each click, per the Ahn'kahet idiom. The
    // UseGameObject step reports Done the instant it calls Use() — it never checks
    // that anything happened. Ahn'kahet follows each device click with
    // MoveToHoldUntilPersistentData on the index the click sets; that variant is
    // unavailable here, because instance_gundrak overrides NEITHER GetData nor the
    // persistent-data store. But Gundrak has something better: the click's
    // observable effect is a GAMEOBJECT STATE CHANGE, so WaitForGOState verifies
    // it directly.
    //
    //     event 1  altar 192518  ->  statue 192564 (Snake)   reaches GO_STATE_READY
    //     event 3  altar 192520  ->  statue 192567 (Troll)   reaches GO_STATE_READY
    //     event 4  altar 192519  ->  statue 192565 (Mammoth) reaches GO_STATE_READY
    //
    // Mind the INITIAL states, which are what make that a true edge: 192564 /
    // 192565 / 192567 all spawn at GO_STATE_ACTIVE (0) and are only ever driven to
    // READY (1) by their own altar's SetData. (192566, the Rhino statue, is the odd
    // one out — it spawns at READY and is only ever driven to ACTIVE_ALTERNATIVE.
    // It is the BRIDGE witness in event 6, not an altar witness.)
    //
    // STATUE_SEARCH MUST BE LARGE. All four statues stand together at
    // (1775.16, 743.46, 119.07), which is 69yd from the Slad'ran altar anchor,
    // 89yd from the Colossus's and 62yd from Moorabi's — and WaitForGOState scans
    // FROM THE BOT with DC_EVENT_GO_SEARCH by default, which does not reach. 120yd
    // does, and cannot find the wrong statue: the four carry distinct entries.
    //
    // REQUIRED, and .Persistent(). Required because these gate the LAST BOSS and a
    // stall the human can act on is the correct failure — a `dc skip`ped boss
    // leaves its altar NOT_SELECTABLE forever, and the executor surfaces that
    // correctly by HOLDING on a not-selectable GO rather than latching a click
    // that never happened. Persistent because these are multi-step events: without
    // it a >1s Drive gap rewinds to step 0 and re-clicks a spent altar.
    //
    // TIMEOUT 60s, matching Nexus. The step SHOULD hold on an altar still flagged
    // NOT_SELECTABLE, and the 30s default would read a just-landed SetBossState as
    // a stall.
    //
    // --- THE TWO ALTARS THAT STAND IN NAVMESH HOLES -----------------------
    //
    // Probed against the live 604 mmtiles at the GOs' own coordinates:
    //
    //   192520  Colossus altar (1693.51, 743.60)  its Z 142.79
    //           -> NO GROUND IN THE COLUMN. Only water at 110.14, and that water
    //              is ISOLATED comp#15.                        drop -32.65yd
    //   192518  Slad'ran altar (1775.29, 679.68)  its Z 129.24
    //           -> water 110.14, ground 103.22, NO 129 surface. drop -19.10yd
    //   192519  Moorabi altar  (1772.22, 804.96)  its Z 129.24
    //           -> 129.34 ground, comp#0, with a 7.25yd walkable disc.      +0.10
    //
    // This is a live detonator, not an anchor-placement nit, because of what the
    // executor's UseGameObject step does when the bot is out of range:
    //
    //     if (!bot->IsWithinDistInMap(go, DC_EVENT_GO_USE_RANGE))   // 5.0f
    //     { HopTo(bot, go->GetPositionX(), go->GetPositionY(), go->GetPositionZ()); ... }
    //
    // — it walks the bot to THE GO'S OWN COORDINATES, i.e. into the hole. It does
    // not fall the full 97yd (GetWaterOrGroundLevel raycasts 50yd down and finds
    // the 110.14 sheet first), but it drops 19-33yd into the moat; and in the
    // Colossus altar's case that pocket is isolated, so the bot is stranded with no
    // path back.
    //
    // THE GEOMETRY, measured. Both holes are effectively circular discs of radius
    // 2.25yd centred on the altar, so for any rim point at distance d from the GO
    // the usable standing pad is p = d - 2.25. That makes the constraint exact, and
    // it rules out doing this with a single anchor:
    //
    //     stand anywhere within arriveRadius a of the anchor, stay on mesh => a <= p
    //     worst-case distance to the GO                                     = d + a
    //     the click requires                                        d + a <= 5.0
    //     taking a = p = d - 2.25                                =>     d <= 3.625
    //
    // and an arriveRadius of ~1.4 is not a navigation radius anything will reliably
    // satisfy — the pathfinder's own segment arrival is 6yd, so the tank would park
    // short and the objective would never fire at all.
    //
    // SO THE TWO JOBS ARE SEPARATED. The OBJECTIVE ANCHOR goes far enough out that
    // its pad comfortably exceeds the 6yd arrival (Slad'ran: d 9.68, pad 7.55;
    // Colossus: d 10.51, pad 8.45 — which also gives the four FOLLOWERS somewhere
    // to stand that is not a hole rim), and an explicit MoveTo — the documented
    // "SHORT intra-room hop" — closes the last 6-7yd onto a measured click pad:
    //
    //                     click point        d      pad    radius   worst reach
    //     192518 Slad'ran (1775.29, 676.18)  3.50   1.40   1.25     4.75
    //     192520 Colossus (1690.01, 743.60)  3.50   1.45   1.25     4.75
    //
    // 4.75 < 5.0, so the HopTo branch never runs; 1.25 < the pad, so the bot cannot
    // step off the rim while turning to face the GO. d = 3.50 is the optimum: any
    // further out and d + p exceeds the use range, any closer and the pad shrinks
    // for nothing.
    //
    // WHY THOSE TWO DIRECTIONS. South is the right side of the Slad'ran altar — it
    // faces his spawn and opens onto the whole boss room, so the only thing capping
    // the pad is the hole behind the bot. West is the right side of the Colossus
    // altar — it faces the flat arena plateau, whereas east is already on the
    // descending causeway ramp.
    out.push_back(EventBuilder(MAP, EVENT_ALTAR_SLADRAN, "Altar of Slad'ran")
                      .Anchored(/*orderIndex (doc)*/ ORDER_ALTAR_SLADRAN)
                      .Persistent()
                      .MoveTo(SLADRAN_CLICK_X, SLADRAN_CLICK_Y, SLADRAN_CLICK_Z, CLICK_RADIUS)
                      .UseGO(ALTAR_SLADRAN, ALTAR_SEARCH)
                          .Timeout(ALTAR_TIMEOUT)
                      .WaitForGOState(STATUE_SNAKE, GO_STATE_READY,
                                      ALTAR_TIMEOUT, STATUE_SEARCH)
                      .Build());

    // --- (2) start the Drakkari Colossus fight ----------------------------
    //
    // boss_drakkari_colossus.cpp:
    //
    //     void MoveInLineOfSight(Unit*) override { }          // never aggros
    //     void Reset() override
    //     {
    //         if (!me->IsInEvadeMode())
    //         {
    //             me->CastSpell(me, SPELL_FREEZE_ANIM, true);
    //             me->SetUnitFlag(UNIT_FLAG_NON_ATTACKABLE);
    //             for (auto const& i : mojoPosition)
    //                 me->SummonCreature(NPC_LIVING_MOJO, i, TEMPSUMMON_MANUAL_DESPAWN, 0);
    //         }
    //         ...
    //     }
    //
    // and the summoned mojos are inert by construction — npc_living_mojoAI
    // overrides MoveInLineOfSight and AttackStart to return early when
    // me->ToTempSummon(), so they never aggro and never swing. Only when a PLAYER
    // attacks one does JustEngagedWith fire ACTION_INFORM at the summoner, which
    // SetInCombatWithZone()s the Colossus, merges all five mojos (they MoveCharge
    // the merge point and DespawnOrUnsummon(1200ms)) and starts the fight 3.5s
    // later. Nothing on this map will ever start it otherwise: the party walks to a
    // boss it cannot target, beside five mobs that will not fight it, and stands
    // there. This is the Nexus-Keristrasza shape with a creature instead of a GO.
    //
    // CONDITIONAL, not an anchored objective, for three reasons: the predicate
    // above solves the post-evade case for free (an anchored Optional step would
    // have to time out to learn the same thing); PanelBeforeBoss folds it into the
    // Colossus's own row, which is where a human reading `dc bosses` expects it;
    // and it costs one fewer roster anchor.
    //
    // NO DrivesInCombat / StepsOwnMovement / OwnsThePull. The party is OUT of
    // combat when it walks in, so the ordinary conditional rung is the right
    // driver. This also matters for the build: all three flags are guarded by
    // allowlist gtests (DrivesInCombatIsConfinedToVettedWaveEncounters,
    // StepsOwnMovementIsConfinedToVettedEvents, PullOwningEventsAreVetted) that
    // would each need a Gundrak entry. Using none of them keeps map 604 off all
    // three lists — asserted by test, so a future author reaching for one has to
    // update the allowlist deliberately rather than discover it as a red build.
    //
    // A CONDITIONAL EVENT CAN ACTUALLY DRIVE THIS. A conditional event has no
    // boss-nav delivering the tank to the spot, so a seeking step would be useless
    // if the driver only gated on it — but DcRunEventAction::Execute handles the
    // case explicitly, walking the leader in through the engage pipeline for
    // exactly this shape (a KillCreature with .engage on a conditional event).
    //
    // MOJO_SEARCH MUST BE TIGHT, AND THIS IS THE TRAP ON THIS ENCOUNTER. There are
    // also FOUR PRE-PLACED Living Mojos in the DB (guids 127076-127079) 39-94yd
    // west of the boss. Those are ordinary trash: not TempSummons, so they DO aggro
    // and DO fight — and hitting one informs nobody. The five summoned ring mojos
    // sit 9.9-10.6yd from the merge point; the nearest trash mojo is 39.3yd from
    // it, and 29.6yd from the nearest RING mojo. A 20yd bot-centred search
    // separates them with room to spare. Get this wrong and the party "starts the
    // fight" 39-94yd from a boss that never wakes up.
    //
    // The step completes naturally: the inform triggers ACTION_MERGE, every mojo
    // charges the merge point and despawns after 1.2s, so "no alive 29830 in range"
    // becomes true right as the boss becomes attackable.
    out.push_back(EventBuilder(MAP, EVENT_COLOSSUS_MOJO, "Drakkari Colossus: pull a Living Mojo")
                      .Conditional(&GdColossusFrozen)
                      .PanelBeforeBoss(COLOSSUS)
                      .KillCreatureEngage(LIVING_MOJO, /*count*/ 1, MOJO_SEARCH)
                          .Timeout(MOJO_TIMEOUT)
                      .Build());

    out.push_back(EventBuilder(MAP, EVENT_ALTAR_COLOSSUS, "Altar of the Drakkari Colossus")
                      .Anchored(/*orderIndex (doc)*/ ORDER_ALTAR_COLOSSUS)
                      .Persistent()
                      .MoveTo(COLOSSUS_CLICK_X, COLOSSUS_CLICK_Y, COLOSSUS_CLICK_Z, CLICK_RADIUS)
                      .UseGO(ALTAR_COLOSSUS, ALTAR_SEARCH)
                          .Timeout(ALTAR_TIMEOUT)
                      .WaitForGOState(STATUE_TROLL, GO_STATE_READY,
                                      ALTAR_TIMEOUT, STATUE_SEARCH)
                      .Build());

    // Moorabi's altar needs NONE of the rim machinery: 129.34 ground directly
    // under the GO, comp#0, with a 7.25yd continuous walkable disc — the roomiest
    // of the three by a wide margin. The anchor is the GO's own position on the
    // probed floor and the event stays at two steps. arriveRadius is the same 6yd
    // as the other two (below the pad, above the pathfinder's own segment arrival),
    // so the worst case here is a short HopTo onto solid ground rather than into a
    // hole — which is precisely the difference this altar's geometry buys.
    out.push_back(EventBuilder(MAP, EVENT_ALTAR_MOORABI, "Altar of Moorabi")
                      .Anchored(/*orderIndex (doc)*/ ORDER_ALTAR_MOORABI)
                      .Persistent()
                      .UseGO(ALTAR_MOORABI, ALTAR_SEARCH)
                          .Timeout(ALTAR_TIMEOUT)
                      .WaitForGOState(STATUE_MAMMOTH, GO_STATE_READY,
                                      ALTAR_TIMEOUT, STATUE_SEARCH)
                      .Build());

    // --- (5) Eck's pool (heroic only) -------------------------------------
    //
    // Eck is gated on a THREE-MEMBER FORMATION, not on "kill the Ruins Dwellers",
    // and that distinction is the whole event. instance_gundrak::OnUnitDeath:
    //
    //     if (Creature* dweller = unit->ToCreature())
    //         if (CreatureGroup* formation = dweller->GetFormation())
    //             scheduler.Schedule(1s, [...] {
    //                 if (!formation->IsAnyMemberAlive()) { ...SummonCreature(ECK...); }
    //             });
    //
    // There are SIX Ruins Dwellers (29920) on the map and only THREE are in a
    // formation (creature_formations leader 127203, groupAI 3):
    //
    //     127201  (1651.26, 936.46, 107.28)   IN THE FORMATION
    //     127202  (1643.20, 943.62, 107.28)   IN THE FORMATION
    //     127203  (1644.73, 936.47, 107.29)   IN THE FORMATION (leader)
    //     127204  (1708.48, 926.96, 116.09)   ungrouped
    //     127205  (1701.66, 951.03, 116.54)   ungrouped
    //     127206  (1717.30, 935.61, 117.11)   ungrouped
    //
    // Killing the ungrouped three does nothing at all — GetFormation() returns null
    // and the handler bails. So a KillCreature(29920, 6) gate would be wrong in
    // BOTH directions: it demands three irrelevant kills and it does not express
    // "these particular three". A position-anchored ClearRadius on the pool names
    // exactly the right set.
    //
    // THE VOLUME. Centred on the formation trio's centroid (1646.40, 938.85) with
    // radius 15 and a 6yd zBand. From that centre the three GATING dwellers are
    // 2.9 / 5.4 / 5.7yd out; the three ungrouped ones are 56.6 / 63.2 / 71.0yd out
    // and 9yd up. Anything under ~50 separates them; 15 leaves generous margin
    // without approaching the ungrouped trio. (They are all reachable, so an
    // over-wide radius costs the run three pointless elite kills rather than a
    // wedged path — still a cost worth not paying.)
    //
    // Z IS THE MESH, NOT THE SPAWN. The trio spawn at 107.28 but the walkable
    // surface above them is the WATER SHEET at 108.05-108.42; the volume centres on
    // 108.22. Probed: all six dwellers are comp#0 and every one is path-connected
    // from Eck's home in 2-12 polys, so the earlier worry that the trio might be
    // marooned in the isolated comp#7 lobe is disproved — the seam runs SOUTH of
    // them, at about (1647, 917).
    //
    // OnlyEntries so the sweep cannot wander into the Drakkari Frenzy (29834, 24
    // spawns at z 105-126) shoaling in the same water.
    //
    // NO BY-ENTRY KillCreatureEngage BACKSTOP, deliberately, and unlike Ahn'kahet's
    // initiate sweep. A backstop keyed on entry 29920 resolves NEAREST FIRST and
    // would walk the party to the UNGROUPED dwellers — the precise failure the
    // volume exists to prevent. The Ahn'kahet reasoning does not transfer: there
    // the risk was a ClearRadius answering "clear" with a live initiate, because
    // NearestHostileNearPoint filters through IsPossibleTarget; here a by-entry
    // backstop actively makes things worse.
    //
    // REQUIRED, not the Optional a tuning pre-clear would take: no formation wipe,
    // no Eck, and Eck is a rostered heroic boss.
    //
    // Persistent by the folder's convention for every ClearRadius event. With one
    // step there is no progress to lose; what it changes is that the timeout clock
    // keeps running across the fight's combat gaps instead of being re-based by
    // each one, which is why the budget is five minutes rather than the 30s
    // default.
    out.push_back(EventBuilder(MAP, EVENT_ECK_POOL, "Eck: clear the dweller pool")
                      .Anchored(/*orderIndex (doc)*/ ORDER_ECK_POOL)
                      .HeroicOnly()
                      .Persistent()
                      .ClearRadius(POOL_X, POOL_Y, POOL_Z, POOL_RADIUS, POOL_ZBAND)
                          .OnlyEntries({ RUINS_DWELLER })
                          .Timeout(POOL_TIMEOUT)
                      .Build());

    // --- (6) cross the bridge — the load-bearing event on this map --------
    //
    // THE BRIDGE DECK IS NOT IN THE NAVMESH AND NEVER CAN BE. mmaps are baked from
    // terrain + WMO + WMO doodads only, and every piece of the bridge apparatus
    // (the four statues, the collision block 192633 and the trapdoor 193188) is a
    // DB-SPAWNED GAMEOBJECT. Flipping them to GO_STATE_ACTIVE_ALTERNATIVE changes
    // what the client draws and what blocks collision; it does not add a single
    // Detour polygon.
    //
    // Cross-section at y = 743.5, ground only, 0.25yd sampling:
    //
    //     west causeway (from the hub)   ... - 1753.50           121.7 -> 119.31
    //     GAP - NO MESH                  1753.75 - 1765.25       11.50yd
    //     central island                 1765.50 - 1784.50       118.40, dead flat
    //     GAP - NO MESH                  1784.75 - 1796.50       11.75yd
    //     east causeway (to Gal'darah)   1796.75 - ...           119.42 -> 136.5
    //
    // Map 604 has 4181 walkable polys in 245 connected components and the three
    // that matter are mutually disconnected: comp#0 is the main dungeon (entrance,
    // Slad'ran, Moorabi, the Colossus arena and Eck's chamber all path fine);
    // comp#60 is the central island, EXACTLY 7 POLYS with zero links in any
    // direction; comp#1 is Gal'darah's entire wing, 293 polys. Minimum
    // ground-to-ground gaps are 11.84 and 12.05yd, comp#0 to comp#1 directly is
    // 20.64yd and purely VERTICAL, and every tile reports offMeshConCount = 0.
    //
    // SWIMMING THE MOAT DOES NOT WORK. The reachable NAV_WATER sheet at z 110.14
    // reaches (1797.33, 747.47) right up under the Gal'darah ledge — but that ledge
    // is 9.12yd straight up, far past the tiles' walkableClimb of 1.6.
    //
    // So an unmodified clear ends at 4/5 (3/4 on normal) staring across a chasm: a
    // bot commanded to Gal'darah gets a start poly in comp#0 and a goal poly in
    // comp#1, dtFindPath returns PATHFIND_INCOMPLETE, and the bot walks to the
    // comp#0 poly nearest the goal — the west causeway tip — and stalls there
    // permanently.
    //
    // WHY TeleportParty AND NOT Jump. The gaps are 11.5 and 11.75yd at nearly
    // constant Z, which is within ballistic-jump range, so Jump looks tempting. It
    // is wrong for a structural reason: JUMP IS A LEADER-ONLY STEP. Unlike
    // DropInHole and TeleportParty, both of which explicitly pull the followers
    // across, nothing carries the rest of the party over a Jump — the tank would
    // land on comp#60 or comp#1 and the other four would stand on the causeway with
    // no path to it. TeleportParty is also fully synchronous and idempotent (Done
    // immediately if the leader is already on the landing), so a tick-gap restart
    // never re-teleports.
    //
    // THE PROBED NUMBERS, AND NOT THE CAUSEWAY TIPS. "Pad radius" here is the
    // largest disc around a point where every sample is walkable, same component,
    // within +/-2.5yd of Z. The obvious checkpoint (the west tip at x 1753.5, where
    // the stock pathfinder already strands a Gal'darah-bound bot) is a KNIFE EDGE
    // at pad 0.00, and the obvious landing (1797.0) is barely better at 0.25. Five
    // bots cannot arrive on either. Along the centrelines the pad peaks at ~5yd
    // (the causeways are only 10-11yd wide, so 5.5 is the physical ceiling):
    //
    //     WEST comp#0 (y 744.0)            EAST comp#1 (y 743.5)
    //      x 1744  z 119.60  pad 5.25       x 1797  z 119.42  pad 0.25
    //      x 1746  z 119.10  pad 5.00  <--  x 1799  z 119.48  pad 2.25
    //      x 1748  z 119.04  pad 4.50       x 1801  z 119.55  pad 4.25
    //      x 1750  z 119.16  pad 3.50       x 1802  z 119.58  pad 4.75  <--
    //      x 1752  z 119.28  pad 1.50       x 1808  z 119.77  pad 5.00
    //      x 1753.5 z 119.29 pad 0.00
    //
    // The checkpoint sits 7.5yd back from the tip and is reachable from the
    // entrance (comp#0, 109 polys); the landing connects onward to Gal'darah (9
    // polys) and to the muster point (7 polys). The objective anchor is ON the
    // checkpoint with a 5yd arrive radius, and CROSS_RADIUS 8 then always satisfies
    // the teleport gate on arrival without a stutter-step closing move.
    //
    // DO NOT STAGE THROUGH THE CENTRAL ISLAND. It is a 7-poly dead component; a
    // party left standing on it is trapped between two gaps.
    //
    // KEEP THE WaitForGOState GATE, AND PUT IT FIRST. Once the crossing is a
    // teleport the bridge's state is mechanically irrelevant — which is exactly why
    // the gate matters. Without it the clear would happily teleport across a chasm
    // the party never opened, silently skipping the whole altar chain and turning a
    // broken altar into an INVISIBLE failure instead of a legible one. Statue
    // 192566 (Rhino) is the right witness: it is the only one of the four that
    // SPAWNS at GO_STATE_READY, so reaching GO_STATE_ACTIVE_ALTERNATIVE can only
    // have come from instance_gundrak::Update's bridge drop and can never be
    // confused with an altar click. It also absorbs the scripted 5s _activateTimer.
    //
    // THE LANDING IS ONE-WAY. comp#1 has no ground link to anything else on the
    // map — even the two post-kill exit corridors behind 193208/193209 dead-end
    // inside it. A bot that ends up OFF comp#1 is stranded permanently; recovery
    // would have to be another teleport, not a path home. That is also why this
    // event sorts AFTER Eck on heroic: once the party crosses, it is not coming
    // back.
    //
    // THE RHINO STAMPEDE LANDS ON THIS ANCHOR. 8s after the bridge drops the
    // collision block's SmartAI sends rhinos 127111 and 127207 west along waypoint
    // paths 1271110 / 1272070, the second ending at (1777.25, 743.66, 119.88) — on
    // the ISOLATED ISLAND, across a 12yd gap from the checkpoint. Creature movement
    // is not bound by the bot pathfinder's component rules, so whether they
    // actually arrive cannot be settled from the mesh. All three outcomes are
    // survivable: they reach the checkpoint and are fought there (the anchor picked
    // the ground), they strand on the island and are ignored (the party teleports
    // past them), or they strand on the island IN COMBAT with the party — which
    // would hold the crossing behind a combat gate that never clears. Only the
    // third needs a fix, and only if a run shows it.
    out.push_back(EventBuilder(MAP, EVENT_BRIDGE, "Cross the bridge")
                      .Anchored(/*orderIndex (doc)*/ ORDER_BRIDGE)
                      .Persistent()
                      .WaitForGOState(STATUE_RHINO, GO_STATE_ACTIVE_ALTERNATIVE,
                                      BRIDGE_TIMEOUT, STATUE_SEARCH)
                      .TeleportParty(CROSS_CHECK_X, CROSS_CHECK_Y, CROSS_CHECK_Z,
                                     CROSS_LAND_X, CROSS_LAND_Y, CROSS_LAND_Z,
                                     CROSS_RADIUS)
                      .Build());
}

// --- roster patches -------------------------------------------------------
//
// BossSpawnIndex::Build keys on instance_encounters.creditEntry and then walks
// CREATURE SPAWNS looking for that entry. The Gundrak credit table is:
//
//     383 / 384   creditType 0 KILL_CREATURE   29304   Slad'ran
//     385 / 386   creditType 0 KILL_CREATURE   29573   Drakkari Colossus  <-- the ELEMENTAL
//     387 / 388   creditType 0 KILL_CREATURE   29305   Moorabi
//     389         creditType 0 KILL_CREATURE   29932   Eck the Ferocious  (heroic only)
//     390 / 391   creditType 0 KILL_CREATURE   29306   Gal'darah
//
// and there is no fallback: the store is populated ONLY by walking
// GetAllCreatureData() and matching each spawn's entry against the credit map, so
// a credit entry with no spawn is never visited and contributes no row and no
// coordinates — silently. 29573 (the Drakkari Elemental) has no `creature` row on
// any map, because the Colossus summons it at 51% and 2% and dies with it; 29932
// (Eck) likewise is summoned by the instance script. So the derived roster is
// Slad'ran + Moorabi + Gal'darah on both difficulties, and the Colossus and Eck
// are simply absent.
//
// Both missing bosses have a REAL DungeonEncounter bit and only the DERIVATION
// failed, so MakeBossWithBit is the right builder — not MakeBoss with a
// doneBossStateIndex. Completion then rides GetCompletedEncounterMask exactly like
// every other boss on the map: KillRewarder passes the victim's entry to
// Map::UpdateEncounterState, which matches creditEntry 29573 / 29932 and sets the
// bit. (instance_gundrak overrides neither GetData nor the persistent-data store,
// so doneBossStateIndex is not even available here as a fallback — but the DBC bit
// works, which is what matters.)
//
// Read off the live DungeonEncounter.dbc:
//
//     map 604 diff 0:  bit0 Slad'ran  bit1 Colossus  bit2 Moorabi            bit3 Gal'darah
//     map 604 diff 1:  bit0 Slad'ran  bit1 Colossus  bit2 Moorabi  bit3 Eck  bit4 Gal'darah
//
// The Colossus is bit 1 on BOTH difficulties, so one add row serves both. Eck's
// bit 3 exists only on heroic, and heroic Gal'darah's shift from bit 3 to bit 4 is
// already carried correctly by his DERIVED row.
void RegisterGundrakRoster(std::vector<BossRosterPatch>& t)
{
    using namespace DcRoster;

    // --- both difficulties -------------------------------------------------
    //
    // THE ORDER SCALE. An anchored event must be referenced by exactly one
    // MakeObjective anchor (MakeBoss takes no eventId, and the gtest
    // AnchoredEventsAreWiredByExactlyOneObjective enforces it), so every anchored
    // event below gets its own objective row, and the bosses are reordered onto one
    // contiguous 1..10 scale so those rows have integer slots between them:
    //
    //      1  boss       29304    Slad'ran
    //      2  objective  OBJ(1)   Altar of Slad'ran               -> event 1
    //      3  boss       29307    Drakkari Colossus               (event 2 is Conditional — no anchor)
    //      4  objective  OBJ(2)   Altar of the Drakkari Colossus  -> event 3
    //      5  boss       29305    Moorabi
    //      6  objective  OBJ(3)   Altar of Moorabi                -> event 4
    //      7  objective  OBJ(4)   Eck: clear the dweller pool     -> event 5   [heroic patch]
    //      8  boss       29932    Eck the Ferocious                            [heroic patch]
    //      9  objective  OBJ(5)   Cross the bridge                -> event 6
    //     10  boss       29306    Gal'darah
    //
    // Kill-bits are untouched throughout — orderOverride moves the clear sequence
    // and nothing else. The ordering also matches the travel path: south spoke ->
    // west spoke -> north spoke -> (north-west pool) -> back east across the bridge.
    //
    // Because each altar gates on its own boss's death and the bridge gates on all
    // three altars, this order is not a preference: it is FORCED by the instance
    // script.
    BossRosterPatch p;
    p.mapId = MAP;

    p.add = {
        // The Colossus, anchored on 29307 — the entry that DOES have a spawn, so
        // liveness, GetLiveBoss and IsCreaturePresentOnMap all work — carrying the
        // Elemental's kill-bit. Z is the probed mesh under his 143.338 spawn.
        MakeBossWithBit(COLOSSUS, MAP, "Drakkari Colossus",
                        COLOSSUS_X, COLOSSUS_Y, COLOSSUS_Z,
                        /*encounterIndex*/ BIT_COLOSSUS,
                        /*orderOverride*/ ORDER_COLOSSUS),

        // The three altar clicks. An objective's encounterIndex is an ordering hint
        // only (it has no kill-bit and NextDungeonBossValue never tests the
        // completion mask for one), so it stays 0 and the clear orders by
        // orderOverride.
        //
        // arriveRadius 6 on all three: above the pathfinder's own 6yd segment
        // arrival so the objective reliably fires, and below every one of the three
        // anchors' measured pads (7.55 / 8.45 / 7.25) so the followers gathering on
        // it cannot be pushed off the mesh.
        MakeObjective(OBJ(1), /*encounterIndex*/ 0, MAP, "Altar of Slad'ran",
                      SLADRAN_ANCHOR_X, SLADRAN_ANCHOR_Y, SLADRAN_ANCHOR_Z,
                      ALTAR_ARRIVE, /*gateEntry*/ 0, /*hook*/ 0,
                      /*eventId*/ EVENT_ALTAR_SLADRAN,
                      /*orderOverride*/ ORDER_ALTAR_SLADRAN),
        MakeObjective(OBJ(2), /*encounterIndex*/ 0, MAP, "Altar of the Drakkari Colossus",
                      COLOSSUS_ANCHOR_X, COLOSSUS_ANCHOR_Y, COLOSSUS_ANCHOR_Z,
                      ALTAR_ARRIVE, /*gateEntry*/ 0, /*hook*/ 0,
                      /*eventId*/ EVENT_ALTAR_COLOSSUS,
                      /*orderOverride*/ ORDER_ALTAR_COLOSSUS),
        MakeObjective(OBJ(3), /*encounterIndex*/ 0, MAP, "Altar of Moorabi",
                      MOORABI_ANCHOR_X, MOORABI_ANCHOR_Y, MOORABI_ANCHOR_Z,
                      ALTAR_ARRIVE, /*gateEntry*/ 0, /*hook*/ 0,
                      /*eventId*/ EVENT_ALTAR_MOORABI,
                      /*orderOverride*/ ORDER_ALTAR_MOORABI),

        // The crossing. OBJ(5), not OBJ(4) — the heroic patch below claims OBJ(4)
        // for the pool, and the synthetic objective entries have to be unique
        // across BOTH patches for the map.
        MakeObjective(OBJ(5), /*encounterIndex*/ 0, MAP, "Cross the bridge",
                      CROSS_CHECK_X, CROSS_CHECK_Y, CROSS_CHECK_Z,
                      CROSS_ARRIVE, /*gateEntry*/ 0, /*hook*/ 0,
                      /*eventId*/ EVENT_BRIDGE,
                      /*orderOverride*/ ORDER_BRIDGE),
    };

    // The three derived bosses onto the same scale. Their relative order is
    // unchanged from what their DBC bits already gave them; the reorder exists
    // purely so the five objectives have somewhere to sit.
    p.reorder = {
        { SLADRAN,  ORDER_SLADRAN  },
        { MOORABI,  ORDER_MOORABI  },
        { GALDARAH, ORDER_GALDARAH },
    };

    t.push_back(std::move(p));

    // --- heroic only: Eck --------------------------------------------------
    //
    // Applied AFTER the patch above (Apply runs every matching patch in
    // registration order over the same list), so the base scale is already in
    // place and these two simply slot into keys 7 and 8.
    //
    // Eck does not exist until summoned, which is the Molten-Core-finale shape the
    // module already handles. He is anchored on his scripted HOME
    // (boss_eck.cpp EckHomePosition, 1642.712 / 934.646 / 107.205) with Z moved to
    // the probed water sheet at 108.00 — and NEVER on his summon point
    // (1624.70, 891.43, 95.08), which has no poly within 5yd and is exactly the
    // column map 604's 0.0 gridHeight punishes. The home is also the more stable
    // target than his combat-start position 15yd away, because SetHomePosition
    // means an evade returns him to it.
    //
    // KNOWN AND DELIBERATELY UNAUTHORED: the entire Eck encounter is fought in
    // DEEP WATER. The approach from the Eck Door is dry for ~190yd, then crosses a
    // shoreline at about (1657, 935) onto the z 108.00 sheet with no drop at the
    // edge (ground 108.98 meets sheet 108.77, so the party wades rather than
    // falls). But shore to Eck's home is ~14.6yd and shore to the furthest gating
    // dweller ~16.3yd — one contiguous swim leg — and at his combat start the pool
    // floor is 6.92yd below the sheet, deeper still over most of the fight area.
    // Everything after the shoreline is on that sheet, so any behaviour assuming
    // ground footing (leash-to-ground, step-out-onto-land, ground-only
    // repositioning) will misbehave for the whole fight. Nothing is authored
    // against it because there is nothing to author until a run says what breaks —
    // but if the heroic Eck fight goes badly, this is the first place to look, not
    // the boss script. (The three UNGROUPED dwellers are all on dry ground at
    // z 116-117 and need no swimming — one more reason the pool sweep must not
    // reach them.)
    BossRosterPatch heroic;
    heroic.mapId = MAP;
    heroic.gate = DcDifficultyGate::HeroicOnly;
    heroic.add = {
        MakeObjective(OBJ(4), /*encounterIndex*/ 0, MAP, "Eck: clear the dweller pool",
                      POOL_X, POOL_Y, POOL_Z, POOL_ARRIVE,
                      /*gateEntry*/ 0, /*hook*/ 0,
                      /*eventId*/ EVENT_ECK_POOL,
                      /*orderOverride*/ ORDER_ECK_POOL),
        MakeBossWithBit(ECK, MAP, "Eck the Ferocious",
                        ECK_X, ECK_Y, ECK_Z,
                        /*encounterIndex*/ BIT_ECK,
                        /*orderOverride*/ ORDER_ECK),
    };

    t.push_back(std::move(heroic));
}
