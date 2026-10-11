/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCCHESSDECISION_H
#define _PLAYERBOT_DCCHESSDECISION_H

#include <algorithm>
#include <array>
#include <cstdint>
#include <utility>
#include <vector>

#include "Define.h"
#include "Ai/Dungeon/DungeonClear/Util/DcChessBoard.h"

// PURE policy for Karazhan's chess event: given the board (DcChessBoard.h), the
// cooldown ledger and the clock, what does each piece the raid controls do this
// tick? The conductor (Overrides/KarazhanChessDriver.cpp) builds the board from
// the world, calls Decide, and hands each order to the bot holding that piece.
//
// DETERMINISTIC, with no randomness seeded or otherwise (tools/check_determinism.sh
// fails on any RNG primitive under src/). Every tie breaks on cell order. The
// randomness of the game lives in the core and, offline, in the simulator's own
// seeded PRNG (t/TestKarazhanChessSim.cpp), which plays THIS policy over hundreds
// of games so the thresholds below are tuned in seconds rather than one live game
// at a time.
//
// THE SHAPE OF A WIN, which is what every rule below serves (plan section 1.5):
//
//   * Medivh's side marches. Every one of his pieces, his King included, steps
//     "forward" at a random adjacent column every 10-20s and jams against our
//     lines. It almost never re-faces: its turn-to-enemy cast targets a piece and
//     the spell's conditions only accept a move trigger, so it faces whatever
//     way it last walked. A piece of ours BESIDE or BEHIND one of his is safe
//     from its melee and its cones.
//   * So we are the anvil and the hammer. The centre pawns march a step or two
//     and the back rank steps up behind them, so the Queen, Bishops and Knights
//     are never walled in behind their own pawns; the line then melees whatever
//     walks into it. The pieces we control spend their one move-or-turn per 12s
//     on getting off fire, turning to face a neighbour, stepping onto a cell
//     that flanks one, and otherwise advancing. Abilities fire off their own 5s
//     cooldown independently.
//   * And then we take his King. It walks into us too; once his material thins,
//     or his King comes within 20yd of our Queen, or nothing has died for 90s,
//     the strike group (Queen, Knights, Rooks, the centre pawns) goes for the
//     cells around it.
namespace DcChess
{
    // Every threshold the policy uses, in one struct so the simulator can sweep
    // it. The defaults are what ships.
    struct Tunables
    {
        // --- posture: when to stop holding and go for his King -------------
        int    assaultEnemyNonKingMax = 6;     // his non-King material at or below this
        int    assaultMaterialLead    = 4;     // we are this many pieces ahead
        float  assaultKingDistYd      = 20.0f; // his King this close to our Queen
        uint32 stalemateNoKillMs      = 90000; // nothing died for this long
        float  holdKingHpPct          = 40.0f; // ...unless our King is below this

        // --- the King's own safety -----------------------------------------
        int    kingFleeAdjacent = 3;       // this many enemies around him
        float  kingFleeHpPct    = 50.0f;   // ...or below this with one facing him

        // --- HOLD: how far forward a flanking step may go ---------------------
        // Rows are normalised: ours are 0-3, and 4 is "our half plus one".
        int    holdMaxRow = 4;

        // --- HOLD: marching the line forward ----------------------------------
        // With nothing beside it and nothing to flank, a pawn steps forward up to
        // advancePawnRow and every other piece but the King follows up to
        // advanceFollowRow, so the back rank is never walled in behind its own
        // pawns. -1 = never.
        int    advancePawnRow = 3;
        int    advanceFollowRow = 1;
        // How many centre pawns outrank the Rooks for a bot until they have
        // marched (PawnMarched): an uncontrolled pawn never moves, so these are
        // the pawns that open the lanes. Once there, the bot goes to a Rook.
        int    controlCentrePawns = 4;

        // --- abilities -------------------------------------------------------
        uint32 healMissingMin  = 12000;    // a heal is only worth its 20s on this much missing
        float  rookShieldHpPct = 60.0f;
        int    rainMinTargets  = 2;        // Rain of Fire only on a cluster of this many
        // Spell ranges are checked as range + caster reach + target reach. The
        // smallest target reach on the board is 2.0 (the Water Elemental); using
        // it keeps every range decision on the side the core will accept.
        float  minTargetReach  = 2.0f;
    };

    // One move-or-turn per 12s, one ability per 5s, and the per-spell recoveries
    // on top. Wraparound-safe: a ready time of 0 means "ready".
    inline bool Due(uint32 readyAtMs, uint32 nowMs)
    {
        return readyAtMs == 0 || static_cast<int32>(nowMs - readyAtMs) >= 0;
    }

    struct LedgerEntry
    {
        uint64 guid{0};
        uint32 actionReadyMs{0};   // move / Change Facing (category 1163, 12s)
        uint32 abilityReadyMs{0};  // either ability (category 1152, 5s)
        uint32 dmgReadyMs{0};      // the damage ability's own recovery
        uint32 utilReadyMs{0};     // the other ability's own recovery
    };

