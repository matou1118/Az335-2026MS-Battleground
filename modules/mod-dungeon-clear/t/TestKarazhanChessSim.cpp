/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

// Karazhan chess (map 532): an OFFLINE SIMULATOR of the core's chess event that
// plays DcChess::Decide (Util/DcChessDecision.h) over hundreds of seeded games.
//
// WHY. A live chess attempt costs five to ten minutes plus an instance reset, and
// the policy's thresholds cannot be tuned one live game at a time. This replays
// the core's rules (boss_chess_event.cpp) closely enough that a policy change is
// measured in seconds. It is a MODEL: when the live win rate disagrees with it,
// fix the model first (the plan's C7), then the policy.
//
// WHAT IS MODELLED, with the core's numbers:
//   * Medivh's movement AI (HandlePieceMoveByAI): the first move 8-20s after the
//     start, then 10-20s after each arrival; one step "forward" to a random
//     adjacent column (queens 1-2, knights sometimes 2); a taken cell retries
//     after 1s. The two ways a piece stops for good are modelled too, because
//     the core has them: turning at the far edge, and the 50% "turn toward a
//     neighbouring enemy" branch — whose cast the spell's conditions refuse, and
//     which returns true anyway, zeroing the move timer with no arrival to reset
//     it. A Medivh piece that comes into side contact is frozen half the time.
//   * The melee tick: every 3s, 1000 to the closest enemy within 10yd inside the
//     piece's 60-degree front arc, on the real board geometry.
//   * Every ability timer of every uncontrolled piece on both sides (the first
//     cast at the core's opening value, then that value plus 6-12s for the raid's
//     pieces and 3-6s for Medivh's), and the abilities' real shapes: cones along
//     the facing, areas around the caster, Rain of Fire's splash, heals.
//   * The cheats every 45-100s: a full heal of his King, three fires on cells
//     holding our pieces (10 000 every 5s for 60s), Hand of Medivh (+200% damage
//     for 90s) on 1-4 of his pieces.
//   * Our controlled pieces obey only the policy, one move-or-turn per 12s and one
//     ability per 5s (the policy's ledger). Taking a piece is staggered at the
//     start, and a bot whose piece dies re-takes the next one 25s later (the
//     teleport, Recently In Game, the walk back from the balcony, the gossip).
//     A bot the policy moves to a better piece (a pawn that has marched) lets
//     go and pays the same 25s.
//
// Randomness lives HERE (a seeded std::mt19937); the policy under test has none.

#include "gtest/gtest.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "Ai/Dungeon/DungeonClear/Util/DcChessBoard.h"
#include "Ai/Dungeon/DungeonClear/Util/DcChessDecision.h"

using namespace DcChess;

namespace
{
    constexpr int32 STEP_MS = 100;
    constexpr int32 POLICY_MS = 500;
    constexpr int32 GAME_LIMIT_MS = 12 * 60 * 1000;
    constexpr float WALK_YD_PER_MS = 5.0f / 1000.0f;   // speed_walk 2 x 2.5
    constexpr int32 MELEE_PERIOD_MS = 3000;
    constexpr int32 MELEE_DAMAGE = 1000;
    constexpr int32 FIRE_TICK_MS = 5000;
    constexpr int32 FIRE_DURATION_MS = 60000;
    constexpr int32 FIRE_DAMAGE = 10000;
    constexpr int32 RECONTROL_MS = 25000;
    constexpr int32 TAKE_STAGGER_MS = 4000;
    constexpr int32 RAID_BOTS = 10;

    struct Rng
    {
        std::mt19937 g;
        explicit Rng(uint32 seed) : g(seed) {}
        int U(int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(g); }
    };

    struct SP
    {
        uint64 guid{0};
        PieceInfo const* info{nullptr};
        bool ours{false};
        bool alive{true};
        int r{0}, c{0};
        Ori facing{ORI_NW};
        int32 hp{0};
        bool moving{false};
        int32 arriveMs{0};
        int32 moveTimer{0};        // Medivh's AI; 0 = no timer running (frozen)
        int32 t1{0}, t2{0}, base1{0}, base2{0};
        int32 meleeMs{0};
        int32 handMs{0};           // Hand of Medivh
        int32 heroismMs{0};
        int32 weakenedMs{0};       // Stomp / Howl
        int32 halfTakenMs{0};      // Water / Fire Shield
        int32 absorbMs{0};
        int32 absorbLeft{0};
        bool controlled{false};
    };

    struct Fire { int r, c; int32 endMs; int32 nextTickMs; };
    struct Dot { uint64 target; int32 nextMs; int ticks; int32 amount; };

    enum class End { Win, KingDied, Timeout };

