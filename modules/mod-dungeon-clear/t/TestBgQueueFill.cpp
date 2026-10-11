/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "gtest/gtest.h"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "SharedDefines.h"

#include "BgQueueFill/DcBgQueueFillPlanner.h"

using namespace DcBgQueueFillPlanner;

namespace
{
    // battleground_template min/max per team, as shipped.
    struct Template
    {
        char const* name;
        std::uint32_t min;
        std::uint32_t max;
    };

    constexpr Template kWS{"WS", 5, 10};
    constexpr Template kAB{"AB", 8, 15};
    constexpr Template kEY{"EY", 8, 15};
    constexpr Template kSA{"SA", 7, 15};
    constexpr Template kAV{"AV", 20, 40};
    constexpr Template kIC{"IC", 20, 40};
    constexpr Template kRandom{"RB", 10, 10};

    constexpr Template kConcrete[] = {kWS, kAB, kEY, kSA, kAV, kIC};

    // A level-80 Alliance player queueing solo into the top bracket — the
    // shape most cases start from.
    Request SoloAt80(Template const& t, std::uint32_t seed = 1)
    {
        Request req;
        req.minPerTeam = t.min;
        req.maxPerTeam = t.max;
        req.bracketMinLevel = 80;
        req.bracketMaxLevel = 80;
        req.playerLevel = 80;
        req.playerTeam = kTeamAlliance;
        req.sides[kTeamAlliance].humansQueued = 1;
        req.seed = seed;
        return req;
    }

    std::uint32_t CountSide(Result const& r, std::uint8_t team)
    {
        std::uint32_t n = 0;
        for (Slot const& s : r.slots)
            if (s.team == team)
                ++n;
        return n;
    }

    std::uint32_t CountRole(Result const& r, std::uint8_t team, char const* role)
    {
        std::uint32_t n = 0;
        for (Slot const& s : r.slots)
            if (s.team == team && std::string(s.role) == role)
                ++n;
        return n;
    }

    // ---- the constants this kernel restates ------------------------------

    TEST(DcBgQueueFillPlanner, TeamIndexesMatchTheCore)
    {
        EXPECT_EQ(static_cast<std::uint8_t>(TEAM_ALLIANCE), kTeamAlliance);
        EXPECT_EQ(static_cast<std::uint8_t>(TEAM_HORDE), kTeamHorde);
        EXPECT_EQ(static_cast<std::size_t>(PVP_TEAMS_COUNT), kTeams);
    }

    // ---- targets: always the template maximum ----------------------------

    TEST(DcBgQueueFillPlanner, EverySideIsFilledToTheTemplateMax)
    {
        for (Template const& t : kConcrete)
        {
            SCOPED_TRACE(t.name);
            Result const r = Plan(SoloAt80(t));
            ASSERT_EQ(Kind::Ok, r.kind) << r.detail;
            EXPECT_EQ(t.max - 1, CountSide(r, kTeamAlliance));  // the player is the 1
            EXPECT_EQ(t.max, CountSide(r, kTeamHorde));
            EXPECT_EQ(t.max - 1, r.need[kTeamAlliance]);
            EXPECT_EQ(t.max, r.need[kTeamHorde]);
            EXPECT_EQ(0u, r.shortfall[kTeamAlliance]);
            EXPECT_EQ(0u, r.shortfall[kTeamHorde]);
        }
    }

    TEST(DcBgQueueFillPlanner, HumansAndBotsInFlightAreSubtracted)
    {
        Request req = SoloAt80(kAB);
        req.sides[kTeamAlliance].humansQueued = 3;   // the player + two others
        req.sides[kTeamHorde].humansQueued = 2;      // a human on the far side
        req.sides[kTeamHorde].botsInFlight = 4;      // another fill's bots on the way
        Result const r = Plan(req);
        ASSERT_EQ(Kind::Ok, r.kind) << r.detail;
        EXPECT_EQ(12u, CountSide(r, kTeamAlliance));
        EXPECT_EQ(9u, CountSide(r, kTeamHorde));
    }

