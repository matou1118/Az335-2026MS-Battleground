/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "gtest/gtest.h"

#include <string>
#include <vector>

#include "Ai/Dungeon/DungeonClear/Data/DcNavPenaltyRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonWingRegistry.h"
#include "Ai/Dungeon/DungeonClear/Overrides/BossRosterRegistry.h"
#include "Ai/Dungeon/DungeonClear/Util/DcRunWing.h"
#include "TestRun/DcTestDungeonRegistry.h"

// The run wing (Blackrock Spire LBRS / UBRS, Blackrock Depths Detention Block /
// Upper City): the wing layout data, the resolver's order and latch rules, the
// fallback at the shared portal, the wing filter and the terminal-boss rule. All
// pure — no world, no bots.

namespace
{
    constexpr uint32 BRS = 229;

    DungeonBossInfo Boss(uint32 entry, uint32 bit, float x, float y, float z, uint32 mapId = BRS)
    {
        DungeonBossInfo b;
        b.entry = entry;
        b.encounterIndex = bit;
        b.name = std::to_string(entry);
        b.mapId = mapId;
        b.x = x;
        b.y = y;
        b.z = z;
        return b;
    }

    // What BossSpawnIndex yields for 229: the eleven encounters with a static
    // spawn (Urok, Gizrul and Solakar are summoned), in DBC order.
    std::vector<DungeonBossInfo> BrsRoster()
    {
        return {
            Boss(9196,  0,  -22.8f, -300.7f,  31.8f),   // Omokk
            Boss(9236,  1, -121.2f, -482.2f,  24.7f),   // Vosh'gajin
            Boss(9237,  2,  -17.0f, -459.1f, -18.6f),   // Voone
            Boss(10596, 3, -135.5f, -565.8f,  10.2f),   // Smolderweb
            Boss(9736,  5, -190.5f, -475.6f,  87.4f),   // Zigris
            Boss(10220, 7, -193.9f, -338.1f,  64.5f),   // Halycon
            Boss(9568,  8,  -22.6f, -486.2f,  90.8f),   // Wyrmthalak
            Boss(9816,  9,  144.4f, -258.0f,  96.4f),   // Emberseer
            Boss(10429, 11, 159.3f, -443.6f, 122.1f),   // Rend
            Boss(10430, 12, 124.2f, -563.8f, 107.4f),   // The Beast
            Boss(10363, 13,  36.5f, -286.0f, 111.0f),   // Drakkisath
        };
    }

    DungeonWingLayout const& Brs()
    {
        DungeonWingLayout const* layout = DungeonWingRegistry::Get(BRS);
        EXPECT_NE(layout, nullptr);
        return *layout;
    }
}

// ---- layout data -------------------------------------------------------------

TEST(DcRunWingTest, BlackrockSpireIsAnExplicitTwoWingLayout)
{
    DungeonWingLayout const& layout = Brs();
    EXPECT_TRUE(layout.isolated);
    EXPECT_EQ(layout.select, WingSelect::Explicit);
    EXPECT_EQ(layout.defaultWing, "lbrs");
    ASSERT_EQ(layout.wings.size(), 2u);

    DungeonWing const* lbrs = DungeonWingRegistry::FindWing(layout, "lbrs");
    DungeonWing const* ubrs = DungeonWingRegistry::FindWing(layout, "ubrs");
    ASSERT_NE(lbrs, nullptr);
    ASSERT_NE(ubrs, nullptr);
    EXPECT_EQ(lbrs->lfgDungeonId, 32u);
    EXPECT_EQ(ubrs->lfgDungeonId, 44u);
    EXPECT_EQ(lbrs->terminalBossEntry, 9568u);    // Overlord Wyrmthalak
    EXPECT_EQ(ubrs->terminalBossEntry, 10363u);   // General Drakkisath
    EXPECT_EQ(lbrs->bossEntries.size(), 9u);      // incl. Urok, Gizrul
    EXPECT_EQ(ubrs->bossEntries.size(), 5u);      // incl. Solakar

    // The DBC bits split cleanly: 0-8 LBRS, 9-13 UBRS.
    EXPECT_EQ(lbrs->encounterMask & ubrs->encounterMask, 0u);
    EXPECT_EQ(lbrs->encounterMask | ubrs->encounterMask, 0x3FFFu);
    EXPECT_TRUE(lbrs->encounterMask & (1u << 8));
    EXPECT_TRUE(ubrs->encounterMask & (1u << 9));
}

