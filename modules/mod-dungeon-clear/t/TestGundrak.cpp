/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "gtest/gtest.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "Ai/Dungeon/DungeonClear/Data/DcHazardRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonBossInfo.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonEventRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"
#include "Ai/Dungeon/DungeonClear/Data/SealedEncounterRegistry.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonClearTuning.h"
#include "Ai/Dungeon/DungeonClear/Overrides/BossRosterRegistry.h"

// Gundrak (map 604) — the authored-data lints for a dungeon that had FOUR
// stacked run-ending defects, each only observable once the one before it was
// fixed. Every test here is named after the defect it stops from coming back;
// the reasoning lives in Data/Events/GundrakEvents.cpp.
//
// Suite name deliberately begins DungeonEvent so it is picked up by the
// `DungeonEvent*` filter that t/run_tests.sh and .github/workflows/tests.yml both
// use; a suite named Gundrak* would build, pass locally, and never run in CI.
//
// Every number checked here is either read out of the core
// (src/server/scripts/Northrend/Gundrak/*), the world DB, DungeonEncounter.dbc /
// Spell.dbc, or column-probed against the live 604 mmtiles.

namespace
{
    using namespace DcGundrak;

    // DungeonEventExecutor.cpp's DC_EVENT_GO_USE_RANGE, restated: it lives in that
    // file's anonymous namespace, and it is the single number the whole two-anchor
    // altar geometry is solved against. If it ever moves, this copy going stale is
    // what makes AltarClickPointsAreInsideTheUseRange fail loudly rather than the
    // altars quietly starting to walk bots into holes again.
    constexpr float GD_GO_USE_RANGE = 5.0f;

    // GOState, restated as plain integers. The enum lives in
    // Entities/GameObject/GameObjectData.h, which drags in G3D and the rest of the
    // entity headers — far too much for a data lint — so this file follows the
    // folder's own idiom (StratholmeEvents' STR_GO_OPEN, BlackrockDepthsEvents'
    // BRD_GO_STATE_READY) and names the values it checks.
    //
    // The distinction that matters on this map: the three altar statues SPAWN at
    // GO_STATE_ACTIVE (0) and their own altar's SetData drives them to READY, while
    // the Rhino statue spawns at READY and only the bridge drop moves it to
    // ACTIVE_ALTERNATIVE. That is what makes each one an unambiguous witness.
    constexpr uint32 GD_GO_STATE_READY              = 1;
    constexpr uint32 GD_GO_STATE_ACTIVE_ALTERNATIVE = 2;

    DungeonBossInfo Boss(uint32 entry, uint32 idx, char const* name,
                         float x, float y, float z)
    {
        DungeonBossInfo b;
        b.entry = entry;
        b.encounterIndex = idx;
        b.name = name;
        b.mapId = MAP;
        b.x = x;
        b.y = y;
        b.z = z;
        b.kind = DungeonAnchorKind::Boss;
        return b;
    }

    // The roster BossSpawnIndex actually derives for map 604: the three bosses
    // whose instance_encounters credit entry has a creature spawn. 29573 (the
    // Drakkari Elemental, the Colossus's credit) and 29932 (Eck) have no spawn on
    // any map, so they are silently absent — which is the whole of defect F1.
    std::vector<DungeonBossInfo> DerivedNormal()
    {
        return {
            Boss(SLADRAN,  0, "Slad'ran",  1775.13f, 674.981f, 129.3f),
            Boss(MOORABI,  2, "Moorabi",   1772.47f, 809.537f, 129.3f),
            Boss(GALDARAH, 3, "Gal'darah", 1914.75f, 743.654f, 136.579f),
        };
    }

    // Heroic shifts Gal'darah from bit 3 to bit 4 (Eck takes bit 3). The DERIVED
    // row already carries that correctly — it is read per difficulty — so the patch
    // must not touch his encounterIndex.
    std::vector<DungeonBossInfo> DerivedHeroic()
    {
        return {
            Boss(SLADRAN,  0, "Slad'ran",  1775.13f, 674.981f, 129.3f),
            Boss(MOORABI,  2, "Moorabi",   1772.47f, 809.537f, 129.3f),
            Boss(GALDARAH, 4, "Gal'darah", 1914.75f, 743.654f, 136.579f),
        };
    }

    std::vector<DungeonBossInfo> Normal()
    {
        return BossRosterRegistry::Apply(MAP, DcDiffKey::Dungeon(DUNGEON_DIFFICULTY_NORMAL),
                                         DerivedNormal());
    }

    std::vector<DungeonBossInfo> Heroic()
    {
        return BossRosterRegistry::Apply(MAP, DcDiffKey::Dungeon(DUNGEON_DIFFICULTY_HEROIC),
                                         DerivedHeroic());
    }

    DungeonBossInfo const* FindEntry(std::vector<DungeonBossInfo> const& v, uint32 entry)
    {
        for (DungeonBossInfo const& b : v)
            if (b.entry == entry)
                return &b;
        return nullptr;
    }

    DungeonBossInfo const* FindEvent(std::vector<DungeonBossInfo> const& v, uint32 eventId)
    {
        for (DungeonBossInfo const& b : v)
            if (b.kind == DungeonAnchorKind::Objective && b.eventId == eventId)
                return &b;
        return nullptr;
    }

    int IndexOfEntry(std::vector<DungeonBossInfo> const& v, uint32 entry)
    {
        for (int i = 0; i < static_cast<int>(v.size()); ++i)
            if (v[i].entry == entry)
                return i;
        return -1;
    }

    int IndexOfEvent(std::vector<DungeonBossInfo> const& v, uint32 eventId)
    {
        for (int i = 0; i < static_cast<int>(v.size()); ++i)
            if (v[i].kind == DungeonAnchorKind::Objective && v[i].eventId == eventId)
                return i;
        return -1;
    }

    float Dist2d(float ax, float ay, float bx, float by)
    {
        float const dx = ax - bx;
        float const dy = ay - by;
        return std::sqrt(dx * dx + dy * dy);
    }

    float Dist3d(float ax, float ay, float az, float bx, float by, float bz)
    {
        float const dx = ax - bx;
        float const dy = ay - by;
        float const dz = az - bz;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }
}

// --- F1: the two bosses the derivation drops ------------------------------

