/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "gtest/gtest.h"

#include <algorithm>
#include <set>
#include <vector>

#include "LFG.h"

#include "DungeonQueueFill/DcDungeonQueueFillJob.h"
#include "DungeonQueueFill/DcDungeonQueueFillPlanner.h"

using namespace DcDungeonQueueFillPlanner;

namespace
{
    // Fills are planned for a dungeon set, and the death-knight rows are gated
    // on its expansion as well as the level. Most of these cases care only
    // about the role arithmetic, so they plan for Wrath content.
    constexpr std::uint32_t kWrath = DcTestDungeonRegistry::kExpansionWrath;
    constexpr std::uint32_t kClassic = 0;
    constexpr std::uint32_t kTbc = 1;

    Human H(std::uint8_t mask)
    {
        Human h;
        h.roleMask = mask;
        return h;
    }

    std::size_t CountRole(Result const& r, std::uint8_t bit)
    {
        std::size_t n = 0;
        for (Slot const& s : r.slots)
            if (s.roleMask == bit)
                ++n;
        return n;
    }

    bool Contains(std::vector<std::uint8_t> const& v, std::uint8_t c)
    {
        return std::find(v.begin(), v.end(), c) != v.end();
    }

    // Was the planner FORCED to re-draw an avoided class for slot `i`? Only
    // when every class that role can field is either on the avoid list or
    // already taken by an EARLIER slot — slots are filled tank, healer, then
    // dps, so "earlier" is exactly what the draw had already spent when it got
    // here. The veto is documented as a preference that yields rather than
    // fail, and a party of four can genuinely swallow all four Wrath tank
    // classes, so the tests below assert "not repeated unless forced" rather
    // than an absolute the kernel never promised.
    bool VetoWasForced(Result const& r, std::size_t i, std::vector<std::uint8_t> const& avoid,
                       DcTestComp::Roster roster)
    {
        for (DcTestComp::Slot const& candidate : DcTestComp::RolePool(r.slots[i].role, roster))
        {
            if (Contains(avoid, candidate.classId))
                continue;
            bool spent = false;
            for (std::size_t j = 0; j < i; ++j)
                if (r.slots[j].classId == candidate.classId)
                    spent = true;
            if (!spent)
                return false;  // an alternative was available and not taken
        }
        return true;
    }

    // ---- the constants this kernel restates ------------------------------
    //
    // The planner deliberately spells the LFG role bits and party quotas out
    // rather than including a core header, so that it stays engine-free. That
    // is only safe while the two agree — this is the test that keeps them
    // honest, and the reason the restated values carry a comment saying so.

    TEST(DcDungeonQueueFillPlanner, RoleBitsMatchTheCore)
    {
        EXPECT_EQ(static_cast<std::uint8_t>(lfg::PLAYER_ROLE_NONE), kRoleNone);
        EXPECT_EQ(static_cast<std::uint8_t>(lfg::PLAYER_ROLE_LEADER), kRoleLeader);
        EXPECT_EQ(static_cast<std::uint8_t>(lfg::PLAYER_ROLE_TANK), kRoleTank);
        EXPECT_EQ(static_cast<std::uint8_t>(lfg::PLAYER_ROLE_HEALER), kRoleHealer);
        EXPECT_EQ(static_cast<std::uint8_t>(lfg::PLAYER_ROLE_DAMAGE), kRoleDamage);
    }

    TEST(DcDungeonQueueFillPlanner, QuotasMatchTheCore)
    {
        EXPECT_EQ(static_cast<std::size_t>(lfg::LFG_TANKS_NEEDED), kTanksNeeded);
        EXPECT_EQ(static_cast<std::size_t>(lfg::LFG_HEALERS_NEEDED), kHealersNeeded);
        EXPECT_EQ(static_cast<std::size_t>(lfg::LFG_DPS_NEEDED), kDpsNeeded);
        EXPECT_EQ(kTanksNeeded + kHealersNeeded + kDpsNeeded, kPartySize);
    }

