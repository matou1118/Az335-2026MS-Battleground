/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCRELEVANCE_H
#define _PLAYERBOT_DCRELEVANCE_H

// The dungeon-clear trigger relevance ladder, as named constants shared by BOTH
// strategies (DungeonClearStrategy = non-combat, DungeonClearCombatStrategy =
// combat) instead of float literals scattered across the InitTriggers bodies
// with the invariants living only in prose. t/TestRelevanceLadder.cpp pins the
// ordering and documents every same-value tie with the partition (role / engine /
// map / anchor-kind) that legitimizes it, so a future edit that reorders a rung —
// or lands a new feature on an occupied one — trips a red test instead of
// silently depending on trigger registration order.
//
// TIES THAT ARE REAL (measured, arch-review F3) and how they are handled:
//   - Chat == PartyDied (100): distinct trigger conditions (keyword vs death),
//     both terminal — kept equal, asserted.
//   - AtBoss == AtObjective (30): mutually exclusive by the anchor-kind check in
//     each trigger — kept equal, asserted.
//   - BlockingTrash == FollowTank (25): leader-only vs follower-only — kept
//     equal, asserted.
//   - PullManeuver == StayAtCamp (60, combat): leader-only vs follower-only —
//     kept equal, asserted.
// TIES THAT WERE NOT PARTITIONED (both armable on the same leader) — BROKEN here
// so ordering is deterministic:
//   - HakkarFlame was 35, tying Pull (35). Now 35.5 (still below the 36
//     suppressor, above the 34 loot-blood): on the Sunken Temple carrier a douse
//     outranks starting a fresh pull.
//   - NeedsRest (drink/eat) was 26, tying RoomTrash (26). Now 26.5: a leader
//     below its rest target tops up before committing to a room pre-clear,
//     matching the rest trigger's "top up before pulling" intent.
namespace DcRel
{
    // ===== shared top band =====
    inline constexpr float Chat            = 100.0f; // chat keyword commands (dc on/off/…)
    inline constexpr float PartyDied       = 100.0f; // death bailout (non-combat only)
    inline constexpr float LootRollPending = 95.0f;  // resolve an open loot-roll window at once
    inline constexpr float DoorReopened    = 90.0f;  // auto-resume when a player opens the door
    inline constexpr float AllCleared      = 50.0f;  // congratulate + disable
    // Stranded-member recovery failsafe (leader-only, non-combat). When the run
    // has frozen for StrandedRecoveryNoProgressSecs with a bot member stuck out of
    // range, teleport the strays to the tank. Sits ABOVE the whole leader driving
    // ladder (which has, by definition, been failing to make progress for minutes)
    // so the recovery wins the tick it arms; its narrow trigger keeps it inert
    // otherwise. Below AllCleared (a finished run should congratulate + disable,
    // not teleport). See Util/DcStrandedRecovery.
    inline constexpr float StrandedRecovery = 42.0f; // leader: rescue a stuck member
    inline constexpr float HealReposition  = 41.0f;  // healer-only; both engines (see note below)