    TEST(DcBgQueueFillPlanner, ASideAlreadyAtMaxGetsNoBots)
    {
        Request req = SoloAt80(kWS);
        req.sides[kTeamHorde].humansQueued = 12;  // over-full: never negative
        Result const r = Plan(req);
        ASSERT_EQ(Kind::Ok, r.kind) << r.detail;
        EXPECT_EQ(9u, CountSide(r, kTeamAlliance));
        EXPECT_EQ(0u, CountSide(r, kTeamHorde));
    }

    TEST(DcBgQueueFillPlanner, NothingToDoWhenBothSidesAreFull)
    {
        Request req = SoloAt80(kWS);
        req.sides[kTeamAlliance].humansQueued = 10;
        req.sides[kTeamHorde].humansQueued = 10;
        Result const r = Plan(req);
        EXPECT_EQ(Kind::NothingToDo, r.kind);
        EXPECT_TRUE(r.slots.empty());
        EXPECT_FALSE(r.detail.empty());
    }

    // ---- Random Battleground: pop at 10v10, then top up -----------------

    TEST(DcBgQueueFillPlanner, RandomPopsAtTheRandomTemplate)
    {
        Result const r = Plan(SoloAt80(kRandom));
        ASSERT_EQ(Kind::Ok, r.kind) << r.detail;
        EXPECT_EQ(9u, CountSide(r, kTeamAlliance));
        EXPECT_EQ(10u, CountSide(r, kTeamHorde));
        // min == max: every Random bot is start-critical.
        for (Slot const& s : r.slots)
            EXPECT_TRUE(s.startCritical);
    }

    TEST(DcBgQueueFillPlanner, RandomTopUpForEachConcreteMap)
    {
        for (Template const& t : kConcrete)
        {
            SCOPED_TRACE(t.name);
            // The pop put 10 a side into the instance; the Random pass's
            // healers are already there.
            Request req = SoloAt80(t);
            req.sides[kTeamAlliance].humansQueued = 10;
            req.sides[kTeamHorde].humansQueued = 10;
            req.sides[kTeamAlliance].healerBots = 2;
            req.sides[kTeamHorde].healerBots = 2;
            Result const r = Plan(req);

            if (t.max == 10)
            {
                EXPECT_EQ(Kind::NothingToDo, r.kind);
                continue;
            }
            ASSERT_EQ(Kind::Ok, r.kind) << r.detail;
            EXPECT_EQ(t.max - 10, CountSide(r, kTeamAlliance));
            EXPECT_EQ(t.max - 10, CountSide(r, kTeamHorde));
            // Only the seats still below the concrete map's minimum are
            // start-critical: none for Arathi/Eye/Strand, ten a side for
            // Alterac and Isle of Conquest (minimum 20, popped at 10).
            std::uint32_t const critPerSide = t.min > 10 ? t.min - 10 : 0;
            std::uint32_t crit[kTeams] = {0, 0};
            for (Slot const& s : r.slots)
                if (s.startCritical)
                    ++crit[s.team];
            EXPECT_EQ(critPerSide, crit[kTeamAlliance]);
            EXPECT_EQ(critPerSide, crit[kTeamHorde]);
            // The quota counts the healers the pop already fielded.
            std::uint32_t const quota = HealerQuota(t.max, 0);
            EXPECT_EQ(quota - 2, CountRole(r, kTeamAlliance, "heal"));
            EXPECT_EQ(quota - 2, CountRole(r, kTeamHorde, "heal"));
        }
    }

    TEST(DcBgQueueFillPlanner, AvTopUpIsSixtyBots)
    {
        Request req = SoloAt80(kAV);
        req.sides[kTeamAlliance].humansQueued = 10;
        req.sides[kTeamHorde].humansQueued = 10;
        Result const r = Plan(req);
        ASSERT_EQ(Kind::Ok, r.kind) << r.detail;
        EXPECT_EQ(60u, r.slots.size());
    }

