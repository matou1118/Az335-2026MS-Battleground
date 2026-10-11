/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

// Karazhan chess (map 532), the pure kernels: the board model (DcChessBoard.h)
// and the policy (DcChessDecision.h). The offline simulator that plays the policy
// over hundreds of games is t/TestKarazhanChessSim.cpp.

#include "gtest/gtest.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <vector>

#include "Ai/Dungeon/DungeonClear/Data/DcTargetExclusionRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"
#include "Ai/Dungeon/DungeonClear/Overrides/ObjectiveHookRegistry.h"
#include "Ai/Dungeon/DungeonClear/Strategy/DcRelevance.h"
#include "Ai/Dungeon/DungeonClear/Util/DcBossStandDown.h"
#include "Ai/Dungeon/DungeonClear/Util/DcChessBoard.h"
#include "Ai/Dungeon/DungeonClear/Util/DcChessConductor.h"
#include "Ai/Dungeon/DungeonClear/Util/DcChessDecision.h"

using namespace DcChess;

namespace
{
    // A piece described in the NORMALISED frame (our home rows 0-1, forward +row).
    struct P
    {
        Kind kind;
        bool ours;
        int r, c;
        Ori facing;
        uint32 hp = 0;         // 0 = full
        bool controlled = false;
        bool buffed = false;
    };

    Side Other(Side s) { return s == Side::Alliance ? Side::Horde : Side::Alliance; }

    // Build a board for a raid playing `ourSide` from normalised descriptions: the
    // pieces are placed at their REAL cells (a Horde raid's rows flipped) and the
    // board normalises them back, so every test also exercises the builder.
    Board Build(Side ourSide, std::vector<P> const& ps, std::vector<std::pair<int, int>> const& fire = {})
    {
        Board b;
        b.Clear(ourSide == Side::Horde);
        uint64 guid = 1;
        for (P const& d : ps)
        {
            Piece p;
            p.guid = guid++;
            p.entry = EntryOf(d.kind, d.ours ? ourSide : Other(ourSide));
            p.kind = d.kind;
            p.ours = d.ours;
            p.maxHp = InfoOf(p.entry)->maxHp;
            p.hp = d.hp ? d.hp : p.maxHp;
            p.controlled = d.controlled;
            p.buffed = d.buffed;
            int const realRow = b.RealRow(d.r);
            Ori const realFacing = b.RealOri(d.facing);
            EXPECT_TRUE(b.Add(p, realRow, d.c, realFacing));
        }
        for (auto const& [r, c] : fire)
            b.SetBurningReal(b.RealRow(r), c);
        return b;
    }

    Order const* OrderFor(Decision const& d, uint64 guid, OrderKind kind)
    {
        for (Order const& o : d.orders)
            if (o.piece == guid && o.kind == kind)
                return &o;
        return nullptr;
    }

    // The 32 DB spawns (acore_world creature, map 532), entry and position.
    struct Spawn { uint32 entry; float x, y, o; };
    Spawn const kSpawns[] = {
        { 17211, -11103.7f, -1869.47f, 0.6981f }, { 17211, -11100.2f, -1873.87f, 0.6458f },
        { 17211, -11096.7f, -1878.41f, 0.6632f }, { 17211, -11093.3f, -1882.73f, 0.6632f },
        { 17211, -11089.7f, -1887.11f, 0.7156f }, { 17211, -11086.3f, -1891.32f, 0.6458f },
        { 17211, -11082.7f, -1895.74f, 0.6632f }, { 17211, -11079.3f, -1900.23f, 0.6632f },
        { 17469, -11082.0f, -1852.19f, 3.8048f }, { 17469, -11078.5f, -1856.99f, 3.8921f },
        { 17469, -11075.0f, -1861.29f, 3.735f },  { 17469, -11071.7f, -1865.53f, 3.8223f },
        { 17469, -11068.1f, -1869.96f, 3.8921f }, { 17469, -11064.7f, -1874.34f, 3.8397f },
        { 17469, -11061.3f, -1878.63f, 3.9095f }, { 17469, -11057.6f, -1883.09f, 3.8048f },
        { 21160, -11107.9f, -1873.03f, 0.7156f }, { 21160, -11083.2f, -1903.35f, 0.6632f },
        { 21664, -11104.6f, -1877.51f, 0.6807f }, { 21664, -11086.9f, -1899.18f, 0.733f },
        { 21682, -11101.0f, -1881.93f, 0.6632f }, { 21682, -11090.6f, -1894.99f, 0.6283f },
        { 21683, -11097.5f, -1886.2f, 0.7854f },  { 21684, -11093.8f, -1890.47f, 0.6981f },
        { 21726, -11077.7f, -1848.79f, 3.9095f }, { 21726, -11053.5f, -1879.72f, 4.0143f },
        { 21747, -11070.9f, -1857.75f, 3.8397f }, { 21747, -11060.3f, -1870.9f, 3.8048f },
        { 21748, -11074.3f, -1853.26f, 3.735f },  { 21748, -11056.9f, -1875.29f, 3.6652f },
        { 21750, -11067.6f, -1861.94f, 3.8397f }, { 21752, -11063.6f, -1866.36f, 3.8223f },
    };
}

// --- geometry ------------------------------------------------------------------

TEST(DcKarazhanChessTest, EveryCellRoundTrips)
{
    for (int r = 0; r < N; ++r)
        for (int c = 0; c < N; ++c)
        {
            float x, y;
            CenterOf(static_cast<float>(r), static_cast<float>(c), x, y);
            int8 rr = -1, cc = -1;
            ASSERT_TRUE(CellOf(x, y, rr, cc)) << r << "," << c;
            EXPECT_EQ(rr, r);
            EXPECT_EQ(cc, c);
            float fr, fc;
            BoardCoords(x, y, fr, fc);
            EXPECT_NEAR(fr, r, 1e-3f);
            EXPECT_NEAR(fc, c, 1e-3f);
            // Half a cell off centre is ambiguous and must not claim a cell.
            float hx, hy;
            CenterOf(r + 0.5f, static_cast<float>(c), hx, hy);
            EXPECT_FALSE(CellOf(hx, hy, rr, cc)) << "half-cell point claimed " << r << "," << c;
        }
}

TEST(DcKarazhanChessTest, TheDbSpawnsSnapToTheirHomeCells)
{
    std::set<int> cells;
    for (Spawn const& s : kSpawns)
    {
        int8 r = -1, c = -1;
        ASSERT_TRUE(CellOf(s.x, s.y, r, c, 0.8f)) << s.entry << " at " << s.x << "," << s.y;
        EXPECT_TRUE(cells.insert(r * N + c).second) << "two spawns on one cell";
        PieceInfo const* info = InfoOf(s.entry);
        ASSERT_NE(info, nullptr);
        bool const alliance = info->side == Side::Alliance;
        if (info->kind == Kind::Pawn)
            EXPECT_EQ(r, alliance ? 1 : 6) << s.entry;
        else
            EXPECT_EQ(r, alliance ? 0 : 7) << s.entry;
        switch (info->kind)
        {
            case Kind::Rook:   EXPECT_TRUE(c == 0 || c == 7) << c; break;
            case Kind::Knight: EXPECT_TRUE(c == 1 || c == 6) << c; break;
            case Kind::Bishop: EXPECT_TRUE(c == 2 || c == 5) << c; break;
            case Kind::Queen:  EXPECT_EQ(c, 3); break;
            case Kind::King:   EXPECT_EQ(c, 4); break;
            default: break;
        }
        // Alliance faces +row (NW), Horde -row (SE).
        EXPECT_EQ(SnapFacing(s.o), alliance ? ORI_NW : ORI_SE) << s.entry;
    }
    EXPECT_EQ(cells.size(), 32u);
}

