/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCREZDECISION_H
#define _PLAYERBOT_DCREZDECISION_H

#include <cstdint>
#include <utility>
#include <vector>

// Pure decision kernel for post-combat party resurrection. Historically the
// FIRST death of any party member ended the run: the party-died trigger fired
// the disable-on-death bailout the moment combat dropped. That throws away runs
// a Priest/Paladin/Shaman/Druid in the party could trivially recover — the
// module even preserves rez-able corpses (StayDeadAction / PreventBotRelease)
// naming a party rez as the intended wake path, but nothing ever performed it.
//
// This kernel answers, from a plain snapshot of the same-map party: should the
// run HOLD for a resurrection (and if so, who rezzes whom), or DISABLE because
// recovery is not viable (full wipe, no living rez class, out-of-combat
// recovery clock expired)?
//
// Election is deterministic from group order so every bot computes the same
// answer independently (no cross-bot negotiation, no stored rezzer that can go
// stale — a dead rezzer simply drops out of the next evaluation and the next
// candidate is elected):
//   rezzer: first living rez-class BOT, healers before non-healers. If only
//           non-bot (human) rezzers are alive -> WaitingOnHuman.
//   target: dead healer -> dead tank -> group order (recover the run's spine
//           first).
//
// A resurrection the INSTANCE refuses is a third answer on top of those two:
// neither hold nor disable, but "carry on with who is standing". See
// Inputs::rezBlocked and the branch that reads it.
//
// The recovery clock is OWNED BY THE GLUE (DcRezRecovery stamps/clears
// DcRunState::rezPendingSinceMs); the kernel only compares. partyEngaged
// freezes the timeout so fighting time never burns the recovery budget (a
// mid-recovery add pull must not expire the clock). It is engagement, not the
// combat flag: a rez cannot be cast while flagged, so under a phantom flag
// (DcCombatFlag) the flag would freeze the very timeout meant to bound a
// recovery that can never complete.
//
// Extracted engine-free so it is unit-testable in isolation, mirroring
// DcSmartRestDecision / DecidePull. Header-only; nothing here touches a
// Player/Unit/context, so no game headers are needed.

namespace DcRezDecision
{
    // One same-map group member, snapshotted by the glue (which, unlike the
    // Smart Rest snapshot, KEEPS dead members — they are the whole point).
    struct Member
    {
        bool isDead = false;
        bool canRezClass = false;  // Priest / Paladin / Shaman / Druid
        bool isHealerRole = false; // PlayerbotAI::IsHeal — healers rez first
        bool isTankRole = false;   // PlayerbotAI::IsTank — target priority
        bool isBot = false;        // has a PlayerbotAI to drive (humans don't)
        // Dead members only: the body lies among live hostiles nobody is fighting,
        // so a bot rezzer walking to it walks into an unpulled pack. See the
        // UnsafeStandDown branch in Decide.
        bool corpseUnsafe = false;
    };

    struct Inputs
    {
        bool          enabled = true;         // DungeonClear.PostCombatRez
        std::uint32_t nowMs = 0;
        std::uint32_t pendingSinceMs = 0;     // recovery clock; 0 = not running
        std::uint32_t timeoutMs = 90000;      // PostCombatRezTimeoutSecs * 1000
        bool          partyEngaged = false;  // freezes the timeout

        // --- raid wipe semantics ---------------------------------------------
        // Fraction of same-map members that must be DEAD (with nobody engaged)
        // for the fight to read as a wipe. 100 (the dungeon default) keeps the
        // literal everyone-dead test; a raid passes ~90 because one hiding
        // survivor must not hold the verdict open at 25-40 members.
        std::uint32_t wipeFractionPct = 100;
        // Raid runs: a wipe verdict asks for the ENTRANCE REGROUP (revive the
        // raid at the instance entrance and continue) instead of the disable.
        bool          regroupOnWipe = false;