    // ===== non-combat leader driving ladder =====
    // BLACKWING LAIR ONLY, every member but the leader: hold the pack inside one
    // leash around the leader's route cursor while it crosses the Suppression
    // Rooms. Registered in BOTH engines (the crossing has out-of-combat ticks and
    // the non-combat driving ladder owns the followers on those), and listed here
    // rather than in the combat block only because its higher-value siblings are.
    //
    // 36 is chosen against what it must OUTRANK, which on this leg is the whole
    // reason the pack strings out: stock MoveChase (~30), AssistCampCombat (35)
    // and RegroupCombat (29) — a follower chasing a whelp 40yd off the line is
    // exactly the string-out this exists to stop, and a strung-out raid sweeps a
    // far larger cylinder of a room whose spawns are the problem. It also clears
    // the non-combat follower rungs it would otherwise lose to (AssistCamp 29,
    // HoldAtCamp 28, FollowTank 25, Advance 15) and RezParty (31.5), which is
    // deliberate and the same call the Razorgore camp makes: walking a rezzer 40yd
    // back across the gauntlet for a corpse is not a recovery, it is a second
    // death.
    //
    // It stays BELOW everything that must still win: StrandedRecovery (42) and
    // HealReposition (41) above it, and in the combat engine the camp owners (60),
    // HazardVacate (55) and the phantom hatch (65) — none of which contend on this
    // leg, and all of which should if they ever did.
    //
    // TIES HakkarSuppressor (36, non-combat, Sunken Temple). Map-partitioned: map
    // 109 and map 469 cannot both be under a bot's feet. Asserted in
    // t/TestRelevanceLadder.cpp with the rest of the legitimate ties.
    inline constexpr float TransitPack      = 36.0f; // raid: hold the transit pack together
    inline constexpr float HakkarSuppressor = 36.0f; // ST Hakkar: silence a resetting suppressor
    inline constexpr float HakkarFlame      = 35.5f; // ST Hakkar: douse (tie-broken above Pull)
    inline constexpr float Pull             = 35.0f; // advanced/dynamic pull-to-camp maneuver
    inline constexpr float HakkarLootBlood  = 34.0f; // ST Hakkar: grab blood (below flame)
    // Post-combat rez driver. Runs on ALL bots (the elected rezzer may be a
    // follower OR the leader — a prot paladin raising its healer), so it must
    // outrank BOTH ladders it can land on: on the leader, EventDue (31) and
    // AtBoss/AtObjective (30) — recover the party before running an event gate
    // or pulling the next boss; on a follower, every follower rung (assist 29 /
    // hold-at-camp 28 / follow-tank 25). 31.5 (not the plan's proposed 31,
    // which EventDue already occupies with no role/engine partition — both are
    // leader-armable on the same tick). Kept BELOW Pull (35): the pull trigger
    // gates on the between-pulls readiness, which the rez IsPending hold makes
    // false, so Pull is inert during a recovery anyway; and below the Hakkar
    // band (34-36, map-partitioned mid-encounter orchestration).
    inline constexpr float RezParty         = 31.5f; // elected rezzer: raise the corpse
    inline constexpr float EventDue         = 31.0f; // off-path conditional event gate due
    inline constexpr float AtBoss           = 30.0f; // engage the next boss
    inline constexpr float AtObjective      = 30.0f; // arrive at a travel objective (anchor-kind peer)
    inline constexpr float AssistCamp       = 29.0f; // follower: pile into the leader's fight
    inline constexpr float HoldAtCamp       = 28.0f; // follower: hold passive at camp mid-pull
    inline constexpr float NeedsRest        = 26.5f; // rest to target HP/mana (tie-broken above RoomTrash)
    inline constexpr float RoomTrash        = 26.0f; // room-aggro boss pre-clear
    inline constexpr float BlockingTrash    = 25.0f; // leader: engage a pack blocking the path
    inline constexpr float FollowTank       = 25.0f; // follower: redirect follow to the tank
    inline constexpr float LeaderAssist     = 24.0f; // leader: help a fight it never saw
    inline constexpr float DoorBlocked      = 22.0f; // stall at a shut door
    inline constexpr float Stalled          = 20.0f; // stalled-no-path fallback
    inline constexpr float RoomPreclearHold = 16.0f; // hold the room-aggro standoff
    inline constexpr float Advance          = 15.0f; // default: walk toward the next boss
    inline constexpr float FilterLoot       = 9.0f;  // enforce loot policy while paused