    // The conductor's cooldown ledger, one row per piece it has ever ordered.
    // AUTHORITATIVE: the core enforces none of these on a charmed piece (see
    // DcChessBoard.h), so the ledger is what keeps the raid to the game's pace.
    struct Ledger
    {
        std::vector<LedgerEntry> rows;

        LedgerEntry const* Find(uint64 guid) const
        {
            for (LedgerEntry const& e : rows)
                if (e.guid == guid)
                    return &e;
            return nullptr;
        }

        LedgerEntry& Get(uint64 guid)
        {
            for (LedgerEntry& e : rows)
                if (e.guid == guid)
                    return e;
            rows.push_back(LedgerEntry{});
            rows.back().guid = guid;
            return rows.back();
        }

        bool ActionDue(uint64 guid, uint32 now) const
        {
            LedgerEntry const* e = Find(guid);
            return !e || Due(e->actionReadyMs, now);
        }

        bool AbilityDue(uint64 guid, uint32 spell, PieceInfo const& info, uint32 now) const
        {
            LedgerEntry const* e = Find(guid);
            if (!e)
                return true;
            if (!Due(e->abilityReadyMs, now))
                return false;
            if (spell == info.dmg.spell)
                return Due(e->dmgReadyMs, now);
            if (spell == info.util.spell)
                return Due(e->utilReadyMs, now);
            return true;
        }

        // A move or turn went out.
        void OnAction(uint64 guid, uint32 now)
        {
            LedgerEntry& e = Get(guid);
            e.actionReadyMs = now + ACTION_COOLDOWN_MS;
            if (!e.actionReadyMs)
                e.actionReadyMs = 1;
        }

        // A move the core refused (the target cell filled in the meantime): the
        // game's own cooldown is only cast on success, so ours is refunded.
        void RefundAction(uint64 guid)
        {
            Get(guid).actionReadyMs = 0;
        }

        void OnCast(uint64 guid, uint32 spell, PieceInfo const& info, uint32 now)
        {
            LedgerEntry& e = Get(guid);
            e.abilityReadyMs = now + ABILITY_COOLDOWN_MS;
            if (spell == info.dmg.spell && info.dmg.recoveryMs)
                e.dmgReadyMs = now + info.dmg.recoveryMs;
            if (spell == info.util.spell && info.util.recoveryMs)
                e.utilReadyMs = now + info.util.recoveryMs;
        }
    };

    enum class Posture : uint8 { Hold, Assault };

    inline char const* PostureName(Posture p) { return p == Posture::Assault ? "ASSAULT" : "HOLD"; }

    enum class OrderKind : uint8 { None, Move, Face, Cast };

    // Why an order was given — the rung of the ladder that produced it, for the
    // per-order log line and the simulator's statistics.
    enum class Why : uint8 { None, Fire, KingSafety, FaceEnemy, Assault, Flank, Advance, Ability };

    inline char const* WhyName(Why w)
    {
        switch (w)
        {
            case Why::Fire:       return "fire";
            case Why::KingSafety: return "king-safety";
            case Why::FaceEnemy:  return "face";
            case Why::Assault:    return "assault";
            case Why::Flank:      return "flank";
            case Why::Advance:    return "advance";
            case Why::Ability:    return "ability";
            default:              return "-";
        }
    }

    // One instruction for one controlled piece. Cells are NORMALISED (the glue
    // flips them back through Board::RealRow). A Move targets the empty cell
    // (r, c); a Face targets the cell (r, c) the piece should turn toward; a Cast
    // names the spell and its unit target (0 = the piece itself / an AoE).
    struct Order
    {
        uint64 piece{0};
        OrderKind kind{OrderKind::None};
        int8 r{-1}, c{-1};
        uint32 spell{0};
        uint64 target{0};
        Why why{Why::None};
    };

    struct Clock
    {
        uint32 nowMs{0};
        uint32 gameStartMs{0};
        uint32 lastKillMs{0};  // the last time any piece, either side, died
    };

    struct Decision
    {
        Posture posture{Posture::Hold};
        int focus{-1};                 // index into the board's pieces, -1 = none
        std::vector<Order> orders;     // at most one action order and one cast per piece
    };

    // --- building blocks -----------------------------------------------------

    // The yard reach of a unit-targeted ability from `caster`: the spell's range
    // plus both combat reaches (IsWithinCombatRange), with the smallest target
    // reach on the board standing in for the target's.
    inline float CastReach(PieceInfo const& caster, Ability const& a, Tunables const& t)
    {
        return a.range + caster.combatReach + t.minTargetReach;
    }

    inline float Dist(Piece const& a, Piece const& b)
    {
        return CellDist(a.r - b.r, a.c - b.c);
    }

    // How much standing on (r, c) costs one of our pieces: fire (everything but
    // a King dies in a minute of it), the enemies whose melee faces the cell, and
    // a little for every enemy around it (Hellfire, Cleave and the Poison Cloud
    // find neighbours whatever their facing).
    inline float Threat(Board const& b, int r, int c)
    {
        float t = b.Burning(r, c) ? 100.0f : 0.0f;
        t += 4.0f * static_cast<float>(EnemiesFacing(b, r, c, /*ours*/ true));
        t += 0.5f * static_cast<float>(AdjacentEnemies(b, r, c, /*ours*/ true));
        return t;
    }