TEST(DcRunWingTest, EveryWingEntryIsInExactlyOneWing)
{
    DungeonWingLayout const& layout = Brs();
    for (DungeonWing const& wing : layout.wings)
        for (uint32 entry : wing.bossEntries)
        {
            DungeonWing const* owner = DungeonWingRegistry::WingOf(BRS, entry);
            ASSERT_NE(owner, nullptr) << entry;
            EXPECT_EQ(owner->token, wing.token) << entry;
        }
}

TEST(DcRunWingTest, FindWingMatchesTokenOrNameIgnoringCase)
{
    DungeonWingLayout const& layout = Brs();
    ASSERT_NE(DungeonWingRegistry::FindWing(layout, "UBRS"), nullptr);
    EXPECT_EQ(DungeonWingRegistry::FindWing(layout, "UBRS")->token, "ubrs");
    ASSERT_NE(DungeonWingRegistry::FindWing(layout, "blackrock spire (lower)"), nullptr);
    EXPECT_EQ(DungeonWingRegistry::FindWing(layout, "blackrock spire (lower)")->token, "lbrs");
    EXPECT_EQ(DungeonWingRegistry::FindWing(layout, "brs"), nullptr);
    EXPECT_EQ(DungeonWingRegistry::FindWing(layout, ""), nullptr);
}

TEST(DcRunWingTest, LfgDungeonPicksTheWing)
{
    ASSERT_NE(DungeonWingRegistry::WingForLfgDungeon(BRS, 32), nullptr);
    EXPECT_EQ(DungeonWingRegistry::WingForLfgDungeon(BRS, 32)->token, "lbrs");
    ASSERT_NE(DungeonWingRegistry::WingForLfgDungeon(BRS, 44), nullptr);
    EXPECT_EQ(DungeonWingRegistry::WingForLfgDungeon(BRS, 44)->token, "ubrs");
    EXPECT_EQ(DungeonWingRegistry::WingForLfgDungeon(BRS, 0), nullptr);
    EXPECT_EQ(DungeonWingRegistry::WingForLfgDungeon(BRS, 99), nullptr);
    EXPECT_EQ(DungeonWingRegistry::WingForLfgDungeon(36, 32), nullptr);   // not a split map
}

TEST(DcRunWingTest, DireMaulAndScarletMonasteryStayProximity)
{
    for (uint32 map : { 429u, 189u })
    {
        DungeonWingLayout const* layout = DungeonWingRegistry::Get(map);
        ASSERT_NE(layout, nullptr) << map;
        EXPECT_TRUE(layout->isolated) << map;
        EXPECT_EQ(layout->select, WingSelect::Proximity) << map;
    }
    // Maraudon stays a label-only layout.
    ASSERT_NE(DungeonWingRegistry::Get(349), nullptr);
    EXPECT_FALSE(DungeonWingRegistry::Get(349)->isolated);
}

TEST(DcRunWingTest, EveryIsolatedWingTokenHasACatalogueRowOnItsMap)
{
    // Rez recovery regroups a wiped party at the row whose token is the run's
    // wing token, and the test harness picks the run wing by the row's token —
    // so the two vocabularies must be one.
    for (uint32 map : { 229u, 230u, 429u, 189u })
    {
        DungeonWingLayout const* layout = DungeonWingRegistry::Get(map);
        ASSERT_NE(layout, nullptr);
        for (DungeonWing const& wing : layout->wings)
        {
            ASSERT_FALSE(wing.token.empty()) << wing.name;
            DcTestDungeonRegistry::Row const* row = DcTestDungeonRegistry::Find(wing.token);
            ASSERT_NE(row, nullptr) << wing.token;
            EXPECT_EQ(row->mapId, map) << wing.token;
        }
    }
}

