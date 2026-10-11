/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCDIAGSNAPSHOT_H
#define _PLAYERBOT_DCDIAGSNAPSHOT_H

#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

class Player;

// A full read-only picture of a dungeon-clear run at one instant: where every
// party member is, what they are doing, which boss the run is on, how the route
// to it looks, and every watchdog counter that could explain a wedge.
//
// Written for the test-run harness's failure path, where a verdict token
// ("no_progress") on its own is not enough to diagnose anything: the run ends,
// the party is disbanded and logged out, and every bit of state that would have
// explained the failure is gone. Capture() must therefore be called BEFORE
// teardown disbands the group.
//
// Capture() is strictly read-only — it never mutates run state, so it is safe
// to call from a watchdog tick as well as from teardown. Note in particular
// that DungeonPathFollower::IsOffPath mutates offPathTicks and is deliberately
// NOT used here; route deviation comes from the pure RouteDeviation helper.
namespace DcDiag
{
    // What is under one unit: the walkable navmesh in its column, the ground the
    // core resolves a fall to, and the bare terrain grid.
    //
    // Written for the Culling of Stratholme King's Square freeze
    // (tp-20260910-121847-3, 2/100): two tanks ended 10-12yd UNDER the street,
    // flagged in combat with a zombie and a ghoul at full health for ten
    // minutes, and the record held their z and nothing to compare it with. Map
    // 595 is one of the flat-grid maps, so a missed VMAP ray drops a unit onto a
    // sheet at z 0.14 — whether that is what happened, or they stood on geometry
    // the mesh does not cover, is exactly what these three numbers separate.
    struct FloorProbe
    {
        bool probed = false;     // false => not measured (off-map, or no map)
        bool meshOk = false;     // a navmesh poly exists in the unit's column
        float meshZ = 0.f;       // height of the nearest poly in that column
        float groundZ = 0.f;     // WorldObject::GetMapHeight from just above the unit
        float gridZ = 0.f;       // Map::GetGridHeight — terrain alone, no VMAP
    };

    // A unit this far or more below the navmesh in its own column is under the
    // floor, not standing on a lower one: a real lower floor has its own poly,
    // and the column search returns the nearest in z. The slack covers a unit
    // on a slope or a step edge that the mesh rounds a yard or two off.
    constexpr float kUnderMeshYd = 3.0f;
    bool IsUnderMesh(FloorProbe const& floor, float z);

    // One copy of the escortee found on the map, alive or not, near or not.
    struct EscorteeCopy
    {
        std::uint64_t guid = 0;
        std::uint32_t spawnId = 0;    // 0 => a summon, not a DB spawn
        bool alive = false;
        std::uint32_t deathState = 0; // Unit::getDeathState, raw
        bool inWorld = false;
        float x = 0.f, y = 0.f, z = 0.f;
        float homeX = 0.f, homeY = 0.f, homeZ = 0.f;  // an instance Reposition moves this
        float distToTank = -1.f;
        bool moving = false;
        bool inCombat = false;
        bool evading = false;
        bool gossipFlag = false;
        std::uint32_t faction = 0;
        // Creature::IsUpdateNeeded — false is the 170yd freeze: a DB-spawned NPC
        // nobody is near drops off the update list and runs no AI at all.
        bool updateNeeded = false;
        bool gridLoaded = false;      // Map::IsGridLoaded at his own position
        bool seenByGridScan = false;  // also found by a grid search around the tank
    };

    // The active EscortCreature step, and where its escortee actually is.
    //
    // Written for the Culling of Stratholme Town Hall stall (tp-20260910-121847-3,
    // 6/100): "can't keep up with the escort" is the driver's escortee-ABSENT
    // branch, and all six tanks held on Arthas's WP14 corner, 75-95yd from where
    // he waits out the waves and well inside the 200yd search. Why the driver's
    // FindNearestCreature found nobody — dead, somewhere else, or there and
    // unseen — the record could not say. `driverSees` repeats the driver's exact
    // call; the copies list is every creature of the entry on the map, so the
    // two disagreeing is itself the answer.
    struct EscortSnapshot
    {
        bool active = false;          // the tank's active event step is an EscortCreature
        std::uint32_t eventId = 0;
        std::string eventName;
        std::uint32_t stepIndex = 0;
        std::uint32_t entry = 0;
        float searchRadius = 0.f;     // the driver's resolve radius, default applied
        bool driverSees = false;      // FindNearestCreature(entry, searchRadius, alive)
        std::uint64_t driverSeesGuid = 0;
        bool deadAirSeen = false;     // escortProgressMs has been stamped
        std::uint32_t deadAirMs = 0;  // since the escort last counted as progress
        std::int32_t instanceDataId = -1;
        std::uint32_t instanceData = 0;
        std::uint32_t instanceDataMin = 0;
        std::uint32_t copyCount = 0;  // total before the cap below
        std::vector<EscorteeCopy> copies;
    };

