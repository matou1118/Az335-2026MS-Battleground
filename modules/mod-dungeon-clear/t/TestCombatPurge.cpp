/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "gtest/gtest.h"

#include <limits>

#include "Ai/Dungeon/DungeonClear/Data/DcCombatPurgeRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/DcNeverTargetRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/DcTargetExclusionRegistry.h"
#include "Ai/Dungeon/DungeonClear/Util/DcRunProgress.h"

namespace
{
    constexpr uint32 MAP_GUNDRAK = 604;
    constexpr uint32 NPC_DRAKKARI_RAIDER = 29982;
    constexpr uint32 NPC_DRAKKARI_RAIDER_DUP = 30934;  // duplicate template, never spawned
    constexpr uint32 NPC_SLADRAN_VIPER = 29680;
    constexpr uint32 NPC_SLADRAN_CONSTRICTOR = 29713;

    constexpr bool EVADING = true;
    constexpr bool FIGHTING = false;
}

// Gundrak's Drakkari Raider is the row this registry exists for. It has no
// `creature` row anywhere in the world DB — every one is a passenger seated on
// the Drakkari Rhino (29931) by vehicle_template_accessory — and the rhino's
// SmartAI ejects all three at point 3 of waypoint path 1272070, at the bottom of
// a sixteen-yard drop into water, then has each of them SET ITS OWN HOME POSITION
// where it lands. Alive, hostile, holding the party's combat reference, off the
// navmesh, and with nowhere to evade back to: nothing in the core ever ends that
// fight.
TEST(DcCombatPurgeRegistryTest, GundrakDrakkariRaiderMayBePurged)
{
    EXPECT_TRUE(DcCombatPurgeRegistry::IsPurgeable(MAP_GUNDRAK, NPC_DRAKKARI_RAIDER, EVADING));
    EXPECT_TRUE(DcCombatPurgeRegistry::HasRowsFor(MAP_GUNDRAK));
}

// ...and it is an Always row, so it does not wait for an evade it can never
// reach. The raider re-homed in the water it was ejected into: evade has nowhere
// to walk it back to, so gating this row on evade would switch it off entirely.
TEST(DcCombatPurgeRegistryTest, TheRaiderRowDoesNotWaitForAnEvade)
{
    EXPECT_TRUE(DcCombatPurgeRegistry::IsPurgeable(MAP_GUNDRAK, NPC_DRAKKARI_RAIDER, FIGHTING));
}

// Slad'ran's snakes are the opposite kind of row. They are summoned by the boss
// with TEMPSUMMON_CORPSE_TIMED_DESPAWN — a timer that starts at the corpse, so an
// unkilled snake is never scheduled to despawn — and BossAI::_EnterEvadeMode does
// not call summons.DespawnAll(), so a wipe leaves them alive at full health
// holding the survivors' combat references forever.
TEST(DcCombatPurgeRegistryTest, SladranSummonsMayBePurgedOnceTheyHaveEvaded)
{
    EXPECT_TRUE(DcCombatPurgeRegistry::IsPurgeable(MAP_GUNDRAK, NPC_SLADRAN_VIPER, EVADING));
    EXPECT_TRUE(DcCombatPurgeRegistry::IsPurgeable(MAP_GUNDRAK, NPC_SLADRAN_CONSTRICTOR, EVADING));
}

// The guard that keeps this table from breaking the fights it exists to unstick.
// These snakes are a REAL mechanic in a real encounter — most runs kill Slad'ran
// with them up — and the purge clock is combat-blind by design (DcRunProgress),
// so a boss fight longer than UnreachableCombatPurgeSecs is indistinguishable
// from a freeze. Without this, the purge would despawn the adds mid-encounter in
// the runs that were winning.
TEST(DcCombatPurgeRegistryTest, SladranSummonsAreNeverPurgedMidFight)
{
    EXPECT_FALSE(DcCombatPurgeRegistry::IsPurgeable(MAP_GUNDRAK, NPC_SLADRAN_VIPER, FIGHTING));
    EXPECT_FALSE(DcCombatPurgeRegistry::IsPurgeable(MAP_GUNDRAK, NPC_SLADRAN_CONSTRICTOR, FIGHTING));
}

TEST(DcCombatPurgeRegistryTest, TheRowIsScopedToItsOwnMapAndEntry)
{
    // Same entry on another map, and another entry on the same map, must both
    // pass. A row is a claim about one creature's mechanics on one map.
    EXPECT_FALSE(DcCombatPurgeRegistry::IsPurgeable(603, NPC_DRAKKARI_RAIDER, EVADING));
    EXPECT_FALSE(DcCombatPurgeRegistry::IsPurgeable(603, NPC_SLADRAN_VIPER, EVADING));
    EXPECT_FALSE(DcCombatPurgeRegistry::IsPurgeable(MAP_GUNDRAK, 29819, EVADING));  // Drakkari Lancer
    EXPECT_FALSE(DcCombatPurgeRegistry::HasRowsFor(603));
}

