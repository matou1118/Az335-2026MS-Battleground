/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "gtest/gtest.h"

#include "DcRezDecision.h"

using DcRezDecision::Decide;
using DcRezDecision::Inputs;
using DcRezDecision::Member;
using DcRezDecision::Outcome;
using DcRezDecision::Reason;
using DcRezDecision::Result;

namespace
{
    // A five-member party, everyone alive. Roster (group order):
    // [0] prot-paladin leader tank (rez class, not healer),
    // [1] priest healer bot,
    // [2] mage DPS bot (no rez class),
    // [3] warrior DPS bot (no rez class),
    // [4] human warlock (no rez class, not a bot).
    // Individual tests flip deaths / classes / roles off this base.
    std::vector<Member> BaseParty()
    {
        Member tank;   tank.canRezClass = true; tank.isTankRole = true; tank.isBot = true;
        Member healer; healer.canRezClass = true; healer.isHealerRole = true; healer.isBot = true;
        Member mage;   mage.isBot = true;
        Member warr;   warr.isBot = true;
        Member human;  // the real player's seat
        return {tank, healer, mage, warr, human};
    }

    Inputs BaseInputs()
    {
        Inputs in;
        in.enabled = true;
        in.nowMs = 100000;
        in.pendingSinceMs = 0;   // clock not yet stamped
        in.timeoutMs = 90000;
        in.partyEngaged = false;
        return in;
    }
}

// ---- no work to do --------------------------------------------------------------

TEST(DcRezDecisionTest, NoDeathsIsNone)
{
    Result const r = Decide(BaseInputs(), BaseParty());
    EXPECT_EQ(r.outcome, Outcome::None);
    EXPECT_EQ(r.reason, Reason::NoDeaths);
}

TEST(DcRezDecisionTest, EmptyRosterIsNone)
{
    Result const r = Decide(BaseInputs(), {});
    EXPECT_EQ(r.outcome, Outcome::None);
    EXPECT_EQ(r.reason, Reason::NoDeaths);
}

TEST(DcRezDecisionTest, FeatureDisabledIsNone)
{
    // With the feature off the kernel stands down entirely — outcome None, so
    // recovery machinery is inert. The glue converts this into the classic
    // immediate disable-on-death.
    auto party = BaseParty();
    party[2].isDead = true;
    Inputs in = BaseInputs();
    in.enabled = false;
    Result const r = Decide(in, party);
    EXPECT_EQ(r.outcome, Outcome::None);
    EXPECT_EQ(r.reason, Reason::Disabled);
}

// ---- election -------------------------------------------------------------------

TEST(DcRezDecisionTest, DeadDpsWithHealerBotHoldsRecovering)
{
    auto party = BaseParty();
    party[2].isDead = true;
    Result const r = Decide(BaseInputs(), party);
    EXPECT_EQ(r.outcome, Outcome::Hold);
    EXPECT_EQ(r.reason, Reason::Recovering);
    EXPECT_EQ(r.rezzerIdx, 1);  // the priest healer, not the paladin tank at [0]
    EXPECT_EQ(r.targetIdx, 2);
}

TEST(DcRezDecisionTest, HealerElectedBeforeEarlierNonHealerRezzer)
{
    // The paladin tank sits FIRST in group order and can rez, but a living
    // healer always outranks a non-healer rezzer.
    auto party = BaseParty();
    party[3].isDead = true;
    Result const r = Decide(BaseInputs(), party);
    EXPECT_EQ(r.rezzerIdx, 1);
}

TEST(DcRezDecisionTest, NonHealerRezzerElectedWhenHealerIsTheCorpse)
{
    // The healer is the one who died -> the prot paladin leader rezzes
    // (the leader-rung case: the rez rung outranks the boss pull).
    auto party = BaseParty();
    party[1].isDead = true;
    Result const r = Decide(BaseInputs(), party);
    EXPECT_EQ(r.outcome, Outcome::Hold);
    EXPECT_EQ(r.reason, Reason::Recovering);
    EXPECT_EQ(r.rezzerIdx, 0);
    EXPECT_EQ(r.targetIdx, 1);
}