    struct Result
    {
        End end{End::Timeout};
        int32 ms{0};
        int ours{0}, theirs{0};
        int theirKingPct{100};
        int cheats[3]{};
        int orders{0};
        int rejected{0};
        int kills{0};
        int losses{0};
        int handoffs{0};             // pieces let go for a better one
        std::map<Why, int> byWhy;
    };

    class Sim
    {
    public:
        // `bots`: how many raid members take pieces (0 = nobody plays: the
        // uncontrolled raid pieces stand as turrets, the model's floor).
        Sim(uint32 seed, Tunables const& t, int bots = RAID_BOTS) : _rng(seed), _t(t), _botCount(bots) { Setup(); }

        Result Run()
        {
            while (_now < GAME_LIMIT_MS && !_over)
            {
                _now += STEP_MS;
                Tick();
            }
            _res.ms = _now;
            if (!_over)
                _res.end = End::Timeout;
            for (SP const& p : _p)
                if (p.alive)
                {
                    (p.ours ? _res.ours : _res.theirs) += 1;
                    if (!p.ours && p.info->kind == Kind::King)
                        _res.theirKingPct = 100 * p.hp / static_cast<int32>(p.info->maxHp);
                }
            return _res;
        }

    private:
        Rng _rng;
        Tunables _t;
        std::vector<SP> _p;
        std::array<std::array<int, N>, N> _at{};
        std::vector<Fire> _fires;
        std::vector<Dot> _dots;
        Ledger _ledger;
        int32 _now{0};
        int32 _lastKillMs{0};
        int32 _cheatMs{0};
        int32 _policyMs{0};
        bool _over{false};
        Result _res;
        int _botCount{RAID_BOTS};
        struct Bot { int piece{-1}; int32 readyMs{0}; };
        std::vector<uint64> _marched;
        std::vector<Bot> _bots;

        // --- setup -----------------------------------------------------------

        void Setup()
        {
            for (auto& row : _at)
                row.fill(-1);
            Kind const back[8] = { Kind::Rook, Kind::Knight, Kind::Bishop, Kind::Queen,
                                   Kind::King, Kind::Bishop, Kind::Knight, Kind::Rook };
            for (int c = 0; c < N; ++c)
            {
                Add(back[c], true, 0, c);
                Add(Kind::Pawn, true, 1, c);
                Add(Kind::Pawn, false, 6, c);
                Add(back[c], false, 7, c);
            }
            _cheatMs = _rng.U(45000, 100000);

            // Who takes what: the policy's own assignment, staggered as bots walk
            // to their pieces; the King at once (taking it starts the game).
            Board const b = BuildBoard();
            std::vector<Seat> seats;
            for (int i = 0; i < _botCount; ++i)
                seats.push_back({ static_cast<uint64>(1000 + i), i == _botCount - 1, true, 0 });
            std::vector<Assignment> const a = Assign(b, seats, {}, _t.controlCentrePawns, _marched);
            _bots.resize(_botCount);
            for (size_t k = 0; k < a.size(); ++k)
            {
                int const idx = IndexOfGuid(a[k].piece);
                _bots[a[k].bot - 1000].piece = idx;
                _bots[a[k].bot - 1000].readyMs = static_cast<int32>(k) * TAKE_STAGGER_MS;
            }
        }

        void Add(Kind k, bool ours, int r, int c)
        {
            SP p;
            p.guid = _p.size() + 1;
            p.info = InfoOf(EntryOf(k, ours ? Side::Alliance : Side::Horde));
            p.ours = ours;
            p.r = r;
            p.c = c;
            p.facing = ours ? ORI_NW : ORI_SE;
            p.hp = static_cast<int32>(p.info->maxHp);
            p.meleeMs = _rng.U(0, MELEE_PERIOD_MS);
            if (!ours)
                p.moveTimer = _rng.U(8000, 20000);
            // InitializeCombatSpellsByEntry.
            switch (k)
            {
                case Kind::Pawn:   p.base1 = _rng.U(7000, 27000); p.base2 = _rng.U(3000, 5000); break;
                case Kind::King:   p.base1 = _rng.U(13000, 15000); p.base2 = _rng.U(3000, 5000); break;
                case Kind::Bishop: p.base1 = _rng.U(7000, 15000); p.base2 = _rng.U(15000, 90000); break;
                default:           p.base1 = _rng.U(7000, 15000); p.base2 = _rng.U(7000, 27000); break;
            }
            p.t1 = p.base1;
            p.t2 = p.base2;
            _at[r][c] = static_cast<int>(_p.size());
            _p.push_back(p);
        }

        int IndexOfGuid(uint64 guid) const
        {
            for (size_t i = 0; i < _p.size(); ++i)
                if (_p[i].guid == guid)
                    return static_cast<int>(i);
            return -1;
        }

