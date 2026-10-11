/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCCHESSBOARD_H
#define _PLAYERBOT_DCCHESSBOARD_H

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "Define.h"

// PURE board model for Karazhan's chess event (map 532) — the geometry, the piece
// table and the board the conductor rebuilds from world positions every tick. No
// world access at all, so the whole of it is gtested (DcKarazhanChessTest) and the
// offline simulator (t/TestKarazhanChessSim.cpp) plays on the same board the live
// conductor does.
//
// Plan: deployment-files/docs/mod-dungeon-clear_karazhan-chess_plan.md, section 1.
//
// WHY A BOARD OF OUR OWN. npc_echo_of_medivh keeps the real one (_boards) private,
// and a module cannot reach it. Everything it holds can be read back off the world
// instead: every piece stands on its cell centre (the move is a MovePoint to the
// cell's trigger), the facing is the creature's own orientation, and the fire is a
// creature (22521) on the cell it burns. The one thing the world does NOT show is
// a move the core has accepted but the piece has not finished walking — the core's
// board updates when the move is accepted, not on arrival — which is why a moving
// piece is placed on its spline's destination (see DcChess::Board::Add).
//
// THE SIDES ARE NORMALISED. The raid plays whichever colour its clicker's team is
// (CHESS_EVENT_TEAM); Alliance starts on rows 0-1 and marches +row, Horde on rows
// 6-7 and marches -row. Every policy decision is written ONCE, from the point of
// view of a side that starts on rows 0-1: the builder mirrors a Horde raid's rows
// (r -> 7-r) and facings on the way in, and the glue mirrors the orders on the way
// out. The policy never sees a colour.
namespace DcChess
{
    constexpr int8 N = 8;

    // --- geometry (boss_chess_event.cpp:253, SetupBoard) -------------------
    //
    //   x = X0 + 3.49*c + 4.40*r
    //   y = Y0 - 4.40*c + 3.45*r
    //
    // A row step is 5.59yd, a column step 5.62yd, a diagonal 7.9yd; the two axes
    // are orthogonal to within a tenth of a degree, so a distance in cells is a
    // distance in yards without a cross term worth carrying.
    constexpr float X0 = -11108.0996f;
    constexpr float Y0 = -1872.9100f;
    constexpr float Z  = 220.667f;
    constexpr float COL_DX = 3.49f,  COL_DY = -4.40f;
    constexpr float ROW_DX = 4.40f,  ROW_DY = 3.45f;
    constexpr float DET = COL_DX * ROW_DY - ROW_DX * COL_DY;  // 31.40
    constexpr float ROW_YD = 5.5914f;  // |(ROW_DX, ROW_DY)|
    constexpr float COL_YD = 5.6157f;  // |(COL_DX, COL_DY)|

    // How far off a cell centre a unit may stand and still read as ON that cell.
    // Every piece is placed exactly on its centre (the 32 DB spawns sit within
    // 0.8yd of theirs); 1.5 absorbs a walk that stopped a step short without
    // ever reaching the 2.8yd half-cell where it would be ambiguous.
    constexpr float SNAP_TOLERANCE = 1.5f;

    inline void CenterOf(float r, float c, float& x, float& y)
    {
        x = X0 + COL_DX * c + ROW_DX * r;
        y = Y0 + COL_DY * c + ROW_DY * r;
    }

    // Fractional board coordinates of a world point (the inverse of CenterOf).
    // Off-board points come back outside 0..7, which is how the landmarks are
    // described: Echo of Medivh stands at (3.45, -1.7).
    inline void BoardCoords(float x, float y, float& r, float& c)
    {
        float const dx = x - X0;
        float const dy = y - Y0;
        c = (ROW_DY * dx - ROW_DX * dy) / DET;
        r = (-COL_DY * dx + COL_DX * dy) / DET;
    }