    TEST(DcDungeonQueueFillPlanner, MaskForRoleKnowsTheThreeTokens)
    {
        EXPECT_EQ(kRoleTank, MaskForRole("tank"));
        EXPECT_EQ(kRoleHealer, MaskForRole("heal"));
        EXPECT_EQ(kRoleDamage, MaskForRole("dps"));
        EXPECT_EQ(kRoleNone, MaskForRole("healer"));  // not the token DcTestComp uses
        EXPECT_EQ(kRoleNone, MaskForRole(nullptr));
    }

    // ---- the deficit -----------------------------------------------------

    TEST(DcDungeonQueueFillPlanner, SoloTankNeedsAHealerAndThreeDps)
    {
        Result const r = Plan({H(kRoleTank)}, 80, kWrath, 1);
        ASSERT_EQ(Kind::Ok, r.kind);
        ASSERT_EQ(4u, r.slots.size());
        EXPECT_EQ(0u, CountRole(r, kRoleTank));
        EXPECT_EQ(1u, CountRole(r, kRoleHealer));
        EXPECT_EQ(3u, CountRole(r, kRoleDamage));
    }

    TEST(DcDungeonQueueFillPlanner, SoloHealerNeedsATankAndThreeDps)
    {
        Result const r = Plan({H(kRoleHealer)}, 80, kWrath, 1);
        ASSERT_EQ(Kind::Ok, r.kind);
        ASSERT_EQ(4u, r.slots.size());
        EXPECT_EQ(1u, CountRole(r, kRoleTank));
        EXPECT_EQ(0u, CountRole(r, kRoleHealer));
        EXPECT_EQ(3u, CountRole(r, kRoleDamage));
    }

    TEST(DcDungeonQueueFillPlanner, SoloDpsNeedsATankAHealerAndTwoDps)
    {
        Result const r = Plan({H(kRoleDamage)}, 80, kWrath, 1);
        ASSERT_EQ(Kind::Ok, r.kind);
        ASSERT_EQ(4u, r.slots.size());
        EXPECT_EQ(1u, CountRole(r, kRoleTank));
        EXPECT_EQ(1u, CountRole(r, kRoleHealer));
        EXPECT_EQ(2u, CountRole(r, kRoleDamage));
    }

    // Every legal party of one to four humans completes to exactly 1 tank /
    // 1 healer / 3 dps once the bots are added. An illegal one — roles that
    // cannot make that party at all — is refused by name instead.
    TEST(DcDungeonQueueFillPlanner, EveryLegalPartyCompletesToOneTankOneHealerThreeDps)
    {
        std::uint8_t const masks[] = {kRoleTank, kRoleHealer, kRoleDamage,
                                      kRoleTank | kRoleDamage,
                                      kRoleHealer | kRoleDamage,
                                      static_cast<std::uint8_t>(kRoleTank | kRoleHealer |
                                                                kRoleDamage)};
        for (std::uint8_t const a : masks)
            for (std::uint8_t const b : masks)
                for (std::uint8_t const c : masks)
                    for (std::uint8_t const d : masks)
                    {
                        std::vector<Human> const parties[4] = {
                            {H(a)}, {H(a), H(b)}, {H(a), H(b), H(c)}, {H(a), H(b), H(c), H(d)}};
                        for (std::vector<Human> const& humans : parties)
                        {
                            Result const r = Plan(humans, 80, kWrath, 7);
                            if (r.kind != Kind::Ok)
                            {
                                // The only legal refusal here: roles that
                                // cannot make a 1/1/3 party. The stock
                                // matchmaker could not complete it either.
                                EXPECT_EQ(Kind::Unresolvable, r.kind);
                                continue;
                            }

                            ASSERT_EQ(kPartySize - humans.size(), r.slots.size());

                            std::size_t tanks = 0, healers = 0, dps = 0;
                            for (std::uint8_t const m : r.humanRoles)
                            {
                                tanks += m == kRoleTank;
                                healers += m == kRoleHealer;
                                dps += m == kRoleDamage;
                            }
                            tanks += CountRole(r, kRoleTank);
                            healers += CountRole(r, kRoleHealer);
                            dps += CountRole(r, kRoleDamage);

                            EXPECT_EQ(kTanksNeeded, tanks);
                            EXPECT_EQ(kHealersNeeded, healers);
                            EXPECT_EQ(kDpsNeeded, dps);
                        }
                    }
    }

