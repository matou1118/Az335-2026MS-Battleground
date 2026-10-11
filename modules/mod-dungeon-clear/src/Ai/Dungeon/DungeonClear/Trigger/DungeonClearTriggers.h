/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DUNGEONCLEARTRIGGERS_H
#define _PLAYERBOT_DUNGEONCLEARTRIGGERS_H

#include "Trigger.h"

#include "Ai/Dungeon/DungeonClear/Util/DcProgressWatchdog.h"

#include <cstdint>

class PlayerbotAI;

class DungeonClearIdleTrigger : public Trigger
{
public:
    DungeonClearIdleTrigger(PlayerbotAI* botAI) : Trigger(botAI, "dungeon clear idle", 1) {}
    bool IsActive() override;
};

class DungeonClearAtBossTrigger : public Trigger
{
public:
    DungeonClearAtBossTrigger(PlayerbotAI* botAI) : Trigger(botAI, "dungeon clear at boss", 1) {}
    bool IsActive() override;
};

// Fires when the next anchor is a travel OBJECTIVE (DungeonAnchorKind::Objective,
// injected by BossRosterRegistry — e.g. a Sunken Temple event waypoint) and the
// tank has arrived within its arriveRadius (or its gateEntry creature is alive).
// Drives DcObjectiveArriveAction, which runs an optional on-arrival hook and
// then marks the objective cleared so the clear advances. Objectives never reach
// the combat/engage triggers (those stand down for non-Boss anchors).
class DungeonClearAtObjectiveTrigger : public Trigger
{
public:
    DungeonClearAtObjectiveTrigger(PlayerbotAI* botAI) : Trigger(botAI, "dungeon clear at objective", 1) {}
    bool IsActive() override;
};

// Leader-only, non-combat. Fires when an off-path CONDITIONAL event
// (DungeonEventRegistry, EventActivation::Conditional) for this map is DUE — its
// bound activation predicate (DungeonEvent::condition) is true and it has not yet latched. Drives
// DcRunEventAction, which runs the event's steps (walk to a lever/NPC, gossip,
// wait for the gate to open). Relevance 31 — just above at-boss (30) — so a due
// pre-boss gate (e.g. "free the prisoner to open the courtyard door") preempts
// the boss pull and the door-blocked stall. Inert when no conditional event is
// due. See DungeonEventExecutor::FindDueConditionalEvent.
class DungeonClearEventDueTrigger : public Trigger
{
public:
    DungeonClearEventDueTrigger(PlayerbotAI* botAI) : Trigger(botAI, "dungeon clear event due", 1) {}
    bool IsActive() override;
};

// COMBAT-engine sibling of DungeonClearEventDueTrigger. Fires only while the
// leader IS in combat and the due conditional event is flagged
// DungeonEvent::drivesInCombat — a continuous WAVE encounter whose event IS the
// fight, so the driver must keep steering while the party is engaged.
//
// The non-combat copy stands down on bot->IsInCombat(), which is correct
// everywhere the event's work happens BETWEEN pulls. It is fatal where the party
// is in combat from the first pull to the last: the driver then runs only in the
// shrinking gaps between waves and stops running entirely once the party falls
// behind and combat stops dropping. Black Morass with two rifts open was exactly
// that — nothing ever walked the tank to a portal to kill the rift keeper, so the
// rifts never closed. See DungeonEvent::drivesInCombat.
class DungeonClearEventDueCombatTrigger : public Trigger
{
public:
    DungeonClearEventDueCombatTrigger(PlayerbotAI* botAI)
        : Trigger(botAI, "dungeon clear event due combat", 1)
    {
    }
    bool IsActive() override;
};

class DungeonClearBlockingTrashTrigger : public Trigger
{
public:
    DungeonClearBlockingTrashTrigger(PlayerbotAI* botAI) : Trigger(botAI, "dungeon clear blocking trash", 1) {}
    bool IsActive() override;
};

// Fires at a room-wide-aggro boss (RoomAggroRegistry) while room trash remains
// AND the player's chosen pull type is NOT pull-to-camp for this pack (pull mode
// current is false: Off, or Dynamic chose Leeroy). Drives the Leeroy room-clear
// engage. When pull-to-camp IS in effect the higher-priority pull pipeline owns
// the room clear instead, so this stands down. Either way the boss pull stays
// gated until the room is clear. See DcTargeting::IsRoomClearActive.
class DungeonClearRoomTrashTrigger : public Trigger
{
public:
    DungeonClearRoomTrashTrigger(PlayerbotAI* botAI) : Trigger(botAI, "dungeon clear room trash", 1) {}
    bool IsActive() override;
};

// Room pre-clear OWNER (fix #2). Active for the WHOLE pre-clear window — a flagged
// room-aggro boss with trash still up and the tank arrived at the standoff (same
// window as DcTargeting::IsRoomClearActive). Registered just ABOVE the default
// Advance (rel 16 vs 15) so that whenever no higher driver (pull/event/room-clear/
// engage) claims the tick, this HOLDS the tank at the standoff instead of letting
// the room-aggro-blind Advance creep at the boss centre. Closes the structural gap
// behind the recurring "boss woken mid-clear" failures: the standoff is now owned
// every tick, not just when the conditional Advance hold rung happens to fire.
class DungeonClearRoomPreClearHoldTrigger : public Trigger
{
public:
    DungeonClearRoomPreClearHoldTrigger(PlayerbotAI* botAI) : Trigger(botAI, "dungeon clear room preclear hold", 1) {}
    bool IsActive() override;
};