    // The cell a world point stands on, or false when it is off the board or too
    // far from any cell centre to claim one.
    inline bool CellOf(float x, float y, int8& r, int8& c, float tolerance = SNAP_TOLERANCE)
    {
        float fr, fc;
        BoardCoords(x, y, fr, fc);
        int const ir = static_cast<int>(std::lround(fr));
        int const ic = static_cast<int>(std::lround(fc));
        if (ir < 0 || ir >= N || ic < 0 || ic >= N)
            return false;
        float cx, cy;
        CenterOf(static_cast<float>(ir), static_cast<float>(ic), cx, cy);
        float const ex = x - cx, ey = y - cy;
        if (ex * ex + ey * ey > tolerance * tolerance)
            return false;
        r = static_cast<int8>(ir);
        c = static_cast<int8>(ic);
        return true;
    }

    inline bool OnBoard(int r, int c) { return r >= 0 && r < N && c >= 0 && c < N; }

    // Yards between two cells, centre to centre.
    inline float CellDist(int dr, int dc)
    {
        float const a = static_cast<float>(dr) * ROW_YD;
        float const b = static_cast<float>(dc) * COL_YD;
        return std::sqrt(a * a + b * b);
    }

    inline int Chebyshev(int dr, int dc) { return std::max(std::abs(dr), std::abs(dc)); }

    // --- facing (boss_chess_event.cpp:74-86 and :178) ------------------------
    //
    // The core's eight orientations, in its own enum order. Each move sets the
    // piece's facing from the SIGN of the step (HandlePieceRotate), so a piece
    // always faces one of these, and the melee tick (32225 -> GetEnemyPiece with
    // checkFront) only hits inside a 60-degree arc — which on this board is the
    // one neighbouring cell the facing points at, and nothing else within its
    // 10yd reach (two cells straight ahead is 11.2yd).
    enum Ori : uint8
    {
        ORI_SE = 0,  // -row        (Horde's start facing)
        ORI_S  = 1,  // -row -col
        ORI_SW = 2,  //      -col
        ORI_W  = 3,  // +row -col
        ORI_NW = 4,  // +row        (Alliance's start facing)
        ORI_N  = 5,  // +row +col
        ORI_NE = 6,  //      +col
        ORI_E  = 7,  // -row +col
        ORI_COUNT = 8
    };

    struct Step { int8 dr, dc; };
    constexpr std::array<Step, ORI_COUNT> ORI_STEP = { {
        { -1, 0 }, { -1, -1 }, { 0, -1 }, { 1, -1 }, { 1, 0 }, { 1, 1 }, { 0, 1 }, { -1, 1 },
    } };
    constexpr std::array<float, ORI_COUNT> ORI_YAW = {
        3.809080f, 3.022091f, 2.235102f, 1.448113f, 0.661124f, 6.1724616f, 5.385472f, 4.598483f,
    };

    // The orientation a step of (dr, dc) leaves a piece facing — the core's own
    // table (HandlePieceRotate), signs only, so a knight's (2, 1) is ORI_N like a
    // (1, 1) is. ORI_COUNT for a zero step.
    inline Ori OriOfStep(int dr, int dc)
    {
        int const sr = (dr > 0) - (dr < 0);
        int const sc = (dc > 0) - (dc < 0);
        for (uint8 o = 0; o < ORI_COUNT; ++o)
            if (ORI_STEP[o].dr == sr && ORI_STEP[o].dc == sc)
                return static_cast<Ori>(o);
        return ORI_COUNT;
    }

    // Snap a creature's orientation to the nearest of the eight.
    inline Ori SnapFacing(float yaw)
    {
        constexpr float TWO_PI = 6.28318531f;
        uint8 best = 0;
        float bestErr = 1e9f;
        for (uint8 o = 0; o < ORI_COUNT; ++o)
        {
            float d = std::fmod(std::fabs(yaw - ORI_YAW[o]), TWO_PI);
            if (d > TWO_PI / 2.0f)
                d = TWO_PI - d;
            if (d < bestErr)
            {
                bestErr = d;
                best = o;
            }
        }
        return static_cast<Ori>(best);
    }

