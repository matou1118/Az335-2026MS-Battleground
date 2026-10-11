/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "Ai/Dungeon/DungeonClear/Data/DungeonClearRouteRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"

#include "InstanceScript.h"
#include "Player.h"

// --- Halls of Lightning (map 602) ------------------------------------------
//
// The declarative half of the one thing on this map the ordinary clear cannot
// do: cross the Slag Furnace. The controller is
// Overrides/HallsOfLightningDriver.cpp; the arithmetic is the SHARED, map-free
// kernel in Util/DcSuppressionTransitDecision.h; the numbers both halves agree
// on are namespace DcHallsOfLightning in DungeonEventTables.h.
//
// THERE IS NO ROSTER PATCH IN THIS FILE, and that is the point of saying so.
// Every other WotLK dungeon authored here needed one — Halls of Stone and
// Drak'Tharon for cast-spell credit, Gundrak for two bosses with no spawn, The
// Nexus for a faction-swapped commander with no kill bit. Map 602's four
// instance_encounters rows are all creditType 0 against real spawns and the DBC
// order IS the travel order, so BossSpawnIndex::Build derives the whole list
// correctly on both difficulties. Adding a patch here could only make it worse.
//
// WHAT IS AUTHORED, and why each piece exists:
//
//   A  the route below (data) — the whole Bjarngrim -> Volkhan leg, with
//      AnchorFlag::NO_STOP over the descent, the pit and the climb out, so the
//      advanced pull never sets up a camp on ground it cannot hold;
//   B  the conditional transit event and its predicate (this file) — the row
//      that stands the pull down, drives in combat and steps its own movement;
//   C  the leader-side driver (hook 24, HallsOfLightningDriver.cpp) — the only
//      thing on this map that moves the leader while it is engaged.
//
// The FOLLOWER half of Blackwing Lair's transit — DungeonClearTransitPackTrigger
// / DcSuppressionTransitActions, relevance 36 — is deliberately NOT generalised
// to this map. BWL needed it because twenty-four followers walking a straight
// chord across a curved corridor fell into a hole in the mesh; the Slag Furnace
// is a straight, flat, 41yd-wide walkway and four followers behind one tank have
// `follow tank` (relevance 25) already. Measure before adding a rung.

namespace
{
    using namespace DcHallsOfLightning;

    // DUE while the leader is inside the Slag Furnace corridor with Volkhan still
    // to kill and the mid ledge still to reach.
    //
    // Four probes, cheapest first, because this runs on every COMBAT tick of the
    // DC leader on map 602 — and on this leg the party is in combat essentially
    // without a break:
    //
    //   1. the map;
    //   2. the corridor, two axis-aligned tests that are false everywhere else on
    //      this dungeon — including Volkhan's gallery 29yd directly overhead,
    //      which is why the Z band is load-bearing, and including the gallery
    //      ramp 7.5yd east of the pit's own, which is why there are two boxes
    //      (see the block in DungeonEventTables.h);
    //   3. the mid ledge — being there is what ENDS the crossing. Expressing
    //      completion as "not yet at the far end" rather than as a latch is what
    //      makes it self-resetting: a party shoved back into the pit re-arms the
    //      transit, which is the correct answer, and there is no flag for a wipe
    //      to leave stale;
    //   4. Volkhan's encounter bit, which reads DONE the moment he dies and is
    //      the authoritative end of this leg.
    //
    // DELIBERATELY NOT a grid scan for Volkhan himself. He stands at the far end
    // of the crossing, ~200yd from the staging point where the transit has to
    // arm, so any scan radius honest enough to be called a room scan reads "not
    // there" for the first two thirds of the leg. The bit answers the question
    // the scan was for, from anywhere, for free.
    //
    // AND NOT GATED ON COMBAT. The crossing starts from the staging point while
    // it is still quiet — the gather gate is the first thing it does — and a
    // predicate that waited for combat would arm the driver only after the party
    // had already walked into the Slags strung out in a line.
    bool SlagFurnaceTransitDue(Player* bot, AiObjectContext* /*context*/)
    {
        if (!bot || bot->GetMapId() != MAP_ID)
            return false;

        if (!InTransitCorridor(bot))
            return false;

        if (bot->GetExactDist(TRANSIT_END_X, TRANSIT_END_Y, TRANSIT_END_Z) <= TRANSIT_END_RADIUS)
            return false;

        InstanceScript* inst = bot->GetInstanceScript();
        return !inst || inst->GetBossState(VOLKHAN_ENCOUNTER_INDEX) != DONE;
    }
}