    // ---- multi-role humans ----------------------------------------------
    //
    // The bug these pin: a player may TICK several roles but plays exactly one
    // of them. Reading "tank or dps" as "tank and dps" leaves the fill one bot
    // short, and a four-man party never matches — the dungeon simply never
    // starts.

    TEST(DcDungeonQueueFillPlanner, AMultiRoleHumanStillNeedsFourBots)
    {
        std::uint8_t const pairs[] = {static_cast<std::uint8_t>(kRoleTank | kRoleDamage),
                                      static_cast<std::uint8_t>(kRoleTank | kRoleHealer),
                                      static_cast<std::uint8_t>(kRoleHealer | kRoleDamage),
                                      static_cast<std::uint8_t>(kRoleTank | kRoleHealer |
                                                                kRoleDamage)};
        for (std::uint8_t const mask : pairs)
            for (std::uint32_t seed = 1; seed <= 50; ++seed)
            {
                Result const r = Plan({H(mask)}, 80, kWrath, seed);
                ASSERT_EQ(Kind::Ok, r.kind);
                EXPECT_EQ(4u, r.slots.size()) << "mask " << int(mask) << " seed " << seed;

                // ...and the four fill exactly the seats the human does not.
                ASSERT_EQ(1u, r.humanRoles.size());
                std::size_t const tanks = CountRole(r, kRoleTank) +
                                          (r.humanRoles[0] == kRoleTank ? 1 : 0);
                std::size_t const healers = CountRole(r, kRoleHealer) +
                                            (r.humanRoles[0] == kRoleHealer ? 1 : 0);
                std::size_t const dps = CountRole(r, kRoleDamage) +
                                        (r.humanRoles[0] == kRoleDamage ? 1 : 0);
                EXPECT_EQ(kTanksNeeded, tanks);
                EXPECT_EQ(kHealersNeeded, healers);
                EXPECT_EQ(kDpsNeeded, dps);
            }
    }

    TEST(DcDungeonQueueFillPlanner, AMultiRoleHumanIsGivenOneOfTheirRolesAtRandom)
    {
        // Both of a "tank or dps" player's roles come up across seeds, and the
        // role they are given is always one they actually ticked.
        std::uint8_t const mask = kRoleTank | kRoleDamage;
        bool sawTank = false, sawDps = false;
        for (std::uint32_t seed = 1; seed <= 50; ++seed)
        {
            std::vector<std::uint8_t> const assigned = AssignHumanRoles({H(mask)}, seed);
            ASSERT_EQ(1u, assigned.size());
            EXPECT_TRUE(assigned[0] & mask) << "seed " << seed;
            sawTank |= assigned[0] == kRoleTank;
            sawDps |= assigned[0] == kRoleDamage;
        }
        EXPECT_TRUE(sawTank);
        EXPECT_TRUE(sawDps);
    }

    TEST(DcDungeonQueueFillPlanner, TheSameSeedAssignsTheSameHumanRoles)
    {
        // The draw is seeded, never rand(): a fill that trips a bug replays
        // from its log line, and this suite does not flake.
        std::uint8_t const all = kRoleTank | kRoleHealer | kRoleDamage;
        for (std::uint32_t seed = 1; seed <= 20; ++seed)
            EXPECT_EQ(AssignHumanRoles({H(all), H(all)}, seed),
                      AssignHumanRoles({H(all), H(all)}, seed));
    }