    // Mirror a facing across the board's row axis (r -> 7-r): the row half of the
    // step flips sign, the column half stays. This is the Horde-raid normalisation.
    inline Ori MirrorOri(Ori o)
    {
        if (o >= ORI_COUNT)
            return o;
        return OriOfStep(-ORI_STEP[o].dr, ORI_STEP[o].dc);
    }

    // --- pieces (karazhan.h, boss_chess_event.cpp, Spell.dbc) ---------------
    enum class Kind : uint8 { Pawn, Rook, Knight, Bishop, Queen, King, None };
    enum class Side : uint8 { Alliance, Horde };

    // Spell ids. Every piece has two abilities sharing category 1152 (a 5s
    // category cooldown, so one of the two per 5s), and some carry a longer
    // recovery of their own. Move and Change Facing share category 1163 with
    // Chess Cooldown: Move (30543, a 12s category cooldown) — one move OR one
    // turn per 12s.
    //
    // NONE OF THOSE COOLDOWNS IS ENFORCED FOR US. A creature's cooldowns are only
    // checked on the pet-cast opcode path (Spell::CheckPetCast) and only stored
    // by AI helpers; Unit::CastSpell on a charmed piece checks neither, which is
    // the Teron "machine-gun" note. The conductor's ledger (DcChessDecision.h)
    // is therefore the only thing keeping the raid to the game's pace, and it is
    // authoritative, not a fallback.
    constexpr uint32 SPELL_MOVE_SHORT   = 37146;  //  8yd: pawn, rook, bishop, king
    constexpr uint32 SPELL_MOVE_KNIGHT  = 37144;  // 15yd
    constexpr uint32 SPELL_MOVE_QUEEN   = 37148;  // 20yd
    constexpr uint32 SPELL_CHANGE_FACING = 30284;
    constexpr uint32 SPELL_MOVE_COOLDOWN = 30543;
    constexpr uint32 SPELL_CONTROL_PIECE = 30019;
    constexpr uint32 SPELL_GAME_IN_SESSION = 39331;
    constexpr uint32 SPELL_RECENTLY_IN_GAME = 30529;
    constexpr uint32 SPELL_HAND_OF_MEDIVH = 39339;

    constexpr uint32 ACTION_COOLDOWN_MS  = 12000;  // move or turn (category 1163)
    constexpr uint32 ABILITY_COOLDOWN_MS = 5000;   // either ability (category 1152)

    // What an ability does, reduced to what the policy and the simulator need.
    enum class Shape : uint8
    {
        None,
        FrontCone,  // hits enemies straight along the facing, within `radius`
        SelfArea,   // hits enemies (or, for a buff, friends) within `radius` of the caster
        Single,     // one unit target within `range`
        TargetArea, // a unit target within `range`, splashing `radius` around it
        SelfBuff,   // the caster itself
        Heal,       // one friendly unit within `range`
    };

    struct Ability
    {
        uint32 spell{0};
        Shape  shape{Shape::None};
        float  range{0.0f};       // Single / TargetArea / Heal: the spell's own range
        float  radius{0.0f};      // FrontCone / SelfArea / TargetArea
        uint32 amount{0};         // damage or heal per hit
        uint32 recoveryMs{0};     // the spell's own recovery, on top of the category's 5s
        uint8  maxTargets{0};
    };

    struct PieceInfo
    {
        uint32 entry{0};
        Kind kind{Kind::None};
        Side side{Side::Alliance};
        uint32 moveSpell{0};
        uint8 reach{0};           // HandlePieceMove's Chebyshev limit
        uint32 maxHp{0};
        float combatReach{0.0f};  // creature_model_info, for the spell-range fudge
        Ability dmg;              // the damage ability
        Ability util;             // the other one
    };