    // Focus order: enemy Queen > Bishops (their heals undo our work) > anything
    // under Hand of Medivh > his King (ASSAULT only) > Knights > Rooks > Pawns.
    // Lower rank is better; lower HP, then cell order, breaks a tie.
    inline int FocusRank(Piece const& p, Posture posture)
    {
        if (p.kind == Kind::Queen)  return 0;
        if (p.kind == Kind::Bishop) return 1;
        if (p.buffed && p.kind != Kind::King) return 2;
        if (p.kind == Kind::King)   return posture == Posture::Assault ? 3 : 7;
        if (p.kind == Kind::Knight) return 4;
        if (p.kind == Kind::Rook)   return 5;
        return 6;
    }

    // The better of two enemy targets by focus order.
    inline bool BetterTarget(Board const& b, int a, int bIdx, Posture posture)
    {
        if (bIdx < 0)
            return true;
        Piece const& pa = b.pieces[a];
        Piece const& pb = b.pieces[bIdx];
        int const ra = FocusRank(pa, posture), rb = FocusRank(pb, posture);
        if (ra != rb)
            return ra < rb;
        if (pa.hp != pb.hp)
            return pa.hp < pb.hp;
        return pa.r * N + pa.c < pb.r * N + pb.c;
    }

    inline int FocusTarget(Board const& b, Posture posture)
    {
        int best = -1;
        for (size_t i = 0; i < b.pieces.size(); ++i)
            if (!b.pieces[i].ours && BetterTarget(b, static_cast<int>(i), best, posture))
                best = static_cast<int>(i);
        return best;
    }

    inline Posture DecidePosture(Board const& b, Clock const& clk, Tunables const& t)
    {
        int const ourKing = b.FindKing(true);
        if (ourKing >= 0 && b.pieces[ourKing].HpPct() < t.holdKingHpPct)
            return Posture::Hold;

        int theirNonKing = 0;
        for (Piece const& p : b.pieces)
            if (!p.ours && p.kind != Kind::King)
                ++theirNonKing;
        if (theirNonKing <= t.assaultEnemyNonKingMax)
            return Posture::Assault;
        if (b.Count(true) - b.Count(false) >= t.assaultMaterialLead)
            return Posture::Assault;

        int const theirKing = b.FindKing(false);
        if (theirKing >= 0)
            for (Piece const& p : b.pieces)
                if (p.ours && p.kind == Kind::Queen &&
                    Dist(p, b.pieces[theirKing]) <= t.assaultKingDistYd)
                    return Posture::Assault;

        uint32 const quietSince = std::max(clk.lastKillMs, clk.gameStartMs);
        if (static_cast<int32>(clk.nowMs - quietSince) >= static_cast<int32>(t.stalemateNoKillMs))
            return Posture::Assault;

        return Posture::Hold;
    }

    // Is (r, c) free for a move this tick: empty on the board, and not already
    // claimed by an order issued earlier in the same tick.
    struct Claims
    {
        std::array<std::array<bool, N>, N> taken{};
        bool Free(Board const& b, int r, int c) const { return b.Empty(r, c) && !taken[r][c]; }
        void Take(int r, int c) { if (OnBoard(r, c)) taken[r][c] = true; }
    };

    // One step of (dr, dc) for `p`: inside its Chebyshev reach AND its move
    // spell's yard range (see CanStep in DcChessBoard.h).
    inline bool StepFits(Piece const& p, int dr, int dc, int reach)
    {
        if (Chebyshev(dr, dc) > reach)
            return false;
        PieceInfo const* info = InfoOf(p.entry);
        return !info || CellDist(dr, dc) <= MoveSpellRange(info->moveSpell) + info->combatReach - MOVE_RANGE_MARGIN;
    }

    // The shortest walk for `p` from its cell to any cell in `goal`, over empty
    // cells, one legal step at a time. Returns the FIRST step, or false when no
    // goal cell is reachable. Breadth-first in cell order, so the answer is
    // deterministic; among equally short walks the goal with the lowest
    // `goalCost` wins.
    inline bool FirstStepToward(Board const& b, Claims const& claims, Piece const& p,
                                std::array<std::array<float, N>, N> const& goalCost,
                                int8& outR, int8& outC)
    {
        PieceInfo const* info = InfoOf(p.entry);
        int const reach = info ? info->reach : 1;
        std::array<std::array<int8, N>, N> dist;
        std::array<std::array<int8, N>, N> firstR, firstC;
        for (auto& row : dist)
            row.fill(-1);
        std::vector<std::pair<int8, int8>> frontier{ { p.r, p.c } }, next;
        dist[p.r][p.c] = 0;
        firstR[p.r][p.c] = -1;
        firstC[p.r][p.c] = -1;

        int bestR = -1, bestC = -1;
        float bestCost = 1e9f;
        for (int depth = 1; depth <= 2 * N && !frontier.empty(); ++depth)
        {
            next.clear();
            for (auto const& [fr, fc] : frontier)
                for (int r = 0; r < N; ++r)
                    for (int c = 0; c < N; ++c)
                    {
                        if (dist[r][c] >= 0 || !StepFits(p, r - fr, c - fc, reach))
                            continue;
                        if (!claims.Free(b, r, c))
                            continue;
                        dist[r][c] = static_cast<int8>(depth);
                        firstR[r][c] = depth == 1 ? static_cast<int8>(r) : firstR[fr][fc];
                        firstC[r][c] = depth == 1 ? static_cast<int8>(c) : firstC[fr][fc];
                        next.emplace_back(static_cast<int8>(r), static_cast<int8>(c));
                        if (goalCost[r][c] < bestCost)
                        {
                            bestCost = goalCost[r][c];
                            bestR = r;
                            bestC = c;
                        }
                    }
            if (bestR >= 0)
            {
                outR = firstR[bestR][bestC];
                outC = firstC[bestR][bestC];
                return true;
            }
            std::swap(frontier, next);
        }
        return false;
    }