    TEST(DcDungeonQueueFillPlanner, TwoAllRoleHumansTakeTwoDifferentRoles)
    {
        std::uint8_t const all = kRoleTank | kRoleHealer | kRoleDamage;
        for (std::uint32_t seed = 1; seed <= 50; ++seed)
        {
            std::vector<std::uint8_t> const assigned = AssignHumanRoles({H(all), H(all)}, seed);
            ASSERT_EQ(2u, assigned.size());
            EXPECT_NE(kRoleNone, assigned[0]);
            EXPECT_NE(kRoleNone, assigned[1]);
            // Two tanks is not a party; two dps is.
            if (assigned[0] != kRoleDamage)
                EXPECT_NE(assigned[0], assigned[1]) << "seed " << seed;

            Result const r = Plan({H(all), H(all)}, 80, kWrath, seed);
            ASSERT_EQ(Kind::Ok, r.kind);
            EXPECT_EQ(3u, r.slots.size());
        }
    }

    // A random pick that ignored feasibility would refuse this party whenever
    // both players rolled tank. The draw is over the LEGAL assignments only.
    TEST(DcDungeonQueueFillPlanner, TwoTankOrDpsHumansAlwaysMakeAParty)
    {
        std::uint8_t const tankOrDps = kRoleTank | kRoleDamage;
        for (std::uint32_t seed = 1; seed <= 100; ++seed)
        {
            Result const r = Plan({H(tankOrDps), H(tankOrDps)}, 80, kWrath, seed);
            ASSERT_EQ(Kind::Ok, r.kind) << "seed " << seed;
            EXPECT_EQ(3u, r.slots.size());

            ASSERT_EQ(2u, r.humanRoles.size());
            EXPECT_FALSE(r.humanRoles[0] == kRoleTank && r.humanRoles[1] == kRoleTank);
        }
    }

    TEST(DcDungeonQueueFillPlanner, AHumanWhoTickedNothingIsUnresolvable)
    {
        Result const r = Plan({H(kRoleNone)}, 80, kWrath, 1);
        EXPECT_EQ(Kind::Unresolvable, r.kind);
        EXPECT_FALSE(r.detail.empty());
    }

    TEST(DcDungeonQueueFillPlanner, TwoTankOnlyHumansCannotMakeAParty)
    {
        Result const r = Plan({H(kRoleTank), H(kRoleTank)}, 80, kWrath, 1);
        EXPECT_EQ(Kind::Unresolvable, r.kind);
        EXPECT_FALSE(r.detail.empty());
    }

    TEST(DcDungeonQueueFillPlanner, FourDpsOnlyHumansCannotMakeAParty)
    {
        // Four DPS need a tank AND a healer, but only one slot is left.
        Result const r = Plan({H(kRoleDamage), H(kRoleDamage), H(kRoleDamage), H(kRoleDamage)},
                              80, kWrath, 1);
        EXPECT_EQ(Kind::Unresolvable, r.kind);
    }

    // ---- refusals --------------------------------------------------------

    TEST(DcDungeonQueueFillPlanner, AFullPartyIsRefused)
    {
        std::uint8_t const all = kRoleTank | kRoleHealer | kRoleDamage;
        Result const r = Plan({H(all), H(all), H(all), H(all), H(all)}, 80, kWrath, 1);
        EXPECT_EQ(Kind::PartyFull, r.kind);
        EXPECT_TRUE(r.slots.empty());
    }

    TEST(DcDungeonQueueFillPlanner, NoHumansIsRefused)
    {
        Result const r = Plan({}, 80, kWrath, 1);
        EXPECT_EQ(Kind::NoHumans, r.kind);
    }

    // ---- what the bots are ----------------------------------------------