// BossSpawnIndex::Build walks CREATURE SPAWNS looking for each encounter's
// creditEntry, so an encounter credited to an entry with no spawn contributes no
// row and no coordinates — silently. Map 604 has two of them, and the missing
// Colossus is the worse: without his anchor the party never walks to his wing, so
// he never dies, so his altar is never clickable, so the bridge never forms and
// Gal'darah is unreachable. The run hard-stalled at 2/4.
TEST(DungeonEventGundrakTest, ColossusAndEckAreRestoredToTheRoster)
{
    std::vector<DungeonBossInfo> const normal = Normal();
    std::vector<DungeonBossInfo> const heroic = Heroic();

    // --- the Drakkari Colossus, on BOTH difficulties -----------------------
    for (std::vector<DungeonBossInfo> const* roster : { &normal, &heroic })
    {
        DungeonBossInfo const* colossus = FindEntry(*roster, COLOSSUS);
        ASSERT_NE(colossus, nullptr)
            << "the Drakkari Colossus (29307) is missing from the roster — the "
               "derivation drops him because his instance_encounters credit is the "
               "ELEMENTAL (29573), which has no creature spawn";
        EXPECT_EQ(colossus->kind, DungeonAnchorKind::Boss);

        // Anchored on 29307, the entry that HAS a spawn, so liveness, GetLiveBoss
        // and IsCreaturePresentOnMap all resolve. Anchoring on the credit entry
        // instead would put the whole encounter back where it started.
        EXPECT_NE(colossus->entry, DRAKKARI_ELEMENTAL)
            << "the Colossus must be anchored on his own spawn entry (29307), never "
               "on the Elemental credit entry (29573) — nothing on any map spawns "
               "29573, so every entry-keyed lookup would miss";

        // MakeBossWithBit, not MakeBoss(doneBossStateIndex): the encounter has a
        // REAL DungeonEncounter row (bit 1 on both difficulties) and only the
        // derivation failed, so completion rides GetCompletedEncounterMask exactly
        // like every other boss here. instance_gundrak overrides neither GetData nor
        // the persistent store, so the boss-state fallback is not even available.
        EXPECT_EQ(colossus->encounterIndex, BIT_COLOSSUS)
            << "the Colossus is DungeonEncounter bit 1 on BOTH difficulties";
        EXPECT_LT(colossus->doneBossStateIndex, 0)
            << "completion must come from the DBC bit, not an instance boss-state slot";

        // His own spawn's column, on the probed navmesh floor (142.94) rather than
        // the 143.338 the spawn carries. On a map whose every .map tile has
        // gridHeight 0.0 that distinction is an 88-201yd fall, not a rounding nit.
        EXPECT_NEAR(colossus->x, COLOSSUS_X, 0.01f);
        EXPECT_NEAR(colossus->y, COLOSSUS_Y, 0.01f);
        EXPECT_NEAR(colossus->z, COLOSSUS_Z, 0.01f);
    }

    // --- Eck: heroic only --------------------------------------------------
    EXPECT_EQ(FindEntry(normal, ECK), nullptr)
        << "Eck has no instance_encounters row on normal (only encounter 389, heroic) "
           "and instance_gundrak's OnUnitDeath bails on !IsHeroic() — he must not "
           "appear in a normal roster";

    DungeonBossInfo const* eck = FindEntry(heroic, ECK);
    ASSERT_NE(eck, nullptr) << "Eck (29932) is missing from the heroic roster";
    EXPECT_EQ(eck->encounterIndex, BIT_ECK) << "Eck is heroic DungeonEncounter bit 3";

    // Anchored on his scripted HOME, never his summon point (1624.70, 891.43,
    // 95.08) — probed, there is no navmesh poly within 5yd of that column, and it
    // is exactly the shape map 604's 0.0 gridHeight punishes.
    EXPECT_NEAR(eck->x, ECK_X, 0.01f);
    EXPECT_NEAR(eck->y, ECK_Y, 0.01f);
    EXPECT_NEAR(eck->z, ECK_Z, 0.01f);
    EXPECT_GT(Dist3d(eck->x, eck->y, eck->z, 1624.70f, 891.43f, 95.08f), 20.0f)
        << "Eck's anchor has drifted toward his summon point, which has no mesh";

    // Gal'darah's derived bit must survive the patch untouched on both difficulties
    // — it differs BETWEEN them (3 normal, 4 heroic), so a hard-coded re-add would
    // be wrong on one of the two.
    ASSERT_NE(FindEntry(normal, GALDARAH), nullptr);
    ASSERT_NE(FindEntry(heroic, GALDARAH), nullptr);
    EXPECT_EQ(FindEntry(normal, GALDARAH)->encounterIndex, 3u);
    EXPECT_EQ(FindEntry(heroic, GALDARAH)->encounterIndex, 4u);
}

// The clear order is FORCED by the instance script, not chosen: each altar gates
// on its own boss's death and the bridge gates on all three altars, so the three
// bosses must precede their three clicks, which must precede the crossing, which
// must precede Gal'darah. On heroic Eck's pool and Eck himself slot in after
// Moorabi and before the crossing — which is also one-way, so he has to come first.
TEST(DungeonEventGundrakTest, ClearOrderFollowsTheForcedProgressionSpine)
{
    std::vector<DungeonBossInfo> const normal = Normal();

    int const sladran     = IndexOfEntry(normal, SLADRAN);
    int const altSladran  = IndexOfEvent(normal, EVENT_ALTAR_SLADRAN);
    int const colossus    = IndexOfEntry(normal, COLOSSUS);
    int const altColossus = IndexOfEvent(normal, EVENT_ALTAR_COLOSSUS);
    int const moorabi     = IndexOfEntry(normal, MOORABI);
    int const altMoorabi  = IndexOfEvent(normal, EVENT_ALTAR_MOORABI);
    int const bridge      = IndexOfEvent(normal, EVENT_BRIDGE);
    int const galdarah    = IndexOfEntry(normal, GALDARAH);

    ASSERT_NE(sladran, -1);
    ASSERT_NE(altSladran, -1);
    ASSERT_NE(colossus, -1);
    ASSERT_NE(altColossus, -1);
    ASSERT_NE(moorabi, -1);
    ASSERT_NE(altMoorabi, -1);
    ASSERT_NE(bridge, -1);
    ASSERT_NE(galdarah, -1);

    // Each altar after ITS OWN boss: instance_gundrak::SetBossState only clears
    // GO_FLAG_NOT_SELECTABLE from that altar on that boss's DONE, and
    // GameObject::Use() early-returns on a not-selectable GO.
    EXPECT_LT(sladran, altSladran);
    EXPECT_LT(colossus, altColossus);
    EXPECT_LT(moorabi, altMoorabi);

    // All three clicks before the crossing (the bridge needs 3/3 statues), and the
    // crossing before the last boss (it is the only way in).
    EXPECT_LT(altSladran, bridge);
    EXPECT_LT(altColossus, bridge);
    EXPECT_LT(altMoorabi, bridge);
    EXPECT_LT(bridge, galdarah);

    // Four anchors' worth of bosses on normal, and nothing else.
    EXPECT_EQ(FindEntry(normal, ECK), nullptr);
    EXPECT_EQ(IndexOfEvent(normal, EVENT_ECK_POOL), -1)
        << "the dweller pool is a heroic-only objective";

    // --- heroic: the pool, then Eck, then the one-way crossing -------------
    std::vector<DungeonBossInfo> const heroic = Heroic();
    int const hMoorabi = IndexOfEntry(heroic, MOORABI);
    int const hPool    = IndexOfEvent(heroic, EVENT_ECK_POOL);
    int const hEck     = IndexOfEntry(heroic, ECK);
    int const hBridge  = IndexOfEvent(heroic, EVENT_BRIDGE);
    int const hGal     = IndexOfEntry(heroic, GALDARAH);
    ASSERT_NE(hPool, -1);
    ASSERT_NE(hEck, -1);
    ASSERT_NE(hBridge, -1);

    EXPECT_LT(hMoorabi, hPool)
        << "the Eck Door opens on Moorabi's death, so the pool is unreachable before him";
    EXPECT_LT(hPool, hEck) << "no formation wipe, no Eck";
    EXPECT_LT(hEck, hBridge)
        << "the crossing is ONE-WAY (comp#1 has no ground link back), so everything "
           "on the near side must be finished before it";
    EXPECT_LT(hBridge, hGal);

    // Five bosses on heroic, four on normal.
    auto bosses = [](std::vector<DungeonBossInfo> const& v)
    {
        int n = 0;
        for (DungeonBossInfo const& b : v)
            if (b.kind == DungeonAnchorKind::Boss)
                ++n;
        return n;
    };
    EXPECT_EQ(bosses(normal), 4);
    EXPECT_EQ(bosses(heroic), 5);
}