// Fires the disable-on-death bailout when a same-map party member is dead AND
// post-combat rez recovery is NOT viable (full wipe, no living rez class,
// recovery timed out, or DungeonClear.PostCombatRez off). While recovery IS
// viable the trigger stays silent — the rez-party rung below drives the
// resurrection and the readiness gates hold the run. See DcRezRecovery.
class DungeonClearPartyDiedTrigger : public Trigger
{
public:
    DungeonClearPartyDiedTrigger(PlayerbotAI* botAI) : Trigger(botAI, "dungeon clear party died", 1) {}
    bool IsActive() override;
};

// ALL bots (leader AND followers), non-combat. Fires when post-combat rez
// recovery is in progress and THIS bot is the elected rezzer (deterministic
// from group order: first living rez-class bot, healers first — every member
// computes the same answer, so exactly one bot fires). Drives
// DungeonClearRezPartyAction: walk to the corpse, cast the class rez. Inert
// for off/paused runs, with no deaths, when a stock class-strategy rez already
// raised everyone, or when only the human can rez (the hold + prompt path).
class DungeonClearRezPartyTrigger : public Trigger
{
public:
    DungeonClearRezPartyTrigger(PlayerbotAI* botAI) : Trigger(botAI, "dungeon clear rez party", 1) {}
    bool IsActive() override;
};

class DungeonClearAllClearedTrigger : public Trigger
{
public:
    DungeonClearAllClearedTrigger(PlayerbotAI* botAI) : Trigger(botAI, "dungeon clear all cleared", 1) {}
    bool IsActive() override;
};

// Stranded-member recovery failsafe. Fires ONLY on the leader tank when the run
// has shown no progress for DungeonClear.StrandedRecoveryNoProgressSecs while a
// bot party member is stuck out of range (fell under the world / wedged in
// geometry). DcStrandedRecovery::Evaluate owns the no-progress clock (this is its
// sole update site) and returns true only when a teleport should fire this tick;
// the paired DungeonClearRecoverStrandedAction relocates the strays to the tank.
class DungeonClearRecoverStrandedTrigger : public Trigger
{
public:
    DungeonClearRecoverStrandedTrigger(PlayerbotAI* botAI) : Trigger(botAI, "dungeon clear recover stranded", 1) {}
    bool IsActive() override;
};

// --- Sunken Temple (map 109) Avatar of Hakkar encounter ------------------
// Both fire ONLY in the Sanctum of the Fallen while the encounter is live (the
// Shade 8440 / a Suppressor 8497 is up) — inert on every other map and run.

// A Nightmare Suppressor (8497) is alive nearby. Drives the suppressor-aggro
// action (top relevance) so the channel that would reset the event is silenced.
class DungeonClearHakkarSuppressorTrigger : public Trigger
{
public:
    DungeonClearHakkarSuppressorTrigger(PlayerbotAI* botAI) : Trigger(botAI, "dungeon clear hakkar suppressor", 1) {}
    bool IsActive() override;
};

// This bot carries Hakkari Blood (10460) AND an un-doused Eternal Flame remains.
// Drives the flame-douse action for the blood carrier (any member).
class DungeonClearHakkarFlameTrigger : public Trigger
{
public:
    DungeonClearHakkarFlameTrigger(PlayerbotAI* botAI) : Trigger(botAI, "dungeon clear hakkar flame", 1) {}
    bool IsActive() override;
};

// A freshly-killed Bloodkeeper (8438) corpse this party hasn't yet looted is
// nearby. Drives the in-combat blood looter so the flame douse has its key item.
class DungeonClearHakkarLootBloodTrigger : public Trigger
{
public:
    DungeonClearHakkarLootBloodTrigger(PlayerbotAI* botAI) : Trigger(botAI, "dungeon clear hakkar loot blood", 1) {}
    bool IsActive() override;
};

// Fires only while DC is enabled AND the advance/engage path has set a stall
// reason. Drives the fallback "kill anything reachable" action.
class DungeonClearStalledTrigger : public Trigger
{
public:
    DungeonClearStalledTrigger(PlayerbotAI* botAI) : Trigger(botAI, "dungeon clear stalled", 1) {}
    bool IsActive() override;
};

// Fires on non-tank party bots when a tank in their party has DC enabled and
// the bot is too far from that tank. Redirects follow from the player master
// to the tank for the duration of the clear.
class DungeonClearFollowTankTrigger : public Trigger
{
public:
    DungeonClearFollowTankTrigger(PlayerbotAI* botAI) : Trigger(botAI, "dungeon clear follow tank", 1) {}
    bool IsActive() override;
};