TEST(DcRezDecisionTest, RezzerDiedReelectsNextCandidate)
{
    // Stateless re-election: the priest (the natural pick) is dead too, so the
    // next living candidate — the paladin tank — is elected. No stored rezzer
    // GUID exists to go stale.
    auto party = BaseParty();
    party[1].isDead = true;
    party[2].isDead = true;
    Result const r = Decide(BaseInputs(), party);
    EXPECT_EQ(r.outcome, Outcome::Hold);
    EXPECT_EQ(r.rezzerIdx, 0);
    EXPECT_EQ(r.targetIdx, 1);  // dead healer outranks dead DPS as the target
}

TEST(DcRezDecisionTest, HumanOnlyRezzerWaits)
{
    // The only living rez class is the human -> hold and prompt, never drive.
    auto party = BaseParty();
    party[0].isDead = true;
    party[1].isDead = true;
    party[4].canRezClass = true;  // the human is (say) a shaman
    Result const r = Decide(BaseInputs(), party);
    EXPECT_EQ(r.outcome, Outcome::Hold);
    EXPECT_EQ(r.reason, Reason::WaitingOnHuman);
    EXPECT_EQ(r.rezzerIdx, 4);
}

// ---- target priority ------------------------------------------------------------

TEST(DcRezDecisionTest, DeadHealerOutranksDeadDps)
{
    auto party = BaseParty();
    party[1].isDead = true;
    party[2].isDead = true;
    EXPECT_EQ(Decide(BaseInputs(), party).targetIdx, 1);
}

TEST(DcRezDecisionTest, DeadTankOutranksDeadDpsWhenNoHealerDown)
{
    auto party = BaseParty();
    party[0].isDead = true;
    party[3].isDead = true;
    Result const r = Decide(BaseInputs(), party);
    EXPECT_EQ(r.targetIdx, 0);
    EXPECT_EQ(r.rezzerIdx, 1);  // the healer raises the tank
}

TEST(DcRezDecisionTest, GroupOrderBreaksTargetTies)
{
    auto party = BaseParty();
    party[2].isDead = true;
    party[3].isDead = true;
    EXPECT_EQ(Decide(BaseInputs(), party).targetIdx, 2);
}

// ---- disable verdicts -----------------------------------------------------------

TEST(DcRezDecisionTest, FullWipeDisables)
{
    auto party = BaseParty();
    for (Member& m : party)
        m.isDead = true;
    Result const r = Decide(BaseInputs(), party);
    EXPECT_EQ(r.outcome, Outcome::Disable);
    EXPECT_EQ(r.reason, Reason::Wipe);
}

TEST(DcRezDecisionTest, NoRezClassAliveDisables)
{
    // Both rez classes are the corpses; the survivors are mage/warrior/warlock.
    auto party = BaseParty();
    party[0].isDead = true;
    party[1].isDead = true;
    Result const r = Decide(BaseInputs(), party);
    EXPECT_EQ(r.outcome, Outcome::Disable);
    EXPECT_EQ(r.reason, Reason::NoRezzer);
}

// ...but NOT while the survivors are still swinging. The sole healer dying is the
// normal shape of a hard heroic pull and the remaining four finishing the pack is a
// normal way for it to end; disabling on the spot ends a run that is being won. Two
// runs in tp-20260805-005412-1 died exactly this way, with four members alive.
TEST(DcRezDecisionTest, NoRezClassAliveHoldsWhileThePartyIsStillFighting)
{
    auto party = BaseParty();
    party[0].isDead = true;
    party[1].isDead = true;
    auto in = BaseInputs();
    in.partyEngaged = true;
    Result const r = Decide(in, party);
    EXPECT_EQ(r.outcome, Outcome::Hold);
    EXPECT_EQ(r.reason, Reason::NoRezzerInFight);
    // A corpse is still named, so the hold can be announced against someone.
    EXPECT_GE(r.targetIdx, 0);
    // Nobody is elected to rez — there is nobody who can.
    EXPECT_EQ(r.rezzerIdx, -1);
}