// --- F3: the three altar clicks are three VERIFIED clicks -----------------

// UseGameObject reports Done the instant it calls Use() — it never checks that
// anything happened. instance_gundrak overrides neither GetData nor the
// persistent-data store, so the Ahn'kahet "click then hold for the data index"
// pair is unavailable; the observable effect here is a GAMEOBJECT STATE CHANGE,
// so each click is verified by watching its own statue reach GO_STATE_READY.
//
// The altar->statue mapping is easy to transpose and the instance script's own
// _bridgeGUIDs indices run in a DIFFERENT order (0 Slad'ran, 1 Drakkari, 2
// Moorabi) from the DATA_* enum, so it is pinned explicitly.
TEST(DungeonEventGundrakTest, AltarEventsAreThreeVerifiedClicks)
{
    struct Row { uint32 eventId; uint32 altar; uint32 statue; char const* name; };
    constexpr Row kRows[] = {
        { EVENT_ALTAR_SLADRAN,  ALTAR_SLADRAN,  STATUE_SNAKE,   "Slad'ran"  },
        { EVENT_ALTAR_COLOSSUS, ALTAR_COLOSSUS, STATUE_TROLL,   "Colossus"  },
        { EVENT_ALTAR_MOORABI,  ALTAR_MOORABI,  STATUE_MAMMOTH, "Moorabi"   },
    };

    for (Row const& r : kRows)
    {
        DungeonEvent const* ev = DungeonEventRegistry::Find(MAP, r.eventId);
        ASSERT_NE(ev, nullptr) << "altar event " << r.eventId << " (" << r.name << ") missing";
        EXPECT_EQ(ev->activation, EventActivation::Anchored);
        EXPECT_TRUE(ev->required)
            << r.name << ": the altars gate the LAST BOSS — a click that cannot land "
               "must stall for the human, not silently skip";
        EXPECT_TRUE(ev->persistent)
            << r.name << ": multi-step, so a >1s Drive gap would rewind to step 0 and "
               "re-click a spent altar";

        // Find the click and its verification, wherever the optional MoveTo puts them.
        EventStep const* use = nullptr;
        EventStep const* wait = nullptr;
        for (EventStep const& s : ev->steps)
        {
            if (s.kind == EventStepKind::UseGameObject)
                use = &s;
            else if (s.kind == EventStepKind::WaitForGameObjectState)
                wait = &s;
        }
        ASSERT_NE(use, nullptr) << r.name << ": no UseGameObject step";
        ASSERT_NE(wait, nullptr) << r.name << ": the click is UNVERIFIED — UseGameObject "
                                            "reports Done without checking anything happened";

        EXPECT_EQ(use->goEntry, r.altar);
        EXPECT_FALSE(use->reportUse)
            << r.name << ": the altars' smart_scripts row is SMART_EVENT_GOSSIP_HELLO "
               "with filter 0 (always execute), which a plain GameObject::Use() reaches "
               "through AI()->GossipHello(player, false). This is the Nexus sphere case, "
               "not the BWL Chromaggus-lever case — .ReportUse() would be wrong.";
        EXPECT_EQ(use->timeoutMs, ALTAR_TIMEOUT)
            << r.name << ": the step deliberately HOLDS on an altar still flagged "
               "NOT_SELECTABLE, and the 30s default reads a just-landed SetBossState "
               "as a stall";

        EXPECT_EQ(wait->goEntry, r.statue)
            << r.name << ": wrong statue — SetData drives 192518->192564, "
               "192520->192567, 192519->192565, and the script's _bridgeGUIDs indices "
               "are in a different order from the DATA_* enum";
        EXPECT_EQ(wait->wantState, GD_GO_STATE_READY)
            << r.name << ": the three altar statues SPAWN at GO_STATE_ACTIVE and are "
               "only ever driven to READY by their own altar, so READY is a true edge";

        // The click must come before its own verification.
        std::size_t useIdx = 0, waitIdx = 0;
        for (std::size_t i = 0; i < ev->steps.size(); ++i)
        {
            if (&ev->steps[i] == use)
                useIdx = i;
            if (&ev->steps[i] == wait)
                waitIdx = i;
        }
        EXPECT_LT(useIdx, waitIdx) << r.name << ": verification must follow the click";
    }
}