    // ---- start-critical ordering ------------------------------------------

    TEST(DcBgQueueFillPlanner, StartCriticalSlotsComeFirstAndAlternate)
    {
        for (Template const& t : kConcrete)
        {
            SCOPED_TRACE(t.name);
            Result const r = Plan(SoloAt80(t));
            ASSERT_EQ(Kind::Ok, r.kind) << r.detail;

            std::uint32_t critical[kTeams] = {0, 0};
            bool seenNonCritical = false;
            for (Slot const& s : r.slots)
            {
                if (s.startCritical)
                {
                    EXPECT_FALSE(seenNonCritical) << "a start-critical slot after a non-critical one";
                    ++critical[s.team];
                }
                else
                    seenNonCritical = true;
            }
            EXPECT_EQ(t.min - 1, critical[kTeamAlliance]);
            EXPECT_EQ(t.min, critical[kTeamHorde]);

            // Alternating: no two consecutive start-critical slots on one side
            // while the other side still has start-critical slots to place.
            // The Horde side is one further from its minimum, so it leads.
            ASSERT_FALSE(r.slots.empty());
            EXPECT_EQ(kTeamHorde, r.slots[0].team);
            for (std::size_t i = 1; i + 1 < 2 * (t.min - 1); ++i)
                EXPECT_NE(r.slots[i].team, r.slots[i - 1].team) << "at slot " << i;
        }
    }

    TEST(DcBgQueueFillPlanner, TheOpponentsLeadOnATie)
    {
        Request req = SoloAt80(kWS);
        req.playerTeam = kTeamHorde;
        req.sides[kTeamAlliance].humansQueued = 0;
        req.sides[kTeamHorde].humansQueued = 0;  // e.g. a group demoted by the join hook, counted elsewhere
        Result const r = Plan(req);
        ASSERT_EQ(Kind::Ok, r.kind) << r.detail;
        EXPECT_EQ(kTeamAlliance, r.slots[0].team);
    }

    // ---- healers ----------------------------------------------------------

    TEST(DcBgQueueFillPlanner, AutoHealerQuotaIsOnePerFiveSeats)
    {
        EXPECT_EQ(2u, HealerQuota(10, 0));
        EXPECT_EQ(3u, HealerQuota(15, 0));
        EXPECT_EQ(8u, HealerQuota(40, 0));
        EXPECT_EQ(4u, HealerQuota(40, 4));

        for (Template const& t : kConcrete)
        {
            SCOPED_TRACE(t.name);
            Result const r = Plan(SoloAt80(t));
            ASSERT_EQ(Kind::Ok, r.kind) << r.detail;
            EXPECT_EQ(HealerQuota(t.max, 0), CountRole(r, kTeamAlliance, "heal"));
            EXPECT_EQ(HealerQuota(t.max, 0), CountRole(r, kTeamHorde, "heal"));
        }
    }

    TEST(DcBgQueueFillPlanner, ConfiguredHealerQuotaWins)
    {
        Request req = SoloAt80(kAV);
        req.healersPerSide = 3;
        Result const r = Plan(req);
        ASSERT_EQ(Kind::Ok, r.kind) << r.detail;
        EXPECT_EQ(3u, CountRole(r, kTeamAlliance, "heal"));
        EXPECT_EQ(3u, CountRole(r, kTeamHorde, "heal"));
    }

    TEST(DcBgQueueFillPlanner, StartCriticalPrefixCarriesHealers)
    {
        // AV pops at 20v20 and the rest stream in: the pop must not be all dps.
        Result const r = Plan(SoloAt80(kAV));
        ASSERT_EQ(Kind::Ok, r.kind) << r.detail;
        for (std::uint8_t t = 0; t < kTeams; ++t)
        {
            std::uint32_t critHeals = 0;
            for (Slot const& s : r.slots)
                if (s.team == t && s.startCritical && std::string(s.role) == "heal")
                    ++critHeals;
            EXPECT_GE(critHeals, 3u) << TeamName(t);
        }
    }