        // --- geometry --------------------------------------------------------

        static void Vec(int dr, int dc, float& x, float& y)
        {
            x = COL_DX * dc + ROW_DX * dr;
            y = COL_DY * dc + ROW_DY * dr;
        }

        static float Yd(SP const& a, SP const& b)
        {
            float x, y;
            Vec(b.r - a.r, b.c - a.c, x, y);
            return std::sqrt(x * x + y * y);
        }

        // Unit::HasInArc(pi/3): within 30 degrees either side of the facing.
        static bool InFront(SP const& a, SP const& b, float halfArc = 0.5236f)
        {
            float fx, fy, tx, ty;
            Vec(ORI_STEP[a.facing].dr, ORI_STEP[a.facing].dc, fx, fy);
            Vec(b.r - a.r, b.c - a.c, tx, ty);
            float const dot = fx * tx + fy * ty;
            float const cross = fx * ty - fy * tx;
            return std::fabs(std::atan2(cross, dot)) <= halfArc + 1e-4f;
        }

        // GetEnemyPiece / GetPiece, over the pieces on the board.
        int Closest(int src, float maxYd, bool checkFront) const
        {
            int best = -1;
            float bestD = maxYd;
            for (size_t i = 0; i < _p.size(); ++i)
            {
                SP const& q = _p[i];
                if (!q.alive || q.ours == _p[src].ours)
                    continue;
                if (checkFront && !InFront(_p[src], q))
                    continue;
                float const d = Yd(_p[src], q);
                if (d < bestD)
                {
                    bestD = d;
                    best = static_cast<int>(i);
                }
            }
            return best;
        }

        int RandomEnemy(int src, float maxYd)
        {
            std::vector<int> v;
            for (size_t i = 0; i < _p.size(); ++i)
                if (_p[i].alive && _p[i].ours != _p[src].ours && Yd(_p[src], _p[i]) < maxYd)
                    v.push_back(static_cast<int>(i));
            if (v.empty())
                return -1;
            return v[_rng.U(0, static_cast<int>(v.size()) - 1)];
        }

        // --- damage ---------------------------------------------------------------

        float DoneMod(SP const& a) const
        {
            float m = 1.0f;
            if (a.handMs > 0) m *= 3.0f;
            if (a.heroismMs > 0) m *= 1.5f;
            if (a.weakenedMs > 0) m *= 0.5f;
            return m;
        }

        void Damage(int target, int32 amount, int attacker, bool physical)
        {
            SP& t = _p[target];
            if (!t.alive)
                return;
            float v = static_cast<float>(amount);
            if (attacker >= 0)
                v *= DoneMod(_p[attacker]);
            if (t.halfTakenMs > 0)
                v *= 0.5f;
            int32 dmg = static_cast<int32>(v);
            if (physical && t.absorbMs > 0 && t.absorbLeft > 0)
            {
                int32 const a = std::min(t.absorbLeft, dmg);
                t.absorbLeft -= a;
                dmg -= a;
            }
            t.hp -= dmg;
            if (t.hp <= 0)
                Kill(target);
        }

        void Heal(int target, int32 amount)
        {
            SP& t = _p[target];
            if (t.alive)
                t.hp = std::min<int32>(t.hp + amount, static_cast<int32>(t.info->maxHp));
        }

        void Kill(int idx)
        {
            SP& p = _p[idx];
            p.alive = false;
            p.moving = false;
            _at[p.r][p.c] = -1;
            _lastKillMs = _now;
            (p.ours ? _res.losses : _res.kills) += 1;
            if (p.info->kind == Kind::King)
            {
                _over = true;
                _res.end = p.ours ? End::KingDied : End::Win;
                return;
            }
            // Its bot is teleported out and comes back for the next piece.
            for (Bot& b : _bots)
                if (b.piece == idx)
                {
                    b.piece = -1;
                    b.readyMs = _now + RECONTROL_MS;
                }
        }

        // --- abilities --------------------------------------------------------

        void FrontCone(int src, float radius, int maxTargets, int32 amount, bool physical, bool weaken)
        {
            SP const& s = _p[src];
            Step const st = ORI_STEP[s.facing];
            int hits = 0;
            for (int k = 1; k < N && hits < maxTargets; ++k)
            {
                int const r = s.r + st.dr * k, c = s.c + st.dc * k;
                if (!OnBoard(r, c) || CellDist(st.dr * k, st.dc * k) > radius)
                    break;
                int const q = _at[r][c];
                if (q < 0 || _p[q].ours == s.ours)
                    continue;
                ++hits;
                if (weaken)
                    _p[q].weakenedMs = 20000;
                else
                    Damage(q, amount, src, physical);
            }
        }