// Two axis-aligned boxes, and the transit's real gate. See the block in
// DungeonEventTables.h for how the bounds were drawn, why one box cannot draw
// them, and what they deliberately exclude.
bool DcHallsOfLightning::InTransitCorridor(Player* bot)
{
    if (!bot || bot->GetMapId() != MAP_ID)
        return false;

    float const x = bot->GetPositionX();
    float const y = bot->GetPositionY();
    float const z = bot->GetPositionZ();

    bool const inPit = x >= PIT_BOX_MIN_X && x <= PIT_BOX_MAX_X &&
                       y >= PIT_BOX_MIN_Y && y <= PIT_BOX_MAX_Y &&
                       z >= PIT_BOX_MIN_Z && z <= PIT_BOX_MAX_Z;
    if (inPit)
        return true;

    return x >= CLIMB_BOX_MIN_X && x <= CLIMB_BOX_MAX_X &&
           y >= CLIMB_BOX_MIN_Y && y <= CLIMB_BOX_MAX_Y &&
           z >= CLIMB_BOX_MIN_Z && z <= CLIMB_BOX_MAX_Z;
}

void RegisterHallsOfLightningEvents(std::vector<DungeonEvent>& out)
{
    using namespace DcHallsOfLightning;

    // CROSS THE SLAG FURNACE — the leg between Bjarngrim and Volkhan, and the
    // only content on this map DC cannot simply walk.
    //
    // ONE Custom step, for the Blackwing Lair / Violet Hold reason: what this leg
    // needs is a standing PREFERENCE re-decided every tick — walk, or stand for
    // the pack, or stand for an elite — not a sequence. A step list can only say
    // "do these in order and block on each", and every one of the thirteen legs
    // can be interrupted by either hold at any point. The gather gate lives
    // INSIDE the hook for the same reason: it is the first state of one
    // controller, not a step that happens to come first.
    //
    // DRIVES IN COMBAT — the load-bearing flag, and the whole point. The ordinary
    // conditional rung stands down on IsInCombat(), which on a leg with fourteen
    // Slags on a 20s respawn covering the route line is a rung that never runs.
    // Same failure DcCombatFlag::MayDrive leaves the clear with, and the same
    // flag that fixed it on map 269 and on map 469.
    //
    // STEPS OWN MOVEMENT — the driver delivers the leader on its own long-range
    // spline, and the at-objective hold runs BEFORE Drive: without this, last
    // tick's glide is cancelled before the hook can even see it and the party
    // creeps one tick at a time while every log line reports a healthy spline
    // issue (Old Hillsbrad's barrels; Black Morass's 151 attempts, 0 arrivals).
    // It also makes a Done RETURN YIELD THE TICK, which is what lets the party
    // fight through every hold.
    //
    // OWNS THE PULL — zero advanced pulls across the gauntlet. A camp-drag is the
    // exact opposite of crossing: the pull's Idle branch answers unplanned aggro
    // by walking a fresh camp BACK along the route until it finds ground clear of
    // hostiles, which among fourteen Slags on a 20s respawn is never nearby, so
    // it runs out to maxDrag and hauls the tank there. There is a second,
    // independent reason on this map: Volkhan's leash is
    // `GetDistance(1331.9, -106, 56) > 95 -> EnterEvadeMode`, and the pit floor is
    // INSIDE it (55yd from the leash centre at (1332, -150, 23.4)) — so a camp
    // dragged down the ramp takes Volkhan into the Slags over a 300yd path he
    // cannot walk, and evades him.
    //
    // REPEATABLE — the crossing is not a thing that completes once. The condition
    // going false (the leader reaches the mid ledge, or leaves the corridor, or
    // Volkhan dies) is the only "done", and a party shoved back into the pit has
    // to re-arm cleanly.
    //
    // PERSISTENT — the step list must not be rewound by the combat gaps. On this
    // leg a "gap" is one Slag dying, several times a minute.
    //
    // NOT EncounterActive: no encounter is in progress here. This is the leg
    // BETWEEN two of them, which is exactly where DC is supposed to work.
    //
    // NOT Optional. Every hold this driver takes is watchdog-bounded from inside,
    // so the step's own five-minute timeout can only fire if a hold's watchdog has
    // itself failed to release — a shape nothing else here can observe. Skipping
    // quietly at that point would hand the leg back to a clear that provably
    // cannot cross it; stalling names the problem for the human, who can
    // `dc skip` if they disagree.
    //
    // PANEL: sorted AFTER Bjarngrim, and NEVER BeforeBoss(Volkhan), which is what
    // it visually wants and what it must not have. PanelBeforeBoss keys
    // DcTargeting::HasPendingSummonEvent, which treats an unlatched gating event
    // as "this boss must still be SUMMONED" and suppresses the dynamic pull
    // within 80yd of him — and a REPEATABLE event is never latched, so the
    // suppression would be permanent: the party would arrive at Volkhan and never
    // pull him ([[dc-panelbeforeboss-repeatable-permanent-hold]]). PanelAfterBoss
    // carries no such second meaning, and Bjarngrim is the anchor immediately
    // before this leg, so the row lands in exactly the same place.
    out.push_back(
        EventBuilder(MAP_ID, EVENT_SLAG_FURNACE_TRANSIT, "Cross the Slag Furnace")
            .Conditional(&SlagFurnaceTransitDue)
            .Repeatable()
            .Persistent()
            .OwnsThePull()
            .DrivesInCombat()
            .StepsOwnMovement()
            .PanelAfterBoss(NPC_BJARNGRIM)
            .Custom(HOOK_SLAG_FURNACE_TRANSIT)
                .Timeout(TRANSIT_TIMEOUT_MS)
            .Build());
}