    // One unit holding a party member in combat, with every input the
    // phantom-combat hatch weighs when it decides whether that hold is a REAL
    // fight — see DungeonClearTriggers.cpp's HasLegitimateCombatHolder, which
    // this mirrors field for field.
    //
    // This exists because "the party is stuck in combat" was, for a long time,
    // the end of what the record could say. The freeze it was written for
    // (tr-20260803-194801-16 / -12, MGT heroic) ended with two members flagged,
    // no victim, no attackers, full health, for seven minutes — and NOTHING in
    // the logs or the snapshot named the unit responsible, so the one question
    // that decides the fix ("is the hatch standing down, and on which guard?")
    // could only be reasoned about, never read. Now it is a field.
    //
    // `legitimate` is this holder's own verdict — the value the trigger's loop
    // would `return true` on. A member whose holders are ALL non-legitimate is
    // in phantom combat and the hatch should be counting down; a member with a
    // legitimate holder is one the hatch will never touch, and the remaining
    // fields say which guard saved it.
    struct CombatHolderSnapshot
    {
        std::string name;
        std::uint32_t entry = 0;      // creature entry; 0 for a player holder
        std::uint64_t guid = 0;
        bool isCreature = false;
        bool pvp = false;             // came from the PvP ref map, not PvE
        bool alive = false;
        bool sameMap = false;
        bool evading = false;         // CombatManager::IsInEvadeMode — bailing home
        bool suppressed = false;      // a suppressed ref does not hold the flag
        // DcEngageGeometry::IsReachable — THE guard that decides most freezes.
        // Only evaluated once alive/sameMap/!evading all pass (the pathfind is
        // the expensive part and the trigger short-circuits the same way), so
        // `reachable == false` means one of two different things and
        // `reachChecked` is what tells them apart: a path test that FAILED
        // versus one that was never run. Without the second field a leashed,
        // evading holder reads as "unreachable", which is the opposite of the
        // truth and points the reader at the navmesh instead of the leash.
        bool reachable = false;
        bool reachChecked = false;
        bool canAttackMe = true;      // CreatureAI::CanAIAttack(member)
        // CREATURE_FLAG_EXTRA_TRIGGER: an invisible script helper no bot can
        // target, so the party can never resolve the reference however reachable
        // and willing the holder is. Always sinks `legitimate` — see
        // DungeonClearMath::IsUnresolvableCombatHolder.
        bool trigger = false;
        // A boss can be ALIVE, reachable and allowed to attack while being
        // scripted out of the fight entirely. MgT's Kael'thas clamps damage to
        // health-1, goes SetImmuneToAll + REACT_PASSIVE, despawns his summons and
        // schedules KillSelf ELEVEN SECONDS later — and for all eleven the blame
        // row read `0% reachable -> LEGITIMATE`, which is true and tells the reader
        // nothing. Two runs in tp-20260808-162331-1 were disabled inside that
        // window, already won, and it took reading the boss script to find out why.
        // These three fields are that answer, printed where the question is asked.
        bool immune = false;          // Unit::IsImmuneToAll
        bool passive = false;         // Creature::GetReactState == REACT_PASSIVE
        bool atOneHp = false;         // health == 1: a damage clamp, not a rounding
        float dist = -1.f;            // -1 when on another map
        std::uint32_t healthPct = 0;
        std::string victim;           // what the holder itself is fighting, if anything
        bool legitimate = false;      // the verdict HasLegitimateCombatHolder derives
        float x = 0.f, y = 0.f, z = 0.f;
        FloorProbe floor;             // probed only when on the member's map
    };