TEST(DcKarazhanChessTest, LandmarksSitOffTheColZeroEdge)
{
    float r, c;
    BoardCoords(DcKarazhan::ECHO_X, DcKarazhan::ECHO_Y, r, c);
    EXPECT_NEAR(r, 3.45f, 0.05f);
    EXPECT_NEAR(c, -1.69f, 0.05f);
    BoardCoords(DcKarazhan::WAITING_X, DcKarazhan::WAITING_Y, r, c);
    EXPECT_NEAR(r, 3.45f, 0.05f);
    EXPECT_NEAR(c, -4.0f, 0.05f);
    // The hall anchor (where the raid arrives, waits and stands) is off the
    // board but only one cell off it — the open floor, not the rim by the wall —
    // and clear of Echo and the chest.
    BoardCoords(DcKarazhan::HALL_X, DcKarazhan::HALL_Y, r, c);
    EXPECT_LT(c, -0.5f) << "off the board";
    EXPECT_GT(c, -1.5f) << "the rim beyond col -1.5 is against the walls";
    EXPECT_GT(std::hypot(DcKarazhan::HALL_X - DcKarazhan::ECHO_X, DcKarazhan::HALL_Y - DcKarazhan::ECHO_Y), 3.0f);
    EXPECT_GT(std::hypot(DcKarazhan::HALL_X - DcKarazhan::CHEST_X, DcKarazhan::HALL_Y - DcKarazhan::CHEST_Y), 3.0f);
}

// --- facing ----------------------------------------------------------------------

TEST(DcKarazhanChessTest, TheOrientationTableMatchesTheBoardsGeometry)
{
    for (uint8 o = 0; o < ORI_COUNT; ++o)
    {
        Step const s = ORI_STEP[o];
        float const vx = COL_DX * s.dc + ROW_DX * s.dr;
        float const vy = COL_DY * s.dc + ROW_DY * s.dr;
        float yaw = std::atan2(vy, vx);
        if (yaw < 0)
            yaw += 6.2831853f;
        EXPECT_NEAR(yaw, ORI_YAW[o], 0.05f) << "ori " << int(o);
        EXPECT_EQ(SnapFacing(ORI_YAW[o]), static_cast<Ori>(o));
        EXPECT_EQ(OriOfStep(s.dr, s.dc), static_cast<Ori>(o));
        // A knight's or a queen's long step faces by the signs, like the core.
        EXPECT_EQ(OriOfStep(s.dr * 2, s.dc * 3), static_cast<Ori>(o));
        EXPECT_EQ(MirrorOri(MirrorOri(static_cast<Ori>(o))), static_cast<Ori>(o));
    }
    EXPECT_EQ(MirrorOri(ORI_NW), ORI_SE);
    EXPECT_EQ(MirrorOri(ORI_N), ORI_E);
    EXPECT_EQ(MirrorOri(ORI_NE), ORI_NE);
    EXPECT_EQ(OriOfStep(0, 0), ORI_COUNT);
}

TEST(DcKarazhanChessTest, FrontCellIsTheOneNeighbourTheFacingPointsAt)
{
    Board const b = Build(Side::Alliance, {
        { Kind::Pawn, true, 3, 3, ORI_N },
        { Kind::Pawn, false, 4, 4, ORI_SE },  // on our front cell, facing (3,4)
        { Kind::Pawn, false, 3, 4, ORI_SE },
    });
    int r, c;
    FrontCell(b.pieces[0], r, c);
    EXPECT_EQ(r, 4);
    EXPECT_EQ(c, 4);
    EXPECT_EQ(EnemyInFront(b, b.pieces[0]), 1);
    // The piece at (4,4) faces (3,4), not us; the one at (3,4) faces (2,4).
    EXPECT_EQ(EnemiesFacing(b, 3, 3, true), 0);
    EXPECT_EQ(EnemiesFacing(b, 3, 4, true), 1);
    EXPECT_EQ(AdjacentEnemies(b, 3, 3, true), 2);
}

// --- moves ---------------------------------------------------------------------

TEST(DcKarazhanChessTest, EveryLegalStepFitsTheMoveSpellsRange)
{
    for (PieceInfo const& info : PieceTable())
    {
        float const widest = CellDist(info.reach, info.reach);
        // The widest step each piece is ALLOWED fits the destination range check
        // (spell range + the caster's own combat reach).
        float allowed = 0.0f;
        for (int dr = 0; dr <= info.reach; ++dr)
            for (int dc = 0; dc <= info.reach; ++dc)
                if (CellDist(dr, dc) <= MoveSpellRange(info.moveSpell) + info.combatReach - MOVE_RANGE_MARGIN)
                    allowed = std::max(allowed, CellDist(dr, dc));
        EXPECT_LE(allowed, MoveSpellRange(info.moveSpell) + info.combatReach) << info.entry;
        (void)widest;
    }
}

TEST(DcKarazhanChessTest, LegalMovesFollowTheChebyshevReach)
{
    std::vector<std::pair<int8, int8>> moves;
    for (auto const& [kind, count] : std::vector<std::pair<Kind, size_t>>{
             { Kind::Pawn, 8 }, { Kind::King, 8 }, { Kind::Knight, 24 }, { Kind::Queen, 44 } })
    {
        Board const b = Build(Side::Alliance, { { kind, true, 3, 3, ORI_NW } });
        LegalMoves(b, b.pieces[0], moves);
        EXPECT_EQ(moves.size(), count) << int(kind);
    }
    // A corner and a blocked neighbour.
    Board const b = Build(Side::Alliance, {
        { Kind::Rook, true, 0, 0, ORI_NW },
        { Kind::Pawn, true, 1, 0, ORI_NW },
    });
    LegalMoves(b, b.pieces[0], moves);
    EXPECT_EQ(moves.size(), 2u);  // (0,1) and (1,1)
    EXPECT_FALSE(CanStep(b, b.pieces[0], 1, 0));
    EXPECT_FALSE(CanStep(b, b.pieces[0], 0, 0));

    // The queen's (3,3) is inside HandlePieceMove's reach but 23.8yd from the
    // trigger, past 20 + her 3.75 reach: the move spell would never land.
    Board const q = Build(Side::Alliance, { { Kind::Queen, true, 0, 0, ORI_NW } });
    EXPECT_FALSE(CanStep(q, q.pieces[0], 3, 3));
    EXPECT_TRUE(CanStep(q, q.pieces[0], 3, 2));
    Board const k = Build(Side::Alliance, { { Kind::Knight, true, 0, 0, ORI_NW } });
    EXPECT_TRUE(CanStep(k, k.pieces[0], 2, 2));
}

// --- the ladder -----------------------------------------------------------------

TEST(DcKarazhanChessTest, FireOutranksEverything)
{
    // A controlled knight on fire, with an enemy beside it it is not facing.
    Board const b = Build(Side::Alliance, {
        { Kind::Knight, true, 3, 3, ORI_NW, 0, true },
        { Kind::Pawn, false, 3, 4, ORI_SE },
    }, { { 3, 3 } });
    Decision const d = Decide(b, Ledger{}, Clock{ 1000, 0, 0 });
    Order const* move = OrderFor(d, 1, OrderKind::Move);
    ASSERT_NE(move, nullptr);
    EXPECT_EQ(move->why, Why::Fire);
    EXPECT_FALSE(b.Burning(move->r, move->c));
    EXPECT_LE(move->r, 3) << "off the fire backward or sideways, not into them";
    EXPECT_EQ(OrderFor(d, 1, OrderKind::Face), nullptr);
}