    TEST(DcBgQueueFillPlanner, HealerSlotsDrawHealerSpecs)
    {
        Result const r = Plan(SoloAt80(kAV));
        ASSERT_EQ(Kind::Ok, r.kind) << r.detail;
        auto const heal = DcTestComp::RolePool("heal", DcTestComp::Roster::WithDeathKnights);
        for (Slot const& s : r.slots)
        {
            if (std::string(s.role) != "heal")
                continue;
            bool found = false;
            for (DcTestComp::Slot const& p : heal)
                if (p.classId == s.classId && std::string(p.specName) == s.specName)
                    found = true;
            EXPECT_TRUE(found) << "class " << int(s.classId) << " " << s.specName;
        }
    }

    // ---- class spread -----------------------------------------------------

    TEST(DcBgQueueFillPlanner, ClassesAreSpreadNotStacked)
    {
        // 39 Alliance bots over the dps + heal pools: least-used-first means
        // no class is fielded more than a handful of times over its share.
        Result const r = Plan(SoloAt80(kAV));
        ASSERT_EQ(Kind::Ok, r.kind) << r.detail;
        std::map<std::uint8_t, std::uint32_t> perClass;
        for (Slot const& s : r.slots)
            if (s.team == kTeamAlliance)
                ++perClass[s.classId];
        EXPECT_GE(perClass.size(), 9u);
        for (auto const& [classId, n] : perClass)
            EXPECT_LE(n, 7u) << "class " << int(classId);
    }

    // ---- levels -----------------------------------------------------------

    TEST(DcBgQueueFillPlanner, TopBracketIsFixed)
    {
        Result const r = Plan(SoloAt80(kAV));
        ASSERT_EQ(Kind::Ok, r.kind) << r.detail;
        for (Slot const& s : r.slots)
            EXPECT_EQ(80u, s.level);
    }

    TEST(DcBgQueueFillPlanner, LowerBracketLevelsStayInTheWindow)
    {
        struct Case
        {
            std::uint32_t bmin, bmax, player, lo, hi;
        };
        Case const cases[] = {
            {20, 29, 20, 20, 22},   // bracket floor
            {20, 29, 29, 27, 29},   // bracket ceiling
            {20, 29, 24, 22, 26},   // middle
            {10, 19, 19, 17, 19},   // the plan's "level 19 gets 17-19"
            {51, 60, 55, 53, 57},
        };
        for (Case const& c : cases)
        {
            SCOPED_TRACE(std::to_string(c.bmin) + "-" + std::to_string(c.bmax) + " @" +
                         std::to_string(c.player));
            Request req = SoloAt80(kAB);
            req.bracketMinLevel = c.bmin;
            req.bracketMaxLevel = c.bmax;
            req.playerLevel = c.player;
            auto const [lo, hi] = LevelWindow(req);
            EXPECT_EQ(c.lo, lo);
            EXPECT_EQ(c.hi, hi);

            for (std::uint32_t seed = 1; seed <= 20; ++seed)
            {
                req.seed = seed;
                Result const r = Plan(req);
                ASSERT_EQ(Kind::Ok, r.kind) << r.detail;
                for (Slot const& s : r.slots)
                {
                    EXPECT_GE(s.level, c.lo);
                    EXPECT_LE(s.level, c.hi);
                }
            }
        }
    }

    TEST(DcBgQueueFillPlanner, ZeroSpreadMirrorsThePlayer)
    {
        Request req = SoloAt80(kWS);
        req.bracketMinLevel = 30;
        req.bracketMaxLevel = 39;
        req.playerLevel = 34;
        req.levelSpread = 0;
        Result const r = Plan(req);
        ASSERT_EQ(Kind::Ok, r.kind) << r.detail;
        for (Slot const& s : r.slots)
            EXPECT_EQ(34u, s.level);
    }

    // ---- death knights ----------------------------------------------------