    TEST(DcDungeonQueueFillPlanner, EveryBotGetsExactlyOneRoleBit)
    {
        // A bot offering two roles adds a branch CheckGroupRoles can resolve
        // into a party with no tank, so the deficit the planner computed would
        // not be the deficit the matchmaker sees.
        for (std::uint32_t seed = 1; seed <= 50; ++seed)
        {
            Result const r = Plan({H(kRoleDamage)}, 80, kWrath, seed);
            ASSERT_EQ(Kind::Ok, r.kind);
            for (Slot const& s : r.slots)
            {
                std::uint8_t const m = s.roleMask;
                EXPECT_TRUE(m == kRoleTank || m == kRoleHealer || m == kRoleDamage);
                EXPECT_EQ(m, MaskForRole(s.role));
            }
        }
    }

    TEST(DcDungeonQueueFillPlanner, FilledSlotsUseDistinctClasses)
    {
        for (std::uint32_t seed = 1; seed <= 50; ++seed)
        {
            Result const r = Plan({H(kRoleDamage)}, 80, kWrath, seed);
            ASSERT_EQ(Kind::Ok, r.kind);
            std::set<std::uint8_t> classes;
            for (Slot const& s : r.slots)
                classes.insert(s.classId);
            EXPECT_EQ(r.slots.size(), classes.size()) << "seed " << seed;
        }
    }

    TEST(DcDungeonQueueFillPlanner, TheSameSeedPlansTheSameParty)
    {
        Result const a = Plan({H(kRoleHealer)}, 80, kWrath, 12345);
        Result const b = Plan({H(kRoleHealer)}, 80, kWrath, 12345);
        ASSERT_EQ(a.slots.size(), b.slots.size());
        for (std::size_t i = 0; i < a.slots.size(); ++i)
        {
            EXPECT_EQ(a.slots[i].classId, b.slots[i].classId);
            EXPECT_STREQ(a.slots[i].specName, b.slots[i].specName);
        }
    }

    // ---- two dungeons in a row are not the same party --------------------
    //
    // The class draw is seeded, and the seed comes off a per-fill job id, so
    // successive fills already draw independently. Independent is not enough:
    // the tank pool is three classes deep before Wrath and four in it, so an
    // independent draw hands the player the same tank a quarter of the time.
    // `avoidClasses` carries the previous party, and these are the tests that
    // say it is actually consulted.

    TEST(DcDungeonQueueFillPlanner, AvoidedClassesAreNotDrawnWhileThePoolHasAlternatives)
    {
        for (std::uint32_t seed = 1; seed <= 300; ++seed)
        {
            Result const first = Plan({H(kRoleDamage)}, 80, kWrath, seed);
            ASSERT_EQ(Kind::Ok, first.kind);

            std::vector<std::uint8_t> avoid;
            for (Slot const& s : first.slots)
                avoid.push_back(s.classId);

            Result const second = Plan({H(kRoleDamage)}, 80, kWrath, seed + 1000, avoid);
            ASSERT_EQ(Kind::Ok, second.kind);
            for (std::size_t i = 0; i < second.slots.size(); ++i)
                if (Contains(avoid, second.slots[i].classId))
                    EXPECT_TRUE(VetoWasForced(second, i, avoid, DcTestComp::Roster::WithDeathKnights))
                        << "seed " << seed << ": re-drew class "
                        << static_cast<int>(second.slots[i].classId) << " for "
                        << second.slots[i].role << " with an alternative going spare";
        }
    }