// All four statues stand together at (1775.16, 743.46, 119.07) — 62 to 89yd from
// the altar anchors that drive them — and WaitForGOState scans FROM THE BOT, with
// DC_EVENT_GO_SEARCH by default. A regression here does not fail loudly: the
// step simply never finds the GO, stays Running, and every altar event hangs on
// its verification step until the timeout.
TEST(DungeonEventGundrakTest, StatueSearchRadiusReachesTheBridgeChamber)
{
    std::vector<DungeonBossInfo> const roster = Normal();

    struct Row { uint32 eventId; char const* name; };
    constexpr Row kRows[] = {
        { EVENT_ALTAR_SLADRAN,  "Slad'ran"  },
        { EVENT_ALTAR_COLOSSUS, "Colossus"  },
        { EVENT_ALTAR_MOORABI,  "Moorabi"   },
    };

    for (Row const& r : kRows)
    {
        DungeonBossInfo const* anchor = FindEvent(roster, r.eventId);
        ASSERT_NE(anchor, nullptr) << r.name;
        DungeonEvent const* ev = DungeonEventRegistry::Find(MAP, r.eventId);
        ASSERT_NE(ev, nullptr) << r.name;

        // Worst case: the tank sits at the far edge of the anchor's arrive radius,
        // on the side away from the statues.
        float const d = Dist3d(anchor->x, anchor->y, anchor->z,
                               STATUE_X, STATUE_Y, STATUE_Z) + anchor->arriveRadius;

        for (EventStep const& s : ev->steps)
        {
            if (s.kind != EventStepKind::WaitForGameObjectState)
                continue;
            EXPECT_GT(s.radius, d)
                << r.name << ": the statue search radius (" << s.radius << ") does not "
                << "reach the bridge chamber from this altar's anchor (" << d
                << "yd worst case) — the verification step would hang forever";
        }
    }

    // The four statues carry distinct entries, so a wide scan cannot find the
    // wrong one; but it must not be so wide it is meaningless either.
    EXPECT_LT(STATUE_SEARCH, 200.0f);
}

// --- F9: the two altars that stand in navmesh HOLES -----------------------

// Probed against the live 604 mmtiles: the Slad'ran and Colossus altars have NO
// walkable surface in their own columns (drops of 19.10 and 32.65yd into the
// moat, and the Colossus one lands in ISOLATED comp#15 with no path back). The
// executor's UseGameObject step walks an out-of-range bot to the GO'S OWN
// COORDINATES, so an anchor that leaves the tank further than DC_EVENT_GO_USE_RANGE
// from either GO strands it in a hole.
//
// The fix separates the two jobs — a roomy objective anchor the pathfinder can
// actually satisfy, plus a short MoveTo onto a measured click pad — and this test
// re-derives the arithmetic that makes it safe.
TEST(DungeonEventGundrakTest, AltarClickPointsAreInsideTheUseRange)
{
    struct Row
    {
        uint32 eventId;
        float goX, goY;
        float clickX, clickY;
        float pad;
        char const* name;
    };
    constexpr Row kRows[] = {
        { EVENT_ALTAR_SLADRAN,  ALTAR_SLADRAN_GO_X,  ALTAR_SLADRAN_GO_Y,
          SLADRAN_CLICK_X,  SLADRAN_CLICK_Y,  SLADRAN_CLICK_PAD,  "Slad'ran" },
        { EVENT_ALTAR_COLOSSUS, ALTAR_COLOSSUS_GO_X, ALTAR_COLOSSUS_GO_Y,
          COLOSSUS_CLICK_X, COLOSSUS_CLICK_Y, COLOSSUS_CLICK_PAD, "Colossus" },
    };

    for (Row const& r : kRows)
    {
        DungeonEvent const* ev = DungeonEventRegistry::Find(MAP, r.eventId);
        ASSERT_NE(ev, nullptr) << r.name;

        // The hole altars need the three-step shape; a bare UseGO would be walked
        // into the hole by the executor's own out-of-range fallback.
        ASSERT_FALSE(ev->steps.empty());
        EXPECT_EQ(ev->steps.front().kind, EventStepKind::MoveTo)
            << r.name << ": this altar stands in a navmesh hole — its event must close "
               "the last yards with an explicit MoveTo onto the measured rim pad before "
               "clicking, or the executor's HopTo drops the bot into the moat";

        EventStep const& move = ev->steps.front();
        EXPECT_NEAR(move.x, r.clickX, 0.01f) << r.name;
        EXPECT_NEAR(move.y, r.clickY, 0.01f) << r.name;

        float const d = Dist2d(move.x, move.y, r.goX, r.goY);

        // (1) The bot must be able to click from ANYWHERE inside the MoveTo radius.
        EXPECT_LE(d + move.radius, GD_GO_USE_RANGE)
            << r.name << ": worst-case distance to the altar is " << (d + move.radius)
            << "yd, past DC_EVENT_GO_USE_RANGE (" << GD_GO_USE_RANGE << ") — a bot that "
               "settles on the far side of its own arrive radius gets HopTo'd to the "
               "GO's coordinates, which here is a hole";

        // (2) ...and it must not be able to step OFF the pad while doing so.
        EXPECT_LT(move.radius, r.pad)
            << r.name << ": the MoveTo radius (" << move.radius << ") exceeds the "
            << "measured walkable pad at the click point (" << r.pad << "yd), so the bot "
               "can turn off the rim into the hole";

        // Both holes measure as discs of radius ~2.25 centred on the altar, so the
        // pad at distance d is d - 2.25 and the click constraint d + (d - 2.25) <= 5
        // caps d at 3.625. Anything further out cannot be made safe at all.
        EXPECT_LE(d, 3.625f)
            << r.name << ": a click point further than 3.625yd from the altar cannot "
               "satisfy both constraints at once on a 2.25yd hole";
    }

    // The objective anchors, by contrast, sit far enough out that their pads clear
    // the pathfinder's own 6yd segment arrival — otherwise the tank parks short and
    // the objective never fires at all, which is how a "safe" tiny arrive radius
    // turns into a dead run.
    std::vector<DungeonBossInfo> const roster = Normal();
    for (uint32 eventId : { EVENT_ALTAR_SLADRAN, EVENT_ALTAR_COLOSSUS, EVENT_ALTAR_MOORABI })
    {
        DungeonBossInfo const* anchor = FindEvent(roster, eventId);
        ASSERT_NE(anchor, nullptr) << "event " << eventId;
        EXPECT_GE(anchor->arriveRadius, 6.0f)
            << "objective for event " << eventId << " has an arrive radius below the "
               "pathfinder's own segment arrival — the tank will park short and the "
               "event will never start";
    }

    // The Slad'ran and Colossus ANCHORS must be well clear of their altars: that
    // distance is the whole point of splitting the anchor from the click.
    DungeonBossInfo const* sladranAnchor = FindEvent(roster, EVENT_ALTAR_SLADRAN);
    DungeonBossInfo const* colossusAnchor = FindEvent(roster, EVENT_ALTAR_COLOSSUS);
    ASSERT_NE(sladranAnchor, nullptr);
    ASSERT_NE(colossusAnchor, nullptr);
    EXPECT_GT(Dist2d(sladranAnchor->x, sladranAnchor->y,
                     ALTAR_SLADRAN_GO_X, ALTAR_SLADRAN_GO_Y), 8.0f);
    EXPECT_GT(Dist2d(colossusAnchor->x, colossusAnchor->y,
                     ALTAR_COLOSSUS_GO_X, ALTAR_COLOSSUS_GO_Y), 8.0f);

    // Moorabi's altar has 7.25yd of continuous mesh under it and needs none of
    // this — its anchor IS the GO. Recorded so a future author does not "fix" the
    // asymmetry by adding a MoveTo that buys nothing.
    DungeonBossInfo const* moorabiAnchor = FindEvent(roster, EVENT_ALTAR_MOORABI);
    ASSERT_NE(moorabiAnchor, nullptr);
    EXPECT_LT(Dist2d(moorabiAnchor->x, moorabiAnchor->y, 1772.22f, 804.963f), 1.0f);
}