// ---- resolver order + latch ------------------------------------------------------

TEST(DcRunWingTest, HigherSourcesWinAndLowerOnesNeverFlipALatch)
{
    using DcRunWing::Latch;
    using DcRunWing::ShouldReplace;
    using DcRunWing::Source;

    Latch empty;
    EXPECT_FALSE(DcRunWing::Holds(empty, 7));
    EXPECT_TRUE(ShouldReplace(empty, 7, Source::Fallback));
    EXPECT_FALSE(ShouldReplace(empty, 7, Source::None));

    Latch const fallback{7, "lbrs", Source::Fallback};
    EXPECT_TRUE(DcRunWing::Holds(fallback, 7));
    EXPECT_TRUE(ShouldReplace(fallback, 7, Source::Fallback));   // same source may refresh
    EXPECT_TRUE(ShouldReplace(fallback, 7, Source::Lfg));
    EXPECT_TRUE(ShouldReplace(fallback, 7, Source::Explicit));

    Latch const lfg{7, "ubrs", Source::Lfg};
    EXPECT_FALSE(ShouldReplace(lfg, 7, Source::Fallback));        // walking can't flip it
    EXPECT_TRUE(ShouldReplace(lfg, 7, Source::Explicit));

    Latch const chosen{7, "ubrs", Source::Explicit};
    EXPECT_FALSE(ShouldReplace(chosen, 7, Source::Fallback));
    EXPECT_FALSE(ShouldReplace(chosen, 7, Source::Lfg));
    EXPECT_TRUE(ShouldReplace(chosen, 7, Source::Explicit));      // `.dc wing` changes it

    // A new instance starts clean: the old latch no longer holds.
    EXPECT_FALSE(DcRunWing::Holds(chosen, 8));
    EXPECT_TRUE(ShouldReplace(chosen, 8, Source::Fallback));
}

TEST(DcRunWingTest, FallbackAtThePortalIsLbrs)
{
    // The portal lies in no wing region, so a bare `.dc on` there clears LBRS —
    // though the nearest bosses to it are UBRS's (the whole reason for Explicit).
    char const* region = DcNavPenaltyRegistry::WingRegionAt(BRS, 78.51f, -225.04f, 49.84f);
    EXPECT_EQ(region, nullptr);
    EXPECT_EQ(DcRunWing::ResolveFallback(Brs(), region), "lbrs");

    std::vector<DungeonBossInfo> const roster = BrsRoster();
    std::size_t const nearest = DcRunWing::PickByProximity(Brs(), roster, 78.51f, -225.04f, 49.84f);
    ASSERT_LT(nearest, Brs().wings.size());
    EXPECT_EQ(Brs().wings[nearest].token, "ubrs") << "proximity would pick UBRS at the portal";
}

TEST(DcRunWingTest, FallbackInsideUbrsIsUbrs)
{
    char const* region = DcNavPenaltyRegistry::WingRegionAt(BRS, 144.4f, -258.0f, 96.4f);
    ASSERT_NE(region, nullptr);
    EXPECT_EQ(DcRunWing::ResolveFallback(Brs(), region), "ubrs");
    // At the Dragonspine Door (shared hall) it is still the default.
    EXPECT_EQ(DcRunWing::ResolveFallback(
                  Brs(), DcNavPenaltyRegistry::WingRegionAt(BRS, 109.1f, -320.4f, 65.5f)),
              "lbrs");
    // A region tag that names no wing falls back to the default too.
    EXPECT_EQ(DcRunWing::ResolveFallback(Brs(), "bogus"), "lbrs");
}

// ---- the filter ------------------------------------------------------------------

