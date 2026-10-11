/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCBOSSSTANDDOWN_H
#define _PLAYERBOT_DCBOSSSTANDDOWN_H

#include "Define.h"

#include <string_view>

class Player;

// Raid boss STAND-DOWN — the non-interference contract of a raid run.
//
// On a raid map, dungeon-clear owns everything BETWEEN fights: it delivers a
// rested, buffed, staged raid to the pull and lets go. The boss fight itself
// belongs to mod-playerbots' raid strategies (mapId-attached, ACTION_RAID=60+
// relevance). While an encounter is live, every DC combat behavior and every
// recovery ladder must go inert — a DC watchdog force-clearing combat or a
// stranded-recovery teleport mid-encounter would sabotage the fight it exists
// to protect. The ONE exemption: encounter events registered for the active
// boss (BWL's Razorgore orb) keep driving through the event executor — that
// orchestration is explicitly DC's job (see DungeonEvent::encounterActive).
//
// The gate lives in TWO places, because "every DC combat behavior" is not the
// same set as "every DC action that runs during a fight".
// DungeonClearCombatMultiplier zeroes the combat-engine rungs; the NON-combat
// pull pipeline is stood down by DungeonClearPullModeCurrentValue, which forces
// the effective pull mode Off and lowers the latched bool so the camp hold, the
// party-spread gate and ReapStrandedPassives all release with it. The combat
// multiplier alone was not enough: the pull rung's Idle branch gates on the
// TANK'S OWN combat flag, so a tank that dropped combat mid-encounter opened a
// fresh trash pull and DcFollowerLifecycle::ApplyFollowerPassive pinned the raid
// passive at a camp 100yd off the boss — a stock `+passive` flip no multiplier
// can undo (BWL Firemaw, tr-20260829-204120-2).
//
// Detection is leader-evaluated and memoised per tick: in-encounter when the
// InstanceScript reports IsEncounterInProgress(), or when a roster boss holds
// any member in PvE combat (the no-script fallback). Hysteresis is asymmetric
// by design — ENTER instantly (the first tick of a pull must already be
// gated), EXIT only after the signals have read clear for kExitGraceMs.
// Submerge phases, RP pauses and the Razorgore phase flip all blink the
// signals for a moment; flapping DC back on inside a fight is exactly the
// failure the grace absorbs.
//
// The pure kernel below (Update) is the tested decision; the glue (IsActive)
// resolves the leader, computes the signals and stores the two state fields in
// the leader-owned DcRunState (standDownActive / standDownSignalMs).
namespace DcBossStandDown
{
    // Exit hysteresis: how long BOTH signals must read clear before stand-down
    // releases. ~3s rides out scripted signal gaps without meaningfully
    // delaying the post-kill loot/advance resume.
    constexpr uint32 kExitGraceMs = 3000;

    // Per-tick memo window for the leader evaluation, same contract as
    // DcTickMemo: dedupes strictly WITHIN one AI tick (>=100ms apart), never
    // across ticks.
    constexpr uint32 kEvalWindowMs = 50;

    struct Verdict
    {
        bool   active;
        uint32 lastSignalMs;  // write back to state (0 = never signalled)
    };

    // PURE kernel: fold one observation into the hysteresis state.
    //   signal  — an encounter reads live THIS tick
    //   nowMs   — getMSTime()
    // Enter is instant; exit requires `exitGraceMs` of continuous quiet.
    // Unsigned subtraction keeps the wraparound behavior of getMSTimeDiff.
    inline Verdict Update(bool wasActive, uint32 lastSignalMs, bool signal,
                          uint32 nowMs, uint32 exitGraceMs = kExitGraceMs)
    {
        if (signal)
            return { true, nowMs };
        if (!wasActive)
            return { false, lastSignalMs };
        if (nowMs - lastSignalMs >= exitGraceMs)
            return { false, lastSignalMs };
        return { true, lastSignalMs };
    }

    // What the stand-down does with ONE combat-engine action, decided by NAME
    // alone. Pure, so the exemption list is pinned by tests instead of living
    // only inside a multiplier that needs a live bot to reach.
    enum class ActionVerdict : uint8
    {
        Stock,  // hand this action back to stock strength (1.0)
        Inert,  // a DC rung the encounter owns instead (0.0)
        Defer,  // name says nothing — the caller's own gate decides
    };