TEST(DcKarazhanChessTest, TurnToANeighbourWeAreNotFacing)
{
    Board const b = Build(Side::Alliance, {
        { Kind::Pawn, true, 2, 3, ORI_NW, 0, true },
        { Kind::Pawn, false, 2, 4, ORI_SE },   // beside us, facing (1,4)
    });
    Decision const d = Decide(b, Ledger{}, Clock{ 1000, 0, 0 });
    Order const* face = OrderFor(d, 1, OrderKind::Face);
    ASSERT_NE(face, nullptr);
    EXPECT_EQ(face->r, 2);
    EXPECT_EQ(face->c, 4);
    EXPECT_EQ(face->target, 2u);
}

TEST(DcKarazhanChessTest, AlreadyHittingSomethingSavesTheAction)
{
    Board const b = Build(Side::Alliance, {
        { Kind::Pawn, true, 2, 3, ORI_NW, 0, true },
        { Kind::Pawn, false, 3, 3, ORI_SE },   // in front
        { Kind::Pawn, false, 2, 4, ORI_SE },   // beside
    });
    Decision const d = Decide(b, Ledger{}, Clock{ 1000, 0, 0 });
    EXPECT_EQ(OrderFor(d, 1, OrderKind::Face), nullptr);
    EXPECT_EQ(OrderFor(d, 1, OrderKind::Move), nullptr);
    // ...and the ability still fires on its own cooldown.
    Order const* cast = OrderFor(d, 1, OrderKind::Cast);
    ASSERT_NE(cast, nullptr);
    EXPECT_EQ(cast->spell, InfoOf(NPC_PAWN_A)->dmg.spell);
}

TEST(DcKarazhanChessTest, HoldFlanksAnEnemyTwoCellsAway)
{
    // Plenty of enemy material, so the posture is HOLD.
    std::vector<P> ps = {
        { Kind::Rook, true, 2, 3, ORI_NW, 0, true },
        { Kind::King, true, 0, 7, ORI_NW },
        { Kind::Pawn, false, 4, 4, ORI_SE },  // faces (3,4)
    };
    for (int c = 0; c < 8; ++c)
        ps.push_back({ Kind::Pawn, false, 7, c, ORI_SE });
    ps.push_back({ Kind::King, false, 6, 0, ORI_SE });
    Board const b = Build(Side::Alliance, ps);
    Decision const d = Decide(b, Ledger{}, Clock{ 1000, 0, 0 });
    EXPECT_EQ(d.posture, Posture::Hold);
    Order const* move = OrderFor(d, 1, OrderKind::Move);
    ASSERT_NE(move, nullptr);
    EXPECT_EQ(move->why, Why::Flank);
    EXPECT_EQ(Chebyshev(move->r - 4, move->c - 4), 1) << "beside the enemy";
    EXPECT_FALSE(move->r == 3 && move->c == 4) << "never onto its front cell";
    EXPECT_LE(move->r, 4);
}

// HOLD with nothing near: the pawns march and open the lanes, and the pieces
// behind them step up into the row the pawns left — never past it.
TEST(DcKarazhanChessTest, HoldAdvancesThePawnsAndTheBackRankFollows)
{
    std::vector<P> ps = {
        { Kind::Pawn, true, 1, 3, ORI_NW, 0, true },   // 1: marches
        { Kind::Bishop, true, 0, 2, ORI_NW, 0, true }, // 2: steps up into row 1
        { Kind::Pawn, true, 3, 5, ORI_NW, 0, true },   // 3: already at the pawn cap
        { Kind::Knight, true, 1, 6, ORI_NW, 0, true }, // 4: already at the follow cap
        { Kind::King, true, 0, 4, ORI_NW, 0, true },   // 5: the King never marches
    };
    for (int c = 0; c < 8; ++c)
        ps.push_back({ Kind::Pawn, false, 7, c, ORI_SE });
    ps.push_back({ Kind::King, false, 6, 0, ORI_SE });
    for (Side side : { Side::Alliance, Side::Horde })
    {
        Board const b = Build(side, ps);
        Decision const d = Decide(b, Ledger{}, Clock{ 1000, 0, 0 });
        EXPECT_EQ(d.posture, Posture::Hold);

        Order const* pawn = OrderFor(d, 1, OrderKind::Move);
        ASSERT_NE(pawn, nullptr);
        EXPECT_EQ(pawn->why, Why::Advance);
        EXPECT_EQ(pawn->r, 2);
        EXPECT_EQ(pawn->c, 3) << "straight ahead before a diagonal";

        Order const* bishop = OrderFor(d, 2, OrderKind::Move);
        ASSERT_NE(bishop, nullptr);
        EXPECT_EQ(bishop->why, Why::Advance);
        EXPECT_EQ(bishop->r, 1);

        EXPECT_EQ(OrderFor(d, 3, OrderKind::Move), nullptr);
        EXPECT_EQ(OrderFor(d, 4, OrderKind::Move), nullptr);
        EXPECT_EQ(OrderFor(d, 5, OrderKind::Move), nullptr);
    }

    // Off by tunable: the old anvil.
    Tunables anvil;
    anvil.advancePawnRow = -1;
    Decision const d = Decide(Build(Side::Alliance, ps), Ledger{}, Clock{ 1000, 0, 0 }, anvil);
    EXPECT_EQ(OrderFor(d, 1, OrderKind::Move), nullptr);
    EXPECT_EQ(OrderFor(d, 2, OrderKind::Move), nullptr);
}

// A marching pawn never steps onto fire; blocked straight ahead it takes the
// diagonal.
TEST(DcKarazhanChessTest, AdvanceNeverStepsOntoFire)
{
    std::vector<P> ps = {
        { Kind::Pawn, true, 1, 3, ORI_NW, 0, true },
        { Kind::King, true, 0, 4, ORI_NW },
    };
    for (int c = 0; c < 8; ++c)
        ps.push_back({ Kind::Pawn, false, 7, c, ORI_SE });
    ps.push_back({ Kind::King, false, 6, 0, ORI_SE });
    Board const b = Build(Side::Alliance, ps, { { 2, 3 } });
    Decision const d = Decide(b, Ledger{}, Clock{ 1000, 0, 0 });
    Order const* move = OrderFor(d, 1, OrderKind::Move);
    ASSERT_NE(move, nullptr);
    EXPECT_EQ(move->why, Why::Advance);
    EXPECT_EQ(move->r, 2);
    EXPECT_NE(move->c, 3);
}

TEST(DcKarazhanChessTest, TheGuardRingDoesNotFlank)
{
    std::vector<P> ps = {
        { Kind::Rook, true, 1, 3, ORI_NW, 0, true },
        { Kind::King, true, 0, 3, ORI_NW },
        { Kind::Pawn, false, 3, 4, ORI_SE },
    };
    for (int c = 0; c < 8; ++c)
        ps.push_back({ Kind::Pawn, false, 7, c, ORI_SE });
    Board const b = Build(Side::Alliance, ps);
    Decision const d = Decide(b, Ledger{}, Clock{ 1000, 0, 0 });
    EXPECT_EQ(OrderFor(d, 1, OrderKind::Move), nullptr);
}