        void AreaAround(int src, float radius, int maxTargets, int32 amount, bool friendsBuff)
        {
            int hits = 0;
            SP const& s = _p[src];
            for (size_t i = 0; i < _p.size() && hits < maxTargets; ++i)
            {
                SP& q = _p[i];
                if (!q.alive || (static_cast<int>(i) == src && !friendsBuff))
                    continue;
                if ((q.ours == s.ours) != friendsBuff)
                    continue;
                if (Yd(s, q) > radius + 0.5f)
                    continue;
                ++hits;
                if (friendsBuff)
                    q.heroismMs = 10000;
                else
                    Damage(static_cast<int>(i), amount, src, false);
            }
        }

        void Dots(int src, int target, float radius)
        {
            // Rain of Fire / Poison Cloud: 3000 at 5s and 10s on the enemies around
            // the target (the damage mod is the caster's at cast time).
            float const mod = DoneMod(_p[src]);
            for (size_t i = 0; i < _p.size(); ++i)
                if (_p[i].alive && _p[i].ours != _p[src].ours && Yd(_p[target], _p[i]) <= radius + 0.5f)
                    _dots.push_back({ _p[i].guid, _now + 5000, 2, static_cast<int32>(3000 * mod) });
        }

        // One ability cast by piece `src` — by the policy (target given) or by the
        // core's timers (target chosen as the core chooses it).
        void Cast(int src, uint32 spell, int target)
        {
            SP& s = _p[src];
            Kind const k = s.info->kind;
            bool const dmgSpell = spell == s.info->dmg.spell;
            switch (k)
            {
                case Kind::Pawn:
                    if (dmgSpell)
                        FrontCone(src, 8.0f, 1, 1000, true, false);
                    else
                    {
                        s.absorbMs = 5000;
                        s.absorbLeft = 500;
                    }
                    break;
                case Kind::Rook:
                    if (dmgSpell)
                        AreaAround(src, 9.0f, 8, 3000, false);
                    else
                        s.halfTakenMs = 5000;
                    break;
                case Kind::Knight:
                    if (dmgSpell)
                        FrontCone(src, 8.0f, 1, 3000, true, false);
                    else
                        FrontCone(src, 10.0f, 3, 0, false, true);
                    break;
                case Kind::Bishop:
                    if (dmgSpell)
                        FrontCone(src, 18.0f, 3, 2000, false, false);
                    else if (target >= 0)
                        Heal(target, 12000);
                    break;
                case Kind::Queen:
                    if (target < 0)
                        break;
                    if (dmgSpell)
                        Damage(target, 4000, src, false);
                    else
                        Dots(src, target, s.info->util.radius);
                    break;
                case Kind::King:
                    if (dmgSpell)
                        AreaAround(src, 10.0f, 3, 4000, false);
                    else
                        AreaAround(src, 8.0f, 9, 0, true);
                    break;
                default:
                    break;
            }
        }

        // Which ability each of the core's two timers casts (UpdateAI,
        // boss_chess_event.cpp:1727-1838). Timer 1: the pawns' block, the Kings'
        // Heroism, the Alliance knight's Smash but the Horde knight's Howl, the
        // Queens' single-target nuke, the Bishops' cone, the Rooks' area. Timer 2
        // is the other one.
        static uint32 TimerSpell(SP const& p, int timer)
        {
            PieceInfo const& i = *p.info;
            bool first;
            switch (i.kind)
            {
                case Kind::Pawn:   first = false; break;   // t1 = util
                case Kind::King:   first = false; break;   // t1 = Heroism (util)
                case Kind::Knight: first = p.ours; break;  // A: t1 Smash; H: t1 Howl
                default:           first = true; break;    // t1 = dmg
            }
            bool const dmg = timer == 1 ? first : !first;
            return dmg ? i.dmg.spell : i.util.spell;
        }