TEST(DcRunWingTest, WingFilterKeepsOnlyTheWingsBosses)
{
    std::vector<DungeonBossInfo> const roster = BrsRoster();
    std::vector<DungeonBossInfo> const lower =
        DcRunWing::FilterToWing(roster, *DungeonWingRegistry::FindWing(Brs(), "lbrs"));
    std::vector<DungeonBossInfo> const upper =
        DcRunWing::FilterToWing(roster, *DungeonWingRegistry::FindWing(Brs(), "ubrs"));
    ASSERT_EQ(lower.size(), 7u);
    ASSERT_EQ(upper.size(), 4u);
    EXPECT_EQ(lower.front().entry, 9196u);   // Omokk, order kept
    EXPECT_EQ(lower.back().entry, 9568u);    // Wyrmthalak last
    EXPECT_EQ(upper.front().entry, 9816u);   // Emberseer next for a UBRS run
    EXPECT_EQ(upper.back().entry, 10363u);

    // An Explicit wing is never widened back to the whole map.
    std::vector<DungeonBossInfo> const onlyUpper(roster.begin() + 7, roster.end());
    EXPECT_TRUE(DcRunWing::FilterToWing(onlyUpper, *DungeonWingRegistry::FindWing(Brs(), "lbrs")).empty());
}

TEST(DcRunWingTest, DireMaulProximityPickIsUnchanged)
{
    DungeonWingLayout const* dm = DungeonWingRegistry::Get(429);
    ASSERT_NE(dm, nullptr);
    std::vector<DungeonBossInfo> const roster = {
        Boss(11490, 0,   60.0f, -200.0f, -2.0f, 429),   // Zevrim (East)
        Boss(11489, 5,  -60.0f,  500.0f, -3.0f, 429),   // Tendris (West)
        Boss(14326, 9,  400.0f,   20.0f, -2.0f, 429),   // Mol'dar (North)
    };
    std::size_t const east = DcRunWing::PickByProximity(*dm, roster, 44.45f, -154.82f, -2.71f);
    ASSERT_LT(east, dm->wings.size());
    EXPECT_EQ(dm->wings[east].token, "dm-east");
    std::size_t const north = DcRunWing::PickByProximity(*dm, roster, 380.0f, 0.0f, -2.0f);
    ASSERT_LT(north, dm->wings.size());
    EXPECT_EQ(dm->wings[north].token, "dm-north");
    // No registered boss in the list -> no pick.
    EXPECT_EQ(DcRunWing::PickByProximity(*dm, { Boss(1, 0, 0, 0, 0, 429) }, 0, 0, 0), dm->wings.size());
}

// ---- terminal boss ---------------------------------------------------------------

TEST(DcRunWingTest, WyrmthalakDownCompletesLbrs)
{
    DungeonWing const& lbrs = *DungeonWingRegistry::FindWing(Brs(), "lbrs");
    std::vector<DungeonBossInfo> const lower = DcRunWing::FilterToWing(BrsRoster(), lbrs);

    EXPECT_FALSE(DcRunWing::TerminalDone(lbrs, lower, 0u, false));
    // Everything but Wyrmthalak down: not complete.
    EXPECT_FALSE(DcRunWing::TerminalDone(lbrs, lower, 0x0FFu, false));
    // His bit alone completes the wing, even with Omokk skipped/unreached.
    EXPECT_TRUE(DcRunWing::TerminalDone(lbrs, lower, 1u << 8, false));
    // A fresh corpse whose bit hasn't flipped yet counts too.
    EXPECT_TRUE(DcRunWing::TerminalDone(lbrs, lower, 0u, true));
    // UBRS bits never complete LBRS.
    EXPECT_FALSE(DcRunWing::TerminalDone(lbrs, lower, 0x3E00u, false));
}

TEST(DcRunWingTest, TerminalRuleNeedsATerminalInTheList)
{
    DungeonWing const& ubrs = *DungeonWingRegistry::FindWing(Brs(), "ubrs");
    std::vector<DungeonBossInfo> const lower =
        DcRunWing::FilterToWing(BrsRoster(), *DungeonWingRegistry::FindWing(Brs(), "lbrs"));
    // Drakkisath is not in an LBRS list -> never "done" from it.
    EXPECT_FALSE(DcRunWing::TerminalDone(ubrs, lower, 1u << 13, true));

    DungeonWing none;
    none.bossEntries = { 9568 };
    EXPECT_FALSE(DcRunWing::TerminalDone(none, lower, 1u << 8, true));
}

