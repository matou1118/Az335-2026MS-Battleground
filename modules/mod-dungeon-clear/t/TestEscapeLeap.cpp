/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "gtest/gtest.h"

#include "DcEscapeLeap.h"

// tr-20260926-151320-21: a mage blinked and a hunter disengaged out of a camp fight
// into the rest of Karazhan's banquet hall.
TEST(DcEscapeLeap, BlinkAndDisengageAreBanned)
{
    EXPECT_TRUE(DcEscapeLeap::IsBanned("blink")) << "CastBlinkBackAction's getName()";
    EXPECT_TRUE(DcEscapeLeap::IsBanned("blink back")) << "its registry key";
    EXPECT_TRUE(DcEscapeLeap::IsBanned("disengage"));
}

TEST(DcEscapeLeap, OrdinaryCombatActionsAreNot)
{
    EXPECT_FALSE(DcEscapeLeap::IsBanned("frost nova"));
    EXPECT_FALSE(DcEscapeLeap::IsBanned("frostbolt"));
    EXPECT_FALSE(DcEscapeLeap::IsBanned("feign death"));
    EXPECT_FALSE(DcEscapeLeap::IsBanned("flee"));
    EXPECT_FALSE(DcEscapeLeap::IsBanned(""));
}