    // `isDcAction` is "the name starts with `dungeon clear`", computed once by
    // the caller (it already needs it) rather than recomputed here.
    inline ActionVerdict ClassifyAction(std::string_view name, bool isDcAction)
    {
        // The combat EVENT DRIVER. Encounter events (BWL's Razorgore orb) are
        // DC's job mid-fight, and the executor itself refuses any event not
        // flagged encounterActive while stand-down holds (see
        // FindDueConditionalEvent), so letting the action through is safe for
        // every other event.
        if (name == "dungeon clear run event combat")
            return ActionVerdict::Stock;

        // The encounter driver's second actor. Razorgore's orb runner is a
        // FOLLOWER, not the leader, so the event-driver exemption above does not
        // cover it — and the whole point of the encounterActive seam is that this
        // orchestration keeps running inside the fight. Its own trigger is gated
        // on being the one elected member of map 469, so this is inert
        // everywhere else. See DungeonClearRazorgoreOrbTrigger.
        //
        // The camp rung rides the same exemption: the raid has to hold the floor
        // below the ledge for the whole egg run, and the egg run happens entirely
        // inside the stand-down.
        if (name == "dungeon clear razorgore orb" || name == "dungeon clear razorgore camp")
            return ActionVerdict::Stock;

        // Hold-fire rides the same exemption, and needs it more than either: the
        // window it guards is a RAID ENCOUNTER by definition — a creature is
        // barred because killing it right now loses the fight — so the stand-down
        // is always up while it has work. Zeroing it here would have left it dead
        // in the only place it exists to run.
        if (name == "dungeon clear hold fire")
            return ActionVerdict::Stock;

        // The Oculus rider rung. The Eregos fight IS the riders' — every station, the
        // pull and the landing afterwards are this rung — so the stand-down that
        // silences every other DC rung for that fight must not silence it. Its own
        // trigger's first test is map 578.
        if (name == "dungeon clear oc rider")
            return ActionVerdict::Stock;

        // The Karazhan chess seat and conductor. Chess sets no boss state, so the
        // stand-down should never hold during it; if another encounter's ever
        // does (an add pulled in from the corridor), the game must still be
        // played, and a silenced rung would hand every bot's tick back to a stock
        // engine that attacks pieces. Its own trigger's first test is map 532.
        if (name == "dungeon clear kz chess")
            return ActionVerdict::Stock;

        // THE OUT-OF-LOS ASSIST, exempt as a PAIR with `drop target` below —
        // either half alone is inert, so both or neither.
        //
        // The stand-down's premise is that the fight belongs to mod-playerbots'
        // raid strategies. Those strategies handle mechanics; NONE of them can
        // hand a follower a target it cannot see, and stock combat cannot
        // either: `dps assist` ranks over `attackers`, which AttackersValue
        // LOS-filters (AttackersValue.cpp, IsValidTarget -> IsWithinLOSInMap),
        // and stock `reach spell` has no LOS clause at all — a ranged bot
        // already inside spell range never moves to gain sight. So when the tank
        // holds a boss around a corner, zeroing this rung leaves the raid with
        // nothing that can engage, and the stand-down protects a fight nobody is
        // having: BWL Firemaw, tr-20260830-152617-3 — 97% boss HP after three
        // minutes, 24 of 25 raiders flagged with `attackers=0 victim=-`, 15 of
        // them ping-ponged off the combat engine entirely.
        //
        // Safe because the assist yields to anything real: it is relevance 35
        // against ACTION_RAID 60+, so a raid strategy with work to do always
        // wins the tick and this only fills a vacuum; and its own trigger
        // (ShouldAssistCampFight) stands down the instant the bot has a target
        // it can reach AND see from where it stands.
        if (name == "dungeon clear assist camp combat")
            return ActionVerdict::Stock;

        // Every other DC combat rung is what the stand-down is FOR.
        if (isDcAction)
            return ActionVerdict::Inert;

        // `drop target` — the other half of the assist pair. Deliberately NOT
        // decided here: the caller's suppressor already asks the three narrow
        // questions that make suppressing it safe (non-healer, an assist is
        // actually wanted, and a current target that is alive and attackable but
        // merely out of LOS). Returning Stock here instead is what re-armed the
        // 1-tick engine ping-pong the suppressor exists to prevent — assist
        // seeds the target and flips the bot to the combat engine, `drop target`
        // (relevance 99) sees the unseeable target, drops it and flips the bot
        // straight back out, forever.
        return ActionVerdict::Defer;
    }

    // THE FIVE-MAN TABLE. A 5-man boss fight is DC's own to drive — except where
    // there is nothing on the ground to drive: The Oculus's Ley-Guardian Eregos is
    // fought entirely from the drakes, with no tank, no pull and no camp, and a DC
    // recovery or combat rung firing during it can only get in the riders' way.
    // Each row names the boss and the instance guid slot that resolves him (the
    // script-state signal is unusable there: instance_oculus never calls
    // SetBossState).
    struct TableRow
    {
        uint32 mapId;
        uint32 bossEntry;
        uint32 guidSlot;
    };

    inline constexpr TableRow kTable[] = {
        { 578, 27656, 3 },  // The Oculus — Ley-Guardian Eregos (DATA_EREGOS)
    };

    inline TableRow const* FindTableRow(uint32 mapId)
    {
        for (TableRow const& r : kTable)
            if (r.mapId == mapId)
                return &r;
        return nullptr;
    }

    // Is the stand-down currently holding DC back for `bot`'s run?
    //
    // False everywhere outside raid maps and the table above — 5-man boss fights
    // are DC's own to drive. On such a map the verdict is the LEADER's, evaluated at most once
    // per tick window and read cross-bot by every member (one check in the
    // combat multiplier gates every DC combat action; the recovery ladders and
    // the phantom-combat breaker consult it directly).
    bool IsActive(Player* bot);
}

#endif  // _PLAYERBOT_DCBOSSSTANDDOWN_H