    // --- the 12s action, one piece ------------------------------------------

    inline bool IsStrikeGroup(Kind k)
    {
        return k == Kind::Queen || k == Kind::Knight || k == Kind::Rook || k == Kind::Pawn;
    }

    // Among the cells `p` may step to, the one that costs least by `score`;
    // false when there is none.
    template <class Score>
    inline bool BestStep(Board const& b, Claims const& claims, Piece const& p, Score score,
                         int8& outR, int8& outC)
    {
        PieceInfo const* info = InfoOf(p.entry);
        int const reach = info ? info->reach : 1;
        float best = 1e9f;
        bool found = false;
        for (int r = 0; r < N; ++r)
            for (int c = 0; c < N; ++c)
            {
                if (!StepFits(p, r - p.r, c - p.c, reach) || (r == p.r && c == p.c))
                    continue;
                if (!claims.Free(b, r, c))
                    continue;
                float const s = score(r, c);
                if (s < best)
                {
                    best = s;
                    outR = static_cast<int8>(r);
                    outC = static_cast<int8>(c);
                    found = true;
                }
            }
        return found && best < 1e8f;
    }

    // The adjacent enemy `p` should turn to: his King > anything buffed > the
    // lowest HP, then cell order.
    inline int BestFaceTarget(Board const& b, std::vector<int> const& adjacent, bool preferKing)
    {
        int best = -1;
        for (int i : adjacent)
        {
            if (best < 0)
            {
                best = i;
                continue;
            }
            Piece const& a = b.pieces[i];
            Piece const& o = b.pieces[best];
            auto key = [preferKing](Piece const& q)
            {
                return (preferKing && q.kind == Kind::King) ? 0 : q.buffed ? 1 : 2;
            };
            if (key(a) != key(o))
            {
                if (key(a) < key(o))
                    best = i;
                continue;
            }
            if (a.hp != o.hp)
            {
                if (a.hp < o.hp)
                    best = i;
                continue;
            }
            if (a.r * N + a.c < o.r * N + o.c)
                best = i;
        }
        return best;
    }