// The two rim altars park the tank OUTSIDE their own objective's arrive radius —
// forced, not sloppy. Their GOs stand in navmesh holes, so the anchor (roomy, for
// the followers to gather on) and the click (a measured rim pad) cannot be the
// same point, and the leading MoveTo between them is longer than arriveRadius.
//
// That shape deadlocked in the field: tp-20260830-185318-1 lost 3 of 10 runs to
//   objective 'Altar of the Drakkari Colossus': dist=7.0 > arriveRadius=6.0
//       (NOT arrived; event not started)
// logged in the same second as the executor driving that event's step 0 — the tank
// pinned ~7yd from its anchor, making no net progress for the rest of the run. The
// fix is in IsPersistentAnchoredEventActive, which now counts a leading MoveTo as
// "started"; this test pins the geometry that makes that latch load-bearing here,
// so if someone later "simplifies" the latch these numbers say what breaks.
//
// Retuning arriveRadius instead is NOT an option and the margins say why: it is
// also the ring the followers gather in, so it is capped by the anchor's measured
// pad and floored by the MoveTo's reach. Slad'ran's window is 7.43..7.60 and the
// Colossus's 8.26..8.50 — every solution stands the party within a few centimetres
// of the drop the tank already fell down.
TEST(DungeonEventGundrakTest, RimAltarsLeadWithAMoveToBeyondTheirArriveRadius)
{
    std::vector<DungeonBossInfo> const roster = Normal();

    struct Row
    {
        uint32 eventId;
        char const* name;
    };
    constexpr Row kRims[] = {
        { EVENT_ALTAR_SLADRAN,  "Slad'ran"  },
        { EVENT_ALTAR_COLOSSUS, "Colossus"  },
    };

    for (Row const& r : kRims)
    {
        DungeonBossInfo const* anchor = FindEvent(roster, r.eventId);
        ASSERT_NE(anchor, nullptr) << r.name;
        DungeonEvent const* ev = DungeonEventRegistry::Find(MAP, r.eventId);
        ASSERT_NE(ev, nullptr) << r.name;
        ASSERT_FALSE(ev->steps.empty()) << r.name;

        // The latch keys off exactly this: step 0 being a MoveTo.
        ASSERT_EQ(ev->steps.front().kind, EventStepKind::MoveTo) << r.name;

        // The tank must stand at the click, up to the step's own radius off it, for
        // that MoveTo to report Done — so this is how far from the anchor the event
        // needs the objective to still count as ARRIVED.
        EventStep const& move = ev->steps.front();
        float const reach = Dist2d(anchor->x, anchor->y, move.x, move.y) + move.radius;
        EXPECT_GT(reach, anchor->arriveRadius)
            << r.name << ": this altar's leading MoveTo now finishes INSIDE the arrive "
               "radius (" << reach << "yd vs " << anchor->arriveRadius << "yd). That is "
               "a fine state of affairs, but it means this dungeon no longer exercises "
               "the leading-MoveTo latch — check something still does before trusting it.";
    }

    // Moorabi's altar has 7.25yd of continuous mesh under it, so anchor and click
    // are the same point and it never needed the latch — nor did it ever reproduce
    // the stall in the field. Recorded so the asymmetry is not "fixed" away.
    DungeonEvent const* moorabi = DungeonEventRegistry::Find(MAP, EVENT_ALTAR_MOORABI);
    ASSERT_NE(moorabi, nullptr);
    ASSERT_FALSE(moorabi->steps.empty());
    EXPECT_NE(moorabi->steps.front().kind, EventStepKind::MoveTo);
}

// --- F2: the Colossus's Living Mojo ring ----------------------------------

// The Colossus spawns NON_ATTACKABLE with MoveInLineOfSight stubbed out; the only
// thing that starts the fight is a PLAYER attacking one of the five summoned
// Living Mojos, whose JustEngagedWith informs the summoner. But there are also
// FOUR PRE-PLACED Living Mojos of the same entry 39-94yd west, and those are
// ordinary trash: not TempSummons, so they aggro, they fight, and hitting one
// informs NOBODY. A search radius that reaches them sends the party to "start the
// fight" against a mob that cannot start it.
TEST(DungeonEventGundrakTest, MojoSearchSeparatesTheRingFromTheTrash)
{
    DungeonEvent const* ev = DungeonEventRegistry::Find(MAP, EVENT_COLOSSUS_MOJO);
    ASSERT_NE(ev, nullptr);
    EXPECT_EQ(ev->activation, EventActivation::Conditional)
        << "UNIT_FLAG_NON_ATTACKABLE is the encounter's own phase bit in both "
           "directions — after an EVADE, Reset() REMOVES it and summons no mojos, so a "
           "conditional event correctly never fires on a retry. An anchored Optional "
           "objective would have to burn a timeout to learn the same thing.";
    EXPECT_EQ(ev->panelGatesBossEntry, COLOSSUS);

    ASSERT_EQ(ev->steps.size(), 1u);
    EventStep const& step = ev->steps.front();
    EXPECT_EQ(step.kind, EventStepKind::KillCreature);
    EXPECT_EQ(step.creatureEntry, LIVING_MOJO);
    EXPECT_TRUE(step.engage)
        << "a conditional event has no boss-nav to deliver the tank, so the step must "
           "seek — DcRunEventAction walks the leader in through the engage pipeline "
           "only for a KillCreature with .engage";

    float const search = step.radius;
    ASSERT_GT(search, 0.0f);

    // Every one of the five SUMMONED ring positions must be inside the search from
    // a leader standing at the merge point (which is the boss's own spawn column).
    for (int i = 0; i < 5; ++i)
    {
        float const d = Dist2d(MOJO_RING_X[i], MOJO_RING_Y[i], COLOSSUS_X, COLOSSUS_Y);
        EXPECT_LT(d, search)
            << "summoned ring mojo " << i << " is " << d << "yd from the merge point, "
            << "outside the " << search << "yd search — the step could report Done with "
               "the ring still standing";
    }

    // ...and every one of the four PRE-PLACED trash mojos must be outside it, both
    // from the merge point AND from the nearest ring position (the search is
    // bot-centred, and the leader ends up standing on a ring mojo).
    for (int i = 0; i < 4; ++i)
    {
        float const dCentre = Dist2d(MOJO_TRASH_X[i], MOJO_TRASH_Y[i], COLOSSUS_X, COLOSSUS_Y);
        EXPECT_GT(dCentre, search)
            << "pre-placed trash mojo " << i << " is only " << dCentre << "yd from the "
            << "merge point — inside the " << search << "yd search. Hitting it informs "
               "NOBODY: it is not a TempSummon, so npc_living_mojoAI::JustEngagedWith "
               "takes the ordinary branch and the Colossus never wakes.";

        float nearestRing = 1e9f;
        for (int k = 0; k < 5; ++k)
            nearestRing = std::min(nearestRing,
                                   Dist2d(MOJO_TRASH_X[i], MOJO_TRASH_Y[i],
                                          MOJO_RING_X[k], MOJO_RING_Y[k]));
        EXPECT_GT(nearestRing, search)
            << "pre-placed trash mojo " << i << " is only " << nearestRing << "yd from "
            << "the nearest RING mojo — the search scans from the bot, which stands on "
               "the ring";
    }

    // The activation predicate is near-gated (it is on
    // IsNearGatedConditionalWhitelisted for exactly this reason); the scan must stay
    // small enough to mean "the leader is in his arena" rather than "somewhere on
    // the map", and small enough not to fire while the party is still on the west
    // corridor's trash.
    EXPECT_LE(COLOSSUS_SCAN, 60.0f);
    EXPECT_GE(COLOSSUS_SCAN, 20.0f);
}