    // The whole point, stated the way a player would put it: run two dungeons
    // back to back and the party is different. Vetoing a four-class party can
    // exhaust the four-deep Wrath tank pool, so this measures how often the
    // fill is forced to reuse anything at all rather than forbidding it — the
    // number that matters is that the overwhelming majority of back-to-back
    // fills share NOTHING.
    TEST(DcDungeonQueueFillPlanner, BackToBackFillsAlmostNeverShareAMember)
    {
        std::size_t identical = 0, sharedAny = 0;
        constexpr std::size_t kTrials = 500;
        for (std::uint32_t seed = 1; seed <= kTrials; ++seed)
        {
            Result const first = Plan({H(kRoleDamage)}, 80, kWrath, seed);
            ASSERT_EQ(Kind::Ok, first.kind);

            std::vector<std::uint8_t> avoid;
            for (Slot const& s : first.slots)
                avoid.push_back(s.classId);

            // The SAME seed as well: a fill must not be able to reproduce the
            // previous party even when the draw repeats itself exactly.
            Result const second = Plan({H(kRoleDamage)}, 80, kWrath, seed, avoid);
            ASSERT_EQ(Kind::Ok, second.kind);
            ASSERT_EQ(first.slots.size(), second.slots.size());

            std::size_t same = 0;
            for (std::size_t i = 0; i < first.slots.size(); ++i)
                if (first.slots[i].classId == second.slots[i].classId)
                    ++same;
            if (same)
                ++sharedAny;
            if (same == first.slots.size())
                ++identical;
        }

        // Never the same four. A repeat of any single member is the exhausted
        // -pool escape hatch and is rare; 5% leaves room for the pools to
        // change shape without turning this into a tripwire.
        EXPECT_EQ(0u, identical);
        EXPECT_LT(sharedAny, kTrials / 20);
    }

    // The veto is a preference, never a refusal. A tank pool three deep with
    // two of its classes already spoken for has to be allowed to repeat the
    // third rather than field a party with no tank.
    TEST(DcDungeonQueueFillPlanner, AnExhaustedPoolDropsTheVetoRatherThanTheParty)
    {
        // Veto every tank class there is, pre-Wrath (warrior, paladin, druid).
        std::vector<std::uint8_t> const allTanks = {1, 2, 11};
        for (std::uint32_t seed = 1; seed <= 100; ++seed)
        {
            Result const r = Plan({H(kRoleDamage)}, 80, kClassic, seed, allTanks);
            ASSERT_EQ(Kind::Ok, r.kind) << "seed " << seed;
            EXPECT_EQ(1u, CountRole(r, kRoleTank));
            EXPECT_EQ(1u, CountRole(r, kRoleHealer));
            EXPECT_EQ(2u, CountRole(r, kRoleDamage));
        }
    }

    // Distinctness within the party survives the veto: dropping a tier must
    // not let two slots land on one class.
    TEST(DcDungeonQueueFillPlanner, FilledSlotsStayDistinctUnderAnAvoidList)
    {
        std::vector<std::uint8_t> const avoidMost = {1, 2, 3, 4, 5, 6, 7, 8};
        for (std::uint32_t seed = 1; seed <= 200; ++seed)
        {
            Result const r = Plan({H(kRoleHealer)}, 80, kWrath, seed, avoidMost);
            ASSERT_EQ(Kind::Ok, r.kind);
            std::set<std::uint8_t> classes;
            for (Slot const& s : r.slots)
                classes.insert(s.classId);
            EXPECT_EQ(r.slots.size(), classes.size()) << "seed " << seed;
        }
    }

    TEST(DcDungeonQueueFillPlanner, TheSameSeedAndAvoidListPlanTheSameParty)
    {
        // Still no rand(): the avoid list is an ARGUMENT, so the kernel stays
        // pure and a fill replays from the seed in its log line.
        std::vector<std::uint8_t> const avoid = {1, 5, 8};
        for (std::uint32_t seed = 1; seed <= 50; ++seed)
        {
            Result const a = Plan({H(kRoleTank)}, 80, kWrath, seed, avoid);
            Result const b = Plan({H(kRoleTank)}, 80, kWrath, seed, avoid);
            ASSERT_EQ(a.slots.size(), b.slots.size());
            for (std::size_t i = 0; i < a.slots.size(); ++i)
            {
                EXPECT_EQ(a.slots[i].classId, b.slots[i].classId);
                EXPECT_STREQ(a.slots[i].specName, b.slots[i].specName);
            }
        }
    }

