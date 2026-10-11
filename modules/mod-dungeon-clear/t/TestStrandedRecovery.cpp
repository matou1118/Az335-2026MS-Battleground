/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "gtest/gtest.h"

#include "DcStrandedDecision.h"

using DcStrandedDecision::Decide;
using DcStrandedDecision::Inputs;
using DcStrandedDecision::Member;
using DcStrandedDecision::RescueSpread;
using DcStrandedDecision::Result;

namespace
{
    // A four-member party, all bots, all alive, all on-map, all in range of the
    // tank at [0]. Individual tests strand/kill/relocate members off this base.
    std::vector<Member> BaseParty()
    {
        auto make = [](bool tank, float dist)
        {
            Member m;
            m.isBot = true;
            m.isAlive = true;
            m.onMap = true;
            m.isTank = tank;
            m.distToTank = dist;
            return m;
        };
        return {
            make(/*tank*/ true, 0.0f),   // [0] leader tank
            make(false, 5.0f),           // [1] healer, in range
            make(false, 8.0f),           // [2] dps, in range
            make(false, 10.0f),          // [3] dps, in range
        };
    }

    // Clock armed and already stale: nowMs is one timeout past lastProgressMs, so
    // the no-progress window has elapsed. maxSpread 25 (the PartyMaxSpread default).
    Inputs StaleInputs()
    {
        Inputs in;
        in.enabled = true;
        in.nowMs = 400000;
        in.lastProgressMs = 100000;       // 300s ago ...
        in.noProgressTimeoutMs = 300000;  // ... == the window: elapsed
        in.partyEngaged = false;
        in.maxSpread = 25.0f;
        return in;
    }

    // Strand member `idx` at `dist` yards from the tank.
    void Strand(std::vector<Member>& p, std::size_t idx, float dist)
    {
        p[idx].distToTank = dist;
    }
}

// --- the two guards: window elapsed AND a stranded bot -----------------------

TEST(DcStrandedRecoveryTest, NoStrayNoRecover)
{
    // Window elapsed but everyone in range -> nothing to do.
    Result const r = Decide(StaleInputs(), BaseParty());
    EXPECT_FALSE(r.recover);
    EXPECT_TRUE(r.strandedIdx.empty());
}

TEST(DcStrandedRecoveryTest, StrandedBotPastWindowRecovers)
{
    std::vector<Member> party = BaseParty();
    Strand(party, 2, 60.0f);  // [2] fell out to 60yd
    Result const r = Decide(StaleInputs(), party);
    ASSERT_TRUE(r.recover);
    ASSERT_EQ(r.strandedIdx.size(), 1u);
    EXPECT_EQ(r.strandedIdx[0], 2);
}

TEST(DcStrandedRecoveryTest, MultipleStrandedAllSelected)
{
    std::vector<Member> party = BaseParty();
    Strand(party, 1, 40.0f);
    Strand(party, 3, 200.0f);  // fell under the world (huge 3D distance)
    Result const r = Decide(StaleInputs(), party);
    ASSERT_TRUE(r.recover);
    ASSERT_EQ(r.strandedIdx.size(), 2u);
    EXPECT_EQ(r.strandedIdx[0], 1);
    EXPECT_EQ(r.strandedIdx[1], 3);
}

// --- the timing guard --------------------------------------------------------

TEST(DcStrandedRecoveryTest, WindowNotElapsedNeverRecovers)
{
    Inputs in = StaleInputs();
    in.nowMs = in.lastProgressMs + in.noProgressTimeoutMs - 1;  // one ms short
    std::vector<Member> party = BaseParty();
    Strand(party, 2, 60.0f);
    EXPECT_FALSE(Decide(in, party).recover);
}

TEST(DcStrandedRecoveryTest, ExactlyAtWindowRecovers)
{
    Inputs in = StaleInputs();
    in.nowMs = in.lastProgressMs + in.noProgressTimeoutMs;  // exactly the window
    std::vector<Member> party = BaseParty();
    Strand(party, 2, 60.0f);
    EXPECT_TRUE(Decide(in, party).recover);
}

TEST(DcStrandedRecoveryTest, UnarmedClockNeverRecovers)
{
    Inputs in = StaleInputs();
    in.lastProgressMs = 0;  // clock never stamped (no run underway)
    std::vector<Member> party = BaseParty();
    Strand(party, 2, 60.0f);
    EXPECT_FALSE(Decide(in, party).recover);
}