// Fires when the cached long-path corridor crosses a closed
// `GAMEOBJECT_TYPE_DOOR`. The bot stops advancing and stalls with a
// specific reason in party chat so the human player can open the door.
class DungeonClearDoorBlockedTrigger : public Trigger
{
public:
    DungeonClearDoorBlockedTrigger(PlayerbotAI* botAI) : Trigger(botAI, "dungeon clear door blocked", 1) {}
    bool IsActive() override;
};

// Fires only while the run is PAUSED for a door the tank couldn't open
// (DungeonClearDoorBlockedAction stashed its GUID in "dungeon clear paused
// door"). Returns true the moment that specific door reads OPEN — a human
// walked up and opened it — or it despawns/unresolves, so the tank can
// auto-resume the route without the player also hitting Resume. Inert for a
// manual `dc pause` (which leaves the paused-door GUID empty).
class DungeonClearDoorReopenedTrigger : public Trigger
{
public:
    DungeonClearDoorReopenedTrigger(PlayerbotAI* botAI) : Trigger(botAI, "dungeon clear door reopened", 1) {}
    bool IsActive() override;
};

// Rest-target triggers. Fire on every bot in an active DC run (tank AND
// followers) while out of combat and below the run's chosen rest target
// (DungeonClear.RestManaPct / RestHealthPct). They drive the stock playerbots
// "drink" / "food" actions so bots top up to the group's target even when it is
// above mod-playerbots' own stop thresholds; DungeonClearMultiplier caps the
// other side so a target below the stock stop is honoured too. Inert when the
// target is 0 (inherit the playerbots value).
class DungeonClearNeedsDrinkTrigger : public Trigger
{
public:
    DungeonClearNeedsDrinkTrigger(PlayerbotAI* botAI) : Trigger(botAI, "dungeon clear needs drink", 1) {}
    bool IsActive() override;
};

class DungeonClearNeedsEatTrigger : public Trigger
{
public:
    DungeonClearNeedsEatTrigger(PlayerbotAI* botAI) : Trigger(botAI, "dungeon clear needs eat", 1) {}
    bool IsActive() override;
};

// Fires on EVERY member of a DC run that is PAUSED — the leader AND its
// followers (see DcLeaderSignal::IsInPausedDungeonClearRun). While paused the
// driving ladder goes inert and "dungeon clear party tank" goes null, so the
// loot-floor filter that normally runs inline in the advance (leader) and
// follow-tank (follower) actions never gets a tick: the party reverts to the
// stock playerbots loot pipeline and loots everything, ignoring the DC loot
// policy (DungeonClear.LootMinQuality / IgnoreChests). Followers grabbing
// below-floor junk also keep IsAnyPartyMemberLooting true, which stalls the
// tank. This trigger fills that gap: it keeps the same filter running every
// non-combat tick while paused, so the DC loot settings stay in force for the
// whole party exactly as they do during an active run.
class DungeonClearFilterLootTrigger : public Trigger
{
public:
    DungeonClearFilterLootTrigger(PlayerbotAI* botAI) : Trigger(botAI, "dungeon clear filter loot", 1) {}
    bool IsActive() override;
};

// Fires while this bot has an un-answered group loot roll (its vote is still
// NOT_EMITED_YET). Stock playerbots only reaches the "loot roll" action off
// the "very often" RandomTrigger — a 1-in-3 coin flipped at most once per
// AiPlayerbot.RepeatDelay (2s default), so bots routinely sit on an open roll
// window for many seconds. This trigger drives the same "loot roll" action
// (the BetterLootRollAction override) every non-combat tick until the vote is
// cast, so bots roll as soon as the window opens. Gated by
// DungeonClear.BetterLootRolling; inert for self-bots, where the human owns
// the roll (improvement #1 — see BetterLootRollAction.h).
//
// STARVATION BOUND. This rung sits at DcRel::LootRollPending (95), above the
// entire driving ladder, and one action runs per tick — so for as long as it
// fires and its action reports success, NOTHING else drives the bot. That is
// only safe while "fires" and "the vote lands" mean the same thing, and once
// they came apart the run died silently for ten minutes (see
// DcLootRoll::IsVotablePendingRoll). The shared predicate closes the case we
// found; the counter below bounds the CLASS, so the next predicate that drifts
// costs one unrolled item instead of a whole run.
//
// The action votes on every pending roll in a single Execute, so one healthy
// firing clears this bot's whole backlog and the rung goes quiet on the very
// next tick. Several consecutive ticks against an UNCHANGED set of pending
// rolls therefore means the votes are not landing. The signature is what makes
// this safe to latch: any real change — a roll resolved, a new item dropped —
// gives a different value, resets the streak and re-arms the rung immediately.
class DungeonClearLootRollPendingTrigger : public Trigger
{
public:
    DungeonClearLootRollPendingTrigger(PlayerbotAI* botAI)
        : Trigger(botAI, "dungeon clear loot roll pending", 1)
    {
    }
    bool IsActive() override;

private:
    // Consecutive ticks this trigger has fired against the same pending set.
    // Generous: a healthy roll needs exactly one.
    static constexpr uint32 MAX_UNCHANGED_TICKS = 5;