    // An empty avoid list is the no-history case, and must plan exactly what
    // the four-argument call always did — the default argument is not allowed
    // to move an old seed's party.
    TEST(DcDungeonQueueFillPlanner, AnEmptyAvoidListChangesNothing)
    {
        for (std::uint32_t seed = 1; seed <= 50; ++seed)
        {
            Result const a = Plan({H(kRoleDamage)}, 80, kWrath, seed);
            Result const b = Plan({H(kRoleDamage)}, 80, kWrath, seed, {});
            ASSERT_EQ(a.slots.size(), b.slots.size());
            for (std::size_t i = 0; i < a.slots.size(); ++i)
                EXPECT_EQ(a.slots[i].classId, b.slots[i].classId) << "seed " << seed;
        }
    }

    // ---- the draw the job shares -----------------------------------------
    //
    // The pool-character pick in DcDungeonQueueFillJob runs off this same
    // stream. It used to take the first free character of the class, which
    // handed every fill that rolled a warrior the SAME warrior — the single
    // biggest reason two dungeons in a row felt identical. It is a modulo draw
    // now, so what matters is that the stream actually spreads over a small
    // candidate list and never leaves it.
    TEST(DcDungeonQueueFillPlanner, NextRandCoversASmallCandidateListAndStaysInIt)
    {
        for (std::size_t count = 2; count <= 8; ++count)
        {
            std::set<std::size_t> seen;
            std::uint32_t state = 0x2545f491u ^ static_cast<std::uint32_t>(count);
            for (int i = 0; i < 400; ++i)
            {
                std::size_t const idx = NextRand(state) % count;
                ASSERT_LT(idx, count);
                seen.insert(idx);
            }
            EXPECT_EQ(count, seen.size()) << "candidates " << count;
        }
    }

    TEST(DcDungeonQueueFillPlanner, NextRandIsPureAndAdvances)
    {
        std::uint32_t a = 12345;
        std::uint32_t b = 12345;
        for (int i = 0; i < 100; ++i)
            EXPECT_EQ(NextRand(a), NextRand(b));
        EXPECT_EQ(a, b);

        // A stream that did not advance would hand every slot in a fill the
        // same draw.
        std::uint32_t c = 1;
        std::uint32_t const first = NextRand(c);
        EXPECT_NE(first, NextRand(c));
    }

    // ---- the death-knight level gate ------------------------------------

    TEST(DcDungeonQueueFillPlanner, NoDeathKnightBelowFiftyFive)
    {
        // A death knight's whole kit starts at 55, so a level-40 fill must
        // never draw one — it would be a party member with nothing to press.
        constexpr std::uint8_t kDeathKnight = 6;
        for (std::uint32_t seed = 1; seed <= 300; ++seed)
        {
            Result const r = Plan({H(kRoleDamage)}, DcTestComp::kDeathKnightMinLevel - 1, kWrath, seed);
            ASSERT_EQ(Kind::Ok, r.kind);
            for (Slot const& s : r.slots)
                EXPECT_NE(kDeathKnight, s.classId) << "seed " << seed;
        }
    }

    TEST(DcDungeonQueueFillPlanner, DeathKnightsAreOnTheTableAtFiftyFive)
    {
        constexpr std::uint8_t kDeathKnight = 6;
        bool seen = false;
        for (std::uint32_t seed = 1; seed <= 300 && !seen; ++seed)
        {
            Result const r = Plan({H(kRoleHealer)}, DcTestComp::kDeathKnightMinLevel, kWrath, seed);
            ASSERT_EQ(Kind::Ok, r.kind);
            for (Slot const& s : r.slots)
                if (s.classId == kDeathKnight)
                    seen = true;
        }
        EXPECT_TRUE(seen);
    }