TEST(DcKarazhanChessTest, AssaultClosesOnTheirKing)
{
    // Six or fewer non-King pieces left: ASSAULT.
    Board const b = Build(Side::Alliance, {
        { Kind::Knight, true, 1, 1, ORI_NW, 0, true },
        { Kind::King, true, 0, 4, ORI_NW },
        { Kind::King, false, 6, 5, ORI_SE },  // faces (5,5)
        { Kind::Pawn, false, 6, 0, ORI_SE },
    });
    Decision const d = Decide(b, Ledger{}, Clock{ 1000, 0, 0 });
    EXPECT_EQ(d.posture, Posture::Assault);
    Order const* move = OrderFor(d, 1, OrderKind::Move);
    ASSERT_NE(move, nullptr);
    EXPECT_EQ(move->why, Why::Assault);
    EXPECT_LT(Chebyshev(move->r - 6, move->c - 5), Chebyshev(1 - 6, 1 - 5));
}

TEST(DcKarazhanChessTest, AssaultNeverTakesTheKingsFrontCellWhenASideIsFree)
{
    Board const b = Build(Side::Alliance, {
        { Kind::Rook, true, 4, 5, ORI_NW, 0, true },
        { Kind::King, true, 0, 4, ORI_NW },
        { Kind::King, false, 6, 5, ORI_SE },  // faces (5,5)
    });
    Decision const d = Decide(b, Ledger{}, Clock{ 1000, 0, 0 });
    Order const* move = OrderFor(d, 1, OrderKind::Move);
    ASSERT_NE(move, nullptr);
    EXPECT_EQ(Chebyshev(move->r - 6, move->c - 5), 1);
    EXPECT_FALSE(move->r == 5 && move->c == 5);
}

TEST(DcKarazhanChessTest, TheQueenOnlyNeedsBlastRange)
{
    Board const b = Build(Side::Alliance, {
        { Kind::Queen, true, 2, 3, ORI_NW, 0, true },
        { Kind::King, true, 0, 4, ORI_NW },
        { Kind::King, false, 6, 3, ORI_SE },  // 22.4yd: within 20 + reaches
    });
    Decision const d = Decide(b, Ledger{}, Clock{ 1000, 0, 0 });
    EXPECT_EQ(OrderFor(d, 1, OrderKind::Move), nullptr) << "already in range";
    Order const* cast = OrderFor(d, 1, OrderKind::Cast);
    ASSERT_NE(cast, nullptr);
    EXPECT_EQ(cast->spell, InfoOf(NPC_QUEEN_A)->dmg.spell);
    EXPECT_EQ(cast->target, 3u) << "his King, in ASSAULT";
}

TEST(DcKarazhanChessTest, AMobbedKingStepsBack)
{
    std::vector<P> ps = {
        { Kind::King, true, 2, 4, ORI_NW, 0, true },
        { Kind::Pawn, false, 3, 3, ORI_S },   // faces (2,2)
        { Kind::Pawn, false, 3, 5, ORI_E },   // faces (2,6)
        { Kind::Pawn, false, 2, 5, ORI_SW },  // faces the King
    };
    for (int c = 0; c < 8; ++c)
        ps.push_back({ Kind::Pawn, false, 7, c, ORI_SE });
    Board const b = Build(Side::Alliance, ps);
    Decision const d = Decide(b, Ledger{}, Clock{ 1000, 0, 0 });
    Order const* move = OrderFor(d, 1, OrderKind::Move);
    ASSERT_NE(move, nullptr);
    EXPECT_EQ(move->why, Why::KingSafety);
    EXPECT_LE(move->r, 2);
    EXPECT_LT(Threat(b, move->r, move->c), Threat(b, 2, 4));
}

// --- abilities ------------------------------------------------------------------

TEST(DcKarazhanChessTest, BishopsHealTheKingFirst)
{
    Board const b = Build(Side::Alliance, {
        { Kind::Bishop, true, 1, 3, ORI_NW, 0, true },
        { Kind::King, true, 0, 4, ORI_NW, 150000 - 20000 },
        { Kind::Knight, true, 1, 1, ORI_NW, 65000 - 40000 },
    });
    Decision const d = Decide(b, Ledger{}, Clock{ 1000, 0, 0 });
    Order const* cast = OrderFor(d, 1, OrderKind::Cast);
    ASSERT_NE(cast, nullptr);
    EXPECT_EQ(cast->spell, InfoOf(NPC_BISHOP_A)->util.spell);
    EXPECT_EQ(cast->target, 2u);
}

TEST(DcKarazhanChessTest, BishopsHealTheBiggestDeficitWhenTheKingIsHealthy)
{
    Board const b = Build(Side::Alliance, {
        { Kind::Bishop, true, 1, 3, ORI_NW, 0, true },
        { Kind::King, true, 0, 4, ORI_NW, 150000 - 5000 },
        { Kind::Knight, true, 1, 1, ORI_NW, 65000 - 40000 },
        { Kind::Rook, true, 0, 0, ORI_NW, 80000 - 13000 },
    });
    Decision const d = Decide(b, Ledger{}, Clock{ 1000, 0, 0 });
    Order const* cast = OrderFor(d, 1, OrderKind::Cast);
    ASSERT_NE(cast, nullptr);
    EXPECT_EQ(cast->target, 3u);
}

TEST(DcKarazhanChessTest, NoHealBelowTheDeficitThreshold)
{
    Board const b = Build(Side::Alliance, {
        { Kind::Bishop, true, 1, 3, ORI_NW, 0, true },
        { Kind::King, true, 0, 4, ORI_NW, 150000 - 5000 },
    });
    Decision const d = Decide(b, Ledger{}, Clock{ 1000, 0, 0 });
    EXPECT_EQ(OrderFor(d, 1, OrderKind::Cast), nullptr) << "nothing to heal and nothing on the ray";
}

TEST(DcKarazhanChessTest, FocusOrder)
{
    Board const b = Build(Side::Alliance, {
        { Kind::Pawn, false, 7, 0, ORI_SE, 1000 },
        { Kind::Rook, false, 7, 1, ORI_SE },
        { Kind::Knight, false, 7, 2, ORI_SE },
        { Kind::King, false, 7, 4, ORI_SE },
        { Kind::Pawn, false, 6, 0, ORI_SE, 0, false, true },  // buffed
        { Kind::Bishop, false, 7, 5, ORI_SE },
        { Kind::Queen, false, 7, 3, ORI_SE },
    });
    std::vector<Kind> order;
    Board c = b;
    while (true)
    {
        int const f = FocusTarget(c, Posture::Assault);
        if (f < 0)
            break;
        order.push_back(c.pieces[f].buffed ? Kind::None : c.pieces[f].kind);
        c.pieces.erase(c.pieces.begin() + f);
    }
    std::vector<Kind> const want = { Kind::Queen, Kind::Bishop, Kind::None, Kind::King,
                                     Kind::Knight, Kind::Rook, Kind::Pawn };
    EXPECT_EQ(order, want);
    // In HOLD his King is last.
    EXPECT_EQ(FocusRank(b.pieces[3], Posture::Hold), 7);
}

// --- posture ----------------------------------------------------------------------