// 30934 is "Drakkari Raider (1)": the same display name, but no vehicle accessory
// row, no spawn row, and nothing on map 604 referencing it. Rows here are claims
// about a MECHANISM, never about a name — listing it by name-match would purge
// combat with a creature nobody has shown can strand a party.
TEST(DcCombatPurgeRegistryTest, TheDuplicateRaiderTemplateIsNotListed)
{
    EXPECT_FALSE(DcCombatPurgeRegistry::IsPurgeable(MAP_GUNDRAK, NPC_DRAKKARI_RAIDER_DUP, EVADING));
}

// A purge that does not stick is a revolving door: the mob is still standing
// there and still a legal target, so the pickers re-select it the next tick and
// re-open the same unendable fight. DcCombatPurge answers that by arming a
// windowed bar, which only reaches stock target selection through
// DcTargetExclusionRegistry — so every map with a purge row needs an exclusion
// row too.
TEST(DcCombatPurgeRegistryTest, EveryPurgeMapAlsoCarriesATargetExclusionRow)
{
    EXPECT_TRUE(DcTargetExclusionRegistry::HasRowsFor(MAP_GUNDRAK));
}

// The three "do not fight that" tables answer different questions and must not be
// confused. A purgeable raider is a perfectly good target when it lands on solid
// ground — killing it IS progress — so it has no business in the never-target
// table, whose rows are permanent.
TEST(DcCombatPurgeRegistryTest, APurgeableMobIsNotAPermanentNeverTarget)
{
    EXPECT_FALSE(DcNeverTargetRegistry::IsNeverTarget(MAP_GUNDRAK, NPC_DRAKKARI_RAIDER));
    EXPECT_FALSE(DcNeverTargetRegistry::IsNeverTarget(MAP_GUNDRAK, NPC_SLADRAN_VIPER));
    EXPECT_FALSE(DcNeverTargetRegistry::IsNeverTarget(MAP_GUNDRAK, NPC_SLADRAN_CONSTRICTOR));
}

// --- the shared no-progress clock ------------------------------------------

TEST(DcRunProgressTest, StampNeverWritesTheUnarmedSentinel)
{
    // 0 means "unarmed", so a stamp that lands exactly on a getMSTime() of 0 must
    // not silently disarm the clock it was meant to re-arm.
    DcRunProgress::Mark mark;
    DcRunProgress::Stamp(mark, 0);
    EXPECT_EQ(mark.stampMs, 1u);

    DcRunProgress::Stamp(mark, 5000);
    EXPECT_EQ(mark.stampMs, 5000u);
}

TEST(DcRunProgressTest, AnUnarmedClockIsNeverStale)
{
    DcRunProgress::Mark mark;  // stampMs == 0
    EXPECT_FALSE(DcRunProgress::Stale(mark, 10'000'000, 60000));
}

TEST(DcRunProgressTest, AZeroWindowDisablesTheClock)
{
    // UnreachableCombatPurgeSecs = 0 is the operator switching the failsafe off.
    DcRunProgress::Mark mark;
    DcRunProgress::Stamp(mark, 1000);
    EXPECT_FALSE(DcRunProgress::Stale(mark, 1000 + 10'000'000, 0));
}

TEST(DcRunProgressTest, StaleOnlyOnceTheFullWindowHasElapsed)
{
    DcRunProgress::Mark mark;
    DcRunProgress::Stamp(mark, 1000);

    EXPECT_FALSE(DcRunProgress::Stale(mark, 1000, 60000));
    EXPECT_FALSE(DcRunProgress::Stale(mark, 1000 + 59999, 60000));
    EXPECT_TRUE(DcRunProgress::Stale(mark, 1000 + 60000, 60000));
    EXPECT_TRUE(DcRunProgress::Stale(mark, 1000 + 60001, 60000));
}

// getMSTime() is a 32-bit millisecond counter that wraps every ~49.7 days. A run
// straddling the wrap must not see its clock read as "not yet stale" forever
// (signed/promoted arithmetic would); unsigned subtraction on the same monotonic
// clock stays correct across it.
TEST(DcRunProgressTest, TheClockSurvivesTheThirtyTwoBitRollover)
{
    constexpr uint32 kMax = std::numeric_limits<uint32>::max();

    DcRunProgress::Mark mark;
    DcRunProgress::Stamp(mark, kMax - 1000);  // 1s before the wrap

    // 500ms later, still before the wrap: not stale on a 60s window.
    EXPECT_FALSE(DcRunProgress::Stale(mark, kMax - 500, 60000));
    // 2s later, now 1s PAST the wrap: still not stale, and crucially not stale
    // for the wrong reason.
    EXPECT_FALSE(DcRunProgress::Stale(mark, 1000, 60000));
    // 60s after the stamp, well past the wrap: stale.
    EXPECT_TRUE(DcRunProgress::Stale(mark, 59000, 60000));
}