    // One party member as seen at capture time. Positions are world coords in
    // the member's own map — a member on a different mapId than the tank has
    // been left behind outside the instance (or is mid-teleport).
    struct MemberSnapshot
    {
        std::string name;
        std::string className;
        std::uint64_t guid = 0;
        std::uint32_t level = 0;
        bool isBot = true;
        bool online = false;      // false => resolved from the group's member slots only
        std::uint32_t mapId = 0;
        float x = 0.f, y = 0.f, z = 0.f;
        float distToTank = 0.f;   // -1 when on a different map (distance is meaningless)
        FloorProbe floor;         // probed for every online member
        // ON A VEHICLE: the base carries the member, and its position is the seat's.
        // A rider is AIRBORNE by design on The Oculus, so the under-the-floor
        // verdict below is suppressed for it — a drake 50yd over a ring reads as
        // "under the mesh" of the ring above, five times a tick, otherwise.
        bool onVehicle = false;
        std::uint32_t baseEntry = 0;
        float baseX = 0.f, baseY = 0.f, baseZ = 0.f;
        bool seatCanControl = false;
        bool alive = false;
        std::uint32_t healthPct = 0;
        std::uint32_t manaPct = 0;   // 0 for non-mana classes
        bool inCombat = false;
        std::string victim;       // name of GetVictim(), "" when not fighting
        bool dcStrategy = false;      // has the "dungeon clear" strategy
        bool dcCombatStrategy = false;

        // HEARTBEAT: how long ago this member's own DC trigger ladder last ran
        // (DcTickHeartbeat, stamped on both engines before any gate).
        // `dcTickSeen == false` means it has never run for this bot.
        //
        // This is the field that separates "DC stood every rung down" from "this
        // bot's AI is not being updated", which every other column reports
        // identically. Read it per MEMBER, not just off the tank: the freeze it
        // was written for is precisely a tank whose age climbs into the hundreds
        // of seconds while its four followers stay in the tens of milliseconds.
        std::uint32_t dcTickAgeMs = 0;
        bool dcTickSeen = false;

        // --- combat blame (populated only while inCombat) ------------------
        // WHICH ENGINE the bot is actually running, which is a different
        // question from whether it is FLAGGED. `drop target` (stock, relevance
        // 99) can move a still-flagged bot onto the non-combat engine, where
        // every DC rung bails on IsInCombat() and every combat rung is out of
        // reach — the bot goes silent and nothing drives it. "noncombat" here
        // next to inCombat=true IS that state, stated outright.
        std::string botState;         // "combat" | "noncombat" | "" (not a bot)
        std::uint32_t attackerCount = 0;   // getAttackers().size()
        std::vector<CombatHolderSnapshot> combatHolders;
        std::uint32_t holderRefCount = 0;  // total refs before the cap below
        // No attacker, no victim, and no legitimate holder — the exact predicate
        // DungeonClearMath::IsPhantomCombat evaluates. True here means the hatch
        // is (or should be) counting down against StuckCombatTimeout.
        bool phantomCombat = false;
    };

    // One roster anchor (boss or objective) with the completion verdict derived
    // exactly as NextDungeonBossValue derives it — all three completion paths,
    // not just the DBC kill-bit.
    struct BossSnapshot
    {
        std::uint32_t entry = 0;
        std::uint32_t orderKey = 0;
        std::string name;
        std::string kind;         // "boss" | "objective"
        std::string status;       // "dead" | "skipped" | "alive" | "missing"
        std::string doneVia;      // "" | "mask" | "bossState" | "anchor" — WHICH path
        std::int32_t encounterIndex = -1;
        float x = 0.f, y = 0.f, z = 0.f;
        bool isTarget = false;    // the run's current NextDungeonBoss
        bool isSticky = false;    // the committed StickyBoss
    };

    struct Snapshot
    {
        bool valid = false;       // false => the tank could not be resolved
        std::string capturedAt;   // "teardown" | "sample" | "frozen"

        // --- run switches -------------------------------------------------
        bool enabled = false;
        bool paused = false;
        std::string pauseReason;
        bool pausedAtDoor = false;
        std::uint32_t selectedBossEntry = 0;
        bool smartRestLatched = false;

        // --- state-machine position ---------------------------------------
        std::string phase;        // raw DcKey::Phase token
        std::string stateStr;     // publisher-synthesized state
        std::string detail;       // publisher-synthesized human sentence
        std::string stallReason;  // THE field — why the run says it is stuck