// And the hold is only a deferral: the instant the fight ends the verdict is the
// classic disable again, so the run still terminates rather than idling to the
// no-progress watchdog.
TEST(DcRezDecisionTest, NoRezClassDisablesOnceTheFightEnds)
{
    auto party = BaseParty();
    party[0].isDead = true;
    party[1].isDead = true;
    auto in = BaseInputs();
    in.partyEngaged = true;
    EXPECT_EQ(Decide(in, party).reason, Reason::NoRezzerInFight);
    in.partyEngaged = false;
    Result const r = Decide(in, party);
    EXPECT_EQ(r.outcome, Outcome::Disable);
    EXPECT_EQ(r.reason, Reason::NoRezzer);
}

// ---- the NoRezzer floor ---------------------------------------------------------
//
// The hold above was still one tick wide, and one tick of quiet is not a fight
// ending. Three shapes, all from tp-20260808-162331-1, where 8 of 20 failures were
// this branch and every one had 2-4 members alive.

namespace
{
    // The party from the two thrown-away Kael'thas runs: sole rezzer dead, four
    // alive, nothing swinging, everyone still carrying the boss's combat flag.
    Inputs FloorInputs()
    {
        Inputs in = BaseInputs();
        in.noRezzerQuietGraceMs = 12000;
        in.noRezzerHoldMaxMs = 60000;
        in.noRezzerSinceMs = in.nowMs;
        return in;
    }

    std::vector<Member> NoRezzerParty()
    {
        auto party = BaseParty();
        party[0].isDead = true;  // the rez-class tank
        party[1].isDead = true;  // the rez-class healer
        return party;
    }
}

// Kael'thas spends 11 seconds immune, passive and summonless at 1 HP before he
// kills himself. For all 11 the party reads unengaged while still flagged by him —
// and tr-20260808-162337-13 was disabled two seconds after its tank was logged
// kiting him through gravity lapse, four members alive, three bosses down.
TEST(DcRezDecisionTest, NoRezClassHoldsWhileSurvivorsAreStillCombatFlagged)
{
    auto in = FloorInputs();
    in.partyEngaged = false;
    in.anySurvivorCombatFlagged = true;
    Result const r = Decide(in, NoRezzerParty());
    EXPECT_EQ(r.outcome, Outcome::Hold);
    EXPECT_EQ(r.reason, Reason::NoRezzerInFight);
}

// Unflagged too, but only just: the silence has to LAST before it counts as the
// fight being over.
TEST(DcRezDecisionTest, NoRezClassHoldsUntilTheQuietHasHeldItsGrace)
{
    auto in = FloorInputs();
    in.noRezzerQuietSinceMs = in.nowMs - 11999;
    EXPECT_EQ(Decide(in, NoRezzerParty()).reason, Reason::NoRezzerInFight);

    in.noRezzerQuietSinceMs = in.nowMs - 12000;
    Result const r = Decide(in, NoRezzerParty());
    EXPECT_EQ(r.outcome, Outcome::Disable);
    EXPECT_EQ(r.reason, Reason::NoRezzer);
}

// A quiet clock that never started (the glue only stamps it once the party reads
// both unengaged and unflagged) must not read as "quiet for long enough".
TEST(DcRezDecisionTest, NoRezClassHoldsWhileTheQuietClockIsUnarmed)
{
    auto in = FloorInputs();
    in.noRezzerQuietSinceMs = 0;
    EXPECT_EQ(Decide(in, NoRezzerParty()).reason, Reason::NoRezzerInFight);
}