    constexpr uint32 NPC_PAWN_A = 17211, NPC_PAWN_H = 17469;
    constexpr uint32 NPC_ROOK_A = 21160, NPC_ROOK_H = 21726;
    constexpr uint32 NPC_KNIGHT_A = 21664, NPC_KNIGHT_H = 21748;
    constexpr uint32 NPC_BISHOP_A = 21682, NPC_BISHOP_H = 21747;
    constexpr uint32 NPC_QUEEN_A = 21683, NPC_QUEEN_H = 21750;
    constexpr uint32 NPC_KING_A = 21684, NPC_KING_H = 21752;
    constexpr uint32 NPC_MOVE_TRIGGER = 22519;
    constexpr uint32 NPC_FIRE = 22521;         // Fury of Medivh's burning cell
    constexpr uint32 NPC_ECHO_OF_MEDIVH = 16816;

    // Level-70 elites; HP = basehp1(70) x HealthModifier. Abilities from
    // Spell.dbc (amounts, radii, recoveries) and spell_cone (the 60-63 degree
    // cones, which on this board reach only straight ahead).
    inline std::array<PieceInfo, 12> const& PieceTable()
    {
        static std::array<PieceInfo, 12> const table = { {
            // Pawns: Heroic Blow / Vicious Strike 1000 in front; Shield Block /
            // Weapon Deflection absorbs 500 physical for 5s.
            { NPC_PAWN_A, Kind::Pawn, Side::Alliance, SPELL_MOVE_SHORT, 1, 50000, 2.25f,
              { 37406, Shape::FrontCone, 0.0f, 8.0f, 1000, 0, 1 },
              { 37414, Shape::SelfBuff, 0.0f, 0.0f, 500, 0, 1 } },
            { NPC_PAWN_H, Kind::Pawn, Side::Horde, SPELL_MOVE_SHORT, 1, 50000, 2.25f,
              { 37413, Shape::FrontCone, 0.0f, 8.0f, 1000, 0, 1 },
              { 37416, Shape::SelfBuff, 0.0f, 0.0f, 500, 0, 1 } },
            // Rooks: Geyser / Hellfire 3000 around them; Water / Fire Shield -50%
            // damage taken for 5s.
            { NPC_ROOK_A, Kind::Rook, Side::Alliance, SPELL_MOVE_SHORT, 1, 80000, 2.0f,
              { 37427, Shape::SelfArea, 0.0f, 9.0f, 3000, 0, 8 },
              { 37432, Shape::SelfBuff, 0.0f, 0.0f, 0, 0, 1 } },
            { NPC_ROOK_H, Kind::Rook, Side::Horde, SPELL_MOVE_SHORT, 1, 80000, 3.0f,
              { 37428, Shape::SelfArea, 0.0f, 9.0f, 3000, 0, 8 },
              { 37434, Shape::SelfBuff, 0.0f, 0.0f, 0, 0, 1 } },
            // Knights: Smash / Bite 3000 in front; Stomp / Howl in front, -50%
            // damage done for 20s — the answer to Hand of Medivh.
            { NPC_KNIGHT_A, Kind::Knight, Side::Alliance, SPELL_MOVE_KNIGHT, 2, 65000, 2.625f,
              { 37453, Shape::FrontCone, 0.0f, 8.0f, 3000, 0, 1 },
              { 37498, Shape::FrontCone, 0.0f, 10.0f, 0, 0, 3 } },
            { NPC_KNIGHT_H, Kind::Knight, Side::Horde, SPELL_MOVE_KNIGHT, 2, 65000, 2.5f,
              { 37454, Shape::FrontCone, 0.0f, 8.0f, 3000, 0, 1 },
              { 37502, Shape::FrontCone, 0.0f, 10.0f, 0, 0, 3 } },
            // Bishops: Holy Lance / Shadow Spear 2000 in a narrow cone out to
            // 18yd; Healing / Shadow Mend 12000 on a friend within 25yd, 20s.
            { NPC_BISHOP_A, Kind::Bishop, Side::Alliance, SPELL_MOVE_SHORT, 1, 60000, 3.0f,
              { 37459, Shape::FrontCone, 0.0f, 18.0f, 2000, 0, 3 },
              { 37455, Shape::Heal, 25.0f, 0.0f, 12000, 20000, 1 } },
            { NPC_BISHOP_H, Kind::Bishop, Side::Horde, SPELL_MOVE_SHORT, 1, 60000, 3.0f,
              { 37461, Shape::FrontCone, 0.0f, 18.0f, 2000, 0, 3 },
              { 37456, Shape::Heal, 25.0f, 0.0f, 12000, 20000, 1 } },
            // Queens: Elemental Blast / Fireball 4000 on one target within 20yd —
            // no facing needed from a non-player caster; Rain of Fire (radius 6) /
            // Poison Cloud (radius 0.5) 3000 per 5s for 10s at a target within
            // 25yd, 15s.
            { NPC_QUEEN_A, Kind::Queen, Side::Alliance, SPELL_MOVE_QUEEN, 3, 80000, 3.75f,
              { 37462, Shape::Single, 20.0f, 0.0f, 4000, 0, 1 },
              { 37465, Shape::TargetArea, 25.0f, 6.0f, 3000, 15000, 5 } },
            { NPC_QUEEN_H, Kind::Queen, Side::Horde, SPELL_MOVE_QUEEN, 3, 80000, 3.75f,
              { 37463, Shape::Single, 20.0f, 0.0f, 4000, 0, 1 },
              { 37469, Shape::TargetArea, 25.0f, 0.5f, 3000, 15000, 5 } },
            // Kings: Sweep / Cleave 4000 around them (3 targets); Heroism /
            // Bloodlust +50% damage to friends within 8yd for 10s, 15s.
            { NPC_KING_A, Kind::King, Side::Alliance, SPELL_MOVE_SHORT, 1, 150000, 4.5f,
              { 37474, Shape::SelfArea, 0.0f, 10.0f, 4000, 0, 3 },
              { 37471, Shape::SelfArea, 0.0f, 8.0f, 0, 15000, 9 } },
            { NPC_KING_H, Kind::King, Side::Horde, SPELL_MOVE_SHORT, 1, 150000, 4.5f,
              { 37476, Shape::SelfArea, 0.0f, 10.0f, 4000, 0, 3 },
              { 37472, Shape::SelfArea, 0.0f, 8.0f, 0, 15000, 9 } },
        } };
        return table;
    }