    std::uint64_t _pendingSignature = 0;   // 0 == nothing was pending last tick
    uint32 _unchangedTicks = 0;
};

// --- Advanced pulls -------------------------------------------------------
// Leader-only, non-combat. Fires when advanced-pull mode is on and either a pull
// is already mid-flight (phase Forming/Advancing, or a post-fight Engage cleanup
// while out of combat) or a fresh, pullable trash pack is in range and the party
// is ready. Drives DungeonClearPullAction, which marks the camp, runs the tank
// in to grab aggro, and (on the combat engine) drags the pack back. Sits above
// engage-trash so the pull preempts the normal walk-in; trash-only — never
// preempts the at-boss engage.
//
// Two things widen the "mode is on" gate, both because the decision to pull was not
// the mode's to make: a BossPullbackRegistry drag (`bossPullback`), and a
// ScriptedPullRegistry stage already IN FLIGHT. The second is a cleanup guarantee,
// not a licence — a stage's mode is forced on only while its row is still DUE, so it
// can drop the moment the pack dies, taking the Engage cleanup that unlatches the
// stage down with it. Scoped to phase != Idle so it can never open the start path.
class DungeonClearPullTrigger : public Trigger
{
public:
    DungeonClearPullTrigger(PlayerbotAI* botAI) : Trigger(botAI, "dungeon clear pull", 1) {}
    bool IsActive() override;
};

// Leader-only, COMBAT engine. Fires once the tank is in combat during a pull
// (phase Advancing or Returning) so DungeonClearPullManeuverAction can run the
// tank back to camp instead of letting stock combat chase/fight at the pack.
// Inert at phase Engage (tank is back at camp; stock combat takes the fight).
class DungeonClearPullManeuverTrigger : public Trigger
{
public:
    DungeonClearPullManeuverTrigger(PlayerbotAI* botAI) : Trigger(botAI, "dungeon clear pull maneuver", 1) {}
    bool IsActive() override;
};

// Follower-only, non-combat. Fires while this bot's leader is in a holding pull
// phase (Forming/Advancing/Returning). Drives DungeonClearHoldAtCampAction,
// which puts the bot passive and parks it at the camp instead of trailing the
// tank into the pull. Outranks follow-tank so the party stays put while the tank
// pulls. (Passive removal is handled centrally by ReapStrandedPassives.)
class DungeonClearHoldAtCampTrigger : public Trigger
{
public:
    DungeonClearHoldAtCampTrigger(PlayerbotAI* botAI) : Trigger(botAI, "dungeon clear hold at camp", 1) {}
    bool IsActive() override;
};

// Follower-only, COMBAT engine. The combat-side twin of the trigger above: fires
// while this bot's leader is in a holding pull phase AND this bot is IN combat.
// A held follower is dragged into combat the moment the tank aggros (group
// combat), switching it to the combat engine where the non-combat hold can't
// run; without a combat-side hold the follower then runs stock follow (which
// PassiveMultiplier permits even while passive) and trails the tank. Drives
// DungeonClearStayAtCampAction. Inert at Engage so the released party fights.
class DungeonClearHoldAtCampCombatTrigger : public Trigger
{
public:
    DungeonClearHoldAtCampCombatTrigger(PlayerbotAI* botAI)
        : Trigger(botAI, "dungeon clear stay at camp", 1)
    {
    }
    bool IsActive() override;
};

// Follower-only, non-combat. Fires while this bot's leader tank is in the
// advanced-pull camp fight (phase Engage, leader in combat) and this bot is not
// yet in combat — REGARDLESS of line of sight. The drag-back can park the pack
// out of the camp's line of sight, but an idle follower can't self-engage even
// WITH sight: DC's multiplier suppresses the stock proactive-engagement pickers
// for every follower while a clear is active, so the party stands idle and the
// camp never enters combat. Drives DungeonClearAssistCampAction, which
// force-targets the pack and forces the bot into combat — flipping it into the
// combat engine where its own rotation/heal logic (un-suppressed there) runs.
// Outranks hold-at-camp so it preempts the camp yield. Goes inert the instant the
// bot is in combat (the combat-engine twin below takes any out-of-LOS handoff).
// See DcLeaderSignal::IsLeaderCampFightActive.
class DungeonClearAssistCampTrigger : public Trigger
{
public:
    DungeonClearAssistCampTrigger(PlayerbotAI* botAI)
        : Trigger(botAI, "dungeon clear assist camp", 1)
    {
    }
    bool IsActive() override;
};

// Follower-only, COMBAT engine. Combat-side twin of the trigger above: the same
// "close on the out-of-LOS camp fight" assist for when the follower is already
// IN combat (dragged in by group combat / a stray hit) but, with the pack around
// a corner, has an empty LOS attacker list and so stands idle in the combat
// engine. Drives DungeonClearAssistCampCombatAction. Inert the instant a valid
// attacker comes into sight, handing the fight back to stock combat.
class DungeonClearAssistCampCombatTrigger : public Trigger
{
public:
    DungeonClearAssistCampCombatTrigger(PlayerbotAI* botAI)
        : Trigger(botAI, "dungeon clear assist camp combat", 1)
    {
    }
    bool IsActive() override;
};