// THE BJARNGRIM -> VOLKHAN LEG, in full.
//
// DERIVED, NOT DRAWN. Every anchor below is a point on the corridor
// LongRangePathfinder itself returns for this leg against the live map-602
// mmtiles, decimated to ~16yd — the [[dc-navharness-prints-the-route]] method,
// and the reason t/TestHallsOfLightningRouteProbe prints the polyline it routed:
// an mmaps regen that moves the corridor is re-authored the same way instead of
// by hand. The routed leg is 395.4yd (2D), maxStepZ 2.54 — no ledge in it.
//
// THE ROUTE STARTS WHERE THE PARTY ACTUALLY STANDS, as far as it can. Bjarngrim
// is an npc_escortAI who Start()s in his constructor and patrols a seven-point
// loop across THREE FLOORS out of combat — (1262, -26.9, 33.5) to
// (1395.1, 36.6, 50.0), 130yd apart — so he can die anywhere in the hall. Anchor
// 0 at his DB spawn is the best single choice and the first four anchors are all
// on the hall's own floor inside his likely death positions, but this is the
// [[dc-anchor-route-must-cover-where-the-party-stands]] /
// [[dc-seedcursor-fallback-resets-to-zero]] trap and it is the highest-risk piece
// of the row. The case to watch is a leader that kills him on the east platform.
//
// NO_STOP RUNS 5..17 — the flag covers the leg LEAVING an anchor, so this spans
// the descent, the whole gauntlet and the climb out, and RELEASES at anchor 18
// where the four z38 trash spawns are worth a real pull. It starts at 5 and not
// at 4 because the leg 4 -> 5 is still the hall's flat south arm; the exposure
// begins where the floor starts dropping. See DcNoStopZone.
void RegisterHallsOfLightningRoute()
{
    using namespace DcHallsOfLightning;

    DungeonClearRouteRegistry::Register(
        MAP_ID, DUNGEON_DIFFICULTY_NORMAL, NPC_VOLKHAN,
        {
            // --- the hall: Bjarngrim's floor east to the south arm -----------
            { 1262.00f,  -26.90f, 33.51f },  //  0  Bjarngrim's spawn
            { 1277.76f,  -29.64f, 34.01f },  //  1
            { 1293.53f,  -32.38f, 36.71f },  //  2
            { 1309.29f,  -35.12f, 40.04f },  //  3
            // The bend south into the arm. PIVOT_TIGHT rather than a wide
            // sweep: the arm is the hall's exit, not a room.
            { 1320.46f,  -39.24f, 40.68f, /*doorGoEntry*/ 0,
              ToFlag(AnchorFlag::PIVOT_TIGHT) },  //  4
            // --- THE STAGING POINT: the top of the descent -------------------
            // The transit's cursor 0. Nearest Slag 78yd; the last quiet ground.
            { TRANSIT_STAGE_X, TRANSIT_STAGE_Y, TRANSIT_STAGE_Z, /*doorGoEntry*/ 0,
              ToFlag(AnchorFlag::NO_STOP) },      //  5
            // --- the descent: 17yd down in 48yd of route --------------------
            { 1325.56f,  -62.38f, 35.43f, 0, ToFlag(AnchorFlag::NO_STOP) },  //  6
            { 1327.91f,  -78.21f, 29.66f, 0, ToFlag(AnchorFlag::NO_STOP) },  //  7
            { 1330.26f,  -94.04f, 23.99f, 0, ToFlag(AnchorFlag::NO_STOP) },  //  8
            // --- the pit floor, z 23.88, south along the central walkway ----
            // The walkway is the strip between two NAV_SLIME moats (x 1278-1306
            // and x 1354-1382; see the DcNavPenaltyRegistry rows). It is 41yd
            // wide at its narrowest and these anchors hug its middle.
            //
            // Anchor 9 passes the pit-level archway GO 191416 at
            // (1332.2, -115.8, 24.7). NOT flagged DOOR_AHEAD: it spawns state 0
            // with template Data0 = 1 (startOpen) and nothing on this map ever
            // shuts it.
            { 1332.61f, -109.86f, 23.88f, 0, ToFlag(AnchorFlag::NO_STOP) },  //  9
            { 1334.96f, -125.69f, 23.88f, 0, ToFlag(AnchorFlag::NO_STOP) },  // 10  <- gauntlet
            { 1337.31f, -141.52f, 23.88f, 0, ToFlag(AnchorFlag::NO_STOP) },  // 11  <- gauntlet
            { 1339.65f, -157.34f, 23.88f, 0, ToFlag(AnchorFlag::NO_STOP) },  // 12  <- gauntlet
            { 1340.78f, -173.26f, 23.88f, 0, ToFlag(AnchorFlag::NO_STOP) },  // 13  <- gauntlet
            { 1340.68f, -189.26f, 23.88f, 0, ToFlag(AnchorFlag::NO_STOP) },  // 14  end of the gauntlet
            // --- the climb out: 15yd up the pit's south wall ----------------
            // The two Unbound Firestorms stand at (1300.0, -214.8) and
            // (1362.2, -215.2) either side of anchor 16 — the driver's Elite
            // hold is what stops here for them.
            { 1340.61f, -201.26f, 24.16f, 0, ToFlag(AnchorFlag::NO_STOP) },  // 15  foot of the climb
            { 1340.53f, -213.26f, 30.50f, 0, ToFlag(AnchorFlag::NO_STOP) },  // 16
            { 1340.53f, -225.26f, 37.56f, 0, ToFlag(AnchorFlag::NO_STOP) },  // 17
            // --- THE MID LEDGE: the transit ends here, NO_STOP released -----
            { TRANSIT_END_X, TRANSIT_END_Y, TRANSIT_END_Z },                 // 18
            // --- the switchback on to Volkhan's gallery ---------------------
            // A genuine hairpin: 18 -> 19 runs 7.5yd EAST along the ledge and
            // 19 -> 20 turns back north up a second ramp that climbs 14yd. The
            // two ramps are parallel and 7.5yd apart, which is the whole reason
            // the transit's corridor needs two boxes rather than one.
            //
            // DO NOT CUT THE BEND. The ledge between them is continuous mesh,
            // but the [[dc-bwl-anchor-16-18-bend-wall-clip]] lesson applies: the
            // route is clean, and shortening a hairpin authored off a real
            // corridor is how a party ends up inside geometry.
            { 1348.03f, -230.46f, 38.57f, /*doorGoEntry*/ 0,
              ToFlag(AnchorFlag::PIVOT_TIGHT) },  // 19
            { 1348.59f, -218.47f, 43.95f },       // 20
            { 1349.34f, -202.49f, 52.65f },       // 21  on to the gallery floor
            // --- Volkhan's gallery, north to the boss -----------------------
            { 1350.09f, -186.51f, 52.68f },       // 22
            { 1350.84f, -170.52f, 52.68f },       // 23
            { 1350.92f, -158.53f, 52.68f },       // 24
            { 1347.18f, -147.13f, 52.74f },       // 25
            { 1342.18f, -131.93f, 56.01f },       // 26
            { 1337.19f, -116.73f, 57.45f },       // 27
            { 1332.38f, -102.08f, 56.98f },       // 28  Volkhan
        });
}