    inline PieceInfo const* InfoOf(uint32 entry)
    {
        for (PieceInfo const& p : PieceTable())
            if (p.entry == entry)
                return &p;
        return nullptr;
    }

    inline bool IsPieceEntry(uint32 entry) { return InfoOf(entry) != nullptr; }

    // The entry of `kind` on `side` (the simulator and the tests build boards
    // from kinds; the live board is built from entries).
    inline uint32 EntryOf(Kind kind, Side side)
    {
        for (PieceInfo const& p : PieceTable())
            if (p.kind == kind && p.side == side)
                return p.entry;
        return 0;
    }

    // The move spells target a DESTINATION (the trigger's position), not a unit,
    // so their range check is IsWithinDist3d(dest, range): the spell's range plus
    // the CASTER's own combat reach, and nothing for the trigger. That admits a
    // knight's (2,2) at 15.9yd (15 + 2.6) and a queen's (3,2) at 20.2 (20 + 3.75),
    // but NOT a queen's (3,3) at 23.8 — HandlePieceMove would allow it, the spell
    // never lands. The legal set is the Chebyshev rule AND this yard rule, with a
    // quarter-yard margin for a piece standing a step off its cell centre.
    constexpr float MOVE_RANGE_MARGIN = 0.25f;
    inline float MoveSpellRange(uint32 moveSpell)
    {
        switch (moveSpell)
        {
            case SPELL_MOVE_KNIGHT: return 15.0f;
            case SPELL_MOVE_QUEEN:  return 20.0f;
            default:                return 8.0f;
        }
    }

    // --- the board ------------------------------------------------------------