// LEADER-only, non-combat engine. The mirror of the follower assist above for the
// tank itself: a groupmate is fighting a pack the tank never saw (a follower
// aggroed around a sharp corner, or the tank called the pull done and walked off),
// so the tank stands frozen on the Advance rest gate instead of rejoining. Drives
// DungeonClearLeaderAssistAction, which moves the tank onto the party's fight and
// forces it into combat so it takes threat. Goes inert the instant the tank sees a
// target of its own (its engage scan owns it) or the party drops combat. See
// DcLeaderSignal::IsLeaderShouldAssistFight.
class DungeonClearLeaderAssistTrigger : public Trigger
{
public:
    DungeonClearLeaderAssistTrigger(PlayerbotAI* botAI)
        : Trigger(botAI, "dungeon clear leader assist", 1)
    {
    }
    bool IsActive() override;
};

// ANY DC party member (leader OR follower), BOTH engines. Phantom-combat escape
// hatch. A member can be left FLAGGED in combat by a mob that spawned far across the
// map or behind a gate (a proximity/gate event spawn) and tagged it: the core combat
// reference never drops because the holder is unreachable, and DC's own gates that
// key off "someone is in combat" (the fight-assist arm, the party-engaged latch)
// then spin forever — a hard deadlock a `dc off`/`on` cannot clear because the flag
// lives in the core CombatManager, not in DC. This trigger fires only when the bot is
// in combat but nothing is fightable — nothing meleeing it, no victim, and EVERY unit
// holding it in combat is unreachable-by-path, evading, forbidden by its own AI from
// attacking us, or REACHABLE BUT NOT COMING — sustained for
// DungeonClear.StuckCombatTimeout seconds (long by default so a scripted encounter
// that intentionally holds combat is never mistaken for a stuck flag). Keying on
// REACHABILITY (not distance) is the safety property: a fleeing or kiting party's
// pursuers are always path-reachable, so it can never fire there; a combat forced by
// a script with no unit reference is likewise never touched.
//
// The "reachable but not coming" arm is the S1487 addition. In an instance a creature
// never leashes (the dungeon short-circuit in CanCreatureAttack), so a mob that tagged
// the party and then stopped keeps its combat reference alive from wherever it stands
// — alive, non-evading, path-reachable, allowed to attack, and completely inert. That
// read as a real fight, so this hatch stood down while every DC gate keyed off "someone
// is in combat" spun. It is bounded by CLOSING DISTANCE, not by distance or time alone:
// a holder inside DC_ENGAGE_RANGE is a fight whatever the numbers say, and one that is
// improving its closest-ever distance to us is chasing. Only a holder that is far AND
// has stopped closing counts as stale. Drives
// DungeonClearBreakStuckCombatAction, which force-clears combat + threat (the same
// effect as a GM `.combatstop`). Inert the instant anything becomes fightable, outside
// a live/unpaused DC run, and — deliberately — in any RAID zone, where an errant
// combat drop could reset a boss for the whole raid (the deadlock this recovers is a
// 5-man problem). Verdict is the pure DungeonClearMath::IsPhantomCombat +
// ShouldBreakStuckCombat kernels.
//
// Registered in BOTH engines, and the NON-combat registration is the load-bearing
// one. Engine transitions are action-driven, not derived from IsInCombat: stock
// `drop target` (relevance 99) moves a bot to BOT_STATE_NON_COMBAT without clearing
// the core flag, and nothing moves it back while a DC run suppresses the stock
// attack/pull actions that call ChangeEngine(BOT_STATE_COMBAT). A bot in that state
// is flagged with every non-combat rung bailing on IsInCombat() and every combat rung
// out of reach — which, while this hatch was combat-only, included the hatch itself.
// See DungeonClearStrategy for the live case.
class DungeonClearBreakStuckCombatTrigger : public Trigger
{
public:
    DungeonClearBreakStuckCombatTrigger(PlayerbotAI* botAI)
        : Trigger(botAI, "dungeon clear break stuck combat", 1)
    {
    }
    bool IsActive() override;

private:
    // Streak clock owned per-bot (the context creates one trigger object per bot):
    // getMSTime the phantom-combat state was first observed this streak, 0 = not
    // streaking. Reset to 0 the instant anything becomes fightable. The action reads
    // nothing from here — the force-clear is stateless.
    std::uint32_t stuckCombatSinceMs = 0;

    // Closing-distance tracker over the NEAREST legitimate combat holder. Reachability
    // proves a holder COULD come; this proves whether it IS coming. Re-armed only when
    // there is nothing to measure (out of combat, a real fight, no legitimate holder),
    // never on a tick where the holder merely happened to close — see IsActive.
    DcProgressWatchdog holderCloseWatch;
};