// ---- Blackrock Depths: Detention Block / Upper City (map 230) --------------------

namespace
{
    constexpr uint32 BRD = 230;

    // What BossSpawnIndex yields for 230: the eighteen encounters with a static
    // spawn (the Ring of Law's Grimstone is summoned), in DBC order.
    std::vector<DungeonBossInfo> BrdSpawnIndex()
    {
        return {
            Boss(9018,  0,  310.6f, -146.3f, -70.3f, BRD),   // Gerstahn
            Boss(9025,  1,  615.5f, -267.4f, -83.6f, BRD),   // Roccor
            Boss(9319,  2,  607.7f, -174.8f, -84.5f, BRD),   // Grebmar
            Boss(9024,  4,  530.2f, -243.9f, -43.0f, BRD),   // Loregrain
            Boss(9017,  5,  893.5f, -267.1f, -71.9f, BRD),   // Incendius
            Boss(9041,  6,  823.4f, -342.3f, -50.1f, BRD),   // Stilgiss
            Boss(9056,  7,  963.3f, -343.7f, -71.7f, BRD),   // Darkvire
            Boss(9016,  8,  702.4f,  184.5f, -72.0f, BRD),   // Bael'Gar
            Boss(9033,  9,  652.4f,   21.4f, -60.0f, BRD),   // Angerforge
            Boss(8983, 10,  846.8f,   16.3f, -53.6f, BRD),   // Argelmach
            Boss(9537, 11,  878.1f, -153.1f, -49.8f, BRD),   // Hurley
            Boss(9502, 12,  869.0f, -225.0f, -43.7f, BRD),   // Phalanx
            Boss(9543, 13,  878.5f, -167.7f, -49.7f, BRD),   // Ribbly
            Boss(9499, 14,  888.5f, -177.9f, -43.0f, BRD),   // Plugger
            Boss(9156, 15, 1009.8f, -239.0f, -61.3f, BRD),   // Flamelash
            Boss(9035, 16, 1215.1f, -220.7f, -85.6f, BRD),   // Anger'rel (The Seven)
            Boss(9938, 17, 1380.7f, -659.3f, -92.0f, BRD),   // Magmus
            Boss(9019, 18, 1380.2f, -831.6f, -87.6f, BRD),   // Thaurissan
        };
    }

    // The live list: the spawn index through the map-230 roster patch, which adds
    // the Ring of Law (event 1) and Shadowforge Lock (event 2) objectives.
    std::vector<DungeonBossInfo> BrdRoster()
    {
        return BossRosterRegistry::Apply(BRD, DcDiffKey::Dungeon(DUNGEON_DIFFICULTY_NORMAL), BrdSpawnIndex());
    }

    DungeonWingLayout const& Brd()
    {
        DungeonWingLayout const* layout = DungeonWingRegistry::Get(BRD);
        EXPECT_NE(layout, nullptr);
        return *layout;
    }

    constexpr float BRD_PORTAL_X = 456.93f;
    constexpr float BRD_PORTAL_Y = 34.09f;
    constexpr float BRD_PORTAL_Z = -68.09f;
}