        // --- the NoRezzer floor (see the branch below) ------------------------
        // Is any LIVING survivor still carrying the core combat flag? Deliberately
        // the flag and not engagement: the case this exists for is a boss that has
        // stopped fighting but not yet released the party.
        bool          anySurvivorCombatFlagged = false;
        // getMSTime() the survivors first read BOTH unengaged and unflagged, with
        // no rezzer left; 0 = not quiet. Owned by the glue, cleared by any tick
        // that is not quiet.
        std::uint32_t noRezzerQuietSinceMs = 0;
        std::uint32_t noRezzerQuietGraceMs = 0;   // quiet must hold this long
        // getMSTime() the party first had no rezzer at all; 0 = not yet. Bounds
        // the flag-only hold so a flag nothing ever clears cannot hang the run.
        std::uint32_t noRezzerSinceMs = 0;
        std::uint32_t noRezzerHoldMaxMs = 0;      // 0 = no ceiling

        // --- the instance refuses every party resurrect (see the branch below) ---
        // Spell.cpp's exploit protection: inside a dungeon whose InstanceScript
        // reports IsEncounterInProgress(), EVERY out-of-combat resurrect is refused
        // with SPELL_FAILED_TARGET_CANNOT_BE_RESURRECTED. Owned by the glue.
        bool          rezBlocked = false;
        std::uint32_t blockedSinceMs = 0;    // getMSTime() the block was first seen
        std::uint32_t blockedHoldMaxMs = 0;  // hold this long for it to lift; 0 = don't
    };

    enum class Outcome
    {
        None,     // no deaths, the feature is disabled (the glue converts), or the
                  // instance forbids the spell and the run carries on short-handed
        Hold,     // suppress the bailout; recovery is in progress
        Disable,  // recovery not viable — run the classic disable funnel
        Regroup   // raid wipe — revive at the instance entrance and continue
    };

    enum class Reason
    {
        NoDeaths,
        Disabled,        // feature off — kernel stands down (glue decides)
        Recovering,      // a bot rezzer is elected and will act
        WaitingOnHuman,  // only a human can rez — hold and prompt
        Wipe,            // everyone on the map is dead
        NoRezzer,        // no living member's class can rez
        NoRezzerInFight, // ditto, but the survivors are still swinging — hold
        TimedOut,        // out-of-combat recovery clock expired
        BlockedWaiting,  // the instance forbids resurrection — hold, it may lift
        BlockedStandDown, // ...it did not lift; carry on short-handed, do not disable
        UnsafeStandDown   // every corpse lies among idle hostiles; carry on short-handed
    };

    struct Result
    {
        Outcome outcome = Outcome::None;
        Reason  reason  = Reason::NoDeaths;
        int     rezzerIdx = -1;  // elected rezzer (the human for WaitingOnHuman)
        int     targetIdx = -1;  // dead member to raise first
        // PARALLEL recovery (raid runs consume this; 5-mans keep the single
        // election above): every living rez-class BOT paired with a distinct
        // corpse, deterministically, so N rezzers raise N corpses at once
        // instead of one rezzer working the pile serially against the clock.
        // pairs[0] always equals {rezzerIdx, targetIdx} when a bot rezzer
        // exists. Corpses beyond the rezzer count are left for the next
        // evaluation (the first wave of raises adds rezzers back). Filled for
        // Hold/Recovering only.
        std::vector<std::pair<int, int>> pairs;  // {rezzerIdx, targetIdx}
    };

    // Target priority: dead healer -> dead tank -> group order. The healer
    // back up first restores the party's ability to survive the next mistake;
    // the tank next restores the run's driver.
    // Safe corpses (see Member::corpseUnsafe) first; when every corpse is unsafe
    // the same priority over all of them, so a branch that only NAMES a corpse
    // still has one to name.
    inline int PickTarget(std::vector<Member> const& members)
    {
        for (bool const skipUnsafe : { true, false })
        {
            int firstDead = -1, deadTank = -1;
            for (std::size_t i = 0; i < members.size(); ++i)
            {
                if (!members[i].isDead || (skipUnsafe && members[i].corpseUnsafe))
                    continue;
                if (members[i].isHealerRole)
                    return static_cast<int>(i);
                if (deadTank < 0 && members[i].isTankRole)
                    deadTank = static_cast<int>(i);
                if (firstDead < 0)
                    firstDead = static_cast<int>(i);
            }
            if (firstDead >= 0)
                return deadTank >= 0 ? deadTank : firstDead;
        }
        return -1;
    }