    // ===== combat engine =====
    // Phantom-combat escape hatch. A DC bot flagged in combat but with nothing it
    // can fight (no attacker, no victim, no reachable holder — e.g. a mob spawned
    // far across the map / behind a gate tagged it) force-clears its combat after a
    // long timeout. Highest combat rung so the recovery always wins the tick when it
    // legitimately fires; it never contends with real content because the trigger is
    // inert whenever anything is actually fightable. See
    // DungeonClearBreakStuckCombatTrigger / DungeonClearMath::ShouldBreakStuckCombat.
    //
    // Registered in BOTH engines (like HazardVacate), and listed here rather than in
    // the non-combat block only because the flag it clears is a combat one. The
    // non-combat half is the load-bearing one: `drop target` (stock, relevance 99)
    // can move a still-flagged bot onto the NON-combat engine, where every DC rung
    // bails on IsInCombat() and every combat rung — including this hatch, while it
    // was combat-only — is out of reach. 65 also tops the non-combat ladder
    // (HealReposition 41, HazardVacate 55), which is what it needs: the recovery
    // must beat the driving rungs it is unblocking. See DungeonClearStrategy.
    inline constexpr float BreakStuckCombat       = 65.0f; // phantom-combat force-clear
    inline constexpr float HakkarSuppressorCombat = 64.0f; // ST Hakkar combat side
    inline constexpr float HakkarFlameCombat      = 63.0f;
    inline constexpr float HakkarLootBloodCombat  = 62.0f;
    // Leader-only. COMBAT copy of the conditional-event rung, restricted to events
    // flagged DungeonEvent::drivesInCombat — a continuous WAVE encounter where the
    // event IS the fight and the driver must keep steering the tank under fire.
    //
    // 61 sits deliberately high: the driver's whole job in that shape is to move
    // the tank somewhere the stock combat engine would never take it (off the pack
    // it is tanking, across the arena, to the next rift), so it has to outrank
    // MoveChase (~30), the DC role repositions (assist 35 / regroup 29) and the
    // camp owners (60). It stays BELOW Hakkar (62-64) and the phantom-combat
    // hatch (65), neither of which can contend — Hakkar is another map, and the
    // hatch only fires when nothing is fightable at all.
    //
    // Inert on every map without a drivesInCombat event, which is all of them bar
    // Black Morass — the trigger resolves the flag before it fires.
    inline constexpr float EventDueCombat         = 61.0f; // leader: wave-encounter event driver
    // BLACKWING LAIR ONLY, one member: the elected orb runner's walk to the Orb of
    // Domination and the click itself (DungeonClearRazorgoreOrbTrigger).
    //
    // 62 because of what it has to beat on a raid map, which is not the DC ladder
    // at all — during a raid encounter every other DC rung is zeroed by the
    // stand-down (see DungeonClearCombatMultiplier) and the bot's tick belongs to
    // mod-playerbots' `bwl` strategy, whose nodes sit at ACTION_RAID (60) and
    // ACTION_RAID+1 (61). One of them, `bwl razorgore avoid aoe`, exists precisely
    // to walk bots out of the boss's frontal cone — and it would walk the runner
    // off the orb ledge every tick of the trip. So this has to outrank 61.
    //
    // Kept BELOW the Hakkar band (62-64, Sunken Temple — another map, cannot
    // contend) and the phantom-combat hatch (65). It ties nothing: it is the only
    // rung in the module gated on a single elected member of a single map's single
    // encounter, and it YIELDS (returns false) the moment the runner is parked, so
    // it owns the tick only while actually travelling or clicking.
    inline constexpr float RazorgoreOrb           = 62.0f; // runner: take the orb
    // The same encounter's other half: every member EXCEPT the runner, walking to
    // the camp at the foot of the orb platform. BOTH engines — the egg run has
    // out-of-combat ticks (the wave dies, the possessed boss attacks nobody), and
    // on those the non-combat driving ladder used to walk the tank at the boss
    // and the raid after it.
    //
    // 61.5 sits between the raid strategy's nodes (ACTION_RAID 60 / +1 61) and the
    // runner's rung (62), and both boundaries are deliberate. ABOVE 61 because
    // `bwl razorgore avoid aoe` exists to walk bots out of the boss's frontal cone
    // and would happily walk them out of the camp; BELOW the runner because if one
    // bot is somehow both, going for the orb is the more urgent job. The rung goes
    // INERT inside the leash rather than yielding, so a bot in position never
    // contends with its own rotation at all — the fine positioning inside the camp
    // stays the strategy's.
    inline constexpr float RazorgoreCamp          = 61.5f; // raid: hold the orb camp
    // Every member, BOTH engines: let go of a creature the run is forbidden to
    // damage right now (DcTargetExclusionRegistry). One tick's work — drop the
    // victim, clear the current target — and then the rung goes inert again.
    //
    // 61.25 puts it under the camp (61.5) and the orb (62) and over the raid
    // strategy's own nodes (ACTION_RAID 60 / +1 61), which is exactly the ordering
    // the Razorgore case needs: `bwl razorgore mark boss` sits at 61 and paints the
    // moon icon on a boss the raid must not kill, and RtiTargetValue short-circuits
    // the exclusion pass, so something above it has to take the target back off the
    // DPS. It ranks BELOW the positioning rungs because a bot in the wrong place is
    // the more urgent problem — and because dropping a target it is no longer
    // shooting at is free to defer by a tick.
    inline constexpr float HoldFire               = 61.25f; // drop a barred target