// --- F4: only three of the six Ruins Dwellers gate Eck --------------------

// instance_gundrak::OnUnitDeath bails unless the dead dweller has a
// CreatureGroup, then waits for `!formation->IsAnyMemberAlive()`. Only
// 127201/127202/127203 are in a formation (leader 127203, groupAI 3); the other
// three are ungrouped and gate NOTHING. A KillCreature(29920, 6) gate would be
// wrong in both directions — it demands three irrelevant elite kills and it does
// not express "these particular three" — so the event is a position-anchored
// volume, and this test is what keeps that volume honest.
//
// This is the single most valuable test on this map: nothing else catches a run
// that kills three elites in the shallows and never summons Eck.
TEST(DungeonEventGundrakTest, EckPoolVolumeNamesExactlyTheFormationTrio)
{
    DungeonEvent const* ev = DungeonEventRegistry::Find(MAP, EVENT_ECK_POOL);
    ASSERT_NE(ev, nullptr);
    EXPECT_EQ(ev->gate, DcDifficultyGate::HeroicOnly)
        << "OnUnitDeath's first test is !instance->IsHeroic(); on normal the six "
           "dwellers exist and do nothing";
    EXPECT_TRUE(ev->required) << "no formation wipe, no Eck, and Eck is a rostered boss";
    EXPECT_TRUE(ev->persistent);

    ASSERT_EQ(ev->steps.size(), 1u);
    EventStep const& s = ev->steps.front();
    ASSERT_EQ(s.kind, EventStepKind::ClearRadius);

    ASSERT_EQ(s.entryFilter.size(), 1u)
        << "unfiltered, the sweep wanders into the Drakkari Frenzy (29834, 24 spawns) "
           "shoaling in the same water";
    EXPECT_EQ(s.entryFilter.front(), RUINS_DWELLER);

    // NearestHostileNearPoint's own test: 2D radius, plus a vertical band on the
    // FLOOR z. Reproduced exactly so the assertion means what the runtime does.
    auto inVolume = [&](float x, float y, float z)
    {
        if (s.zBand > 0.0f && std::fabs(z - s.z) > s.zBand)
            return false;
        float const dx = x - s.x;
        float const dy = y - s.y;
        return dx * dx + dy * dy <= s.radius * s.radius;
    };

    for (int i = 0; i < 3; ++i)
    {
        EXPECT_TRUE(inVolume(DWELLER_GATING_X[i], DWELLER_GATING_Y[i], DWELLER_GATING_Z[i]))
            << "gating Ruins Dweller " << i << " (formation member) is OUTSIDE the pool "
               "volume — Eck would never be summoned and the heroic run ends at 4/5";
        EXPECT_FALSE(inVolume(DWELLER_UNGROUPED_X[i], DWELLER_UNGROUPED_Y[i],
                              DWELLER_UNGROUPED_Z[i]))
            << "ungrouped Ruins Dweller " << i << " is INSIDE the pool volume — killing "
               "it summons nothing and costs the run a pointless level-74 elite fight";
    }

    // Z IS THE WATER SHEET, NOT THE SPAWN. The trio spawn at 107.28; the walkable
    // surface above them is the sheet at 108.05-108.42. On a map whose every .map
    // tile carries gridHeight 0.0, taking a creature's own z as ground is the
    // mistake that sinks parties.
    EXPECT_GT(s.z, 107.9f)
        << "the pool volume is centred on the dwellers' SPAWN z rather than the water "
           "sheet the party actually stands on";

    // The objective anchor is the volume's centre, so boss-nav travels the tank in
    // before the sweep starts.
    std::vector<DungeonBossInfo> const heroic = Heroic();
    DungeonBossInfo const* anchor = FindEvent(heroic, EVENT_ECK_POOL);
    ASSERT_NE(anchor, nullptr);
    EXPECT_NEAR(anchor->x, s.x, 0.01f);
    EXPECT_NEAR(anchor->y, s.y, 0.01f);
    EXPECT_NEAR(anchor->z, s.z, 0.01f);
}

// --- F8: the crossing -----------------------------------------------------