        // The core's UpdateAI timers for an uncontrolled piece (both sides).
        void AutoCast(int idx)
        {
            SP& p = _p[idx];
            bool const raid = p.ours;
            auto jitter = [&]() { return raid ? _rng.U(6000, 12000) : _rng.U(3000, 6000); };
            for (int timer = 1; timer <= 2 && _p[idx].alive && !_over; ++timer)
            {
                int32& t = timer == 1 ? p.t1 : p.t2;
                t -= STEP_MS;
                if (t > 0)
                    continue;
                t = (timer == 1 ? p.base1 : p.base2) + jitter();
                uint32 const spell = TimerSpell(p, timer);
                int target = -1;
                if (p.info->kind == Kind::Queen)
                {
                    // Timer 1: the closest enemy in front within 20yd; timer 2: a
                    // random enemy within 25yd, facing ignored. No target, no cast.
                    target = timer == 1 ? Closest(idx, 20.0f, true) : RandomEnemy(idx, 25.0f);
                    if (target < 0)
                        continue;
                }
                else if (p.info->kind == Kind::Bishop && spell == p.info->util.spell)
                {
                    // The friendly piece missing the most (over 5000) within 25yd.
                    int32 most = 5000;
                    for (size_t i = 0; i < _p.size(); ++i)
                    {
                        SP const& f = _p[i];
                        int32 const missing = static_cast<int32>(f.info->maxHp) - f.hp;
                        if (f.alive && f.ours == p.ours && missing > most && Yd(p, f) < 25.0f)
                        {
                            most = missing;
                            target = static_cast<int>(i);
                        }
                    }
                    if (target < 0)
                        continue;
                }
                Cast(idx, spell, target);
            }
        }

        // --- Medivh's movement AI (HandlePieceMoveByAI) -----------------------------

        void MedivhMove(int idx)
        {
            SP& p = _p[idx];
            if (p.moving || !p.moveTimer)
                return;
            p.moveTimer -= STEP_MS;
            if (p.moveTimer > 0)
                return;

            Ori o = p.facing;
            // At the far edge: turn around (a Change Facing on a trigger, which
            // lands) — and the move timer is zeroed with no arrival to restart it.
            if ((o == ORI_SE && p.r == 0) || (o == ORI_NW && p.r == 7) ||
                (o == ORI_SW && p.c == 0) || (o == ORI_NE && p.c == 7))
            {
                p.facing = static_cast<Ori>((o + 4) % ORI_COUNT);
                p.moveTimer = 0;
                return;
            }
            // The 50% "turn toward an enemy beside me" — its cast is refused by
            // the spell's conditions, but the branch returns true: frozen.
            if (_rng.U(0, 1) && HasSideEnemy(idx))
            {
                p.moveTimer = 0;
                return;
            }
            if (o == ORI_W || o == ORI_N)
                o = ORI_NW;
            else if (o == ORI_S || o == ORI_E)
                o = ORI_SE;

            bool const queen = p.info->kind == Kind::Queen;
            bool const knight = p.info->kind == Kind::Knight;
            int rr = p.r, cc = p.c;
            if (o == ORI_SE || o == ORI_NW)
            {
                rr += o == ORI_SE ? -1 : 1;
                switch (_rng.U(0, 2))
                {
                    case 0: cc -= queen ? _rng.U(1, 2) : 1; break;
                    case 1: cc += queen ? _rng.U(1, 2) : 1; break;
                    default:
                        if (queen)
                            rr += o == ORI_SE ? _rng.U(-2, 0) : _rng.U(0, 2);
                        else if (knight && _rng.U(0, 1))
                            rr += o == ORI_SE ? -1 : 1;
                        break;
                }
            }
            else
            {
                cc += o == ORI_SW ? -1 : 1;
                switch (_rng.U(0, 2))
                {
                    case 0: rr -= queen ? _rng.U(1, 2) : 1; break;
                    case 1: rr += queen ? _rng.U(1, 2) : 1; break;
                    default:
                        if (queen)
                            cc += o == ORI_SW ? _rng.U(-2, 0) : _rng.U(0, 2);
                        else if (knight && _rng.U(0, 1))
                            cc += o == ORI_SW ? -1 : 1;
                        break;
                }
            }
            rr = std::clamp(rr, 0, N - 1);
            cc = std::clamp(cc, 0, N - 1);
            if (_at[rr][cc] >= 0)
            {
                p.moveTimer = 1000;  // taken: re-check after a second
                return;
            }
            StartMove(idx, rr, cc);
            p.moveTimer = 0;  // restarted on arrival (MovementInform)
        }

        bool HasSideEnemy(int idx) const
        {
            // GetHostileTargetForChangeFacing: the four cells to the piece's sides
            // and back, per its facing (boss_chess_event.cpp:404-429).
            static std::array<std::array<Ori, 4>, ORI_COUNT> const dirs = { {
                { ORI_NE, ORI_E, ORI_S, ORI_SW }, { ORI_E, ORI_SE, ORI_SW, ORI_W },
                { ORI_SE, ORI_S, ORI_W, ORI_NW }, { ORI_NE, ORI_SW, ORI_NW, ORI_N },
                { ORI_SW, ORI_W, ORI_N, ORI_NE }, { ORI_W, ORI_NW, ORI_NE, ORI_E },
                { ORI_NW, ORI_N, ORI_E, ORI_SE }, { ORI_N, ORI_NE, ORI_SE, ORI_S },
            } };
            SP const& p = _p[idx];
            for (Ori o : dirs[p.facing])
            {
                int const r = p.r + ORI_STEP[o].dr, c = p.c + ORI_STEP[o].dc;
                if (!OnBoard(r, c))
                    continue;
                int const q = _at[r][c];
                if (q >= 0 && _p[q].ours != p.ours)
                    return true;
            }
            return false;
        }