// ...and the flag hold is CAPPED, so a flag nothing ever clears degrades to the
// old verdict instead of hanging the run open forever.
TEST(DcRezDecisionTest, NoRezClassDisablesOnceTheFlagHoldHitsItsCeiling)
{
    auto in = FloorInputs();
    in.anySurvivorCombatFlagged = true;
    in.noRezzerSinceMs = in.nowMs - 59999;
    EXPECT_EQ(Decide(in, NoRezzerParty()).reason, Reason::NoRezzerInFight);

    in.noRezzerSinceMs = in.nowMs - 60000;
    Result const r = Decide(in, NoRezzerParty());
    EXPECT_EQ(r.outcome, Outcome::Disable);
    EXPECT_EQ(r.reason, Reason::NoRezzer);
}

// The ceiling outranks an active fight too — otherwise a party permanently pinned
// by something it cannot kill would hold a dead run open indefinitely.
TEST(DcRezDecisionTest, TheFlagHoldCeilingOutranksEngagement)
{
    auto in = FloorInputs();
    in.partyEngaged = true;
    in.noRezzerSinceMs = in.nowMs - 60000;
    EXPECT_EQ(Decide(in, NoRezzerParty()).reason, Reason::NoRezzer);
}

// With the floor left at its defaults (both graces 0) the branch behaves exactly
// as it did before it existed — the property every pre-floor test above relies on.
TEST(DcRezDecisionTest, TheFloorIsInertAtItsDefaults)
{
    auto in = BaseInputs();
    in.partyEngaged = false;
    EXPECT_EQ(Decide(in, NoRezzerParty()).reason, Reason::NoRezzer);
}

// A full wipe outranks the in-fight hold: with nobody alive there is no fight to
// finish, and Reason::Wipe is reached before the rezzer election either way.
TEST(DcRezDecisionTest, AFullWipeStillDisablesEvenIfFlaggedEngaged)
{
    auto party = BaseParty();
    for (auto& m : party)
        m.isDead = true;
    auto in = BaseInputs();
    in.partyEngaged = true;
    Result const r = Decide(in, party);
    EXPECT_EQ(r.outcome, Outcome::Disable);
    EXPECT_EQ(r.reason, Reason::Wipe);
}

// ---- the recovery clock ---------------------------------------------------------

TEST(DcRezDecisionTest, TimeoutExpiryDisables)
{
    auto party = BaseParty();
    party[2].isDead = true;
    Inputs in = BaseInputs();
    in.pendingSinceMs = 1000;
    in.nowMs = 1000 + in.timeoutMs;  // exactly at the budget
    Result const r = Decide(in, party);
    EXPECT_EQ(r.outcome, Outcome::Disable);
    EXPECT_EQ(r.reason, Reason::TimedOut);
}

TEST(DcRezDecisionTest, JustUnderTimeoutStillHolds)
{
    auto party = BaseParty();
    party[2].isDead = true;
    Inputs in = BaseInputs();
    in.pendingSinceMs = 1000;
    in.nowMs = 1000 + in.timeoutMs - 1;
    Result const r = Decide(in, party);
    EXPECT_EQ(r.outcome, Outcome::Hold);
    EXPECT_EQ(r.reason, Reason::Recovering);
}

TEST(DcRezDecisionTest, CombatFreezesTheTimeout)
{
    // A mid-recovery add pull must not burn the budget: with the party (still)
    // in combat an expired clock does NOT disable.
    auto party = BaseParty();
    party[2].isDead = true;
    Inputs in = BaseInputs();
    in.pendingSinceMs = 1000;
    in.nowMs = 1000 + in.timeoutMs * 10;
    in.partyEngaged = true;
    Result const r = Decide(in, party);
    EXPECT_EQ(r.outcome, Outcome::Hold);
    EXPECT_EQ(r.reason, Reason::Recovering);
}

TEST(DcRezDecisionTest, UnstampedClockNeverTimesOut)
{
    // pendingSinceMs == 0 means the glue hasn't started the clock (e.g. the
    // first out-of-combat evaluation this tick) — no timeout can fire off it.
    auto party = BaseParty();
    party[2].isDead = true;
    Inputs in = BaseInputs();
    in.pendingSinceMs = 0;
    in.nowMs = 0xFFFF0000u;
    EXPECT_EQ(Decide(in, party).outcome, Outcome::Hold);
}