    struct Piece
    {
        uint64 guid{0};
        uint32 entry{0};
        Kind kind{Kind::None};
        bool ours{false};
        int8 r{0}, c{0};          // NORMALISED cell (our home rows are 0-1)
        uint32 hp{0}, maxHp{0};
        Ori facing{ORI_NW};       // NORMALISED facing (our forward is +row)
        bool buffed{false};       // Hand of Medivh (+200% damage)
        bool controlled{false};   // ours, and charmed by one of our bots
        bool moving{false};       // still walking to (r, c)
        bool shielded{false};     // Water/Fire Shield or Shield Block/Deflection up
        bool weakened{false};     // Stomp/Howl'd (-50% damage done)

        float HpPct() const { return maxHp ? 100.0f * static_cast<float>(hp) / static_cast<float>(maxHp) : 0.0f; }
        uint32 Missing() const { return maxHp > hp ? maxHp - hp : 0; }
    };

    struct Board
    {
        std::vector<Piece> pieces;
        std::array<std::array<int16, N>, N> at{};    // index into pieces, -1 = empty
        std::array<std::array<bool, N>, N> burning{};
        bool mirrored{false};                        // a Horde raid: rows were flipped on the way in

        Board() { Clear(false); }

        void Clear(bool mirror)
        {
            pieces.clear();
            for (auto& row : at)
                row.fill(-1);
            for (auto& row : burning)
                row.fill(false);
            mirrored = mirror;
        }

        // Normalise a REAL row / facing into this board's frame.
        int8 NormRow(int r) const { return static_cast<int8>(mirrored ? N - 1 - r : r); }
        Ori NormOri(Ori o) const { return mirrored ? MirrorOri(o) : o; }
        // ...and back out (the same flip — it is its own inverse).
        int8 RealRow(int r) const { return NormRow(r); }
        Ori RealOri(Ori o) const { return NormOri(o); }

        // Place a piece given its REAL cell and facing. Returns false (and adds
        // nothing) for a second piece on an occupied cell: the first wins, which is
        // what the core's board does too (a move onto an occupied cell is refused).
        bool Add(Piece p, int realRow, int col, Ori realFacing)
        {
            if (!OnBoard(realRow, col))
                return false;
            p.r = NormRow(realRow);
            p.c = static_cast<int8>(col);
            p.facing = NormOri(realFacing);
            if (at[p.r][p.c] >= 0)
                return false;
            at[p.r][p.c] = static_cast<int16>(pieces.size());
            pieces.push_back(p);
            return true;
        }

        void SetBurningReal(int realRow, int col)
        {
            if (OnBoard(realRow, col))
                burning[NormRow(realRow)][col] = true;
        }

        Piece const* At(int r, int c) const
        {
            if (!OnBoard(r, c) || at[r][c] < 0)
                return nullptr;
            return &pieces[at[r][c]];
        }
        int16 IndexAt(int r, int c) const { return OnBoard(r, c) ? at[r][c] : int16(-1); }
        bool Empty(int r, int c) const { return OnBoard(r, c) && at[r][c] < 0; }
        bool Burning(int r, int c) const { return OnBoard(r, c) && burning[r][c]; }

        int FindKing(bool ours) const
        {
            for (size_t i = 0; i < pieces.size(); ++i)
                if (pieces[i].kind == Kind::King && pieces[i].ours == ours)
                    return static_cast<int>(i);
            return -1;
        }

        int Count(bool ours) const
        {
            int n = 0;
            for (Piece const& p : pieces)
                n += p.ours == ours ? 1 : 0;
            return n;
        }
    };

    // The cell `p` faces — the only one its melee, and its cones' first cell,
    // can reach. May be off the board.
    inline void FrontCell(Piece const& p, int& r, int& c)
    {
        r = p.r + ORI_STEP[p.facing].dr;
        c = p.c + ORI_STEP[p.facing].dc;
    }

    // The enemy standing on `p`'s front cell, or -1.
    inline int EnemyInFront(Board const& b, Piece const& p)
    {
        int r, c;
        FrontCell(p, r, c);
        Piece const* q = b.At(r, c);
        if (!q || q->ours == p.ours)
            return -1;
        return b.IndexAt(r, c);
    }

