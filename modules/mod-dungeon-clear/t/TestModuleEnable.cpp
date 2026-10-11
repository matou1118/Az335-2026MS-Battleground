/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "gtest/gtest.h"

#include "DcModuleEnable.h"

// The master switch's one load-bearing property is that it LATCHES: the answer
// to "is mod-dungeon-clear live in this process?" is resolved once and never
// moves again. Registration into mod-playerbots' shared contexts is a one-way
// append that already-built bots hold by reference, so a value that could change
// mid-process would produce a half-state — contexts registered but strategies
// refused, or the reverse. That is what these pin.
//
// The conf read itself (LatchFromConf -> DcSettings -> sConfigMgr) needs a live
// config manager, so it is exercised at runtime; LatchValue is the same latch
// with the conf read spliced out.

namespace
{
    struct DcModuleEnableTest : public ::testing::Test
    {
        void SetUp() override { DcModule::ResetLatchForTests(); }
        void TearDown() override { DcModule::ResetLatchForTests(); }
    };
}

TEST_F(DcModuleEnableTest, LatchedEnabledIsReportedEnabled)
{
    DcModule::LatchValue(true);
    EXPECT_TRUE(DcModule::IsEnabled());
}

TEST_F(DcModuleEnableTest, LatchedDisabledIsReportedDisabled)
{
    DcModule::LatchValue(false);
    EXPECT_FALSE(DcModule::IsEnabled());
}

TEST_F(DcModuleEnableTest, FirstLatchWins)
{
    // A later conf reload (or any stray second call) must not move the answer
    // out from under bots whose engines were built against the first one.
    DcModule::LatchValue(false);
    DcModule::LatchValue(true);
    EXPECT_FALSE(DcModule::IsEnabled());

    DcModule::ResetLatchForTests();

    DcModule::LatchValue(true);
    DcModule::LatchValue(false);
    EXPECT_TRUE(DcModule::IsEnabled());
}

TEST_F(DcModuleEnableTest, RepeatedReadsAreStable)
{
    DcModule::LatchValue(false);
    for (int i = 0; i < 5; ++i)
        EXPECT_FALSE(DcModule::IsEnabled());
}
