/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"
#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonRosterBuilders.h"

#include <vector>

#include "Group.h"
#include "InstanceScript.h"
#include "Player.h"

#include "Ai/Dungeon/DungeonClear/Util/DcFlightLeg.h"
#include "Ai/Dungeon/DungeonClear/Util/DcTargeting.h"

// --- The Oculus (map 578) ------------------------------------------------------
//
// The constants, and the reasoning behind them, live in namespace DcOculus in
// DungeonEventTables.h. This file is the authored shape, and it maps one-to-one
// onto a manual clear:
//
//   1  from the entrance, clear to the Nexus Portal and click it     OBJ(1)  TeleportParty
//   2  kill Drakos                                                    derived boss row
//   3  pick a drake from the freed prisoners                          the rider rung
//   4  clear the central ring's constructs                            OBJ(2)  flight + ClearRadius
//   5  clear the south and north pads                                 OBJ(3), OBJ(4)
//   6  kill Varos                                                     boss row, re-anchored
//   7  chase Urom across his three platforms                          OBJ(5..7)
//   8  kill Urom in the inner arena                                   boss row, re-anchored
//   9  fly up and kill Eregos from the drakes                         OBJ(8)  + the rider stations
//
// EVERY FLIGHT IS THE DRIVER'S. The objectives below only describe what happens
// once the party stands on an island: arrive, clear. The conditional driver
// (event 9, hook 38) outranks their arrival (EventDue 31 > AtObjective 30)
// whenever the party is on the wrong island, and yields the moment the tank is on
// foot on the right one.

namespace
{
    using namespace DcOculus;

    // --- event 9's gate --------------------------------------------------------
    //
    // DUE from Drakos's death until Eregos's, and after that for as long as anyone
    // is still in a saddle (the driver flies them down onto the Ring 4 floor). The
    // gate is instance state, and a stronger near-gate than any distance: the
    // drake-givers only leave their cages once DATA_DRAKOS reads DONE, and nothing
    // on this map can set it but Drakos's death on his own island.
    bool AnyMemberMounted(Player* bot)
    {
        Group* const group = bot->GetGroup();
        if (!group)
            return DcFlightLeg::OculusDrakeOf(bot) != nullptr;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* const m = ref->GetSource();
            if (m && m->GetMapId() == bot->GetMapId() && DcFlightLeg::OculusDrakeOf(m))
                return true;
        }
        return false;
    }

    bool OcDriverDue(Player* bot, AiObjectContext* /*context*/)
    {
        return OculusDriverDue(bot);
    }

    // Arrive, then clear the island. The leading MoveTo is the sticky-at-step-1
    // rule and its radius is the objective's own arriveRadius, so arriving IS
    // being inside it (a larger gap is the leading-MoveTo deadlock).
    //
    // `poke` is a creature that must be HIT before the island holds anything to
    // clear: Urom never aggroes (his MoveInLineOfSight is empty) and his pack only
    // exists once he is struck. The step walks the tank into him and is Done once
    // he has teleported out of UROM_POKE_RADIUS. A ClearRadius alone can never do
    // this — NearestHostileNearPoint skips every roster boss entry, so each
    // platform was certified empty and the party flew on without him.
    //
    // `sweep` walks the island stop by stop after the arrival (the central ring's
    // arcs, R2C_SWEEP): a MoveTo hop for a zero radius, a ClearRadius otherwise.
    // The island's own clear follows when its row has one.
    DungeonEvent SiteClear(uint32 eventId, int32 order, char const* name, uint8 site,
                           std::vector<uint32> const& onlyEntries, bool constructGuard,
                           std::vector<OcRingStop> const& sweep = {}, uint32 poke = 0)
    {
        OcSite const& s = SITES[site];
        EventBuilder b(MAP_ID, eventId, name);
        b.Anchored(order).Persistent().MoveTo(s.padX, s.padY, s.padZ, OBJ_ARRIVE);

        if (poke)
            b.KillCreatureEngage(poke, 1, UROM_POKE_RADIUS).Timeout(SITE_TIMEOUT_MS);

        auto const clear = [&](float x, float y, float z, float radius)
        {
            b.ClearRadius(x, y, z, radius, s.islandZBand);
            if (!onlyEntries.empty())
                b.OnlyEntries(onlyEntries);
            b.Timeout(SITE_TIMEOUT_MS);
        };
        for (OcRingStop const& stop : sweep)
        {
            if (stop.radius > 0.0f)
                clear(stop.x, stop.y, stop.z, stop.radius);
            else
                b.MoveTo(stop.x, stop.y, stop.z, RING_HOP_ARRIVE).Timeout(RING_HOP_TIMEOUT_MS);
        }
        if (s.clearRadius > 0.0f)
            clear(s.clearX, s.clearY, s.clearZ, s.clearRadius);

        // The last construct island: a missed construct is a VISIBLE hold on the
        // counter here, not a silent skip to a Varos who is still NON_ATTACKABLE.
        if (constructGuard)
            b.MoveToHoldUntilInstanceData(s.padX, s.padY, s.padZ, OBJ_ARRIVE + 4.0f, DATA_CC_COUNT, CC_TOTAL)
                .Timeout(CC_HOLD_TIMEOUT_MS);
        return b.Build();
    }
}