TEST(DcRunWingTest, BlackrockDepthsIsAnExplicitTwoWingLayout)
{
    DungeonWingLayout const& layout = Brd();
    EXPECT_TRUE(layout.isolated);
    EXPECT_EQ(layout.select, WingSelect::Explicit);
    EXPECT_EQ(layout.defaultWing, "brd-db");
    ASSERT_EQ(layout.wings.size(), 2u);

    DungeonWing const* db = DungeonWingRegistry::FindWing(layout, "brd-db");
    DungeonWing const* uc = DungeonWingRegistry::FindWing(layout, "brd-uc");
    ASSERT_NE(db, nullptr);
    ASSERT_NE(uc, nullptr);
    EXPECT_EQ(db->terminalBossEntry, 9016u);   // Bael'Gar
    EXPECT_EQ(uc->terminalBossEntry, 9019u);   // Emperor Dagran Thaurissan

    // The DBC bits split cleanly at the Shadowforge Lock: 0-8 / 9-18.
    EXPECT_EQ(db->encounterMask & uc->encounterMask, 0u);
    EXPECT_EQ(db->encounterMask | uc->encounterMask, 0x7FFFFu);
    EXPECT_TRUE(db->encounterMask & (1u << 8));    // Bael'Gar
    EXPECT_TRUE(uc->encounterMask & (1u << 9));    // Angerforge
    EXPECT_TRUE(uc->encounterMask & (1u << 18));   // Thaurissan

    // Every wing entry has one owner.
    for (DungeonWing const& wing : layout.wings)
        for (uint32 entry : wing.bossEntries)
        {
            DungeonWing const* owner = DungeonWingRegistry::WingOf(BRD, entry);
            ASSERT_NE(owner, nullptr) << entry;
            EXPECT_EQ(owner->token, wing.token) << entry;
        }

    // The panel tags a row with the part in parentheses.
    EXPECT_EQ(DungeonWingRegistry::WingName(BRD, 9033), "Blackrock Depths (Upper City)");
    EXPECT_EQ(DungeonWingRegistry::WingName(BRD, 9018), "Blackrock Depths (Detention Block)");
    ASSERT_NE(DungeonWingRegistry::FindWing(layout, "blackrock depths (upper city)"), nullptr);
}

TEST(DcRunWingTest, BlackrockDepthsLfgDungeonPicksTheWing)
{
    ASSERT_NE(DungeonWingRegistry::WingForLfgDungeon(BRD, 30), nullptr);    // "Prison"
    EXPECT_EQ(DungeonWingRegistry::WingForLfgDungeon(BRD, 30)->token, "brd-db");
    ASSERT_NE(DungeonWingRegistry::WingForLfgDungeon(BRD, 276), nullptr);   // "Upper City"
    EXPECT_EQ(DungeonWingRegistry::WingForLfgDungeon(BRD, 276)->token, "brd-uc");
    EXPECT_EQ(DungeonWingRegistry::WingForLfgDungeon(BRD, 32), nullptr);    // LBRS's id
}

TEST(DcRunWingTest, EveryBlackrockDepthsRosterRowIsInExactlyOneWing)
{
    // The whole live list — bosses AND both objectives — splits 9 / 11 with
    // nothing dropped: a row in no wing would never be cleared by either run.
    std::vector<DungeonBossInfo> const roster = BrdRoster();
    ASSERT_EQ(roster.size(), 20u);
    std::size_t const db = DcRunWing::FilterToWing(roster, *DungeonWingRegistry::FindWing(Brd(), "brd-db")).size();
    std::size_t const uc = DcRunWing::FilterToWing(roster, *DungeonWingRegistry::FindWing(Brd(), "brd-uc")).size();
    EXPECT_EQ(db, 9u);
    EXPECT_EQ(uc, 11u);
    EXPECT_EQ(db + uc, roster.size());
    for (DungeonBossInfo const& b : roster)
        EXPECT_NE(DungeonWingRegistry::WingOf(BRD, b.entry), nullptr) << b.name;
}