TEST(DcStrandedRecoveryTest, ZeroTimeoutDisablesTheClock)
{
    Inputs in = StaleInputs();
    in.noProgressTimeoutMs = 0;  // "never give up"
    std::vector<Member> party = BaseParty();
    Strand(party, 2, 60.0f);
    EXPECT_FALSE(Decide(in, party).recover);
}

// --- the feature / combat gates ----------------------------------------------

TEST(DcStrandedRecoveryTest, FeatureOffNeverRecovers)
{
    Inputs in = StaleInputs();
    in.enabled = false;
    std::vector<Member> party = BaseParty();
    Strand(party, 2, 60.0f);
    EXPECT_FALSE(Decide(in, party).recover);
}

TEST(DcStrandedRecoveryTest, CombatNeverRecovers)
{
    Inputs in = StaleInputs();
    in.partyEngaged = true;  // a fight is progress; never teleport mid-fight
    std::vector<Member> party = BaseParty();
    Strand(party, 2, 60.0f);
    EXPECT_FALSE(Decide(in, party).recover);
}

TEST(DcStrandedRecoveryTest, APhantomCombatFlagStillRecovers)
{
    // The Arcatraz freeze (tr-20260801-194932-20). A 45yd hostile area aura holds
    // the whole party's combat flag with NOTHING aggroed, so followers stop where
    // they stand and the tank waits on them forever. The glue used to pass the raw
    // flag in here, which made this failsafe — the only thing that could have
    // broken that deadlock — permanently unreachable. `partyEngaged` is a fight,
    // and a flag with no fight behind it is not one.
    Inputs in = StaleInputs();
    in.partyEngaged = false;  // flagged, but nothing is fighting
    std::vector<Member> party = BaseParty();
    Strand(party, 2, 30.0f);
    Strand(party, 3, 31.0f);
    Result const r = Decide(in, party);
    EXPECT_TRUE(r.recover);
    EXPECT_EQ(r.strandedIdx.size(), 2u);
}

// --- who qualifies as a stray ------------------------------------------------

TEST(DcStrandedRecoveryTest, HumanIsNeverTeleported)
{
    std::vector<Member> party = BaseParty();
    party[2].isBot = false;   // the real player's seat
    Strand(party, 2, 80.0f);  // stranded, but a human — respect player agency
    EXPECT_FALSE(Decide(StaleInputs(), party).recover);
}

TEST(DcStrandedRecoveryTest, DeadMemberIsRezRecoverysJobNotOurs)
{
    std::vector<Member> party = BaseParty();
    party[2].isAlive = false;
    Strand(party, 2, 80.0f);
    EXPECT_FALSE(Decide(StaleInputs(), party).recover);
}

TEST(DcStrandedRecoveryTest, OffMapMemberIsIgnored)
{
    std::vector<Member> party = BaseParty();
    party[2].onMap = false;   // zoned out / different instance
    Strand(party, 2, 80.0f);
    EXPECT_FALSE(Decide(StaleInputs(), party).recover);
}

TEST(DcStrandedRecoveryTest, TankItselfIsNeverAStray)
{
    std::vector<Member> party = BaseParty();
    Strand(party, 0, 90.0f);  // the tank's own distance reads high (defensive)
    EXPECT_FALSE(Decide(StaleInputs(), party).recover);
}

TEST(DcStrandedRecoveryTest, RangeThresholdIsStrictlyGreater)
{
    Inputs const in = StaleInputs();  // maxSpread 25
    std::vector<Member> party = BaseParty();

    Strand(party, 2, 25.0f);          // exactly at the spread: still in range
    EXPECT_FALSE(Decide(in, party).recover);

    Strand(party, 2, 25.01f);         // just past it: stranded
    EXPECT_TRUE(Decide(in, party).recover);
}

// --- the anchor may be a CORPSE ---------------------------------------------

// When the tank dies, the glue keeps running this failsafe but re-anchors it on
// the run owner's BODY: the survivors' only way out of a wipe is a rez, a rez is
// cast at the corpse, and a rescue that gathers them anywhere else leaves the run
// exactly as stuck. So the kernel must not treat a dead tank row as a reason to
// stand down — it is the reference point, not a participant.
//
// Gundrak tp-20260830-231921-1: five runs wiped on Slad'ran, survivors parked
// 122yd from the corpse, every one killed by the 600s no-progress watchdog.
TEST(DcStrandedRecoveryTest, ADeadTankAnchorStillStrandsTheSurvivors)
{
    std::vector<Member> party = BaseParty();
    party[0].isAlive = false;      // the anchor is a corpse
    Strand(party, 2, 122.0f);      // ...and a survivor is 122yd out

    Result const r = Decide(StaleInputs(), party);

    EXPECT_TRUE(r.recover);
    ASSERT_EQ(r.strandedIdx.size(), 1u);
    EXPECT_EQ(r.strandedIdx[0], 2);
}

