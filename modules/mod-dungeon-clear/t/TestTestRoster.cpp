/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "gtest/gtest.h"

#include "TestRun/DcTestRoster.h"

using DcTestRoster::Kind;
using DcTestRoster::Parse;
using DcTestRoster::Result;
using DcTestRoster::RoleForIndex;

namespace
{
    TEST(DcTestRoster, PositionalRolesAreTankHealThenDps)
    {
        EXPECT_STREQ("tank", RoleForIndex(0));
        EXPECT_STREQ("heal", RoleForIndex(1));
        EXPECT_STREQ("dps", RoleForIndex(2));
        EXPECT_STREQ("dps", RoleForIndex(3));
        EXPECT_STREQ("dps", RoleForIndex(4));
    }

    TEST(DcTestRoster, ParsesFiveNamesIntoRoles)
    {
        Result const r = Parse("Tanky,Healy,Stabby,Casty,Shooty");
        ASSERT_EQ(Kind::Ok, r.kind);
        ASSERT_EQ(5u, r.members.size());
        EXPECT_EQ("Tanky", r.members[0].name);
        EXPECT_STREQ("tank", r.members[0].role);
        EXPECT_EQ("Healy", r.members[1].name);
        EXPECT_STREQ("heal", r.members[1].role);
        EXPECT_EQ("Shooty", r.members[4].name);
        EXPECT_STREQ("dps", r.members[4].role);
    }

    TEST(DcTestRoster, TrimsWhitespaceAroundNames)
    {
        Result const r = Parse("  Tanky , Healy,Stabby ,  Casty,Shooty  ");
        ASSERT_EQ(Kind::Ok, r.kind);
        EXPECT_EQ("Tanky", r.members[0].name);
        EXPECT_EQ("Healy", r.members[1].name);
        EXPECT_EQ("Casty", r.members[3].name);
        EXPECT_EQ("Shooty", r.members[4].name);
    }

    // A stray separator is a typo in the list, not an extra (empty) member — the
    // alternative is refusing a roster of five perfectly good names.
    TEST(DcTestRoster, IgnoresEmptyFieldsFromStraySeparators)
    {
        EXPECT_EQ(Kind::Ok, Parse("Tanky,Healy,Stabby,Casty,Shooty,").kind);
        EXPECT_EQ(Kind::Ok, Parse("Tanky,,Healy,Stabby,Casty,Shooty").kind);
        EXPECT_EQ(Kind::Ok, Parse(",Tanky,Healy,Stabby,Casty,Shooty").kind);
    }

    TEST(DcTestRoster, RejectsEmptySpec)
    {
        for (char const* spec : {"", "   ", ",", ",,,"})
        {
            Result const r = Parse(spec);
            EXPECT_EQ(Kind::Empty, r.kind) << "spec: '" << spec << "'";
            EXPECT_FALSE(r.detail.empty());
            EXPECT_TRUE(r.members.empty());
        }
    }

    TEST(DcTestRoster, RejectsWrongCountAndSaysHowMany)
    {
        // Bounds are 2-40 now (raid rosters). One name is too few...
        Result const one = Parse("Tanky");
        EXPECT_EQ(Kind::WrongCount, one.kind);
        EXPECT_NE(std::string::npos, one.detail.find('1'));
        EXPECT_TRUE(one.members.empty());

        // ...41 too many...
        std::string many;
        for (int i = 0; i < 41; ++i)
            many += (i ? std::string(",N") : std::string("N")) + std::to_string(i);
        Result const over = Parse(many);
        EXPECT_EQ(Kind::WrongCount, over.kind);
        EXPECT_NE(std::string::npos, over.detail.find("41"));

        // ...and the once-rejected counts inside the bounds now parse, with
        // positional roles (tank, heal, dps...).
        Result const three = Parse("Tanky,Healy,Stabby");
        EXPECT_EQ(Kind::Ok, three.kind);
        ASSERT_EQ(three.members.size(), 3u);
        EXPECT_STREQ(three.members[0].role, "tank");
        EXPECT_STREQ(three.members[1].role, "heal");
        EXPECT_STREQ(three.members[2].role, "dps");

        Result const six = Parse("A,B,C,D,E,F");
        EXPECT_EQ(Kind::Ok, six.kind);
        EXPECT_EQ(six.members.size(), 6u);
    }

    // WoW names are unique up to case, so "Bob" and "bob" are one character being
    // asked to fill two slots — the group build would never complete.
    TEST(DcTestRoster, RejectsDuplicateNamesCaseInsensitively)
    {
        Result const r = Parse("Tanky,Healy,Stabby,tanky,Shooty");
        EXPECT_EQ(Kind::Duplicate, r.kind);
        EXPECT_NE(std::string::npos, r.detail.find("Tanky"));
        EXPECT_TRUE(r.members.empty());
    }

    TEST(DcTestRoster, DuplicateInAnyPositionIsCaught)
    {
        EXPECT_EQ(Kind::Duplicate, Parse("A,B,C,D,d").kind);
        EXPECT_EQ(Kind::Duplicate, Parse("A,A,B,C,D").kind);
        EXPECT_EQ(Kind::Ok, Parse("A,B,C,D,E").kind);
    }
}