    inline Order ActionFor(Board const& b, Claims& claims, int idx, Posture posture,
                           Tunables const& t)
    {
        Piece const& p = b.pieces[idx];
        Order o;
        o.piece = p.guid;

        auto move = [&](int8 r, int8 c, Why why)
        {
            o.kind = OrderKind::Move;
            o.r = r;
            o.c = c;
            o.why = why;
            claims.Take(r, c);
            return o;
        };

        // 1. OFF THE FIRE. Sixty seconds of Burning Flames is 120k, which kills
        //    everything but a King, so this outranks everything. Backward or
        //    sideways first: forward is where the enemy is.
        if (b.Burning(p.r, p.c))
        {
            int8 r, c;
            if (BestStep(b, claims, p,
                         [&](int rr, int cc)
                         {
                             if (b.Burning(rr, cc))
                                 return 1e9f;
                             return Threat(b, rr, cc) * 10.0f + (rr > p.r ? 1.0f : 0.0f);
                         },
                         r, c))
                return move(r, c, Why::Fire);
        }

        std::vector<int> adjacent;
        int const nAdj = AdjacentEnemies(b, p.r, p.c, true, &adjacent);

        // 2. THE KING'S OWN SAFETY: mobbed, or hurt with something swinging at
        //    him — step toward the back rank, but only onto a cell that is
        //    actually safer.
        if (p.kind == Kind::King &&
            (nAdj >= t.kingFleeAdjacent ||
             (p.HpPct() < t.kingFleeHpPct && EnemiesFacing(b, p.r, p.c, true) > 0)))
        {
            float const here = Threat(b, p.r, p.c);
            int8 r, c;
            if (BestStep(b, claims, p,
                         [&](int rr, int cc)
                         {
                             if (rr > p.r)
                                 return 1e9f;
                             float const th = Threat(b, rr, cc);
                             return th < here ? th * 10.0f + static_cast<float>(rr) : 1e9f;
                         },
                         r, c))
                return move(r, c, Why::KingSafety);
        }

        // 3. TURN TO A NEIGHBOUR the piece is not facing. His pieces rarely turn,
        //    so an enemy beside us is one we hit for free once we face it. In
        //    ASSAULT his King outranks whatever we are already hitting.
        {
            int const front = EnemyInFront(b, p);
            bool const kingBeside = posture == Posture::Assault &&
                std::any_of(adjacent.begin(), adjacent.end(),
                            [&](int i) { return b.pieces[i].kind == Kind::King; });
            bool const facingKing = front >= 0 && b.pieces[front].kind == Kind::King;
            if (nAdj > 0 && (front < 0 || (kingBeside && !facingKing)))
            {
                int const target = BestFaceTarget(b, adjacent, posture == Posture::Assault);
                if (target >= 0)
                {
                    o.kind = OrderKind::Face;
                    o.r = b.pieces[target].r;
                    o.c = b.pieces[target].c;
                    o.target = b.pieces[target].guid;
                    o.why = Why::FaceEnemy;
                    return o;
                }
            }
            if (front >= 0)
                return o;  // already hitting something: keep the action for the fire
        }

        if (p.kind == Kind::King)
            return o;  // the King holds; it only ever moves for 1 and 2

        // 4. ASSAULT: the strike group closes on his King. The Queen only needs
        //    Elemental Blast range; everyone else wants a cell beside or behind
        //    him (never his front cell — his Cleave and melee live there), and
        //    arrives facing him because a move leaves a piece facing its step.
        int const theirKing = b.FindKing(false);
        if (posture == Posture::Assault && theirKing >= 0 && IsStrikeGroup(p.kind))
        {
            Piece const& k = b.pieces[theirKing];
            PieceInfo const* info = InfoOf(p.entry);
            std::array<std::array<float, N>, N> goal;
            for (auto& row : goal)
                row.fill(1e9f);
            int kfr, kfc;
            FrontCell(k, kfr, kfc);
            bool haveGoal = false;
            for (int r = 0; r < N; ++r)
                for (int c = 0; c < N; ++c)
                {
                    if (p.kind == Kind::Queen)
                    {
                        if (info && CellDist(r - k.r, c - k.c) <= CastReach(*info, info->dmg, t) - 1.0f)
                            goal[r][c] = Threat(b, r, c);
                    }
                    else if (Chebyshev(r - k.r, c - k.c) == 1)
                        goal[r][c] = Threat(b, r, c) + ((r == kfr && c == kfc) ? 50.0f : 0.0f);
                    haveGoal |= goal[r][c] < 1e9f && (r != p.r || c != p.c);
                }
            bool const there = goal[p.r][p.c] < 1e9f;
            int8 r, c;
            if (!there && haveGoal && FirstStepToward(b, claims, p, goal, r, c))
                return move(r, c, Why::Assault);
            if (there)
                return o;
        }

        // 5. HOLD: flank an enemy two cells away — step onto a cell beside it
        //    that is not its front cell, arriving facing it. Never out of the
        //    King's guard ring and never past our half plus one row.
        if (posture == Posture::Hold && nAdj == 0)
        {
            int const ourKing = b.FindKing(true);
            bool const guard = ourKing >= 0 &&
                Chebyshev(p.r - b.pieces[ourKing].r, p.c - b.pieces[ourKing].c) == 1;
            if (!guard)
            {
                int8 bestR = -1, bestC = -1;
                float bestScore = 1e9f;
                for (size_t i = 0; i < b.pieces.size(); ++i)
                {
                    Piece const& e = b.pieces[i];
                    if (e.ours || Chebyshev(e.r - p.r, e.c - p.c) != 2)
                        continue;
                    int efr, efc;
                    FrontCell(e, efr, efc);
                    int8 r, c;
                    if (!BestStep(b, claims, p,
                                  [&](int rr, int cc)
                                  {
                                      if (rr > t.holdMaxRow || b.Burning(rr, cc) ||
                                          Chebyshev(rr - e.r, cc - e.c) != 1 ||
                                          (rr == efr && cc == efc))
                                          return 1e9f;
                                      bool const arrivesFacing =
                                          OriOfStep(rr - p.r, cc - p.c) == OriOfStep(e.r - rr, e.c - cc);
                                      return Threat(b, rr, cc) * 10.0f + (arrivesFacing ? 0.0f : 5.0f);
                                  },
                                  r, c))
                        continue;
                    float const score = static_cast<float>(FocusRank(e, posture)) * 100.0f +
                                        Threat(b, r, c) * 10.0f;
                    if (score < bestScore)
                    {
                        bestScore = score;
                        bestR = r;
                        bestC = c;
                    }
                }
                if (bestR >= 0)
                    return move(bestR, bestC, Why::Flank);
            }
        }

        // 6. HOLD: ADVANCE. Nothing beside it and nothing to flank: march. Pawns
        //    go first and furthest, opening the lanes; the pieces behind step up
        //    into the row the pawns left. One row forward per step, straight
        //    ahead before a diagonal, never onto fire. (The simulator: letting
        //    the back rank past the pawns' old row costs games.)
        if (posture == Posture::Hold && nAdj == 0 && t.advancePawnRow >= 0)
        {
            int const cap = p.kind == Kind::Pawn ? t.advancePawnRow : t.advanceFollowRow;
            int8 r, c;
            if (p.r < cap &&
                BestStep(b, claims, p,
                         [&](int rr, int cc)
                         {
                             if (rr != p.r + 1 || b.Burning(rr, cc))
                                 return 1e9f;
                             return Threat(b, rr, cc) * 10.0f + (cc == p.c ? 0.0f : 1.0f);
                         },
                         r, c))
                return move(r, c, Why::Advance);
        }

        // 7. Nothing worth a 12s action: keep it for the fire.
        return o;
    }