TEST(DcRezDecisionTest, ClockRestampTrajectoryFreezesAcrossCombat)
{
    // Multi-eval trajectory of the glue's stamp/clear contract: clock runs out
    // of combat, combat clears it (glue passes 0 + inCombat), the post-combat
    // re-stamp starts a FRESH budget — the earlier elapsed time is forgiven.
    auto party = BaseParty();
    party[2].isDead = true;
    Inputs in = BaseInputs();

    // t=10s: recovery starts, clock stamped.
    in.pendingSinceMs = 10000;
    in.nowMs = 10000;
    EXPECT_EQ(Decide(in, party).outcome, Outcome::Hold);

    // t=80s: an add pull — glue cleared the stamp while engaged.
    in.pendingSinceMs = 0;
    in.nowMs = 80000;
    in.partyEngaged = true;
    EXPECT_EQ(Decide(in, party).outcome, Outcome::Hold);

    // t=100s: combat over, glue re-stamped. 90s elapsed since the FIRST stamp,
    // but the fresh budget holds.
    in.pendingSinceMs = 100000;
    in.nowMs = 100000 + 5000;
    in.partyEngaged = false;
    EXPECT_EQ(Decide(in, party).outcome, Outcome::Hold);

    // ...and only the fresh budget expiring disables.
    in.nowMs = 100000 + in.timeoutMs;
    EXPECT_EQ(Decide(in, party).reason, Reason::TimedOut);
}

// ---- degenerates ----------------------------------------------------------------

TEST(DcRezDecisionTest, TimeoutBeatsWaitingOnHuman)
{
    // An ignored "waiting for you to rez" prompt still disables on the clock.
    auto party = BaseParty();
    party[0].isDead = true;
    party[1].isDead = true;
    party[4].canRezClass = true;
    Inputs in = BaseInputs();
    in.pendingSinceMs = 1000;
    in.nowMs = 1000 + in.timeoutMs;
    Result const r = Decide(in, party);
    EXPECT_EQ(r.outcome, Outcome::Disable);
    EXPECT_EQ(r.reason, Reason::TimedOut);
}

TEST(DcRezDecisionTest, SoloDeadTankIsAWipe)
{
    Member solo;
    solo.canRezClass = true;
    solo.isTankRole = true;
    solo.isBot = true;
    solo.isDead = true;
    Result const r = Decide(BaseInputs(), {solo});
    EXPECT_EQ(r.outcome, Outcome::Disable);
    EXPECT_EQ(r.reason, Reason::Wipe);
}

// ---- the instance refuses the spell ----------------------------------------------
//
// Spell::CheckCast refuses every resurrect cast inside a dungeon reporting an
// encounter in progress. The Violet Hold holds that state for the entire run, so
// the elected rezzer stood over a corpse re-casting into the refusal until the 90s
// budget expired: 18 of the first 72 runs of tp-20260827-065217-2 ended as
// "Couldn't get X resurrected in time" with the party alive and the hold draining.

TEST(DcRezDecisionTest, BlockedHoldsWhileTheBlockMightStillLift)
{
    auto party = BaseParty();
    party[2].isDead = true;
    Inputs in = BaseInputs();
    in.rezBlocked = true;
    in.blockedSinceMs = in.nowMs - 5000;
    in.blockedHoldMaxMs = 20000;
    Result const r = Decide(in, party);
    EXPECT_EQ(r.outcome, Outcome::Hold);
    EXPECT_EQ(r.reason, Reason::BlockedWaiting);
    // A corpse is named so the status panel has someone to name; nobody is elected,
    // because electing a rezzer is what arms the cast rung that cannot succeed.
    EXPECT_EQ(r.targetIdx, 2);
    EXPECT_EQ(r.rezzerIdx, -1);
}