TEST(DcRunWingTest, DetentionBlockRunsGerstahnToBaelGarThroughTheRingOfLaw)
{
    std::vector<DungeonBossInfo> const db =
        DcRunWing::FilterToWing(BrdRoster(), *DungeonWingRegistry::FindWing(Brd(), "brd-db"));
    ASSERT_EQ(db.size(), 9u);
    // Gerstahn, Roccor, Grebmar, Ring of Law, Loregrain, Stilgiss, Darkvire,
    // Incendius, Bael'Gar — Incendius after Stilgiss and Darkvire, not in DBC order.
    std::vector<uint32> const want = { 9018, 9025, 9319, BossRosterRegistry::ObjectiveEntry(1),
                                       9024, 9041, 9056, 9017, 9016 };
    for (std::size_t i = 0; i < want.size(); ++i)
        EXPECT_EQ(db[i].entry, want[i]) << "slot " << i << " is " << db[i].name;
    // The reorder moves the path, never the kill bits.
    EXPECT_EQ(db[5].encounterIndex, 6u);   // Stilgiss
    EXPECT_EQ(db[7].encounterIndex, 5u);   // Incendius
    bool ring = false;
    for (DungeonBossInfo const& b : db)
    {
        EXPECT_NE(b.eventId, 2u) << "the Shadowforge Lock belongs to the Upper City";
        ring = ring || (b.kind == DungeonAnchorKind::Objective && b.eventId == 1u);
    }
    EXPECT_TRUE(ring) << "the Ring of Law stays in the Detention Block";
}

TEST(DcRunWingTest, UpperCityRunGoesToTheShadowforgeLockThenAngerforge)
{
    std::vector<DungeonBossInfo> const uc =
        DcRunWing::FilterToWing(BrdRoster(), *DungeonWingRegistry::FindWing(Brd(), "brd-uc"));
    ASSERT_EQ(uc.size(), 11u);
    EXPECT_EQ(uc[0].kind, DungeonAnchorKind::Objective);
    EXPECT_EQ(uc[0].eventId, 2u) << "an Upper City run opens at the Shadowforge Lock";
    EXPECT_EQ(uc[1].entry, 9033u) << "then General Angerforge";
    EXPECT_EQ(uc.back().entry, 9019u);   // Thaurissan last
}

TEST(DcRunWingTest, FallbackAtTheBrdPortalIsTheDetentionBlock)
{
    // BRD has no wing regions, so a bare `.dc on` anywhere clears the Detention
    // Block — though the nearest bosses to the portal are the Upper City's.
    char const* region = DcNavPenaltyRegistry::WingRegionAt(BRD, BRD_PORTAL_X, BRD_PORTAL_Y, BRD_PORTAL_Z);
    EXPECT_EQ(region, nullptr);
    EXPECT_EQ(DcRunWing::ResolveFallback(Brd(), region), "brd-db");

    std::size_t const nearest =
        DcRunWing::PickByProximity(Brd(), BrdRoster(), BRD_PORTAL_X, BRD_PORTAL_Y, BRD_PORTAL_Z);
    ASSERT_LT(nearest, Brd().wings.size());
    EXPECT_EQ(Brd().wings[nearest].token, "brd-uc") << "proximity would pick the Upper City at the portal";
}

TEST(DcRunWingTest, BaelGarCompletesTheDetentionBlockAndThaurissanTheUpperCity)
{
    std::vector<DungeonBossInfo> const roster = BrdRoster();
    DungeonWing const& db = *DungeonWingRegistry::FindWing(Brd(), "brd-db");
    DungeonWing const& uc = *DungeonWingRegistry::FindWing(Brd(), "brd-uc");
    std::vector<DungeonBossInfo> const dbList = DcRunWing::FilterToWing(roster, db);
    std::vector<DungeonBossInfo> const ucList = DcRunWing::FilterToWing(roster, uc);

    EXPECT_FALSE(DcRunWing::TerminalDone(db, dbList, 0x0FFu, false));
    EXPECT_TRUE(DcRunWing::TerminalDone(db, dbList, 1u << 8, false));
    // Upper City kills never complete the Detention Block.
    EXPECT_FALSE(DcRunWing::TerminalDone(db, dbList, 0x7FE00u, false));

    EXPECT_FALSE(DcRunWing::TerminalDone(uc, ucList, 0x3FFFFu, false));
    EXPECT_TRUE(DcRunWing::TerminalDone(uc, ucList, 1u << 18, false));
    // Bael'Gar is not in an Upper City list.
    EXPECT_FALSE(DcRunWing::TerminalDone(db, ucList, 1u << 8, true));
}