// Gal'darah's entire wing is a disconnected navmesh component. The bridge is a
// DB-spawned GameObject, so it is not in the mmaps and never will be: flipping it
// to GO_STATE_ACTIVE_ALTERNATIVE changes what the client draws and what blocks
// collision, and adds no Detour polygon. Clicking all three altars therefore
// produces a bridge the bots still cannot walk on, and the run ends staring
// across a chasm at 4/5.
TEST(DungeonEventGundrakTest, BridgeCrossingIsGatedThenTeleported)
{
    DungeonEvent const* ev = DungeonEventRegistry::Find(MAP, EVENT_BRIDGE);
    ASSERT_NE(ev, nullptr);
    EXPECT_EQ(ev->activation, EventActivation::Anchored);
    EXPECT_TRUE(ev->required) << "this is the only way into the last boss";
    EXPECT_TRUE(ev->persistent)
        << "a TeleportParty step is one-way — a combat gap rewinding to step 0 is the "
           "module's most-repeated bug class";

    ASSERT_EQ(ev->steps.size(), 2u);

    // ORDER IS LOAD-BEARING. Once the crossing is a teleport, the bridge's state is
    // mechanically irrelevant to it — which is exactly why the gate has to come
    // FIRST. A teleport that ran first would happily cross a chasm the party never
    // opened, silently skipping the whole altar chain and turning a broken altar
    // into an invisible failure instead of a legible one.
    EventStep const& gate = ev->steps[0];
    EventStep const& hop  = ev->steps[1];

    ASSERT_EQ(gate.kind, EventStepKind::WaitForGameObjectState)
        << "the crossing must be GATED on the bridge having actually formed, and the "
           "gate must be the FIRST step";
    EXPECT_EQ(gate.goEntry, STATUE_RHINO)
        << "192566 (Rhino) is the right witness: it is the only one of the four "
           "statues that SPAWNS at GO_STATE_READY, so ACTIVE_ALTERNATIVE can only have "
           "come from instance_gundrak::Update's bridge drop and can never be confused "
           "with an altar click";
    EXPECT_EQ(gate.wantState, GD_GO_STATE_ACTIVE_ALTERNATIVE);
    EXPECT_GT(gate.radius, 40.0f)
        << "the witness is scanned FROM THE BOT at the checkpoint, ~29yd from the "
           "statues, and the default search does not reach";

    ASSERT_EQ(hop.kind, EventStepKind::TeleportParty)
        << "NOT a Jump. The gaps are 11.5 and 11.75yd at nearly constant Z, which is "
           "within ballistic range — but Jump is a LEADER-ONLY step: nothing carries "
           "the followers across, so the tank would land on the far side and the other "
           "four would stand on the causeway with no path to it.";

    // The checkpoint is on comp#0 (west of the west causeway tip) and the landing
    // on comp#1 (east of the east tip). Both are set back from the tips, which
    // measure 0.00 and 0.25yd of walkable pad — knife edges five bots cannot arrive
    // on.
    EXPECT_NEAR(hop.x, CROSS_CHECK_X, 0.01f);
    EXPECT_NEAR(hop.y, CROSS_CHECK_Y, 0.01f);
    EXPECT_NEAR(hop.landX, CROSS_LAND_X, 0.01f);
    EXPECT_NEAR(hop.landY, CROSS_LAND_Y, 0.01f);

    EXPECT_LT(hop.x, WEST_TIP_X)
        << "the checkpoint is past the west causeway tip — that is a 0.00yd pad and "
           "the exact spot the stock pathfinder already strands a Gal'darah-bound bot";
    EXPECT_GT(hop.landX, EAST_TIP_X)
        << "the landing is west of where the east causeway's mesh begins";

    // ...and neither may land on the central island, which is a 7-poly DEAD
    // component with zero links in any direction: a party left on it is trapped
    // between two gaps.
    constexpr float ISLAND_MIN_X = 1765.33f;
    constexpr float ISLAND_MAX_X = 1784.80f;
    EXPECT_FALSE(hop.x >= ISLAND_MIN_X && hop.x <= ISLAND_MAX_X)
        << "the checkpoint is on the dead central island";
    EXPECT_FALSE(hop.landX >= ISLAND_MIN_X && hop.landX <= ISLAND_MAX_X)
        << "the landing is on the dead central island";

    // The objective anchor puts the tank on the checkpoint, and the teleport radius
    // must comfortably cover its arrive radius or the step stutter-steps.
    std::vector<DungeonBossInfo> const roster = Normal();
    DungeonBossInfo const* anchor = FindEvent(roster, EVENT_BRIDGE);
    ASSERT_NE(anchor, nullptr);
    EXPECT_NEAR(anchor->x, CROSS_CHECK_X, 0.01f);
    EXPECT_NEAR(anchor->y, CROSS_CHECK_Y, 0.01f);
    EXPECT_GT(hop.radius, anchor->arriveRadius)
        << "a teleport gate tighter than the anchor's own arrive radius means the "
           "leader can be 'at the objective' and still not satisfy the hop";
}

// --- F5: Gal'darah's room seals -------------------------------------------

TEST(DungeonEventGundrakTest, GaldarahArenaIsASealedEncounter)
{
    SealedEncounterRow const* row = SealedEncounterRegistry::Find(MAP, GALDARAH);
    ASSERT_NE(row, nullptr)
        << "192568 is registered DOOR_TYPE_ROOM against DATA_GAL_DARAH, so the arena "
           "shuts when the encounter goes IN_PROGRESS and anyone outside is locked out "
           "for the whole fight";

    // The boss is inside; the bridge chamber the party crosses from is not.
    EXPECT_TRUE(SealedEncounterRegistry::InSealedRoom(*row, 1914.75f, 743.654f));
    EXPECT_FALSE(SealedEncounterRegistry::InSealedRoom(*row, STATUE_X, STATUE_Y));
    EXPECT_FALSE(SealedEncounterRegistry::InSealedRoom(*row, CROSS_LAND_X, CROSS_LAND_Y))
        << "the teleport landing is on the approach causeway, OUTSIDE the door";

    // The muster spot is inside, with margin.
    EXPECT_TRUE(SealedEncounterRegistry::InSealedRoom(*row, MUSTER_X, MUSTER_Y));
    EXPECT_GT(MUSTER_X - row->minX, 2.0f);

    // approachRadius must reach BACK PAST THE DOOR, or the gates arm only once the
    // party is already inside and neither the clump nor the muster can do anything.
    // Gal'darah's door hangs at x 1848.03 and he spawns at x 1914.75 — 66.76yd of
    // room, against 27yd at Selin and a corridor mouth at 45 for Anub'arak. This is
    // the number a copy-paste of those rows gets wrong.
    constexpr float DOOR_X = 1848.03f;
    float const doorToBoss = Dist3d(DOOR_X, 743.816f, 135.949f,
                                    1914.75f, 743.654f, 136.579f);
    EXPECT_GT(row->approachRadius, doorToBoss)
        << "approachRadius (" << row->approachRadius << ") does not reach the door ("
        << doorToBoss << "yd from the boss), so the muster and the clump only arm once "
           "the party has already crossed the threshold";

    // ...but not so far back that it arms on the far side of the chasm, where the
    // crossing must run under the ordinary gates.
    float const landingToBoss = Dist3d(CROSS_LAND_X, CROSS_LAND_Y, CROSS_LAND_Z,
                                       1914.75f, 743.654f, 136.579f);
    EXPECT_LT(row->approachRadius, landingToBoss)
        << "approachRadius reaches back to the teleport landing — the clump would fight "
           "the crossing";

    EXPECT_FLOAT_EQ(row->musterSpread, 10.0f);
}

// --- the hazard row -------------------------------------------------------