TEST(DcRezDecisionTest, BlockedStandsDownOnceTheWaitRunsOut)
{
    auto party = BaseParty();
    party[2].isDead = true;
    Inputs in = BaseInputs();
    in.rezBlocked = true;
    in.blockedSinceMs = in.nowMs - 20000;
    in.blockedHoldMaxMs = 20000;
    Result const r = Decide(in, party);
    // Neither hold nor disable: the party is alive and the dungeon is winnable.
    EXPECT_EQ(r.outcome, Outcome::None);
    EXPECT_EQ(r.reason, Reason::BlockedStandDown);
}

TEST(DcRezDecisionTest, BlockedWithNoWaitConfiguredStandsDownImmediately)
{
    auto party = BaseParty();
    party[2].isDead = true;
    Inputs in = BaseInputs();
    in.rezBlocked = true;  // blockedHoldMaxMs left at 0
    Result const r = Decide(in, party);
    EXPECT_EQ(r.outcome, Outcome::None);
    EXPECT_EQ(r.reason, Reason::BlockedStandDown);
}

TEST(DcRezDecisionTest, AnUnstampedBlockClockStillHolds)
{
    // The glue stamps the clock a tick behind the first blocked read; until it does,
    // the block is treated as brand new rather than as already expired.
    auto party = BaseParty();
    party[2].isDead = true;
    Inputs in = BaseInputs();
    in.rezBlocked = true;
    in.blockedSinceMs = 0;
    in.blockedHoldMaxMs = 20000;
    Result const r = Decide(in, party);
    EXPECT_EQ(r.outcome, Outcome::Hold);
    EXPECT_EQ(r.reason, Reason::BlockedWaiting);
}

TEST(DcRezDecisionTest, BlockedOutranksTheNoRezzerDisable)
{
    // Both rez classes are down. Normally that ends the run — but while the instance
    // forbids the spell, which classes are still standing is not a fact about
    // anything, and the survivors can still finish the dungeon (99 of 100 heroic runs
    // of tp-20260826-233949-1 did, having never needed a rez at all).
    auto party = BaseParty();
    party[0].isDead = true;
    party[1].isDead = true;
    Inputs in = BaseInputs();
    in.rezBlocked = true;
    in.blockedSinceMs = in.nowMs - 30000;
    in.blockedHoldMaxMs = 20000;
    Result const r = Decide(in, party);
    EXPECT_EQ(r.outcome, Outcome::None);
    EXPECT_EQ(r.reason, Reason::BlockedStandDown);
}

TEST(DcRezDecisionTest, ABlockedFullWipeIsStillAWipe)
{
    auto party = BaseParty();
    for (Member& m : party)
        m.isDead = true;
    Inputs in = BaseInputs();
    in.rezBlocked = true;
    in.blockedSinceMs = in.nowMs - 30000;
    in.blockedHoldMaxMs = 20000;
    Result const r = Decide(in, party);
    EXPECT_EQ(r.outcome, Outcome::Disable);
    EXPECT_EQ(r.reason, Reason::Wipe);
}

TEST(DcRezDecisionTest, TheBlockedBranchIsInertWhenNothingIsBlocked)
{
    // Nothing latches: the ordinary election is unchanged the moment the block lifts.
    auto party = BaseParty();
    party[2].isDead = true;
    Inputs in = BaseInputs();
    in.rezBlocked = false;
    in.blockedSinceMs = in.nowMs - 30000;  // stale stamp the glue has yet to clear
    in.blockedHoldMaxMs = 20000;
    Result const r = Decide(in, party);
    EXPECT_EQ(r.outcome, Outcome::Hold);
    EXPECT_EQ(r.reason, Reason::Recovering);
    EXPECT_EQ(r.rezzerIdx, 1);
}

// ---- a corpse among idle hostiles is not walked to --------------------------------
//
// tr-20260927-184653-9 (Karazhan): a healer died among the Servants' Quarters
// Shadowbats under the Maiden corridor; the second healer walked down to raise her
// and died, then the tank and the enhancement shaman walked down after them both.