// LEADER-only, COMBAT engine. The combat-side rung of the KillCreature-engage
// objective (Shattered Halls' stealthed Shattered Hand Assassins). A stealthed
// mob can Sap the tank: the sap flags the party into combat AND the assassin stays
// stealthed, so once the incapacitate wears off stock combat has no detectable
// victim and the run wedges "in combat, nothing to hit". The non-combat objective
// driver (DcObjectiveArriveAction's engage branch) cannot run then — combat owns
// the engine — so this fires it from the combat side. Active only when: DC leader
// on a live/unpaused run, in combat, an active KillCreature-ENGAGE objective step
// (DungeonEventExecutor::ActiveEngageStep), AND a live creature of that step's
// entry sits nearby, REACHABLE, but this bot canNOT see/detect it — the exact
// stealthed-sapper deadlock signature. Drives DcObjectiveEngageCombatAction, which
// walks the tank onto the assassin by ENTRY and Attacks it (breaking stealth on the
// first swing). Inert the instant the target becomes detectable, handing the kill
// back to stock combat.
class DungeonClearObjectiveEngageCombatTrigger : public Trigger
{
public:
    DungeonClearObjectiveEngageCombatTrigger(PlayerbotAI* botAI)
        : Trigger(botAI, "dungeon clear objective engage combat", 1)
    {
    }
    bool IsActive() override;
};

// Follower-only, COMBAT engine. A CONTRIBUTION-GATED reconnector (Option B): it no
// longer tethers on distance. It fires only when a follower in combat has no useful
// work it can do from where it stands — a DPS with an empty LOS attacker list, or a
// healer parked where it could not heal the tank when damage starts (nobody hurt
// yet) — and drives DungeonClearRegroupCombatAction to a role-correct standoff point
// with LOS on the fight, never onto the tank's cell. The verdict is the pure
// DcRegroupDecision::DecideCombatRegroup kernel; this trigger gathers the game-state
// reads, then layers debounce (predicate must hold DC_REGROUP_DEBOUNCE_MS before
// firing) + a latch (keep firing until it clears) + a post-fire cooldown
// (DungeonClear.CombatRegroupCooldown) so it cannot flap on an LOS flicker.
// DungeonClear.CombatRegroupDistance survives as a HARD OUTER TETHER: beyond it the
// bot reconnects regardless of the contribution test, bypassing debounce/cooldown
// (the drifted-into-nowhere emergency path). The hurt-heal-target case is owned by
// DungeonClearHealRepositionTrigger (rel 41); this stands down whenever it holds.
// Deliberately INERT while the party is held passive at an advanced-pull camp
// (GetLeaderCampHold passive) — the camp/assist actions own positioning there.
// Gated by DungeonClear.CombatRegroup.
class DungeonClearRegroupCombatTrigger : public Trigger
{
public:
    DungeonClearRegroupCombatTrigger(PlayerbotAI* botAI)
        : Trigger(botAI, "dungeon clear regroup combat", 1)
    {
    }
    bool IsActive() override;

private:
    // Flap control. Per-bot (the context creates one trigger object per bot); the
    // action never reads these — the move is stateless and re-derives everything.
    uint32 _pendingSince  = 0;  // first getMSTime the contribution predicate held (0 = not pending)
    uint32 _cooldownUntil = 0;  // no non-emergency re-fire before this getMSTime
    bool   _latched       = false;  // currently firing: keep firing until the predicate clears
};

// Healer-only, BOTH engines. The real fix for the long-standing "healer stops
// healing once the tank is dragged out of line of sight" bug. Fires when this
// bot is a healer on an active run and the most-hurt party member (the DC
// `dungeon clear heal target` value — chosen LOS-blind, tank-biased) is below
// the heal HP floor but currently UNHEALABLE from where the bot stands (out of
// LOS or beyond heal range). Drives DungeonClearHealRepositionAction to move to
// a point with line of sight + heal range, after which the stock heal stack
// re-acquires the target. Stands down when there is in-LOS heal work the stock
// engine can already do (it defers to a visible hurt member), during advanced-
// pull passive camp holds, for the leader/tank itself, while CC'd, and when the
// target is implausibly far. Gated by DungeonClear.HealReposition.
class DungeonClearHealRepositionTrigger : public Trigger
{
public:
    DungeonClearHealRepositionTrigger(PlayerbotAI* botAI)
        : Trigger(botAI, "dungeon clear heal reposition", 1)
    {
    }
    bool IsActive() override;
};

// ANY role, BOTH engines. Fires when the bot is standing inside the pulse of an
// active-vacate DcHazardRegistry row it cannot fight. Two of those exist:
//
//   * the Arcatraz "Destroyed Sentinel" (21761) summoned at a Sentinel's corpse,
//     NOT_SELECTABLE, pulsing 15yd/1s until it despawns;
//   * Scholomance's "Cloud of Disease" (17742), the persistent area aura a dying
//     Diseased Ghoul (10495) leaves on the ground — 5yd, 350/s, 20s. There is no
//     creature at all here, only a DynamicObject, so nothing in the combat AI can
//     even see it.
//
// Drives DungeonClearHazardVacateAction to clear the pulse; once out, normal
// driving resumes and the party advances past the spot (it does NOT hold at the
// rim for the emitter's whole lifetime — see the band note in DcHazard.h). No
// combat gate (both tick after the kill, often out of combat, and the ghoul pool
// drops mid-pack-fight) and no role exemption (neither can be tanked). Inert on
// maps with no rows of either kind, while CC'd/rooted (can't move), and when
// DungeonClear.HazardVacate is off. See DcHazard::NearestVacate.
class DungeonClearHazardVacateTrigger : public Trigger
{
public:
    DungeonClearHazardVacateTrigger(PlayerbotAI* botAI)
        : Trigger(botAI, "dungeon clear hazard vacate", 1)
    {
    }
    bool IsActive() override;
};