        void StartMove(int idx, int r, int c)
        {
            SP& p = _p[idx];
            float const yd = CellDist(r - p.r, c - p.c);
            p.facing = OriOfStep(r - p.r, c - p.c);
            _at[p.r][p.c] = -1;
            _at[r][c] = idx;
            p.r = r;
            p.c = c;
            p.moving = true;
            p.arriveMs = _now + static_cast<int32>(yd / WALK_YD_PER_MS);
        }

        // --- cheats -------------------------------------------------------------

        void Cheat()
        {
            int const which = _rng.U(0, 2);
            ++_res.cheats[which];
            if (which == 0)
            {
                for (SP& p : _p)
                    if (p.alive && !p.ours && p.info->kind == Kind::King)
                        p.hp = static_cast<int32>(p.info->maxHp);
                return;
            }
            std::vector<int> v;
            for (size_t i = 0; i < _p.size(); ++i)
                if (_p[i].alive && _p[i].ours == (which == 1))
                    v.push_back(static_cast<int>(i));
            std::shuffle(v.begin(), v.end(), _rng.g);
            size_t const n = which == 1 ? 3u : static_cast<size_t>(_rng.U(1, 4));
            for (size_t k = 0; k < v.size() && k < n; ++k)
            {
                SP& p = _p[v[k]];
                if (which == 1)
                    _fires.push_back({ p.r, p.c, _now + FIRE_DURATION_MS, _now + FIRE_TICK_MS });
                else
                    p.handMs = 90000;
            }
        }

        // --- the policy -----------------------------------------------------------

        Board BuildBoard() const
        {
            Board b;
            b.Clear(false);
            for (SP const& s : _p)
            {
                if (!s.alive)
                    continue;
                Piece q;
                q.guid = s.guid;
                q.entry = s.info->entry;
                q.kind = s.info->kind;
                q.ours = s.ours;
                q.hp = static_cast<uint32>(std::max<int32>(s.hp, 0));
                q.maxHp = s.info->maxHp;
                q.buffed = s.handMs > 0;
                q.controlled = s.controlled;
                q.moving = s.moving;
                q.shielded = s.halfTakenMs > 0 || (s.absorbMs > 0 && s.absorbLeft > 0);
                q.weakened = s.weakenedMs > 0;
                b.Add(q, s.r, s.c, s.facing);
            }
            for (Fire const& f : _fires)
                b.SetBurningReal(f.r, f.c);
            return b;
        }

        void Policy()
        {
            Board const b = BuildBoard();
            uint32 const now = static_cast<uint32>(_now) + 1;
            Decision const d = Decide(b, _ledger, Clock{ now, 1, static_cast<uint32>(_lastKillMs) + 1 }, _t);
            for (Order const& o : d.orders)
            {
                int const idx = IndexOfGuid(o.piece);
                if (idx < 0 || !_p[idx].alive || !_p[idx].controlled)
                    continue;
                SP& p = _p[idx];
                ++_res.orders;
                ++_res.byWhy[o.why];
                switch (o.kind)
                {
                    case OrderKind::Move:
                        // HandlePieceMove: the cell must be empty, the step within reach.
                        if (!OnBoard(o.r, o.c) || _at[o.r][o.c] >= 0 ||
                            Chebyshev(o.r - p.r, o.c - p.c) > p.info->reach || p.moving)
                        {
                            ++_res.rejected;
                            break;
                        }
                        StartMove(idx, o.r, o.c);
                        _ledger.OnAction(p.guid, now);
                        break;
                    case OrderKind::Face:
                    {
                        Ori const f = OriOfStep(o.r - p.r, o.c - p.c);
                        if (f != ORI_COUNT)
                            p.facing = f;
                        _ledger.OnAction(p.guid, now);
                        break;
                    }
                    case OrderKind::Cast:
                        Cast(idx, o.spell, o.target ? IndexOfGuid(o.target) : -1);
                        _ledger.OnCast(p.guid, o.spell, *p.info, now);
                        break;
                    default:
                        break;
                }
                if (_over)
                    return;
            }
        }