bool OculusDriverDue(Player* bot)
{
    if (!bot || bot->GetMapId() != MAP_ID)
        return false;
    InstanceScript* const inst = DcTargeting::GetInstanceScript(bot);
    if (!inst || inst->GetData(DATA_DRAKOS) != STATE_DONE)
        return false;
    return inst->GetData(DATA_EREGOS) != STATE_DONE || AnyMemberMounted(bot);
}

void RegisterOculusEvents(std::vector<DungeonEvent>& out)
{
    // (1) THE NEXUS PORTAL — walkthrough steps 1 and 2.
    //
    // The portal GO casts 49305 on its clicker, and 49305 is a spell_target_position
    // row on Drakos's ring. TeleportParty lands the leader and every bot on exactly
    // that point: indistinguishable from five clicks, with the follower pull, the
    // combat drop and the target invalidation built in. The objective's navigation
    // walks the tank across the entrance floor to the checkpoint first; the
    // blocking-trash rungs clear the 120yd of Azure trash on the way.
    //
    // The Orb of the Nexus 8yd from the entrance is the EXIT. Nothing here clicks
    // gameobjects, and t/TestOculus.cpp pins this anchor to the portal, not the orb.
    out.push_back(
        EventBuilder(MAP_ID, EVENT_PORTAL, "The Nexus Portal")
            .Anchored(ORDER_PORTAL)
            .Persistent()
            .TeleportParty(PORTAL_X, PORTAL_Y, PORTAL_Z, PORTAL_LAND_X, PORTAL_LAND_Y, PORTAL_LAND_Z, PORTAL_RADIUS)
                .Timeout(PORTAL_TIMEOUT_MS)
            .Build());

    // (2)-(4) THE CONSTRUCT ISLANDS — ten Centrifuge Constructs across three
    // islands, and only all ten dead makes Varos attackable. Filtered to the ground
    // entries so a hovering Azure Ring Guardian inside the volume is never the
    // thing the party walks off the rim after.
    std::vector<uint32> const ring2(std::begin(RING2_CLEAR_ENTRIES), std::end(RING2_CLEAR_ENTRIES));
    // The central ring is walked round from its landing, arc by arc (R2C_SWEEP).
    std::vector<OcRingStop> const ringSweep(std::begin(R2C_SWEEP), std::end(R2C_SWEEP));
    out.push_back(SiteClear(EVENT_R2C, ORDER_R2C, "Centrifuge Constructs (central ring)", SITE_R2C, ring2, false,
                            ringSweep));
    out.push_back(SiteClear(EVENT_R2S, ORDER_R2S, "Centrifuge Constructs (south pad)", SITE_R2S, ring2, false));
    out.push_back(SiteClear(EVENT_R2N, ORDER_R2N, "Centrifuge Constructs (north pad)", SITE_R2N, ring2, true));

    // (5)-(7) UROM'S PLATFORMS — hit him, then clear. Striking him anywhere but
    // the inner arena makes him cast Summon Menagerie (a TELEPORT_UNITS spell):
    // four summons at his feet, and he is gone to the next platform. He never
    // starts it himself, so the poke step comes first; the unfiltered clear then
    // takes the pack.
    out.push_back(SiteClear(EVENT_UROM_P0, ORDER_UROM_P0, "Mage-Lord Urom (platform 0)", SITE_R3P0, {}, false, {}, NPC_UROM));
    out.push_back(SiteClear(EVENT_UROM_P1, ORDER_UROM_P1, "Mage-Lord Urom (platform 1)", SITE_R3P1, {}, false, {}, NPC_UROM));
    out.push_back(SiteClear(EVENT_UROM_P2, ORDER_UROM_P2, "Mage-Lord Urom (platform 2)", SITE_R3P2, {}, false, {}, NPC_UROM));

    // (8) LEY-GUARDIAN EREGOS — fought entirely from the drakes, so this objective
    // is only the landing afterwards: arrive on the Ring 4 floor, hold until the
    // slot reads DONE (it already does by the time anyone stands here), with hook
    // 39's telemetry while it holds.
    OcSite const& r4 = SITES[SITE_R4];
    out.push_back(
        EventBuilder(MAP_ID, EVENT_EREGOS, "Ley-Guardian Eregos")
            .Anchored(ORDER_EREGOS)
            .Persistent()
            .MoveTo(r4.padX, r4.padY, r4.padZ, EREGOS_ARRIVE)
            .MoveToHoldUntilInstanceData(r4.padX, r4.padY, r4.padZ, EREGOS_ARRIVE, DATA_EREGOS, STATE_DONE)
                .WhileHolding(HOOK_OC_EREGOS)
                .Timeout(EREGOS_TIMEOUT_MS)
            .Build());

    // (9) THE ASCENT — the conditional driver.
    //
    // ONE Custom step, for the Culling / Trial of the Champion reason: what the
    // ascent needs is a standing answer re-decided every tick from live state, not
    // a sequence a wipe would leave half-run.
    //
    // DRIVES IN COMBAT only so it can SEE combat ticks and hand them back — every
    // on-foot fight is the stock engine's, and the Eregos fight is the riders'.
    //
    // STEPS OWN MOVEMENT because the per-tick position hold would pin the tank on
    // the pad it is supposed to be taking off from, and because it is what makes a
    // Done return YIELD.
    //
    // OWNS THE PULL: the advanced pull's drag-back camp is meaningless on an island
    // twenty yards across, and a camp stamped on one island is unreachable from
    // the next.
    //
    // YIELDS THE APPROACH: every tick the driver moves the party it claims (Hold);
    // it yields only with the tank on foot on the destination island, and then the
    // walk to the objective's arrival — an off-line rejoin included — is Advance's.
    //
    // REPEATABLE because it never completes — the predicate going false ends it —
    // and so its step timeout re-arms rather than skips. PERSISTENT so a combat gap
    // cannot rewind it.
    out.push_back(
        EventBuilder(MAP_ID, EVENT_DRIVER, "The Oculus ascent")
            .Conditional(&OcDriverDue)
            .Repeatable()
            .Persistent()
            .Optional()
            .OwnsThePull()
            .YieldsTheApproach()
            .DrivesInCombat()
            .StepsOwnMovement()
            .Custom(HOOK_OC_DRIVER)
                .Timeout(DRIVER_TIMEOUT_MS)
            .Build());
}