// HALLS OF REFLECTION ONLY (map 668), every role, BOTH engines. Fires while the
// escape is running and THIS bot is either inside the Lich King's Remorseless
// Winter ring or has fallen behind him along the path.
//
// WHY THE ESCAPE NEEDS A RUNG OF ITS OWN rather than the generic hazard vacate.
// The driver (hook 35) steers the TANK ONLY — DungeonClearEventDueTrigger is
// leader-only — while the four followers are in the combat engine killing the
// wall's summons, and the summons are the problem: they spawn AT the Lich King
// and run to the leader's stop, so a melee chasing the last Risen Witch Doctor
// (which casts from 20yd on HIS side) walks straight into the ring, and a bot
// that takes the 70653 Zap is KNOCKED FURTHER BACK by it.
//
// Both of those are answered by moving forward, to the party's stand point. The
// generic vacate would answer them by moving away from him, which for a bot
// behind him is further behind — deeper into the zap rule, whose own knockback
// then makes the next check worse. That is why the ring is registered as a
// placement keep-out with NO vacateRadius (see DcHazardRegistry) and why this
// exists at relevance 56, one above HazardVacate.
//
// TWO TRIGGERS, and they are the encounter's own numbers scaled back so the
// correction happens before the damage rather than after it:
//   * inside LK_PRESSURE_DIST (16yd) of him — the pulse is 10yd and deals
//     7068 +/- 863 frost per second;
//   * (bot.x + bot.y) - (lk.x + lk.y) above LK_BEHIND_SUM (6) — the core zaps at
//     20 on exactly this scalar, and the path runs -x -y so it IS "behind him".
//
// ...OR the party has simply MOVED ON without it: the wall opened, the leader ran
// 100-186yd to her next stop, the escape driver took the tank after her and this
// bot is still at the old wall. That arm is latched (STAND_LEAVE_LEASH to arm,
// STAND_LEASH to clear) and is NOT gated on Remorseless Winter, because the last
// 131yd to the gunship is run with the aura already gone. Without it the rung was
// a pressure valve that moved a bot a few yards and disarmed, and the party split
// at every wall — see the block comment on DcHorStayAheadAction.cpp.
//
// Free on every other map — the first test is an integer compare on the map id.
class DungeonClearHorStayAheadTrigger : public Trigger
{
public:
    DungeonClearHorStayAheadTrigger(PlayerbotAI* botAI)
        : Trigger(botAI, "dungeon clear hor stay ahead", 1)
    {
    }
    bool IsActive() override;
};

// THE OCULUS ONLY: active for a member that sits on a drake, or whose party plan
// (the flight driver's, DcOculusPlanView) has something for the riders to do.
// Free on every other map — the first test is an integer compare on the map id.
class DungeonClearOculusRiderTrigger : public Trigger
{
public:
    DungeonClearOculusRiderTrigger(PlayerbotAI* botAI) : Trigger(botAI, "dungeon clear oc rider", 1) {}
    bool IsActive() override;
};

// KARAZHAN ONLY: active for a bot in the Gamesman's Hall while chess is the raid's
// business — a game on the board, or the run's conductor armed and ticking. Free on
// every other map — the first test is an integer compare on the map id. See
// DcKarazhan::ChessRungLive.
class DungeonClearKzChessTrigger : public Trigger
{
public:
    DungeonClearKzChessTrigger(PlayerbotAI* botAI) : Trigger(botAI, "dungeon clear kz chess", 1) {}
    bool IsActive() override;
};

// BLACKWING LAIR ONLY, and only for ONE member of the raid: the bot the leader's
// Razorgore driver elected to take the Orb of Domination.
//
// The rest of the encounter is the raid strategy's and the leader driver's. This
// exists because the orb has requirements the tank cannot meet — the clicker must
// own no pet, must not be inside the 60s Mind Exhaustion lockout, and is ROOTED
// for the 90 seconds the mind control lasts — so a second actor has to walk 78yd
// to the ledge and stand there while the raid holds the floor.
//
// It is deliberately the RUNNER'S OWN trigger rather than something the leader
// does to it. A leader reaching across to drive a follower's MotionMaster fights
// that bot's AI and the `bwl` strategy's own repositioning every tick and loses;
// publishing "you are the runner" and letting the bot act on its own tick is the
// pattern every other cross-bot decision in this module uses.
//
// Free everywhere else: the map compare rejects before anything else is read.
// See DcLeaderSignal::IsLeaderRazorgoreRunner and DungeonClearRazorgoreOrbAction.
class DungeonClearRazorgoreOrbTrigger : public Trigger
{
public:
    DungeonClearRazorgoreOrbTrigger(PlayerbotAI* botAI)
        : Trigger(botAI, "dungeon clear razorgore orb", 1)
    {
    }
    bool IsActive() override;
};