        // --- target -------------------------------------------------------
        std::uint32_t stickyBoss = 0;
        std::uint32_t nextBossEntry = 0;
        std::string nextBossName;
        std::uint32_t committedTargetEntry = 0;  // approach's committed boss
        std::uint32_t approachTargetEntry = 0;   // boss the BUILT ROUTE leads to
        float distToTarget = -1.f;
        bool targetMismatch = false;  // the above disagree — usually IS the bug

        // --- route --------------------------------------------------------
        bool pathReachable = false;
        bool pathComplete = false;
        bool pathStartFarFromPoly = false;
        std::string pathFailureReason;
        std::uint32_t pathSegments = 0;
        std::uint32_t segmentIdx = 0;
        std::uint32_t pointIdx = 0;
        std::uint32_t offPathTicks = 0;
        float routeDeviation = -1.f;   // -1 = not measurable, NOT "on the corridor"
        bool cursorPastPathEnd = false;  // route consumed but target not reached

        // --- wedge watchdogs ----------------------------------------------
        std::uint32_t routeGlideStuck = 0;
        std::uint32_t doorWalkInStuck = 0;
        std::uint32_t pursuitStuck = 0;
        std::uint32_t finalApproachStuck = 0;
        std::uint32_t stuckCount = 0;
        std::uint32_t rebuildAttempts = 0;
        std::uint32_t resnapAttempts = 0;
        std::uint32_t partyNotReadyTicks = 0;

        // --- door ---------------------------------------------------------
        bool doorStalled = false;
        std::uint32_t doorStalledForMs = 0;

        // --- pull ---------------------------------------------------------
        std::uint32_t pullSetting = 0;
        std::uint32_t pullPhase = 0;
        std::uint32_t pullDecision = 0;
        std::uint32_t pullPhaseForMs = 0;
        std::uint32_t pullFizzleCount = 0;
        bool pullHasCamp = false;

        // --- world / party ------------------------------------------------
        std::uint32_t mapId = 0;
        std::uint32_t instanceId = 0;
        float tankX = 0.f, tankY = 0.f, tankZ = 0.f;
        bool tankInCombat = false;
        bool tankMoving = false;
        std::string tankVictim;
        std::uint32_t completedEncounterMask = 0;
        std::uint32_t partySize = 0;
        std::uint32_t aliveCount = 0;    // of the ONLINE members only
        std::uint32_t offlineCount = 0;  // in the group but not in the world
        std::uint32_t inCombatCount = 0;
        std::uint32_t clearedAnchors = 0;
        std::uint32_t skippedCount = 0;

        // The tank's own heartbeat, mirrored out of its member row so the
        // one-line Summarize can carry it. See MemberSnapshot::dcTickAgeMs.
        std::uint32_t dcTickAgeMs = 0;
        bool dcTickSeen = false;

        std::vector<MemberSnapshot> members;
        std::vector<BossSnapshot> roster;

        EscortSnapshot escort;    // serialized only while an escort step is active
    };

    // Read every field above off the leader tank. Safe on a null/despawned
    // tank (returns valid == false) and on a solo bot with no group.
    Snapshot Capture(Player* tank, char const* capturedAt);

    // Match the recovery trigger's PvE-only holder verdict while retaining PvP
    // refs for diagnostic display.
    bool IsLegitimatePvECombatHolder(bool isPvp, bool holderIsLegitimate);

    // An empty PvE ref map is the trigger's opaque-combat branch. A non-empty
    // PvE ref map is legitimate only when at least one PvE holder is legitimate.
    bool HasLegitimatePvECombatHolder(bool hasPvERefs, bool anyLegitimatePvEHolder);

    // Serialize as a JSON object (no trailing comma, no key) onto the stream,
    // matching DcTestRunRecord's hand-rolled JSONL style.
    void AppendJson(std::ostringstream& s, Snapshot const& snap);

    // One-line human summary for the worldserver log — the fields that most
    // often identify a wedge, in a form that greps well.
    std::string Summarize(Snapshot const& snap);

    // "Who is keeping us in combat", one line, for the freeze that Summarize can
    // only flag. Names every holder of every flagged member and the verdict each
    // one earns, so a stuck run says whether the phantom-combat hatch is armed
    // and — if it is not — which guard is holding it back. Safe on an invalid
    // snapshot and on a party nobody is fighting; both answer in words.
    std::string SummarizeCombat(Snapshot const& snap);
}

#endif  // _PLAYERBOT_DCDIAGSNAPSHOT_H