    // --- the ability, one piece ----------------------------------------------

    inline Order CastFor(Board const& b, int idx, Posture posture, int focus, Ledger const& ledger,
                         uint32 now, Tunables const& t)
    {
        Piece const& p = b.pieces[idx];
        Order o;
        o.piece = p.guid;
        PieceInfo const* info = InfoOf(p.entry);
        if (!info)
            return o;

        auto due = [&](Ability const& a) { return ledger.AbilityDue(p.guid, a.spell, *info, now); };
        auto cast = [&](Ability const& a, uint64 target)
        {
            o.kind = OrderKind::Cast;
            o.spell = a.spell;
            o.target = target;
            o.why = Why::Ability;
            return o;
        };
        if (!ledger.AbilityDue(p.guid, 0, *info, now))
            return o;

        int const front = EnemyInFront(b, p);
        std::vector<int> ray;

        switch (p.kind)
        {
            case Kind::Queen:
            {
                // Rain of Fire on the biggest cluster in reach, when it is a
                // cluster; otherwise Elemental Blast — his King in ASSAULT, else
                // the focus target, else the best thing in reach.
                if (due(info->util) && info->util.radius >= 5.0f)
                {
                    int best = -1, bestCount = 0;
                    for (size_t i = 0; i < b.pieces.size(); ++i)
                    {
                        Piece const& e = b.pieces[i];
                        if (e.ours || Dist(p, e) > CastReach(*info, info->util, t))
                            continue;
                        int n = 1;
                        for (Step s : { Step{ 1, 0 }, Step{ -1, 0 }, Step{ 0, 1 }, Step{ 0, -1 } })
                        {
                            Piece const* q = b.At(e.r + s.dr, e.c + s.dc);
                            n += (q && !q->ours) ? 1 : 0;
                        }
                        if (n > bestCount)
                        {
                            bestCount = n;
                            best = static_cast<int>(i);
                        }
                    }
                    if (best >= 0 && bestCount >= t.rainMinTargets)
                        return cast(info->util, b.pieces[best].guid);
                }
                if (due(info->dmg))
                {
                    float const reach = CastReach(*info, info->dmg, t);
                    int const king = b.FindKing(false);
                    if (posture == Posture::Assault && king >= 0 && Dist(p, b.pieces[king]) <= reach)
                        return cast(info->dmg, b.pieces[king].guid);
                    if (focus >= 0 && Dist(p, b.pieces[focus]) <= reach)
                        return cast(info->dmg, b.pieces[focus].guid);
                    int best = -1;
                    for (size_t i = 0; i < b.pieces.size(); ++i)
                        if (!b.pieces[i].ours && Dist(p, b.pieces[i]) <= reach &&
                            BetterTarget(b, static_cast<int>(i), best, posture))
                            best = static_cast<int>(i);
                    if (best >= 0)
                        return cast(info->dmg, b.pieces[best].guid);
                }
                break;
            }
            case Kind::Bishop:
            {
                // Heal the King first, then whoever is missing the most — and only
                // on a real deficit, the heal is on a 20s recovery.
                if (due(info->util))
                {
                    float const reach = CastReach(*info, info->util, t);
                    int const king = b.FindKing(true);
                    if (king >= 0 && b.pieces[king].Missing() > t.healMissingMin &&
                        Dist(p, b.pieces[king]) <= reach)
                        return cast(info->util, b.pieces[king].guid);
                    int best = -1;
                    uint32 bestMissing = t.healMissingMin;
                    for (size_t i = 0; i < b.pieces.size(); ++i)
                    {
                        Piece const& f = b.pieces[i];
                        if (f.ours && f.Missing() > bestMissing && Dist(p, f) <= reach)
                        {
                            bestMissing = f.Missing();
                            best = static_cast<int>(i);
                        }
                    }
                    if (best >= 0)
                        return cast(info->util, b.pieces[best].guid);
                }
                EnemiesOnRay(b, p, info->dmg.radius, ray);
                if (!ray.empty() && due(info->dmg))
                    return cast(info->dmg, 0);
                break;
            }
            case Kind::Knight:
                // Stomp a buffed piece (-50% of a +200% hit), else Smash.
                if (front >= 0 && b.pieces[front].buffed && !b.pieces[front].weakened && due(info->util))
                    return cast(info->util, 0);
                if (front >= 0 && due(info->dmg))
                    return cast(info->dmg, 0);
                break;
            case Kind::Rook:
                if ((p.HpPct() < t.rookShieldHpPct || b.Burning(p.r, p.c)) && !p.shielded && due(info->util))
                    return cast(info->util, 0);
                if (AdjacentEnemies(b, p.r, p.c, true) > 0 && due(info->dmg))
                    return cast(info->dmg, 0);
                break;
            case Kind::Pawn:
                if (front >= 0 && due(info->dmg))
                    return cast(info->dmg, 0);
                if (EnemiesFacing(b, p.r, p.c, true) > 0 && !p.shielded && due(info->util))
                    return cast(info->util, 0);
                break;
            case Kind::King:
            {
                if (AdjacentEnemies(b, p.r, p.c, true) > 0 && due(info->dmg))
                    return cast(info->dmg, 0);
                // Heroism when at least two of ours around him are in contact.
                int inContact = 0;
                for (int dr = -1; dr <= 1; ++dr)
                    for (int dc = -1; dc <= 1; ++dc)
                    {
                        Piece const* q = b.At(p.r + dr, p.c + dc);
                        if ((dr || dc) && q && q->ours && AdjacentEnemies(b, q->r, q->c, true) > 0)
                            ++inContact;
                    }
                if (inContact >= 2 && due(info->util))
                    return cast(info->util, 0);
                break;
            }
            default:
                break;
        }
        return o;
    }