// The other half of the same encounter: every member EXCEPT the orb runner, while
// the egg run is being driven and that member is off the camp. Fires only while
// the leader is stamping "the driver has work" (DcLeaderSignal::
// IsLeaderRazorgoreDriving), so it arms with phase 1 and releases with it.
//
// Free everywhere else — the map compare rejects first. See
// DungeonClearRazorgoreCampAction for what the camp is for and why it is on the
// floor rather than the ledge.
class DungeonClearRazorgoreCampTrigger : public Trigger
{
public:
    DungeonClearRazorgoreCampTrigger(PlayerbotAI* botAI)
        : Trigger(botAI, "dungeon clear razorgore camp", 1)
    {
    }
    bool IsActive() override;
};

// BLACKWING LAIR ONLY, and only while the leader is crossing the Suppression
// Rooms: is this bot outside the pack leash around the leader's route cursor?
//
// The Razorgore camp one room over holds the raid at a FIXED point. This one
// cannot: on the Vaelastrasz -> Broodlord leg there is no place to stand, only a
// 342yd line to walk, so the anchor MOVES — it is the authored anchor the leader
// is currently walking toward, published tick by tick
// (DcLeaderSignal::GetTransitAnchor).
//
// WHY A PACK RUNG EXISTS AT ALL ON THIS LEG. A raid strung over 100yd sweeps a
// far larger cylinder of a room that holds 160 whelps on a 30s respawn, and
// every straggler independently holds the WHOLE party in combat
// (AnyPartyEngagement is party-wide) — which is what makes DcCombatFlag::MayDrive
// false and leaves the clear with no driver. Travelling as one body is not a
// nicety here, it is the precondition for travelling at all.
//
// INERT INSIDE THE LEASH, never "own the tick and return false": a bot already in
// the pack must not contend with its own rotation for the whole crossing. The
// leader is exempt — it IS the anchor.
//
// Free everywhere else: the map compare rejects before anything is read. See
// DungeonClearTransitPackAction for where a drifted bot is walked back to.
class DungeonClearTransitPackTrigger : public Trigger
{
public:
    DungeonClearTransitPackTrigger(PlayerbotAI* botAI)
        : Trigger(botAI, "dungeon clear transit pack", 1)
    {
    }
    bool IsActive() override;
};

// IS THIS BOT AIMED AT SOMETHING IT IS FORBIDDEN TO DAMAGE RIGHT NOW?
//
// The second half of DcTargetExclusionRegistry, and the half that had to exist.
// The registry's first half (DungeonClearCombatStrategy::AppendTargetExclusions)
// takes a barred creature out of the DPS pool, which is enough only if the pool is
// what points the damage. On Razorgore it is not: `bwl razorgore mark boss` paints
// the moon raid icon on him so the off-tank picks him up, and both DpsTargetValue
// and DpsAoeTargetValue return RtiTargetValue's answer BEFORE the exclusion pass
// ever runs. So every DPS in the raid pointed at a boss whose phase-1 death casts
// 20038 on all of them. Measured, tr-20260827-233058-1: pull at 23:31:36, Razorgore
// dead at 23:31:44, seventeen of twenty-five bots dead with him.
//
// The marks are deliberate and stay (the off-tank needs them). What changes is that
// a barred target is TAKEN BACK: the DC-side picker refuses to hand one out
// (Value/DungeonClearDpsTargetValue), and this rung lets go of one the bot is
// already holding — the case the picker cannot reach, because a bot that acquired
// the boss a tick before the bar came up keeps auto-attacking it off GetVictim()
// with no further target pick involved.
//
// TANKS ARE EXEMPT PER ROW, NOT WHOLESALE, and the distinction is the row's own
// `alsoTank` flag — which already means exactly "the bar reaches the tank's pick
// too". A row WITHOUT it (Razorgore) is an encounter where somebody must still
// HOLD the barred creature, and the tank keeps it. A row WITH it is an encounter
// that wants NO HOLDER at all, and the tank drops it like everyone else.
//
// Halls of Reflection's Lich King is what forced the change. He is a fully legal
// target — unit_flags 0, no immunities — so stock assist triggers acquire him
// within a second of the escape gossip; the exclusion pool stops a fresh PICK,
// but only this rung can take him off a bot that already holds him, and under the
// old blanket exemption a tank that acquired him in that first tick kept him for
// the rest of the escape. That is not merely wasted damage (he heals to 75%
// below 70): a held victim makes DcCombatFlag::IsEngaged true for the party and
// freezes every MayDrive rung while he walks at them.
//
// Free on every map with no rows — HasRowsFor is a scan of a table with one entry
// in it, keyed on the map.
class DungeonClearHoldFireTrigger : public Trigger
{
public:
    DungeonClearHoldFireTrigger(PlayerbotAI* botAI)
        : Trigger(botAI, "dungeon clear hold fire", 1)
    {
    }
    bool IsActive() override;
};

#endif