    // Enemies of `p` along its facing ray within `radius` yards (a cone that, at
    // 60 degrees, is only ever the straight line on this board).
    inline void EnemiesOnRay(Board const& b, Piece const& p, float radius, std::vector<int>& out)
    {
        out.clear();
        Step const s = ORI_STEP[p.facing];
        for (int k = 1; k < N; ++k)
        {
            int const r = p.r + s.dr * k, c = p.c + s.dc * k;
            if (!OnBoard(r, c) || CellDist(s.dr * k, s.dc * k) > radius)
                break;
            Piece const* q = b.At(r, c);
            if (q && q->ours != p.ours)
                out.push_back(b.IndexAt(r, c));
        }
    }

    // The pieces on the eight cells around (r, c) that belong to the side
    // opposite `ours`.
    inline int AdjacentEnemies(Board const& b, int r, int c, bool ours, std::vector<int>* out = nullptr)
    {
        int n = 0;
        if (out)
            out->clear();
        for (int dr = -1; dr <= 1; ++dr)
            for (int dc = -1; dc <= 1; ++dc)
            {
                if (!dr && !dc)
                    continue;
                Piece const* q = b.At(r + dr, c + dc);
                if (q && q->ours != ours)
                {
                    ++n;
                    if (out)
                        out->push_back(b.IndexAt(r + dr, c + dc));
                }
            }
        return n;
    }

    inline int AdjacentFriends(Board const& b, int r, int c, bool ours)
    {
        int n = 0;
        for (int dr = -1; dr <= 1; ++dr)
            for (int dc = -1; dc <= 1; ++dc)
            {
                if (!dr && !dc)
                    continue;
                Piece const* q = b.At(r + dr, c + dc);
                n += (q && q->ours == ours) ? 1 : 0;
            }
        return n;
    }

    // Enemies whose FRONT cell is (r, c) — i.e. that would melee a piece of side
    // `ours` standing there. Medivh's pieces barely ever re-face (the core's
    // turn-to-enemy cast targets a piece, which the spell's conditions refuse),
    // so this is the exposure that actually costs HP.
    inline int EnemiesFacing(Board const& b, int r, int c, bool ours)
    {
        int n = 0;
        for (int dr = -1; dr <= 1; ++dr)
            for (int dc = -1; dc <= 1; ++dc)
            {
                if (!dr && !dc)
                    continue;
                Piece const* q = b.At(r + dr, c + dc);
                if (!q || q->ours == ours)
                    continue;
                int fr, fc;
                FrontCell(*q, fr, fc);
                if (fr == r && fc == c)
                    n += q->buffed ? 3 : 1;
            }
        return n;
    }

    // May `p` step to (r, c) right now: on the board, empty, not where it stands,
    // and inside its Chebyshev reach (HandlePieceMove).
    inline bool CanStep(Board const& b, Piece const& p, int r, int c)
    {
        if (!b.Empty(r, c))
            return false;
        int const dr = r - p.r, dc = c - p.c;
        if (!dr && !dc)
            return false;
        PieceInfo const* info = InfoOf(p.entry);
        if (!info)
            return Chebyshev(dr, dc) <= 1;
        return Chebyshev(dr, dc) <= info->reach &&
               CellDist(dr, dc) <= MoveSpellRange(info->moveSpell) + info->combatReach - MOVE_RANGE_MARGIN;
    }

    // Every cell `p` may step to, in cell order (row-major) — the deterministic
    // enumeration every tie-break downstream relies on.
    inline void LegalMoves(Board const& b, Piece const& p, std::vector<std::pair<int8, int8>>& out)
    {
        out.clear();
        for (int r = 0; r < N; ++r)
            for (int c = 0; c < N; ++c)
                if (CanStep(b, p, r, c))
                    out.emplace_back(static_cast<int8>(r), static_cast<int8>(c));
    }
}

#endif  // _PLAYERBOT_DCCHESSBOARD_H