TEST(DcRezDecisionTest, EveryCorpseUnsafeStandsDownShortHanded)
{
    auto party = BaseParty();
    party[1].isDead = true;          // the healer, among the bats
    party[1].corpseUnsafe = true;
    Result const r = Decide(BaseInputs(), party);
    // Neither hold (nobody walks) nor disable (the party is alive).
    EXPECT_EQ(r.outcome, Outcome::None);
    EXPECT_EQ(r.reason, Reason::UnsafeStandDown);
    EXPECT_EQ(r.rezzerIdx, -1);
    EXPECT_TRUE(r.pairs.empty());
    // Named, for the announcement.
    EXPECT_EQ(r.targetIdx, 1);
}

TEST(DcRezDecisionTest, AnUnsafeCorpseIsSkippedForASafeOne)
{
    // Healer corpse unsafe, mage corpse safe: the tank raises the mage, and the
    // healer — normally first in line — waits for a body that is safe to reach.
    auto party = BaseParty();
    party[1].isDead = true;
    party[1].corpseUnsafe = true;
    party[2].isDead = true;
    Result const r = Decide(BaseInputs(), party);
    EXPECT_EQ(r.outcome, Outcome::Hold);
    EXPECT_EQ(r.reason, Reason::Recovering);
    EXPECT_EQ(r.rezzerIdx, 0);
    EXPECT_EQ(r.targetIdx, 2);
    ASSERT_EQ(r.pairs.size(), 1u);
    EXPECT_EQ(r.pairs[0], std::make_pair(0, 2));
}

TEST(DcRezDecisionTest, TheCorpseComesBackIntoPlayOnceItIsSafe)
{
    // Nothing latches: the same corpse, its pack cleared, is an ordinary recovery.
    auto party = BaseParty();
    party[2].isDead = true;
    party[2].corpseUnsafe = true;
    EXPECT_EQ(Decide(BaseInputs(), party).reason, Reason::UnsafeStandDown);
    party[2].corpseUnsafe = false;
    Result const r = Decide(BaseInputs(), party);
    EXPECT_EQ(r.outcome, Outcome::Hold);
    EXPECT_EQ(r.reason, Reason::Recovering);
    EXPECT_EQ(r.targetIdx, 2);
}

TEST(DcRezDecisionTest, AnUnsafeCorpseDoesNotSaveARunWithNoRezzer)
{
    // Both rez classes dead: the classic NoRezzer disable, unsafe or not.
    auto party = BaseParty();
    party[0].isDead = true;
    party[1].isDead = true;
    party[1].corpseUnsafe = true;
    Result const r = Decide(BaseInputs(), party);
    EXPECT_EQ(r.outcome, Outcome::Disable);
    EXPECT_EQ(r.reason, Reason::NoRezzer);
}

TEST(DcRezDecisionTest, AnUnsafeCorpseIsLeftToAHumanRezzer)
{
    // Only a human can rez: it is their walk to judge, so the hold is unchanged.
    auto party = BaseParty();
    party[0].isDead = true;
    party[1].isDead = true;
    party[1].corpseUnsafe = true;
    party[4].canRezClass = true;
    Result const r = Decide(BaseInputs(), party);
    EXPECT_EQ(r.outcome, Outcome::Hold);
    EXPECT_EQ(r.reason, Reason::WaitingOnHuman);
    EXPECT_EQ(r.rezzerIdx, 4);
}

TEST(DcRezDecisionTest, AnUnsafeFullWipeIsStillAWipe)
{
    auto party = BaseParty();
    for (Member& m : party)
    {
        m.isDead = true;
        m.corpseUnsafe = true;
    }
    Result const r = Decide(BaseInputs(), party);
    EXPECT_EQ(r.outcome, Outcome::Disable);
    EXPECT_EQ(r.reason, Reason::Wipe);
}