TEST(DungeonEventGundrakTest, MojoPuddleIsTheOnlyGroundHazard)
{
    DcGroundHazard const* puddle = DcHazardRegistry::FindGround(MAP, SPELL_MOJO_PUDDLE);
    ASSERT_NE(puddle, nullptr)
        << "55627 Mojo Puddle is the only SPELL_EFFECT_PERSISTENT_AREA_AURA on map 604 "
           "(Effect[0] = 27, EffectRadiusIndex 15 = 3.0yd, 1000ms amplitude, 10s)";

    // The invariant every row here keeps: the retreat aims at vacate + slack, which
    // must land OUTSIDE the placement keep-out or PointIsHot rejects its own aim
    // point and the retreat can never find a spot.
    EXPECT_GT(puddle->vacateRadius + puddle->retreatSlack, puddle->radius);
    EXPECT_GT(puddle->retreatSlack, puddle->holdBand);
    EXPECT_GE(puddle->vacateRadius, 3.0f) << "vacateRadius is the RAW aura radius";

    // ONE ROW, not two: no Gundrak spell has a SpellDifficulty.dbc entry, so the
    // heroic templates cast the same id and the DynamicObject reports 55627 on both
    // difficulties.
    int rows = 0;
    for (uint32 spellId : { 55627u, 54888u, 55081u, 55101u, 55142u, 55250u, 55292u,
                            54956u, 55218u, 55626u })
        if (DcHazardRegistry::FindGround(MAP, spellId))
            ++rows;
    EXPECT_EQ(rows, 1)
        << "map 604 should carry exactly one ground-hazard row. 54888 Elemental Spawn "
           "Effect has a PERSISTENT_AREA_AURA leg but at 1.0yd with a dummy aura (a "
           "spawn visual); the novas and self-auras have nothing to stand outside of.";
}

// --- allowlist drift ------------------------------------------------------

// None of the six Gundrak events uses drivesInCombat, stepsOwnMovement or
// ownsThePull, so map 604 stays off all three allowlist gtests. A future author
// reaching for one of those must update the corresponding allowlist table in the
// same commit — deliberately, rather than discovering it as a red build and
// pasting an entry to make it green.
TEST(DungeonEventGundrakTest, NoGundrakEventClaimsAVettedFlag)
{
    int found = 0;
    for (DungeonEvent const& ev : DungeonEventRegistry::AllEvents())
    {
        if (ev.mapId != MAP)
            continue;
        ++found;
        EXPECT_FALSE(ev.drivesInCombat)
            << "event " << ev.id << " (" << ev.name << ") — the party is OUT of combat "
               "for every Gundrak event; see DrivesInCombatIsConfinedToVettedWaveEncounters";
        EXPECT_FALSE(ev.stepsOwnMovement)
            << "event " << ev.id << " (" << ev.name
            << ") — see StepsOwnMovementIsConfinedToVettedEvents";
        EXPECT_FALSE(ev.ownsThePull)
            << "event " << ev.id << " (" << ev.name << ") — see PullOwningEventsAreVetted";
        EXPECT_FALSE(ev.encounterActive) << "map 604 is a 5-man; stand-down never arms";
    }
    EXPECT_EQ(found, 6) << "Gundrak authors six events (three altars, the mojo pull, the "
                           "dweller pool and the crossing)";
}

// --- F6: the rhino stampede stands on the Gal'darah approach ---------------
//
// The fifth stacked defect, and the one that only became visible once the
// bridge crossing (F4) started working: the party lands, walks east, and runs
// straight into the stampede staging ground.
//
// Live, tp-20260830-195430-1: of the six runs that killed Moorabi, THREE were
// ended here. tr-…-7 wiped outright to the charging rhino. tr-…-2 and tr-…-6
// hung until the operator aborted them at 29 minutes, with every member held in
// combat by a dismounted Drakkari Raider at 16-26yd that never closed and never
// died — closest approach to Gal'darah 27.6yd and 46.7yd against an engage range
// of 22, so the at-boss path never armed and the boss never aggroed at all.
//
// What is pinned here is the GEOMETRY that makes the encounter unavoidable. The
// combat-resolution half of the fix (walking the PvE combat refs, not just
// getAttackers) lives in DcTargeting::CollectCombatHolders and needs a live
// CombatManager, so it is verified in-game rather than here.
TEST(DungeonEventGundrakTest, RhinoStampedeStandsOnTheGaldarahApproach)
{
    std::vector<DungeonBossInfo> const roster = DerivedNormal();
    auto const boss = std::find_if(roster.begin(), roster.end(),
                                   [](DungeonBossInfo const& b) { return b.entry == GALDARAH; });
    ASSERT_NE(boss, roster.end());

    // Both rhinos sit BETWEEN the teleport landing and the boss, on the causeway
    // centreline. There is no way to reach Gal'darah that does not pass them, so
    // "route around the stampede" is not an available fix.
    for (float rhinoX : {RHINO_LOADED_X, RHINO_CHARGING_X})
    {
        EXPECT_GT(rhinoX, CROSS_LAND_X)
            << "a rhino spawning west of the teleport landing would not be on the approach";
        EXPECT_LT(rhinoX, boss->x)
            << "a rhino spawning east of Gal'darah would be behind the party, not in front";
    }

    // The loaded rhino is inside the sealed arena's approach radius, so its
    // raiders dismount INTO the window where the muster and the tank-anchored
    // clump are already armed — which is why their combat flag stalls the gates
    // rather than merely delaying the walk.
    SealedEncounterRow const* const row = SealedEncounterRegistry::Find(MAP, GALDARAH);
    ASSERT_NE(row, nullptr);
    float const rhinoToBoss = Dist3d(RHINO_LOADED_X, RHINO_LOADED_Y, boss->z,
                                     boss->x, boss->y, boss->z);
    EXPECT_LT(rhinoToBoss, row->approachRadius)
        << "the loaded rhino (" << rhinoToBoss << "yd from the boss) must fall inside "
           "approachRadius (" << row->approachRadius << "), or this encounter is an "
           "ordinary trash fight and the gates it stalls are not armed yet";

    // ...and it is far enough out that a party held there is NOT inside engage
    // range: this is exactly the band in which the tank was pinned, close enough
    // for the at-boss gates to be armed and too far for the engage to fire.
    EXPECT_GT(rhinoToBoss, DC_ENGAGE_RANGE)
        << "a party stalled at the rhino must be outside DC_ENGAGE_RANGE — that gap "
           "is the failure, and if it ever closes this test is measuring the wrong thing";

    // The loaded rhino is the one that matters: it is the raider carrier. If this
    // ever stops being three seats, the dismount count in GundrakEvents is stale.
    EXPECT_EQ(RAIDER_SEATS, 3u);
    EXPECT_NE(RHINO_LOADED, RHINO_CHARGING);
    EXPECT_NE(RAIDER, RHINO_LOADED);
}