        void Control()
        {
            // Bots take (or re-take) pieces when their delay is up.
            Board const b = BuildBoard();
            std::vector<Seat> seats;
            for (int i = 0; i < _botCount; ++i)
            {
                Bot const& bot = _bots[i];
                seats.push_back({ static_cast<uint64>(1000 + i), i == _botCount - 1, true,
                                  bot.piece >= 0 ? _p[bot.piece].guid : 0 });
            }
            NoteMarched(b, _marched, _t);
            std::vector<Assignment> const a = Assign(b, seats, {}, _t.controlCentrePawns, _marched);
            for (Assignment const& x : a)
            {
                Bot& bot = _bots[x.bot - 1000];
                int const idx = IndexOfGuid(x.piece);
                // Given a better piece than the one it holds: let go, and pay what
                // a death costs (the teleport, Recently In Game, the walk back).
                if (bot.piece >= 0 && _now >= bot.readyMs && idx >= 0 && idx != bot.piece &&
                    !_p[idx].controlled)
                {
                    bot.piece = -1;
                    bot.readyMs = _now + RECONTROL_MS;
                    ++_res.handoffs;
                    continue;
                }
                if (bot.piece < 0 && _now >= bot.readyMs && idx >= 0 && !_p[idx].controlled)
                    bot.piece = idx;
            }
            for (SP& p : _p)
                p.controlled = false;
            for (Bot const& bot : _bots)
                if (bot.piece >= 0 && _now >= bot.readyMs && _p[bot.piece].alive)
                    _p[bot.piece].controlled = true;
        }

        // --- the clock ------------------------------------------------------------

        void Tick()
        {
            // Arrivals: MovementInform restarts Medivh's move timer.
            for (SP& p : _p)
                if (p.alive && p.moving && _now >= p.arriveMs)
                {
                    p.moving = false;
                    if (!p.ours && !p.controlled && !p.moveTimer)
                        p.moveTimer = _rng.U(10000, 20000);
                }

            // Buff clocks.
            for (SP& p : _p)
            {
                for (int32* t : { &p.handMs, &p.heroismMs, &p.weakenedMs, &p.halfTakenMs, &p.absorbMs })
                    *t = std::max(0, *t - STEP_MS);
            }

            // Melee: every 3s, the closest enemy inside the 60-degree front arc.
            for (size_t i = 0; i < _p.size() && !_over; ++i)
            {
                SP& p = _p[i];
                if (!p.alive)
                    continue;
                p.meleeMs -= STEP_MS;
                if (p.meleeMs > 0)
                    continue;
                p.meleeMs += MELEE_PERIOD_MS;
                if (p.moving)
                    continue;
                int const t = Closest(static_cast<int>(i), 10.0f, true);
                if (t >= 0)
                    Damage(t, MELEE_DAMAGE, static_cast<int>(i), true);
            }

            // Uncontrolled pieces' ability timers, and Medivh's marching.
            for (size_t i = 0; i < _p.size() && !_over; ++i)
            {
                if (!_p[i].alive || _p[i].controlled)
                    continue;
                AutoCast(static_cast<int>(i));
                if (_p[i].alive && !_p[i].ours)
                    MedivhMove(static_cast<int>(i));
            }

            // Fire and DoTs.
            for (Fire& f : _fires)
                if (_now >= f.nextTickMs && _now <= f.endMs)
                {
                    f.nextTickMs += FIRE_TICK_MS;
                    int const q = _at[f.r][f.c];
                    if (q >= 0 && !_p[q].moving)
                        Damage(q, FIRE_DAMAGE, -1, false);
                }
            _fires.erase(std::remove_if(_fires.begin(), _fires.end(),
                                        [&](Fire const& f) { return _now > f.endMs; }),
                         _fires.end());
            for (Dot& d : _dots)
                if (d.ticks > 0 && _now >= d.nextMs)
                {
                    --d.ticks;
                    d.nextMs += 5000;
                    int const q = IndexOfGuid(d.target);
                    if (q >= 0)
                        Damage(q, d.amount, -1, false);
                }
            _dots.erase(std::remove_if(_dots.begin(), _dots.end(), [](Dot const& d) { return d.ticks <= 0; }),
                        _dots.end());
            if (_over)
                return;

            _cheatMs -= STEP_MS;
            if (_cheatMs <= 0)
            {
                Cheat();
                _cheatMs = _rng.U(45000, 100000);
            }

            if (_now - _policyMs >= POLICY_MS)
            {
                _policyMs = _now;
                Control();
                Policy();
            }
        }
    };

    struct Summary
    {
        int games{0}, wins{0}, kingDied{0}, timeout{0};
        std::vector<int32> winMs;
        long orders{0}, rejected{0}, handoffs{0};
        int cheats[3]{};
        std::map<Why, long> byWhy;
        int timeoutTheirKingPct{0};
        int timeoutMaterial{0};