// The corpse is never itself a stray, however far the party has drifted from it:
// isTank is skipped before any distance test, so a dead anchor can never be
// selected for a teleport to itself.
TEST(DcStrandedRecoveryTest, TheAnchorIsNeverTeleportedEvenWhenDead)
{
    std::vector<Member> party = BaseParty();
    party[0].isAlive = false;
    party[0].distToTank = 900.0f;  // nonsense distance; isTank must win regardless
    Strand(party, 1, 122.0f);

    Result const r = Decide(StaleInputs(), party);

    ASSERT_EQ(r.strandedIdx.size(), 1u);
    EXPECT_EQ(r.strandedIdx[0], 1);
}

// --- the rescue threshold tracks the LEADER's live gate ----------------------
//
// RescueSpread is what closed the sealed-encounter dead band: the advance gate
// tightens to a row's musterSpread on a boss's final approach while this
// failsafe was still measuring against the raw PartyMaxSpread, so a straggler
// between the two was blocked by the gate and invisible to the rescue.
// tr-20260831-164201-61: healer 24.5yd out, gate 10, setting 25, 3174
// party-not-ready yields, run killed one boss from the end.

// The regression itself. 10 < 24.5 <= 25 was a permanent stall; it must now
// resolve to a rescue.
TEST(DcStrandedRecoveryTest, SealedApproachGateClosesTheDeadBand)
{
    float const spread = RescueSpread(/*partySpread*/ 25.0f, /*leaderGateSpread*/ 10.0f,
                                      /*gateIsTankAnchored*/ true);
    EXPECT_FLOAT_EQ(spread, 10.0f);

    Inputs in = StaleInputs();
    in.maxSpread = spread;
    std::vector<Member> party = BaseParty();
    Strand(party, 2, 24.5f);          // Zyaly: inside the old threshold, outside the gate
    EXPECT_TRUE(Decide(in, party).recover);

    // ...and the pre-fix threshold is exactly what made it invisible.
    in.maxSpread = 25.0f;
    EXPECT_FALSE(Decide(in, party).recover);
}

// MIN, never max: a gate that is LOOSER than the setting must not loosen the
// rescue. The waived gate (a pull maneuver holding the party at camp) is the
// live case — 100000 there — and it has to leave the threshold at 25.
TEST(DcStrandedRecoveryTest, RescueSpreadNeverLoosensPastTheSetting)
{
    EXPECT_FLOAT_EQ(RescueSpread(25.0f, 100000.0f, true), 25.0f);
    EXPECT_FLOAT_EQ(RescueSpread(25.0f, 40.0f, true), 25.0f);
    EXPECT_FLOAT_EQ(RescueSpread(25.0f, 25.0f, true), 25.0f);   // the ordinary gate
}

// A CAMP-anchored gate is measured from a different origin than this rescue
// (which measures from the tank), so its radius must be ignored outright rather
// than borrowed — otherwise a member correctly set at its camp gets yanked.
TEST(DcStrandedRecoveryTest, CampAnchoredGateIsNotBorrowed)
{
    EXPECT_FLOAT_EQ(RescueSpread(25.0f, 10.0f, /*gateIsTankAnchored*/ false), 25.0f);

    Inputs in = StaleInputs();
    in.maxSpread = RescueSpread(25.0f, 10.0f, false);
    std::vector<Member> party = BaseParty();
    Strand(party, 2, 18.0f);          // set at a camp 18yd behind the tank
    EXPECT_FALSE(Decide(in, party).recover);
}

// Degenerate gate readings fall back to the setting rather than to zero — a 0
// threshold would strand-and-teleport the entire party every timeout.
TEST(DcStrandedRecoveryTest, RescueSpreadIgnoresANonPositiveGate)
{
    EXPECT_FLOAT_EQ(RescueSpread(25.0f, 0.0f, true), 25.0f);
    EXPECT_FLOAT_EQ(RescueSpread(25.0f, -1.0f, true), 25.0f);
}