TEST(DcKarazhanChessTest, PostureTransitions)
{
    std::vector<P> ps = { { Kind::King, true, 0, 4, ORI_NW }, { Kind::Queen, true, 0, 3, ORI_NW } };
    for (int c = 0; c < 8; ++c)
        ps.push_back({ Kind::Pawn, false, 6, c, ORI_SE });
    ps.push_back({ Kind::King, false, 7, 4, ORI_SE });
    Board b = Build(Side::Alliance, ps);
    Tunables const t;

    EXPECT_EQ(DecidePosture(b, Clock{ 10000, 0, 0 }, t), Posture::Hold);
    // A quiet 90s breaks the stalemate.
    EXPECT_EQ(DecidePosture(b, Clock{ 91000, 0, 0 }, t), Posture::Assault);
    EXPECT_EQ(DecidePosture(b, Clock{ 91000, 0, 60000 }, t), Posture::Hold);
    // Thin his material to six non-Kings.
    b.pieces.pop_back();  // (keep the King out of the count below)
    Board thin = Build(Side::Alliance, { { Kind::King, true, 0, 4, ORI_NW },
                                         { Kind::Pawn, false, 6, 0, ORI_SE },
                                         { Kind::King, false, 7, 4, ORI_SE } });
    EXPECT_EQ(DecidePosture(thin, Clock{ 1000, 0, 0 }, t), Posture::Assault);
    // ...but never with our King below 40%.
    thin.pieces[0].hp = thin.pieces[0].maxHp / 3;
    EXPECT_EQ(DecidePosture(thin, Clock{ 1000, 0, 0 }, t), Posture::Hold);
    // His King walking within 20yd of our Queen.
    Board close = Build(Side::Alliance, ps);
    close.pieces.back().r = 3;
    close.pieces.back().c = 3;
    EXPECT_EQ(DecidePosture(close, Clock{ 1000, 0, 0 }, t), Posture::Assault);
}

// --- the ledger -------------------------------------------------------------------

TEST(DcKarazhanChessTest, TheLedgerKeepsTheGamesPace)
{
    PieceInfo const& bishop = *InfoOf(NPC_BISHOP_A);
    Ledger l;
    EXPECT_TRUE(l.ActionDue(7, 0));
    l.OnAction(7, 1000);
    EXPECT_FALSE(l.ActionDue(7, 12999));
    EXPECT_TRUE(l.ActionDue(7, 13000));
    l.RefundAction(7);
    EXPECT_TRUE(l.ActionDue(7, 1001));

    l.OnCast(7, bishop.util.spell, bishop, 1000);  // the heal: 5s category, 20s own
    EXPECT_FALSE(l.AbilityDue(7, bishop.dmg.spell, bishop, 5999));
    EXPECT_TRUE(l.AbilityDue(7, bishop.dmg.spell, bishop, 6000));
    EXPECT_FALSE(l.AbilityDue(7, bishop.util.spell, bishop, 6000));
    EXPECT_TRUE(l.AbilityDue(7, bishop.util.spell, bishop, 21000));

    // Wraparound: a ready time just past the wrap is still in the future.
    Ledger w;
    w.OnAction(8, 0xFFFFF000u);
    EXPECT_FALSE(w.ActionDue(8, 0xFFFFF100u));
    EXPECT_TRUE(w.ActionDue(8, 0xFFFFF000u + ACTION_COOLDOWN_MS));
}

TEST(DcKarazhanChessTest, NoActionBeforeTheTwelveSeconds)
{
    Board const b = Build(Side::Alliance, {
        { Kind::Pawn, true, 2, 3, ORI_NW, 0, true },
        { Kind::Pawn, false, 2, 4, ORI_SE },
    });
    Ledger l;
    l.OnAction(1, 1000);
    EXPECT_EQ(OrderFor(Decide(b, l, Clock{ 5000, 0, 0 }), 1, OrderKind::Face), nullptr);
    EXPECT_NE(OrderFor(Decide(b, l, Clock{ 13000, 0, 0 }), 1, OrderKind::Face), nullptr);
}

// --- assignment -------------------------------------------------------------------

namespace
{
    // The opening position; GUIDs follow this order, so a test can move a piece
    // in the list and rebuild the board with every GUID unchanged.
    std::vector<P> StartPieces()
    {
        std::vector<P> ps;
        Kind const back[8] = { Kind::Rook, Kind::Knight, Kind::Bishop, Kind::Queen,
                               Kind::King, Kind::Bishop, Kind::Knight, Kind::Rook };
        for (int c = 0; c < 8; ++c)
        {
            ps.push_back({ back[c], true, 0, c, ORI_NW });
            ps.push_back({ Kind::Pawn, true, 1, c, ORI_NW });
            ps.push_back({ Kind::Pawn, false, 6, c, ORI_SE });
            ps.push_back({ back[c], false, 7, c, ORI_SE });
        }
        return ps;
    }

    Board StartBoard(Side our) { return Build(our, StartPieces()); }

    // Hand every seat the piece it was given, as the conductor does between ticks.
    void Hold(std::vector<Seat>& seats, std::vector<Assignment> const& a)
    {
        for (Seat& s : seats)
            for (Assignment const& x : a)
                if (x.bot == s.bot)
                    s.current = x.piece;
    }

    uint64 BotOn(std::vector<Assignment> const& a, uint64 piece)
    {
        for (Assignment const& x : a)
            if (x.piece == piece)
                return x.bot;
        return 0;
    }

    Kind KindOfGuid(Board const& b, uint64 guid)
    {
        for (Piece const& p : b.pieces)
            if (p.guid == guid)
                return p.kind;
        return Kind::None;
    }
}

TEST(DcKarazhanChessTest, TenBotsTakeTheTenPiecesThatMatter)
{
    Board const b = StartBoard(Side::Alliance);
    std::vector<Seat> seats;
    for (uint64 bot = 100; bot < 110; ++bot)
        seats.push_back({ bot, bot == 100, true, 0 });
    std::vector<Assignment> const a = Assign(b, seats);
    ASSERT_EQ(a.size(), 10u);
    // The King goes to the lowest non-leader GUID; the leader (100) goes last.
    EXPECT_EQ(a.front().bot, 101u);
    EXPECT_EQ(KindOfGuid(b, a.front().piece), Kind::King);
    EXPECT_EQ(a.back().bot, 100u);
    std::multiset<Kind> kinds;
    for (Assignment const& x : a)
        kinds.insert(KindOfGuid(b, x.piece));
    EXPECT_EQ(kinds.count(Kind::King), 1u);
    EXPECT_EQ(kinds.count(Kind::Queen), 1u);
    EXPECT_EQ(kinds.count(Kind::Bishop), 2u);
    EXPECT_EQ(kinds.count(Kind::Knight), 2u);
    EXPECT_EQ(kinds.count(Kind::Rook), 0u);
    EXPECT_EQ(kinds.count(Kind::Pawn), 4u);
    // The four pawns are the centre ones, the ones that march and open lanes.
    for (Assignment const& x : a)
        for (Piece const& p : b.pieces)
            if (p.guid == x.piece && p.kind == Kind::Pawn)
                EXPECT_TRUE(p.c >= 2 && p.c <= 5) << int(p.c);
    // With no centre pawns asked for, the Rooks come back ahead of the pawns
    // (the old order: the last two seats go to the centre pawns).
    std::multiset<Kind> old;
    for (Assignment const& x : Assign(b, seats, {}, /*centrePawns*/ 0))
        old.insert(KindOfGuid(b, x.piece));
    EXPECT_EQ(old.count(Kind::Rook), 2u);
    EXPECT_EQ(old.count(Kind::Pawn), 2u);
}