        double WinRate() const { return games ? static_cast<double>(wins) / games : 0.0; }
        int32 MedianWinMs()
        {
            if (winMs.empty())
                return 0;
            std::sort(winMs.begin(), winMs.end());
            return winMs[winMs.size() / 2];
        }
    };

    Summary Play(int games, Tunables const& t, uint32 firstSeed = 1, int bots = RAID_BOTS)
    {
        Summary s;
        for (int g = 0; g < games; ++g)
        {
            Result const r = Sim(firstSeed + static_cast<uint32>(g), t, bots).Run();
            ++s.games;
            s.orders += r.orders;
            s.rejected += r.rejected;
            s.handoffs += r.handoffs;
            for (int k = 0; k < 3; ++k)
                s.cheats[k] += r.cheats[k];
            for (auto const& [w, n] : r.byWhy)
                s.byWhy[w] += n;
            switch (r.end)
            {
                case End::Win:
                    ++s.wins;
                    s.winMs.push_back(r.ms);
                    break;
                case End::KingDied:
                    ++s.kingDied;
                    break;
                case End::Timeout:
                    ++s.timeout;
                    s.timeoutTheirKingPct += r.theirKingPct;
                    s.timeoutMaterial += r.ours - r.theirs;
                    break;
            }
        }
        return s;
    }

    void Print(char const* label, Summary& s)
    {
        std::printf("%s: %d games, win %.1f%% (median %.0fs), our King died %d, timeout %d",
                    label, s.games, 100.0 * s.WinRate(), s.MedianWinMs() / 1000.0, s.kingDied, s.timeout);
        if (s.timeout)
            std::printf(" (their King at %d%%, material %+.1f on average)",
                        s.timeoutTheirKingPct / s.timeout, static_cast<double>(s.timeoutMaterial) / s.timeout);
        std::printf("\n  orders %ld (rejected %ld); handoffs %.1f/game; cheats heal/fire/buff %d/%d/%d; by rung:",
                    s.orders, s.rejected, s.games ? static_cast<double>(s.handoffs) / s.games : 0.0, s.cheats[0],
                    s.cheats[1], s.cheats[2]);
        for (auto const& [w, n] : s.byWhy)
            std::printf(" %s=%ld", WhyName(w), n);
        std::printf("\n");
    }
}

// The plan's bar (section 5): the shipped policy wins at least 95% of 500 seeded
// games within 12 minutes of game time.
TEST(DcKarazhanChessSim, TheShippedPolicyWinsNinetyFivePercent)
{
    Summary s = Play(500, Tunables{});
    Print("shipped", s);
    EXPECT_GE(s.WinRate(), 0.95);
}

// THE MODEL'S FLOORS, printed for the record. A raid that takes no pieces at all
// loses (its uncontrolled pieces are turrets that never step off the fire, and
// Medivh's heal-King cheat outlasts what they deal) — if that ever starts winning,
// the model is broken, not the policy good. A raid that plays but never goes for
// his King (HOLD forever) still wins most games, because the core's Medivh AI
// marches every piece, his King included, into our lines; what ASSAULT buys on
// top is TIME, the King dead minutes sooner, which keeps a live game inside the
// scenario's budget and away from the heal-King cheat.
TEST(DcKarazhanChessSim, ThePolicyBeatsItsFloors)
{
    Summary passive = Play(100, Tunables{}, 7001, /*bots*/ 0);
    Print("no bots", passive);
    Tunables holdOnly;
    holdOnly.assaultEnemyNonKingMax = -1;
    holdOnly.assaultMaterialLead = 99;
    holdOnly.assaultKingDistYd = -1.0f;
    holdOnly.stalemateNoKillMs = 0x7FFFFFFF;
    Summary hold = Play(100, holdOnly, 7001);
    Print("hold-only", hold);
    Summary shipped = Play(100, Tunables{}, 7001);
    Print("shipped (same seeds)", shipped);

    EXPECT_LT(passive.WinRate(), 0.5) << "a raid that plays no piece must not win";
    EXPECT_GE(shipped.WinRate(), passive.WinRate());
    EXPECT_GE(shipped.WinRate(), hold.WinRate());
    EXPECT_LT(shipped.MedianWinMs(), hold.MedianWinMs()) << "ASSAULT must end the game sooner";
}

// The simulator is deterministic per seed, so a failure can be replayed.
TEST(DcKarazhanChessSim, SameSeedSameGame)
{
    Result const a = Sim(42, Tunables{}).Run();
    Result const b = Sim(42, Tunables{}).Run();
    EXPECT_EQ(int(a.end), int(b.end));
    EXPECT_EQ(a.ms, b.ms);
    EXPECT_EQ(a.orders, b.orders);
}