    TEST(DcDungeonQueueFillPlanner, RosterFollowsTheDeathKnightFloorAndTheExpansion)
    {
        EXPECT_EQ(DcTestComp::Roster::NoDeathKnights,
                  RosterFor(DcTestComp::kDeathKnightMinLevel - 1, kWrath));
        EXPECT_EQ(DcTestComp::Roster::WithDeathKnights,
                  RosterFor(DcTestComp::kDeathKnightMinLevel, kWrath));
        EXPECT_EQ(DcTestComp::Roster::WithDeathKnights, RosterFor(80, kWrath));

        // Both halves of the gate, the way DcTestRunJob applies them: a
        // level-80 party running classic or TBC content still draws no death
        // knight.
        EXPECT_EQ(DcTestComp::Roster::NoDeathKnights, RosterFor(80, kClassic));
        EXPECT_EQ(DcTestComp::Roster::NoDeathKnights, RosterFor(80, kTbc));
    }

    // The live failure this pins: a level-60 player queued a TBC random, the
    // fill drew a death knight, and that bot could not enter the queue at all
    // — no pool death knight has the starting chain LFGMgr demands, so the
    // fill sat in queueing until it timed out and the dungeon never started.
    TEST(DcDungeonQueueFillPlanner, NoDeathKnightForPreWrathContent)
    {
        constexpr std::uint8_t kDeathKnight = 6;
        for (std::uint32_t const expansion : {kClassic, kTbc})
            for (std::uint32_t seed = 1; seed <= 300; ++seed)
            {
                Result const r = Plan({H(kRoleDamage)}, 80, expansion, seed);
                ASSERT_EQ(Kind::Ok, r.kind);
                for (Slot const& s : r.slots)
                    EXPECT_NE(kDeathKnight, s.classId) << "expansion " << expansion
                                                       << " seed " << seed;
            }
    }

    // ---- the job's stage table ------------------------------------------

    using Stage = DcDungeonQueueFillJob::Stage;

    TEST(DcDungeonQueueFillJob, EveryStageHasItsOwnName)
    {
        Stage const stages[] = {Stage::Observed,  Stage::Planning,  Stage::Claiming,
                                Stage::LoggingIn, Stage::Evicting,  Stage::Provisioning,
                                Stage::Sanitizing, Stage::Queueing, Stage::Waiting,
                                Stage::Formed,    Stage::Releasing, Stage::Done};
        std::set<std::string> names;
        for (Stage const s : stages)
        {
            std::string const name = DcDungeonQueueFillJob::StageName(s);
            EXPECT_NE("?", name);
            names.insert(name);
        }
        EXPECT_EQ(sizeof(stages) / sizeof(stages[0]), names.size());
    }

    // The "did the player change their mind mid-setup" watch is expressed as a
    // RANGE over this enum (Claiming..Queueing), so reordering it silently
    // changes which stages notice a cancelled queue. Pin the order.
    TEST(DcDungeonQueueFillJob, SetupStagesAreContiguousAndOrdered)
    {
        EXPECT_LT(Stage::Observed, Stage::Planning);
        EXPECT_LT(Stage::Planning, Stage::Claiming);
        EXPECT_LT(Stage::Claiming, Stage::LoggingIn);
        // The capital eviction sits between login and the factory roll: every
        // stage after it may assume the bot is standing in the open world.
        EXPECT_LT(Stage::LoggingIn, Stage::Evicting);
        EXPECT_LT(Stage::Evicting, Stage::Provisioning);
        EXPECT_LT(Stage::Provisioning, Stage::Sanitizing);
        EXPECT_LT(Stage::Sanitizing, Stage::Queueing);
        EXPECT_LT(Stage::Queueing, Stage::Waiting);
        EXPECT_LT(Stage::Waiting, Stage::Formed);
        EXPECT_LT(Stage::Formed, Stage::Releasing);
        EXPECT_LT(Stage::Releasing, Stage::Done);
    }
}