    // Controlled pieces in the order they get first claim on a cell this tick:
    // the King first (his safety move must never find its cell taken), then by
    // value.
    inline int ClaimOrder(Kind k)
    {
        switch (k)
        {
            case Kind::King:   return 0;
            case Kind::Queen:  return 1;
            case Kind::Bishop: return 2;
            case Kind::Knight: return 3;
            case Kind::Rook:   return 4;
            default:           return 5;
        }
    }

    // THE POLICY. One action order (move / turn) for each controlled piece whose
    // 12s is up and one cast for each whose ability is up. Pieces still walking
    // get no action order — the core would take it, but a piece walking is a
    // piece whose last order has not landed yet.
    inline Decision Decide(Board const& b, Ledger const& ledger, Clock const& clk,
                           Tunables const& t = Tunables{})
    {
        Decision d;
        d.posture = DecidePosture(b, clk, t);
        d.focus = FocusTarget(b, d.posture);

        std::vector<int> mine;
        for (size_t i = 0; i < b.pieces.size(); ++i)
            if (b.pieces[i].ours && b.pieces[i].controlled)
                mine.push_back(static_cast<int>(i));
        std::stable_sort(mine.begin(), mine.end(), [&](int a, int c)
        {
            int const ka = ClaimOrder(b.pieces[a].kind), kc = ClaimOrder(b.pieces[c].kind);
            if (ka != kc)
                return ka < kc;
            return b.pieces[a].r * N + b.pieces[a].c < b.pieces[c].r * N + b.pieces[c].c;
        });

        Claims claims;
        for (int idx : mine)
        {
            Piece const& p = b.pieces[idx];
            if (!p.moving && ledger.ActionDue(p.guid, clk.nowMs))
            {
                Order const a = ActionFor(b, claims, idx, d.posture, t);
                if (a.kind != OrderKind::None)
                    d.orders.push_back(a);
            }
            Order const c = CastFor(b, idx, d.posture, d.focus, ledger, clk.nowMs, t);
            if (c.kind != OrderKind::None)
                d.orders.push_back(c);
        }
        return d;
    }

    // --- who takes which piece ------------------------------------------------

    // A pawn's job is the march, and once it is done the pawn is left where it
    // stands (an uncontrolled piece still fights: the core casts its ability and
    // turns it to face a neighbour on its own) and its bot goes to a piece worth
    // more — the Rooks first. Done = at advancePawnRow, or an enemy beside it (it
    // will not march past one).
    inline bool PawnMarched(Board const& b, Piece const& p, Tunables const& t = Tunables{})
    {
        if (p.kind != Kind::Pawn || !p.ours)
            return false;
        return (t.advancePawnRow >= 0 && p.r >= t.advancePawnRow) || AdjacentEnemies(b, p.r, p.c, p.ours) > 0;
    }

    // Sticky: a pawn that has marched stays marched for the rest of the game, so
    // its bot is not called back to it when the enemy beside it dies.
    inline void NoteMarched(Board const& b, std::vector<uint64>& marched, Tunables const& t = Tunables{})
    {
        for (Piece const& p : b.pieces)
            if (PawnMarched(b, p, t) && std::find(marched.begin(), marched.end(), p.guid) == marched.end())
                marched.push_back(p.guid);
    }