    TEST(DcBgQueueFillPlanner, NoDeathKnightsBelowTheirStartingLevel)
    {
        for (std::uint32_t seed = 1; seed <= 30; ++seed)
        {
            Request req = SoloAt80(kAV, seed);
            req.bracketMinLevel = 51;
            req.bracketMaxLevel = 60;
            req.playerLevel = 51;  // window 51-53
            Result const r = Plan(req);
            ASSERT_EQ(Kind::Ok, r.kind) << r.detail;
            for (Slot const& s : r.slots)
                EXPECT_NE(CLASS_DEATH_KNIGHT, s.classId) << "level " << s.level;
        }
    }

    TEST(DcBgQueueFillPlanner, DeathKnightsAppearAtEighty)
    {
        Result const r = Plan(SoloAt80(kAV));
        ASSERT_EQ(Kind::Ok, r.kind) << r.detail;
        bool sawDk = false;
        for (Slot const& s : r.slots)
            sawDk = sawDk || s.classId == CLASS_DEATH_KNIGHT;
        EXPECT_TRUE(sawDk);
    }

    TEST(DcBgQueueFillPlanner, DeathKnightGateIsPerSlotLevel)
    {
        // A 51-60 bracket around a level-55 player straddles the gate: a death
        // knight may only ever land on a slot levelled 55 or higher.
        for (std::uint32_t seed = 1; seed <= 30; ++seed)
        {
            Request req = SoloAt80(kAV, seed);
            req.bracketMinLevel = 51;
            req.bracketMaxLevel = 60;
            req.playerLevel = 55;
            Result const r = Plan(req);
            ASSERT_EQ(Kind::Ok, r.kind) << r.detail;
            for (Slot const& s : r.slots)
                if (s.classId == CLASS_DEATH_KNIGHT)
                    EXPECT_GE(s.level, 55u);
        }
    }

    // ---- determinism and the avoid list ----------------------------------

    TEST(DcBgQueueFillPlanner, SameSeedSamePlan)
    {
        Result const a = Plan(SoloAt80(kAV, 42));
        Result const b = Plan(SoloAt80(kAV, 42));
        ASSERT_EQ(a.slots.size(), b.slots.size());
        for (std::size_t i = 0; i < a.slots.size(); ++i)
        {
            EXPECT_EQ(a.slots[i].team, b.slots[i].team);
            EXPECT_EQ(a.slots[i].classId, b.slots[i].classId);
            EXPECT_EQ(std::string(a.slots[i].specName), b.slots[i].specName);
            EXPECT_EQ(a.slots[i].level, b.slots[i].level);
            EXPECT_EQ(a.slots[i].startCritical, b.slots[i].startCritical);
        }
    }

    TEST(DcBgQueueFillPlanner, DifferentSeedsDifferentPlans)
    {
        Result const a = Plan(SoloAt80(kWS, 1));
        Result const b = Plan(SoloAt80(kWS, 2));
        ASSERT_EQ(a.slots.size(), b.slots.size());
        bool differ = false;
        for (std::size_t i = 0; i < a.slots.size(); ++i)
            differ = differ || a.slots[i].classId != b.slots[i].classId ||
                     std::string(a.slots[i].specName) != b.slots[i].specName;
        EXPECT_TRUE(differ);
    }

    TEST(DcBgQueueFillPlanner, AvoidedClassesArePassedOverWhileThereIsAnAlternative)
    {
        // One dps seat to fill, and every dps class but mage on the avoid list:
        // the draw must land on mage.
        Request req = SoloAt80(kWS);
        req.sides[kTeamAlliance].humansQueued = 9;
        req.sides[kTeamAlliance].healerBots = 2;  // the healer quota is already met
        req.sides[kTeamHorde].humansQueued = 10;
        req.avoidClasses = {CLASS_WARRIOR, CLASS_PALADIN, CLASS_HUNTER, CLASS_ROGUE, CLASS_PRIEST,
                            CLASS_DEATH_KNIGHT, CLASS_SHAMAN, CLASS_WARLOCK, CLASS_DRUID};
        for (std::uint32_t seed = 1; seed <= 20; ++seed)
        {
            req.seed = seed;
            Result const r = Plan(req);
            ASSERT_EQ(Kind::Ok, r.kind) << r.detail;
            ASSERT_EQ(1u, r.slots.size());
            EXPECT_EQ(CLASS_MAGE, r.slots[0].classId);
        }
    }