TEST(DcKarazhanChessTest, AssignmentIsStableAndRefillsOnADeath)
{
    Board b = StartBoard(Side::Alliance);
    std::vector<Seat> seats;
    for (uint64 bot = 100; bot < 110; ++bot)
        seats.push_back({ bot, bot == 109, true, 0 });
    std::vector<Assignment> const first = Assign(b, seats);
    for (Seat& s : seats)
        for (Assignment const& x : first)
            if (x.bot == s.bot)
                s.current = x.piece;

    // Kill the Queen: her bot takes the next piece in line, everyone else stays.
    uint64 queen = 0, queenBot = 0;
    for (Assignment const& x : first)
        if (KindOfGuid(b, x.piece) == Kind::Queen)
        {
            queen = x.piece;
            queenBot = x.bot;
        }
    b.pieces.erase(std::remove_if(b.pieces.begin(), b.pieces.end(),
                                  [&](Piece const& p) { return p.guid == queen; }),
                   b.pieces.end());
    for (Seat& s : seats)
        if (s.bot == queenBot)
            s.current = 0;
    std::vector<Assignment> const second = Assign(b, seats);
    ASSERT_EQ(second.size(), 10u);
    for (Assignment const& x : second)
    {
        if (x.bot == queenBot)
        {
            EXPECT_EQ(KindOfGuid(b, x.piece), Kind::Rook);
            continue;
        }
        auto it = std::find_if(first.begin(), first.end(),
                               [&](Assignment const& f) { return f.bot == x.bot; });
        ASSERT_NE(it, first.end());
        EXPECT_EQ(it->piece, x.piece) << "bot " << x.bot << " kept its piece";
    }

    // A refused pair is not retried.
    std::vector<Assignment> const refused = { { queenBot, second.back().piece } };
    for (Assignment const& x : Assign(b, seats, refused))
        EXPECT_FALSE(x.bot == queenBot && x.piece == refused.front().piece);
}

// The user-facing rule: march the pawns, then leave them where they stand and
// put their bots on the Rooks — never sit on a pawn until it dies.
TEST(DcKarazhanChessTest, MarchedPawnsHandTheirBotsToTheRooks)
{
    std::vector<P> ps = StartPieces();
    Board b = Build(Side::Alliance, ps);
    std::vector<Seat> seats;
    for (uint64 bot = 100; bot < 110; ++bot)
        seats.push_back({ bot, bot == 109, true, 0 });
    std::vector<Assignment> const first = Assign(b, seats);
    Hold(seats, first);

    // The four centre pawns reach advancePawnRow.
    for (P& d : ps)
        if (d.kind == Kind::Pawn && d.ours && d.c >= 2 && d.c <= 5)
            d.r = Tunables{}.advancePawnRow;
    b = Build(Side::Alliance, ps);
    std::vector<uint64> marched;
    NoteMarched(b, marched);
    ASSERT_EQ(marched.size(), 4u);

    std::vector<Assignment> const second = Assign(b, seats, {}, Tunables{}.controlCentrePawns, marched);
    ASSERT_EQ(second.size(), 10u);
    std::multiset<Kind> kinds;
    for (Assignment const& x : second)
    {
        kinds.insert(KindOfGuid(b, x.piece));
        EXPECT_EQ(std::count(marched.begin(), marched.end(), x.piece), 0) << "a marched pawn keeps no bot";
    }
    EXPECT_EQ(kinds.count(Kind::Rook), 2u);
    EXPECT_EQ(kinds.count(Kind::Pawn), 2u) << "the other two go to pawns that still have to march";
    // The King, Queen, Bishops and Knights keep their bots.
    for (Assignment const& x : first)
        if (KindOfGuid(b, x.piece) != Kind::Pawn)
            EXPECT_EQ(BotOn(second, x.piece), x.bot);
    // The Rooks go to the bots that left the marched pawns.
    for (Assignment const& x : second)
        if (KindOfGuid(b, x.piece) == Kind::Rook)
        {
            auto it = std::find_if(first.begin(), first.end(), [&](Assignment const& f) { return f.bot == x.bot; });
            ASSERT_NE(it, first.end());
            EXPECT_EQ(KindOfGuid(b, it->piece), Kind::Pawn);
        }
}

// tr-20260923-213634-1: a held pawn stepped diagonally out of the centre columns
// mid-march and its bot was handed a Rook it could never take. A pawn a bot holds
// keeps its rank until it has marched, wherever it wanders.
TEST(DcKarazhanChessTest, AHeldPawnKeepsItsBotUntilItHasMarched)
{
    std::vector<P> ps = StartPieces();
    Board b = Build(Side::Alliance, ps);
    std::vector<Seat> seats;
    for (uint64 bot = 100; bot < 110; ++bot)
        seats.push_back({ bot, bot == 109, true, 0 });
    std::vector<Assignment> const first = Assign(b, seats);
    Hold(seats, first);

    for (P& d : ps)
        if (d.kind == Kind::Pawn && d.ours && d.c == 2)
        {
            d.r = 2;
            d.c = 1;
        }
    b = Build(Side::Alliance, ps);
    std::vector<uint64> marched;
    NoteMarched(b, marched);
    EXPECT_TRUE(marched.empty());
    std::vector<Assignment> const second = Assign(b, seats, {}, Tunables{}.controlCentrePawns, marched);
    for (Assignment const& x : first)
        EXPECT_EQ(BotOn(second, x.piece), x.bot) << "nobody moves";
}

// Marched is sticky: a pawn stopped by an enemy has marched, and stays marched
// when that enemy dies, so its bot is not called back to it.
TEST(DcKarazhanChessTest, AMarchedPawnStaysMarched)
{
    std::vector<P> ps = {
        { Kind::King, true, 0, 4, ORI_NW },
        { Kind::Pawn, true, 2, 3, ORI_NW },
        { Kind::Pawn, false, 3, 3, ORI_SE },
        { Kind::King, false, 7, 4, ORI_SE },
    };
    Board b = Build(Side::Alliance, ps);
    std::vector<uint64> marched;
    NoteMarched(b, marched);
    ASSERT_EQ(marched.size(), 1u);
    EXPECT_EQ(marched.front(), 2u);

    ps.erase(ps.begin() + 2);
    b = Build(Side::Alliance, ps);
    EXPECT_FALSE(PawnMarched(b, b.pieces[1]));
    NoteMarched(b, marched);
    EXPECT_EQ(marched.size(), 1u);
}

// With nothing better on the board, a bot keeps the marched pawn it holds rather
// than being shuffled to another marched pawn worth the same.
TEST(DcKarazhanChessTest, NoShuffleBetweenMarchedPawns)
{
    std::vector<P> ps;
    ps.push_back({ Kind::King, true, 0, 4, ORI_NW });
    for (int c = 0; c < 8; ++c)
        ps.push_back({ Kind::Pawn, true, 3, c, ORI_NW });
    ps.push_back({ Kind::King, false, 7, 4, ORI_SE });
    Board const b = Build(Side::Alliance, ps);
    std::vector<uint64> marched;
    NoteMarched(b, marched);
    ASSERT_EQ(marched.size(), 8u);

    // Two bots on the edge pawns (GUIDs 2 and 9), the King free.
    std::vector<Seat> seats = { { 100, false, true, 2 }, { 101, false, true, 9 }, { 102, true, true, 0 } };
    std::vector<Assignment> const a = Assign(b, seats, {}, Tunables{}.controlCentrePawns, marched);
    EXPECT_EQ(BotOn(a, 2), 100u);
    EXPECT_EQ(BotOn(a, 9), 101u);
    EXPECT_EQ(BotOn(a, 1), 102u) << "the free bot takes the King";
}

// --- sides ------------------------------------------------------------------------