    // Deterministic parallel pairing (see Result::pairs). Target order is the
    // same priority PickTarget uses, extended to a full ordering: dead healers
    // (group order), then dead tanks, then the remaining dead. Rezzer order:
    // living rez-class bots, healers first, then group order — so pairs[0]
    // reproduces the single election exactly.
    inline std::vector<std::pair<int, int>> PickPairs(std::vector<Member> const& members)
    {
        std::vector<int> targets;
        auto const pushDead = [&](bool healerPass, bool tankPass)
        {
            for (std::size_t i = 0; i < members.size(); ++i)
            {
                Member const& m = members[i];
                if (!m.isDead || m.corpseUnsafe)
                    continue;
                if (healerPass != m.isHealerRole)
                    continue;
                if (!healerPass && tankPass != m.isTankRole)
                    continue;
                targets.push_back(static_cast<int>(i));
            }
        };
        pushDead(/*healers*/ true, false);
        pushDead(false, /*tanks*/ true);
        pushDead(false, /*rest*/ false);

        std::vector<int> rezzers;
        for (int healerPass = 1; healerPass >= 0; --healerPass)
            for (std::size_t i = 0; i < members.size(); ++i)
            {
                Member const& m = members[i];
                if (m.isDead || !m.canRezClass || !m.isBot)
                    continue;
                if ((healerPass != 0) != m.isHealerRole)
                    continue;
                rezzers.push_back(static_cast<int>(i));
            }

        std::vector<std::pair<int, int>> pairs;
        std::size_t const n = targets.size() < rezzers.size() ? targets.size()
                                                              : rezzers.size();
        pairs.reserve(n);
        for (std::size_t i = 0; i < n; ++i)
            pairs.emplace_back(rezzers[i], targets[i]);
        return pairs;
    }