    TEST(DcBgQueueFillPlanner, AnAvoidListCoveringEverythingStillPlans)
    {
        Request req = SoloAt80(kWS);
        req.avoidClasses = {CLASS_WARRIOR, CLASS_PALADIN, CLASS_HUNTER, CLASS_ROGUE, CLASS_PRIEST,
                            CLASS_DEATH_KNIGHT, CLASS_SHAMAN, CLASS_MAGE, CLASS_WARLOCK, CLASS_DRUID};
        Result const r = Plan(req);
        ASSERT_EQ(Kind::Ok, r.kind) << r.detail;
        EXPECT_EQ(19u, r.slots.size());
    }

    // ---- the pool ---------------------------------------------------------

    TEST(DcBgQueueFillPlanner, UnresolvableWhenASideCannotReachTheMinimum)
    {
        Request req = SoloAt80(kAV);
        req.sides[kTeamHorde].poolFree = 19;  // min is 20
        Result const r = Plan(req);
        EXPECT_EQ(Kind::Unresolvable, r.kind);
        EXPECT_TRUE(r.slots.empty());
        EXPECT_NE(std::string::npos, r.detail.find("Horde"));
    }

    TEST(DcBgQueueFillPlanner, AShortPoolPastTheMinimumStartsShort)
    {
        Request req = SoloAt80(kAV);
        req.sides[kTeamHorde].poolFree = 25;
        Result const r = Plan(req);
        ASSERT_EQ(Kind::Ok, r.kind) << r.detail;
        EXPECT_EQ(25u, CountSide(r, kTeamHorde));
        EXPECT_EQ(40u, r.need[kTeamHorde]);
        EXPECT_EQ(25u, r.planned[kTeamHorde]);
        EXPECT_EQ(15u, r.shortfall[kTeamHorde]);
        EXPECT_EQ(0u, r.shortfall[kTeamAlliance]);
    }

    TEST(DcBgQueueFillPlanner, ThePlayersOwnSideCountsTowardTheMinimum)
    {
        // Four humans (a demoted group) on the Alliance side of a WS: the
        // Alliance pool only has to find one more to reach the minimum of 5.
        Request req = SoloAt80(kWS);
        req.sides[kTeamAlliance].humansQueued = 4;
        req.sides[kTeamAlliance].poolFree = 1;
        Result const r = Plan(req);
        ASSERT_EQ(Kind::Ok, r.kind) << r.detail;
        EXPECT_EQ(1u, CountSide(r, kTeamAlliance));
        EXPECT_EQ(5u, r.shortfall[kTeamAlliance]);
    }

    // ---- bad inputs -------------------------------------------------------

    TEST(DcBgQueueFillPlanner, BadTemplateOrBracketIsUnresolvable)
    {
        Request req = SoloAt80(kWS);
        req.minPerTeam = 11;  // min > max
        EXPECT_EQ(Kind::Unresolvable, Plan(req).kind);

        req = SoloAt80(kWS);
        req.maxPerTeam = 0;
        req.minPerTeam = 0;
        EXPECT_EQ(Kind::Unresolvable, Plan(req).kind);

        req = SoloAt80(kWS);
        req.bracketMinLevel = 30;
        req.bracketMaxLevel = 29;
        EXPECT_EQ(Kind::Unresolvable, Plan(req).kind);

        req = SoloAt80(kWS);
        req.playerTeam = 2;
        EXPECT_EQ(Kind::Unresolvable, Plan(req).kind);
    }
}