// The same position, played by an Alliance raid and by a Horde raid, gives the
// same orders in the normalised frame — the policy never sees a colour.
TEST(DcKarazhanChessTest, AHordeRaidPlaysTheMirroredGame)
{
    std::vector<P> const ps = {
        { Kind::Knight, true, 2, 2, ORI_NW, 0, true },
        { Kind::Pawn, true, 2, 5, ORI_NW, 0, true },
        { Kind::Queen, true, 1, 3, ORI_NW, 0, true },
        { Kind::King, true, 0, 4, ORI_NW, 0, true },
        { Kind::Pawn, false, 2, 6, ORI_SE },
        { Kind::Pawn, false, 4, 3, ORI_SE },
        { Kind::Bishop, false, 5, 5, ORI_SE },
        { Kind::King, false, 7, 4, ORI_SE },
    };
    Board const a = Build(Side::Alliance, ps, { { 2, 2 } });
    Board const h = Build(Side::Horde, ps, { { 2, 2 } });
    EXPECT_FALSE(a.mirrored);
    EXPECT_TRUE(h.mirrored);
    // The Horde raid's pieces really are on the far rows.
    EXPECT_EQ(h.RealRow(h.pieces[3].r), 7);
    EXPECT_EQ(h.RealOri(h.pieces[3].facing), ORI_SE);

    for (uint32 now : { 1000u, 60000u, 95000u })
    {
        Decision const da = Decide(a, Ledger{}, Clock{ now, 0, 0 });
        Decision const dh = Decide(h, Ledger{}, Clock{ now, 0, 0 });
        EXPECT_EQ(da.posture, dh.posture);
        ASSERT_EQ(da.orders.size(), dh.orders.size()) << now;
        for (size_t i = 0; i < da.orders.size(); ++i)
        {
            EXPECT_EQ(da.orders[i].piece, dh.orders[i].piece);
            EXPECT_EQ(int(da.orders[i].kind), int(dh.orders[i].kind));
            EXPECT_EQ(da.orders[i].r, dh.orders[i].r);
            EXPECT_EQ(da.orders[i].c, dh.orders[i].c);
            EXPECT_EQ(da.orders[i].target, dh.orders[i].target);
            // Spells differ by colour; the ability slot must not.
            PieceInfo const* ia = nullptr;
            PieceInfo const* ih = nullptr;
            for (Piece const& p : a.pieces)
                if (p.guid == da.orders[i].piece)
                    ia = InfoOf(p.entry);
            for (Piece const& p : h.pieces)
                if (p.guid == dh.orders[i].piece)
                    ih = InfoOf(p.entry);
            ASSERT_TRUE(ia && ih);
            EXPECT_EQ(da.orders[i].spell == ia->dmg.spell, dh.orders[i].spell == ih->dmg.spell);
        }
    }
}

TEST(DcKarazhanChessTest, DecideIsDeterministic)
{
    Board const b = StartBoard(Side::Alliance);
    Board c = b;
    for (Piece& p : c.pieces)
        p.controlled = p.ours;
    Decision const x = Decide(c, Ledger{}, Clock{ 95000, 0, 0 });
    Decision const y = Decide(c, Ledger{}, Clock{ 95000, 0, 0 });
    ASSERT_EQ(x.orders.size(), y.orders.size());
    for (size_t i = 0; i < x.orders.size(); ++i)
    {
        EXPECT_EQ(x.orders[i].piece, y.orders[i].piece);
        EXPECT_EQ(x.orders[i].r, y.orders[i].r);
        EXPECT_EQ(x.orders[i].c, y.orders[i].c);
    }
}

// --- the conductor (DcChessConductor) ------------------------------------------

namespace
{
    using CState = DcChessConductor::State;
    using CAct = DcChessConductor::Act;

    DcChessConductor::View Armed()
    {
        DcChessConductor::View v;
        v.armed = true;
        return v;
    }
}

TEST(DcKarazhanChessTest, ConductorStartsAGameFromScratch)
{
    DcChessConductor::View v = Armed();
    EXPECT_EQ(DcChessConductor::Decide(CState::Idle, DcChessConductor::View{}).next, CState::Idle)
        << "nothing happens until hook 43 arms it";
    EXPECT_EQ(DcChessConductor::Decide(CState::Idle, v).next, CState::Setup);

    // Setup waits for the sideline, bounded.
    auto step = DcChessConductor::Decide(CState::Setup, v);
    EXPECT_EQ(step.next, CState::Setup);
    EXPECT_EQ(step.act, CAct::Gather);
    v.raidAtSideline = true;
    step = DcChessConductor::Decide(CState::Setup, v);
    EXPECT_EQ(step.next, CState::Start);
    EXPECT_EQ(step.act, CAct::StartGame);
    v.raidAtSideline = false;
    v.stateAgeMs = DcChessConductor::SETUP_MAX_MS;
    EXPECT_EQ(DcChessConductor::Decide(CState::Setup, v).next, CState::Start);

    // Start: gossip Echo at phase 0, then WARMUP -> TakeKing -> INPROGRESS -> Play.
    v.stateAgeMs = 0;
    step = DcChessConductor::Decide(CState::Start, v);
    EXPECT_EQ(step.act, CAct::StartGame);
    v.phase = DcChessConductor::PHASE_WARMUP;
    step = DcChessConductor::Decide(CState::Start, v);
    EXPECT_EQ(step.next, CState::TakeKing);
    EXPECT_EQ(step.act, CAct::TakeKing);
    v.phase = DcChessConductor::PHASE_INPROGRESS;
    step = DcChessConductor::Decide(CState::TakeKing, v);
    EXPECT_EQ(step.next, CState::Play);
    EXPECT_EQ(step.act, CAct::Play);
}

TEST(DcKarazhanChessTest, ConductorNeverGossipsEchoInTheWrongPhase)
{
    // H8: ordinal 0 is Start at phase 0, Restart at 1/2, PvP at 4. The FSM asks
    // for StartGame only with the phase at 0 and nobody still charmed or
    // recently released; RestartGame only from Play/TakeKing/Restart.
    DcChessConductor::View v = Armed();
    v.phase = DcChessConductor::PHASE_NOT_STARTED;
    v.anyRecentlyInGame = true;
    EXPECT_EQ(DcChessConductor::Decide(CState::Start, v).act, CAct::WaitReset);
    v.anyRecentlyInGame = false;
    v.anyCharmed = true;
    EXPECT_EQ(DcChessConductor::Decide(CState::Start, v).act, CAct::WaitReset);

    // Won: from ANY live state the next thing is the chest, never a gossip.
    DcChessConductor::View won = Armed();
    won.event = DcChessConductor::EVENT_DONE;
    won.phase = 4;
    won.chestPresent = true;
    for (CState s : { CState::Setup, CState::Start, CState::TakeKing, CState::Play, CState::Restart, CState::Lost })
    {
        auto const step = DcChessConductor::Decide(s, won);
        EXPECT_EQ(step.next, CState::Loot) << DcChessConductor::StateName(s);
        EXPECT_EQ(step.act, CAct::Loot);
    }
}

TEST(DcKarazhanChessTest, ConductorAdoptsAGameAlreadyRunning)
{
    DcChessConductor::View v = Armed();
    v.phase = DcChessConductor::PHASE_INPROGRESS;
    EXPECT_EQ(DcChessConductor::Decide(CState::Idle, v).next, CState::Play);
    v.phase = DcChessConductor::PHASE_WARMUP;
    EXPECT_EQ(DcChessConductor::Decide(CState::Setup, v).next, CState::TakeKing);
}