// --- the roster: three boss rows, eight objectives ---------------------------------
//
// encounterIndex 0 on every objective: an objective's index is an ordering hint
// only, and given a real bit the completed-mask check would read the objective
// done the instant its encounter credit landed. The order is orderOverride alone.
//
// Varos and Urom are remove + re-add rather than `reorder` because only an add can
// carry hand-authored coordinates; completionFrom = their own entries keeps their
// real kill bits (1 and 2). Eregos is removed for good: his spawn is 53.6yd above
// the highest mesh, NavmeshSnap's vertical extent is a fixed 10yd, and a row that
// never snaps never fires its at-boss trigger. OBJ(8) replaces him, and bit 3
// still lands on his death.
void RegisterOculusRoster(std::vector<BossRosterPatch>& t)
{
    using namespace DcRoster;

    OcSite const& r2c = SITES[SITE_R2C];
    OcSite const& r2s = SITES[SITE_R2S];
    OcSite const& r2n = SITES[SITE_R2N];
    OcSite const& r2v = SITES[SITE_R2V];
    OcSite const& p0 = SITES[SITE_R3P0];
    OcSite const& p1 = SITES[SITE_R3P1];
    OcSite const& p2 = SITES[SITE_R3P2];
    OcSite const& r4 = SITES[SITE_R4];
    Pt3 const& arena = UROM_CORDS[3];

    BossRosterPatch p;
    p.mapId = MAP_ID;
    p.remove = { NPC_VAROS, NPC_UROM, NPC_EREGOS };
    // Drakos keeps his derived anchor and bit 0; only his place in the order moves.
    p.reorder = { { NPC_DRAKOS, ORDER_DRAKOS } };
    p.add = {
        // The portal itself, NOT the Orb of the Nexus 8yd from the entrance.
        MakeObjective(OBJ(EVENT_PORTAL), /*encounterIndex*/ 0, MAP_ID, "The Nexus Portal",
                      PORTAL_X, PORTAL_Y, PORTAL_Z, PORTAL_RADIUS,
                      /*gateEntry*/ 0, /*hook*/ 0, /*eventId*/ EVENT_PORTAL, /*orderOverride*/ ORDER_PORTAL),

        MakeObjective(OBJ(EVENT_R2C), 0, MAP_ID, "Centrifuge Constructs (central ring)",
                      r2c.padX, r2c.padY, r2c.padZ, OBJ_ARRIVE, 0, 0, EVENT_R2C, ORDER_R2C),
        MakeObjective(OBJ(EVENT_R2S), 0, MAP_ID, "Centrifuge Constructs (south pad)",
                      r2s.padX, r2s.padY, r2s.padZ, OBJ_ARRIVE, 0, 0, EVENT_R2S, ORDER_R2S),
        MakeObjective(OBJ(EVENT_R2N), 0, MAP_ID, "Centrifuge Constructs (north pad)",
                      r2n.padX, r2n.padY, r2n.padZ, OBJ_ARRIVE, 0, 0, EVENT_R2N, ORDER_R2N),

        // On the west edge of his platform, 36yd from him: the pad the drakes land on.
        MakeBoss(NPC_VAROS, MAP_ID, "Varos Cloudstrider", r2v.padX, r2v.padY, r2v.padZ,
                 /*completionFrom*/ NPC_VAROS, /*orderOverride*/ ORDER_VAROS),

        MakeObjective(OBJ(EVENT_UROM_P0), 0, MAP_ID, "Mage-Lord Urom (platform 0)",
                      p0.padX, p0.padY, p0.padZ, OBJ_ARRIVE, 0, 0, EVENT_UROM_P0, ORDER_UROM_P0),
        MakeObjective(OBJ(EVENT_UROM_P1), 0, MAP_ID, "Mage-Lord Urom (platform 1)",
                      p1.padX, p1.padY, p1.padZ, OBJ_ARRIVE, 0, 0, EVENT_UROM_P1, ORDER_UROM_P1),
        MakeObjective(OBJ(EVENT_UROM_P2), 0, MAP_ID, "Mage-Lord Urom (platform 2)",
                      p2.padX, p2.padY, p2.padZ, OBJ_ARRIVE, 0, 0, EVENT_UROM_P2, ORDER_UROM_P2),

        // cords[3], where he is actually fought (AttackStart only works within 55yd
        // of the arena centre).
        MakeBoss(NPC_UROM, MAP_ID, "Mage-Lord Urom", arena.x, arena.y, arena.z,
                 /*completionFrom*/ NPC_UROM, /*orderOverride*/ ORDER_UROM),

        MakeObjective(OBJ(EVENT_EREGOS), 0, MAP_ID, "Ley-Guardian Eregos",
                      r4.padX, r4.padY, r4.padZ, EREGOS_ARRIVE, 0, 0, EVENT_EREGOS, ORDER_EREGOS),
    };
    t.push_back(std::move(p));
}