    // Registered in BOTH engines (like BreakStuckCombat / HazardVacate). The combat
    // registration is the working one; the NON-combat registration is a liveness net.
    // Every watchdog the maneuver owns — tag-leg and return-leg timeouts, the CC
    // abort, the arrive-at-camp release — is evaluated inside the action, so they
    // only tick while the action runs. An LOS-break pull ends with the tank unable to
    // see its own target, which is InvalidTargetValue's out-of-LOS clause, so stock
    // `drop target` (99) can move the tank to the non-combat engine mid-drag and
    // freeze the FSM in a holding phase with no clock running (Deadmines workshop,
    // tp-20260815-162044-2: Returning pinned 130-215s). 60 also clears the non-combat
    // ladder it lands in — above HazardVacate (55), below BreakStuckCombat (65).
    inline constexpr float PullManeuver           = 60.0f; // leader: drag the pack back to camp
    inline constexpr float StayAtCamp             = 60.0f; // follower: pin at camp (role peer of PullManeuver)
    // Survival: move OUT of an active-vacate hazard's pulse. Two shapes, one rung.
    // The Arcatraz "Destroyed Sentinel" (21761) is summoned on a Sentinel's death
    // at the corpse, is NOT_SELECTABLE (can't be fought), and pulses ~563-937 every
    // second in 15yd until it despawns. Scholomance's "Cloud of Disease" (17742) is
    // the same problem one tier down and with no creature at all — a persistent
    // area aura dropped where a Diseased Ghoul dies, 350/s in 5yd for 20s.
    // Either way the party is standing right on it after the kill and
    // nothing else moves them off, so they die where they stand. This drives EVERY
    // bot (no tank exemption — there is nothing to tank) out of the pulse, after
    // which normal driving advances them past it. Registered in BOTH engines because the summon
    // ticks after the kill, often once combat has already dropped. Outranks stock
    // MoveChase (~30), the DC role repositions (heal 41 / assist 35 / regroup 29),
    // and the whole non-combat driving ladder (advance 15, rest, loot). Kept BELOW
    // the combat camp owners (60) and Hakkar (62-64), which never contend (the
    // summon isn't a fight; different map than Hakkar), and BELOW the terminal
    // death/chat bailouts (100). See DungeonClearHazardVacate{Trigger,Action}.
    // HALLS OF REFLECTION ONLY, every member, BOTH engines: while the escape is
    // running, step FORWARD along the path whenever this bot is inside the Lich
    // King's Remorseless Winter ring or has fallen behind him.
    //
    // IT SITS ONE RUNG ABOVE HazardVacate (55) AND THAT IS THE WHOLE POINT. The
    // ring IS a hazard and it IS registered as one — but the generic vacate
    // retreats RADIALLY, directly away from the emitter, and on this encounter
    // "away from him" is the worst available direction. The path runs -x -y, he
    // follows the party down it, and every 2 seconds each player whose
    // (p.x - lk.x) + (p.y - lk.y) exceeds 20 takes 10 000 damage and a KNOCKBACK
    // THAT THROWS THEM FURTHER BEHIND — so a radial vacate fired on a bot that is
    // already behind aims it deeper into a rule that then reinforces itself. This
    // rung answers the same danger with the only move that works: toward the
    // party's stand point, which is ahead of the leader and ahead of him.
    //
    // 56 clears every stock combat mover (MoveChase ~30), the DC role repositions
    // (heal 41 / assist 35 / regroup 29) and the generic vacate it must
    // pre-empt, while staying under the camp owners (60) — which never contend,
    // because this map's two events carry OwnsThePull and there is no camp — and
    // under the terminal bailouts (100). Inert everywhere else: the trigger's
    // first test is the map id, its second the escape's boss state.
    // See DungeonClearHorStayAhead{Trigger,Action}.
    // THE OCULUS ONLY, every member, BOTH engines: the rider rung — gossip for an
    // essence, mount, fly this member's drake on its lane, land, dismount, hold
    // station on Eregos. Every member steers its OWN drake.
    //
    // 64.5 because of what it must beat and what must beat it. Above every DC
    // rung a rider could otherwise be handed (the camp owners 60, the event
    // driver 61, the hazard vacate 55) and above stock `occ drake attack` (15) —
    // though it returns false whenever it has nothing to steer, so the drake's
    // rotation still gets every idle tick. Below the phantom-combat hatch (65) and
    // the terminal bailouts (100). Half a rung over HakkarSuppressorCombat (64)
    // rather than on it, so the ladder carries no new tie.
    //
    // It is a plain Action, NOT a DcMovementAction: stock `wotlk-occ`'s
    // OccFlyingMultiplier zeroes every MovementAction on a mounted bot. It drives
    // the vehicle base's MotionMaster directly. See DungeonClearOculusRider{Trigger,Action}.
    inline constexpr float OcRider                = 64.5f; // any role: the Oculus drake rider
    // KARAZHAN ONLY, every member in the Gamesman's Hall, BOTH engines: the chess
    // rung — take the piece the conductor assigned, keep it, stand on the
    // sideline, fight nothing; and on the run owner, the conductor itself.
    //
    // It must own EVERY tick while the game is on (H1): a controller loses Game
    // In Session and could otherwise cast its own spells at a piece, and a healer
    // would heal one. So it sits above every DC rung a bot in the hall could be
    // handed (the camp owners 60, the event driver 61, the hazard vacate 55) and
    // every stock mover, and below the phantom-combat hatch (65, which the chess
    // gate keeps inert) and the terminal bailouts (100). A quarter-rung under the
    // Oculus rider and over the Hakkar suppressor (64), so the ladder carries no
    // new tie (all three are map-partitioned anyway). The multiplier clamp does
    // the rest: see KaraChessClamp.
    inline constexpr float KzChess                = 64.25f; // any role: the Karazhan chess seat (and conductor)
    inline constexpr float HorStayAhead           = 56.0f; // any role: forward, with the party and out of his ring
    inline constexpr float HazardVacate           = 55.0f; // any role: clear an unfightable hazard's pulse
    inline constexpr float AssistCampCombat       = 35.0f; // follower: onto the leader's pack
    // Leader-only, combat side of the KillCreature-engage objective. A stealthed
    // sapper (Shattered Halls' Shattered Hand Assassins) flags the party into combat
    // and re-stealths — stock combat then has no detectable victim and the run
    // wedges. This drives EngageDirect BY ENTRY on the undetected creature to break
    // stealth. Above the stock combat movers (MoveChase ~30) so it owns the tick and
    // walks the tank onto the sapper; below the camp owners / assist (35) and Hakkar
    // (62-64) which never contend (role/zone partitioned). Inert the instant the
    // target is detectable — stock combat then owns the kill.
    inline constexpr float ObjectiveEngageCombat  = 34.0f; // leader: break a stealthed sapper's combat
    // Contribution-gated combat regroup (Option B). Fires ONLY when the pure kernel
    // says a follower can't contribute from where it stands (see DcRegroupDecision),
    // so it no longer needs to out-shout the stock movers — it sits BELOW them
    // (ACTION_MOVE / MoveChase ~30) and the stock critical heals (30): anything stock
    // *can* do legitimately wins the tick, and this is the fallback when it can't.
    // 29 collides numerically with AssistCamp (29, NON-combat engine) — an engine-
    // partitioned tie (this rung is combat-only), same class as the other asserted
    // ties; pinned in t/TestRelevanceLadder.cpp. Kept at 29 (not lower) so it stays
    // above idle/default rungs and above rotation casts that isUseful might mis-report
    // during an LOS gap.
    inline constexpr float RegroupCombat          = 29.0f; // follower: contribution-gated reconnect
    // HealReposition (41) also registers in the combat engine, ABOVE AssistCampCombat
    // (35) / RegroupCombat (29) and the stock reach-heal (40), BELOW the camp owners
    // (60). Healer-only, so it never contends with the leader-only camp owners.
}

#endif