    // The verdict. Empty roster / no deaths -> None. See the header comment
    // for the election and timeout rules.
    inline Result Decide(Inputs const& in, std::vector<Member> const& members)
    {
        Result r;

        std::size_t total = 0, dead = 0;
        for (Member const& m : members)
        {
            ++total;
            if (m.isDead)
                ++dead;
        }
        bool const anyDead  = dead > 0;
        bool const anyAlive = dead < total;
        if (!anyDead)
            return r;  // None / NoDeaths

        if (!in.enabled)
        {
            // Feature off: the kernel stands down entirely. The glue converts
            // this into the classic immediate disable.
            r.reason = Reason::Disabled;
            return r;
        }

        // Wipe. Full (everyone dead — a dead self-res candidate deliberately
        // does not count in v1), or the raid fraction form: enough of the raid
        // dead with nobody still fighting. The engagement test keeps a losing-
        // but-live fight out of the verdict — it resolves to full wipe or to a
        // normal recovery once combat drops.
        bool const fractionWipe = in.wipeFractionPct < 100 && !in.partyEngaged &&
                                  total > 0 && dead * 100 >= total * in.wipeFractionPct;
        if (!anyAlive || fractionWipe)
        {
            r.outcome = in.regroupOnWipe ? Outcome::Regroup : Outcome::Disable;
            r.reason = Reason::Wipe;
            return r;
        }

        // THE INSTANCE ITSELF REFUSES THE SPELL.
        //
        // Spell::CheckCast carries an exploit guard (Spell.cpp, "Xinef: exploit
        // protection"): a resurrect cast by a player inside a dungeon whose
        // InstanceScript reports IsEncounterInProgress() returns
        // SPELL_FAILED_TARGET_CANNOT_BE_RESURRECTED — unconditionally, whether or not
        // anyone is in combat. In most dungeons that window is one boss fight, and
        // recovery is out of combat anyway, so it never showed.
        //
        // The Violet Hold is the counter-example that broke this feature. Its script
        // OVERRIDES IsEncounterInProgress to `_encounterStatus == IN_PROGRESS`
        // (instance_violet_hold.cpp), and that status is set the moment Sinclari
        // starts the event and only clears when the hold is finished or reset. So for
        // the WHOLE RUN, every party resurrect is refused — and the feature had no
        // idea. The elected rezzer stood on the corpse re-casting into the refusal at
        // the tick rate ("rez party: cast 'ancestral spirit' on Shikne -> not possible
        // yet", 403 times in tr-20260827-075417-165 alone) until the 90s recovery
        // budget expired and the run was disabled with "Couldn't get X resurrected in
        // time". Eighteen of the first 72 runs of tp-20260827-065217-2 died that way,
        // and every one of them spent the whole window parked over a body while the
        // hold kept draining behind them.
        //
        // Two moves, and the ORDER matters. Hold briefly first: the ordinary shape of
        // this input is a boss encounter mid-reset, where the block lifts in seconds
        // and the normal recovery below is exactly right — standing down there would
        // march the party into the next pull with a corpse it was about to raise. If
        // the block outlives blockedHoldMaxMs it is structural (Violet Hold, the Black
        // Morass, any long event) and no amount of waiting buys anything: keep
        // clearing with who is standing. NOT a disable — the party is alive and the
        // hold is winnable four-handed (99 of the 100 runs in tp-20260826-233949-1
        // cleared it without ever needing a resurrection); and not a hold, which under
        // a wave siege is only a slower loss.
        //
        // Deliberately ahead of the election, so it also covers the no-rezzer party:
        // while the instance forbids the spell, which classes are still standing is
        // not a fact about anything. Nothing latches — the moment the block lifts,
        // the verdict falls through to the ordinary election/timeout logic below and
        // recovery (or the classic disable) resumes.
        if (in.rezBlocked)
        {
            bool const blockOutlivedTheWait =
                in.blockedHoldMaxMs == 0 ||
                (in.blockedSinceMs != 0 && in.nowMs >= in.blockedSinceMs &&
                 (in.nowMs - in.blockedSinceMs) >= in.blockedHoldMaxMs);
            r.targetIdx = PickTarget(members);
            r.outcome = blockOutlivedTheWait ? Outcome::None : Outcome::Hold;
            r.reason = blockOutlivedTheWait ? Reason::BlockedStandDown
                                            : Reason::BlockedWaiting;
            return r;
        }

        // Election: first living rez-class bot, healers before non-healers;
        // remember the first living human rezzer for the WaitingOnHuman path.
        int botHealer = -1, botAny = -1, humanAny = -1;
        for (std::size_t i = 0; i < members.size(); ++i)
        {
            Member const& m = members[i];
            if (m.isDead || !m.canRezClass)
                continue;
            if (m.isBot)
            {
                if (m.isHealerRole && botHealer < 0)
                    botHealer = static_cast<int>(i);
                if (botAny < 0)
                    botAny = static_cast<int>(i);
            }
            else if (humanAny < 0)
                humanAny = static_cast<int>(i);
        }

        int const botRezzer = botHealer >= 0 ? botHealer : botAny;
        if (botRezzer < 0 && humanAny < 0)
        {
            // NO REZ CLASS LEFT — but not necessarily a lost run.
            //
            // This fires the instant the last rez-capable member dies, and it used
            // to end the run on the spot regardless of what the survivors were
            // doing. The sole healer dying is the NORMAL shape of a hard heroic
            // pull, and the remaining four finishing the pack off is a normal way
            // for it to end: twice in the MgT heroic audit (tp-20260805-005412-1)
            // a run was disabled with four members alive and the fight already
            // being won. NoRezzer is 7 of that plan's 12 disables.
            //
            // Nothing about the verdict is wrong, only its TIMING. Deciding "we
            // cannot recover from this death" while the death is still being
            // avenged reads a mid-fight snapshot as a final state. Hold while the
            // party is engaged; the moment combat ends, partyEngaged goes false and
            // this same branch returns the Disable it always did. A party that goes
            // on to wipe reaches Reason::Wipe above instead, which is the more
            // accurate verdict anyway.
            //
            // THAT HOLD WAS STILL ONE TICK WIDE, and one tick is not a fight ending.
            // partyEngaged is `victim || attackers`, which goes false in the gap
            // between a mob's swings — and, decisively, for the ELEVEN SECONDS
            // Magisters' Terrace's Kael'thas spends immune, passive and summonless
            // at 1 HP playing his defeat sequence before KillSelf. In
            // tp-20260808-162331-1 that ended two runs that had already won:
            // tr-20260808-162337-13 was disabled two seconds after its tank was
            // logged kiting him through gravity lapse, tr-20260808-165249-40 eight
            // seconds after, both with four members alive and all four still
            // carrying his combat flag. Eight of that plan's twenty failures were
            // this branch, and EVERY ONE had 2-4 members standing.
            //
            // So three gates now, not one:
            //   * engaged            — a fight is visibly in progress (unchanged);
            //   * still FLAGGED      — something has us and has not let go, which is
            //                          exactly the defeat-sequence shape;
            //   * quiet not yet held — even with both clear, the silence has to last
            //                          noRezzerQuietGraceMs before it counts as over.
            // The flag hold is capped by noRezzerHoldMaxMs so a flag nothing ever
            // clears degrades to the old behaviour instead of hanging the run.
            bool const holdCapped =
                in.noRezzerHoldMaxMs != 0 && in.noRezzerSinceMs != 0 &&
                in.nowMs >= in.noRezzerSinceMs &&
                (in.nowMs - in.noRezzerSinceMs) >= in.noRezzerHoldMaxMs;
            bool const quietHeld =
                in.noRezzerQuietGraceMs == 0 ||
                (in.noRezzerQuietSinceMs != 0 && in.nowMs >= in.noRezzerQuietSinceMs &&
                 (in.nowMs - in.noRezzerQuietSinceMs) >= in.noRezzerQuietGraceMs);
            if (!holdCapped &&
                (in.partyEngaged || in.anySurvivorCombatFlagged || !quietHeld))
            {
                r.outcome = Outcome::Hold;
                r.reason = Reason::NoRezzerInFight;
                r.targetIdx = PickTarget(members);
                return r;
            }
            r.outcome = Outcome::Disable;
            r.reason = Reason::NoRezzer;
            return r;
        }

        // EVERY CORPSE LIES AMONG LIVE HOSTILES — don't walk a rezzer into them.
        //
        // The rezzer's approach is a straight walk to the body; nothing on the way
        // asks what else is standing there. A body that died off the route, in rooms
        // the party never cleared, is surrounded by the pack that killed it, back on
        // its spawn and idle. Karazhan's Servants' Quarters lie directly under the
        // Moroes -> Maiden corridor: in tr-20260927-184653-9 a healer was dragged down
        // there and killed by Shadowbats, the second healer walked down to raise her
        // and died, and then the tank and the enhancement shaman followed. 38 of 186
        // Karazhan runs have deaths to Servants' Quarters mobs.
        //
        // So the verdict is the Blocked stand-down's: carry on with who is standing,
        // do not disable. Nothing latches — a body on the route comes back into play
        // the moment the party clears the pack beside it, and the ordinary election
        // below takes over. Unsafe corpses are simply skipped while any safe one is
        // left (PickTarget / PickPairs). Humans are not second-guessed: WaitingOnHuman
        // leaves the walk to the player.
        if (botRezzer >= 0)
        {
            bool anySafeCorpse = false;
            for (Member const& m : members)
                anySafeCorpse = anySafeCorpse || (m.isDead && !m.corpseUnsafe);
            if (!anySafeCorpse)
            {
                r.targetIdx = PickTarget(members);
                r.outcome = Outcome::None;
                r.reason = Reason::UnsafeStandDown;
                return r;
            }
        }

        r.targetIdx = PickTarget(members);
        r.rezzerIdx = botRezzer >= 0 ? botRezzer : humanAny;

        // Timeout: only while the glue's no-fight clock is running. A fight
        // freezes it (the glue additionally clears the stamp, so a fight resets
        // the budget rather than expiring it early — the safe direction).
        if (!in.partyEngaged && in.pendingSinceMs != 0 && in.timeoutMs != 0 &&
            in.nowMs - in.pendingSinceMs >= in.timeoutMs)
        {
            r.outcome = Outcome::Disable;
            r.reason = Reason::TimedOut;
            return r;
        }

        r.outcome = Outcome::Hold;
        r.reason = botRezzer >= 0 ? Reason::Recovering : Reason::WaitingOnHuman;
        if (botRezzer >= 0)
            r.pairs = PickPairs(members);
        return r;
    }
}

#endif  // _PLAYERBOT_DCREZDECISION_H