    // The pieces the raid wants controlled, best first: King (it has to be first
    // — taking it is what starts the game), Queen, Bishops, Knights, the pawns
    // still to march (the `centrePawns` centre ones, and any a bot already holds),
    // Rooks, the other pawns still to march centre-out, and last the pawns that
    // have marched. Ten bots fill all ten; a piece that dies or a pawn that
    // arrives drops out and the next one in the list takes its place. `held` are
    // the pieces a bot holds or was given: within a rank they come first, so a
    // bot is never moved between two pieces that are worth the same.
    inline std::vector<int> ControlPriority(Board const& b, int centrePawns = Tunables{}.controlCentrePawns,
                                            std::vector<uint64> const& marched = {},
                                            std::vector<uint64> const& held = {})
    {
        auto in = [](std::vector<uint64> const& v, uint64 g) { return std::find(v.begin(), v.end(), g) != v.end(); };
        std::vector<int> v;
        for (size_t i = 0; i < b.pieces.size(); ++i)
            if (b.pieces[i].ours)
                v.push_back(static_cast<int>(i));
        // Pawn columns by distance from the centre: 0.5 for d/e, 1.5 for c/f...
        float const centreReach = static_cast<float>(centrePawns) / 2.0f;
        auto rank = [&](Piece const& p)
        {
            switch (p.kind)
            {
                case Kind::King:   return 0;
                case Kind::Queen:  return 1;
                case Kind::Bishop: return 2;
                case Kind::Knight: return 3;
                case Kind::Rook:   return 5;
                default:
                    if (in(marched, p.guid))
                        return 7;
                    return (std::abs(p.c - 3.5f) < centreReach || in(held, p.guid)) ? 4 : 6;
            }
        };
        std::stable_sort(v.begin(), v.end(), [&](int a, int c)
        {
            Piece const& pa = b.pieces[a];
            Piece const& pc = b.pieces[c];
            if (rank(pa) != rank(pc))
                return rank(pa) < rank(pc);
            bool const ha = in(held, pa.guid), hc = in(held, pc.guid);
            if (ha != hc)
                return ha;
            // Pawns centre-out; everything else by column (both of a pair are
            // wanted, so the order within the pair is only a tie-break).
            float const ca = pa.kind == Kind::Pawn ? std::abs(pa.c - 3.5f) : 0.0f;
            float const cc = pc.kind == Kind::Pawn ? std::abs(pc.c - 3.5f) : 0.0f;
            if (ca != cc)
                return ca < cc;
            return pa.guid < pc.guid;
        });
        return v;
    }

    struct Seat
    {
        uint64 bot{0};
        bool leader{false};      // the brain goes last, so it is the last one teleported
        bool available{true};    // alive, on the map, not given up on
        uint64 current{0};       // the piece it holds or was given last tick (0 = none)
    };

    struct Assignment
    {
        uint64 bot{0};
        uint64 piece{0};
    };

    // Stable: a bot keeps its piece while the piece is alive and still wanted.
    // Free bots, lowest GUID first and the leader last, fill the best free
    // pieces. `refused` lists (bot, piece) pairs that have already failed — a
    // gossip that never took — so the same pair is not tried again. `marched`
    // (NoteMarched) are the pawns whose job is done: a bot on one is given a
    // better piece when there is one, and the conductor has it let go.
    inline std::vector<Assignment> Assign(Board const& b, std::vector<Seat> seats,
                                          std::vector<Assignment> const& refused = {},
                                          int centrePawns = Tunables{}.controlCentrePawns,
                                          std::vector<uint64> const& marched = {})
    {
        std::vector<uint64> held;
        for (Seat const& s : seats)
            if (s.available && s.current)
                held.push_back(s.current);
        std::vector<int> const priority = ControlPriority(b, centrePawns, marched, held);
        std::stable_sort(seats.begin(), seats.end(), [](Seat const& a, Seat const& c)
        {
            if (a.leader != c.leader)
                return !a.leader;
            return a.bot < c.bot;
        });

        size_t available = 0;
        for (Seat const& s : seats)
            available += s.available ? 1 : 0;
        size_t const slots = std::min(available, priority.size());

        auto isRefused = [&](uint64 bot, uint64 piece)
        {
            return std::any_of(refused.begin(), refused.end(),
                               [&](Assignment const& a) { return a.bot == bot && a.piece == piece; });
        };

        std::vector<Assignment> out;
        std::vector<bool> pieceTaken(b.pieces.size(), false);
        std::vector<bool> seatDone(seats.size(), false);

        // Keep what is still good.
        for (size_t s = 0; s < seats.size(); ++s)
        {
            if (!seats[s].available || !seats[s].current)
                continue;
            for (size_t k = 0; k < slots; ++k)
            {
                int const idx = priority[k];
                if (b.pieces[idx].guid == seats[s].current && !pieceTaken[idx])
                {
                    pieceTaken[idx] = true;
                    seatDone[s] = true;
                    out.push_back({ seats[s].bot, seats[s].current });
                    break;
                }
            }
        }

        // Fill the rest, best piece first.
        for (size_t k = 0; k < slots; ++k)
        {
            int const idx = priority[k];
            if (pieceTaken[idx])
                continue;
            for (size_t s = 0; s < seats.size(); ++s)
            {
                if (seatDone[s] || !seats[s].available || isRefused(seats[s].bot, b.pieces[idx].guid))
                    continue;
                pieceTaken[idx] = true;
                seatDone[s] = true;
                out.push_back({ seats[s].bot, b.pieces[idx].guid });
                break;
            }
        }
        return out;
    }
}

#endif  // _PLAYERBOT_DCCHESSDECISION_H