TEST(DcKarazhanChessTest, ConductorRetriesALossThenBlocks)
{
    DcChessConductor::View v = Armed();
    v.phase = DcChessConductor::PHASE_NOT_STARTED;  // our King died: the core reset the game
    v.attempts = 1;
    EXPECT_EQ(DcChessConductor::Decide(CState::Play, v).next, CState::Lost);
    // Settling: everyone released and Recently In Game gone, and a few seconds.
    v.anyCharmed = true;
    EXPECT_EQ(DcChessConductor::Decide(CState::Lost, v).next, CState::Lost);
    v.anyCharmed = false;
    v.stateAgeMs = DcChessConductor::LOST_SETTLE_MS;
    EXPECT_EQ(DcChessConductor::Decide(CState::Lost, v).next, CState::Setup);
    // Third loss: Blocked, with the reason.
    v.attempts = DcChessConductor::MAX_ATTEMPTS;
    auto const step = DcChessConductor::Decide(CState::Lost, v);
    EXPECT_EQ(step.next, CState::Blocked);
    EXPECT_STREQ(step.why, "chess lost 3x");
}

TEST(DcKarazhanChessTest, ConductorRestartsAHopelessGame)
{
    DcChessConductor::View v = Armed();
    v.phase = DcChessConductor::PHASE_INPROGRESS;
    v.gameAgeMs = DcChessConductor::GAME_MAX_MS;
    auto step = DcChessConductor::Decide(CState::Play, v);
    EXPECT_EQ(step.next, CState::Restart);
    EXPECT_EQ(step.act, CAct::RestartGame);
    v.gameAgeMs = 60000;
    v.sinceKillMs = DcChessConductor::NO_KILL_MAX_MS;
    EXPECT_EQ(DcChessConductor::Decide(CState::Play, v).next, CState::Restart);
    // The restart lands: phase 0 -> Lost (and then the retry path above).
    v.phase = DcChessConductor::PHASE_NOT_STARTED;
    EXPECT_EQ(DcChessConductor::Decide(CState::Restart, v).next, CState::Lost);
    // A WARMUP nobody finishes restarts too.
    DcChessConductor::View w = Armed();
    w.phase = DcChessConductor::PHASE_WARMUP;
    w.stateAgeMs = DcChessConductor::TAKE_KING_MAX_MS;
    EXPECT_EQ(DcChessConductor::Decide(CState::TakeKing, w).next, CState::Blocked)
        << "Echo offers no gossip in WARMUP, so a warmup cannot be restarted";
    EXPECT_EQ(DcChessConductor::Decide(CState::Restart, w).next, CState::Blocked);
}

TEST(DcKarazhanChessTest, ConductorLootsTheChestThenFinishes)
{
    DcChessConductor::View v = Armed();
    v.event = DcChessConductor::EVENT_DONE;
    v.phase = 4;
    v.chestPresent = true;
    EXPECT_EQ(DcChessConductor::Decide(CState::Loot, v).act, CAct::Loot);
    v.chestOpened = true;
    EXPECT_EQ(DcChessConductor::Decide(CState::Loot, v).next, CState::Done);
    v.chestOpened = false;
    v.stateAgeMs = DcChessConductor::LOOT_MAX_MS;
    EXPECT_EQ(DcChessConductor::Decide(CState::Loot, v).next, CState::Done) << "the chest never blocks the run";
    // The chest respawns a map update after the win: give it a moment.
    v.stateAgeMs = 0;
    v.chestPresent = false;
    EXPECT_EQ(DcChessConductor::Decide(CState::Loot, v).next, CState::Loot);
    v.stateAgeMs = DcChessConductor::CHEST_WAIT_MS;
    EXPECT_EQ(DcChessConductor::Decide(CState::Loot, v).next, CState::Done);
    // Terminal states stay put, and hold nobody.
    EXPECT_EQ(DcChessConductor::Decide(CState::Done, v).next, CState::Done);
    EXPECT_FALSE(DcChessConductor::HoldsTheRaid(CState::Done));
    EXPECT_FALSE(DcChessConductor::HoldsTheRaid(CState::Loot)) << "the chest is the leader's errand alone";
    EXPECT_TRUE(DcChessConductor::HoldsTheRaid(CState::Play));
}

// --- wiring ---------------------------------------------------------------------

TEST(DcKarazhanChessTest, TheChessHooksAreRegistered)
{
    EXPECT_TRUE(ObjectiveHookRegistry::Has(DcKarazhan::HOOK_KZ_CHESS_PLAY));
    EXPECT_TRUE(ObjectiveHookRegistry::Has(DcKarazhan::HOOK_KZ_CHESS_SETUP));
}

TEST(DcKarazhanChessTest, NobodyTargetsAPieceTheTriggersOrTheFire)
{
    EXPECT_TRUE(DcTargetExclusionRegistry::HasRowsFor(DcKarazhan::MAP));
    for (DcChess::PieceInfo const& p : DcChess::PieceTable())
    {
        EXPECT_TRUE(DcTargetExclusionRegistry::IsExcluded(nullptr, DcKarazhan::MAP, p.entry)) << p.entry;
        EXPECT_TRUE(DcTargetExclusionRegistry::IsExcluded(nullptr, DcKarazhan::MAP, p.entry, /*forTank*/ true))
            << p.entry;
        EXPECT_FALSE(DcTargetExclusionRegistry::IsExcluded(nullptr, 469u, p.entry)) << "map 532 only";
    }
    EXPECT_TRUE(DcTargetExclusionRegistry::IsExcluded(nullptr, DcKarazhan::MAP, DcChess::NPC_MOVE_TRIGGER));
    EXPECT_TRUE(DcTargetExclusionRegistry::IsExcluded(nullptr, DcKarazhan::MAP, DcChess::NPC_FIRE));
    // Karazhan's real bosses are untouched.
    EXPECT_FALSE(DcTargetExclusionRegistry::IsExcluded(nullptr, DcKarazhan::MAP, DcKarazhan::NPC_MOROES));
}

TEST(DcKarazhanChessTest, TheChessRungOutranksEveryLadderInTheHall)
{
    EXPECT_GT(DcRel::KzChess, DcRel::HakkarSuppressorCombat) << "a tie with no partition test";
    EXPECT_LT(DcRel::KzChess, DcRel::OcRider) << "a tie with no partition test";
    EXPECT_GT(DcRel::KzChess, DcRel::EventDueCombat);
    EXPECT_GT(DcRel::KzChess, DcRel::RazorgoreOrb);
    EXPECT_GT(DcRel::KzChess, DcRel::PullManeuver);
    EXPECT_GT(DcRel::KzChess, DcRel::StayAtCamp);
    EXPECT_GT(DcRel::KzChess, DcRel::HazardVacate);
    EXPECT_GT(DcRel::KzChess, DcRel::AllCleared) << "the run must not finish before the chest is looted";
    EXPECT_GT(DcRel::KzChess, DcRel::StrandedRecovery);
    EXPECT_GT(DcRel::KzChess, DcRel::AtObjective);
    EXPECT_GT(DcRel::KzChess, DcRel::FollowTank);
    EXPECT_GT(DcRel::KzChess, DcRel::NeedsRest);
    EXPECT_LT(DcRel::KzChess, DcRel::BreakStuckCombat);
    EXPECT_LT(DcRel::KzChess, DcRel::PartyDied);
    EXPECT_LT(DcRel::KzChess, DcRel::Chat);
}

TEST(DcKarazhanChessTest, TheStandDownNeverSilencesTheChessRung)
{
    EXPECT_EQ(DcBossStandDown::ClassifyAction("dungeon clear kz chess", true),
              DcBossStandDown::ActionVerdict::Stock);
}
