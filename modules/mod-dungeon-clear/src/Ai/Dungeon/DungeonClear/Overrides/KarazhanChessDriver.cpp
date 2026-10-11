/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include <algorithm>
#include <array>
#include <cmath>
#include <list>
#include <string>
#include <unordered_map>
#include <vector>

#include "CharmInfo.h"
#include "Creature.h"
#include "GameObject.h"
#include "Group.h"
#include "InstanceScript.h"
#include "Log.h"
#include "Map.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "ObjectAccessor.h"
#include "PathGenerator.h"
#include "Pet.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "SharedDefines.h"
#include "Timer.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonBossInfo.h"
#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"
#include "Ai/Dungeon/DungeonClear/DcValueKeys.h"
#include "Ai/Dungeon/DungeonClear/Overrides/ObjectiveHookRegistry.h"
#include "Ai/Dungeon/DungeonClear/Util/DcChessBoard.h"
#include "Ai/Dungeon/DungeonClear/Util/DcChessConductor.h"
#include "Ai/Dungeon/DungeonClear/Util/DcChessDecision.h"
#include "Ai/Dungeon/DungeonClear/Util/DcLeaderSignal.h"
#include "Ai/Dungeon/DungeonClear/Util/DcMovement.h"
#include "Ai/Dungeon/DungeonClear/Util/DcRun.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonEventExecutor.h"

// Karazhan's chess event (map 532): the CONDUCTOR and the members' seats — the
// glue around the pure kernels DcChessBoard.h (the board), DcChessDecision.h (the
// policy) and DcChessConductor.h (the game's state machine). Plan:
// deployment-files/docs/mod-dungeon-clear_karazhan-chess_plan.md, sections 3.3
// and 3.4.
//
// WHERE IT RUNS. Not in the objective action. An anchored objective is driven only
// by the leader's non-combat objective rung, and the game keeps the raid combat-
// flagged half the time (the charm), holds every bot at a sideline for ten
// minutes, and must own every one of their ticks so nobody casts a spell of their
// own at a piece (H1). So the whole game runs inside ONE member rung
// (Action/DcChessPieceAction.cpp, relevance KzChess, both engines): on every bot
// in the hall it holds the bot's seat, and on the RUN OWNER it also ticks the
// conductor, every 500ms. The event's two Custom hooks only arm it (43) and hand
// its verdict back to the event (42) once the rung lets go of the leader's tick.
//
// THE CONDUCTOR CASTS THE ORDERS ITSELF. The plan has each member cast its own
// piece's order; doing it here instead keeps one writer for the cooldown ledger,
// sees a refused move on the next tick instead of a round trip later, and needs
// no order mailbox. The members' part is to take their piece and keep it — the
// charm is the only thing the policy cannot do from the leader's tick.
//
// DIAGNOSTICS (the triage rule: every failure diagnosable from dc_test_run.py):
//   * `DC_CHESS state=...` every 5s: phase, posture, attempt, material, both
//     Kings' HP, the cheats seen, controllers, orders.
//   * one line per move / turn (piece, cell, why, the cast result) and per
//     refused move; casts at DEBUG.
//   * every state transition, every piece taken or lost, every gossip give-up.
//   * the run record's `extras` (DcRunState::SetTestExtra) and the event
//     progress sequence (DcRunState::BumpEventProgress) on every accepted move,
//     piece death and phase change.

using namespace DcKarazhan;
using DcChessConductor::Act;
using DcChessConductor::State;

namespace
{
    constexpr uint32 TICK_MS = 500;
    constexpr uint32 LOG_MS = 5000;
    constexpr uint32 ECHO_GOSSIP_FLOOR_MS = 3000;
    constexpr uint32 OPEN_FLOOR_MS = 6000;
    constexpr uint32 PIECE_GOSSIP_FLOOR_MS = 3000;
    constexpr uint8  PIECE_GOSSIP_TRIES = 5;
    constexpr uint32 SEAT_TELEPORT_FLOOR_MS = 2000;
    constexpr uint32 CONFIRM_AFTER_MS = 300;
    constexpr uint32 CONFIRM_MAX_MS = 2500;
    constexpr uint32 HELD_STAMP_MS = 1000;
    constexpr float  INTERACT_YD = 4.0f;       // gossip and loot reach, with margin
    constexpr float  SLOT_ARRIVE_YD = 2.0f;
    constexpr float  SCAN_YD = 90.0f;          // the whole hall from anywhere in it
    constexpr float  HALL_FLOOR_Z = 221.4f;
    constexpr float  SEAT_DROP_YD = 3.0f;      // teleport above the floor, never into it

    // "Opening" for LOCKTYPE_OPEN (effect 0 misc 5) — what lock 57 on the Dust
    // Covered Chest accepts (types 5 OPEN and 6 TREASURE). NOT mod-playerbots'
    // fallback 6477, which is LOCKTYPE_OPEN_TINKERING and is refused here.
    constexpr uint32 SPELL_OPENING = 3365;
    constexpr uint32 SPELL_STOMP = 37498, SPELL_HOWL = 37502;

    constexpr std::array<uint32, 12> PIECE_ENTRIES = {
        DcChess::NPC_PAWN_A, DcChess::NPC_PAWN_H, DcChess::NPC_ROOK_A, DcChess::NPC_ROOK_H,
        DcChess::NPC_KNIGHT_A, DcChess::NPC_KNIGHT_H, DcChess::NPC_BISHOP_A, DcChess::NPC_BISHOP_H,
        DcChess::NPC_QUEEN_A, DcChess::NPC_QUEEN_H, DcChess::NPC_KING_A, DcChess::NPC_KING_H,
    };

    void BoardCentre(float& x, float& y) { DcChess::CenterOf(3.5f, 3.5f, x, y); }

    bool InHall(Player* bot)
    {
        if (!bot || !bot->IsInWorld() || bot->GetMapId() != MAP)
            return false;
        float const z = bot->GetPositionZ();
        if (z < HALL_Z_MIN || z > HALL_Z_MAX)
            return false;
        float x, y;
        BoardCentre(x, y);
        return bot->GetExactDist2d(x, y) <= CHESS_HALL_RADIUS;
    }

    // Where every bot waits and stands while it holds a piece: the hall anchor,
    // the same point the raid arrives at (see HALL_X).
    void StandPoint(float& x, float& y)
    {
        x = HALL_X;
        y = HALL_Y;
    }

    bool IsPiece(Unit const* u) { return u && u->IsCreature() && DcChess::IsPieceEntry(u->GetEntry()); }

    Creature* ControlledPiece(Player* bot)
    {
        Unit* charm = bot ? bot->GetCharm() : nullptr;
        return IsPiece(charm) ? charm->ToCreature() : nullptr;
    }

    // A walk inside the hall. Every leg here is under ~70yd (the balcony stair is
    // the longest), inside what one generated path delivers, so a plain MovePoint
    // with the same-destination floor is enough.
    void Walk(Player* bot, PlayerbotAI* botAI, float x, float y, float z)
    {
        if (DcRun::Of(botAI).ThrottledIssue(DcThrottle::ChessMoveIssue, x, y, z, 1.0f, 2000))
            return;
        // Diag: bots were seen walking through the hall's walls on full runs.
        // A Player's off-mesh end comes back as a straight line still labelled
        // NORMAL (PATHFIND_NOT_USING_PATH), so name every walk that is not a
        // complete navmesh path.
        PathGenerator path(bot);
        path.CalculatePath(x, y, z, false);
        PathType const type = path.GetPathType();
        if (type & (PATHFIND_NOT_USING_PATH | PATHFIND_SHORTCUT | PATHFIND_INCOMPLETE | PATHFIND_NOPATH))
            LOG_WARN("playerbots.dungeonclear",
                     "DC_CHESS {} walk not on the mesh: type=0x{:X} pts={} from ({:.1f},{:.1f},{:.1f}) to ({:.1f},{:.1f},{:.1f})",
                     bot->GetName(), uint32(type), path.GetPath().size(), bot->GetPositionX(), bot->GetPositionY(),
                     bot->GetPositionZ(), x, y, z);
        bot->GetMotionMaster()->MovePoint(0, x, y, z, FORCED_MOVEMENT_NONE, 0.0f, 0.0f,
                                          /*generatePath*/ true, /*forceDestination*/ false);
    }

    // Walk to within `reach` of (x, y); true once there (and stopped).
    bool Reach(Player* bot, PlayerbotAI* botAI, float x, float y, float z, float reach)
    {
        if (bot->GetExactDist2d(x, y) > reach)
        {
            Walk(bot, botAI, x, y, z);
            return false;
        }
        if (bot->isMoving())
            DcMovement::StopBot(bot, DcMovement::Stop::Hold);
        return true;
    }

    // The members of `leader`'s group who play: bots, alive, in the hall.
    std::vector<Player*> Members(Player* leader)
    {
        std::vector<Player*> out;
        Group* group = leader->GetGroup();
        if (!group)
        {
            if (GET_PLAYERBOT_AI(leader) && leader->IsAlive() && InHall(leader))
                out.push_back(leader);
            return out;
        }
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* m = ref->GetSource();
            if (!m || m->GetMap() != leader->GetMap() || !m->IsAlive() || !GET_PLAYERBOT_AI(m) || !InHall(m))
                continue;
            out.push_back(m);
        }
        return out;
    }

    // Who ticks the conductor. Its state lives on the run owner, dead or alive
    // (FindRunOwner); the body that ticks it and runs its errands is the owner
    // while it is alive in the hall, else the living bot in the hall with the
    // lowest GUID — every member computes the same answer. A tank that died at
    // the board used to freeze the game where it stood (tr-20260926-152116-24).
    Player* ConductorHost(Player* owner)
    {
        if (!owner)
            return nullptr;
        if (owner->IsAlive() && InHall(owner) && GET_PLAYERBOT_AI(owner))
            return owner;
        Group* group = owner->GetGroup();
        if (!group)
            return nullptr;
        Player* host = nullptr;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* m = ref->GetSource();
            if (!m || m == owner || !m->IsAlive() || !GET_PLAYERBOT_AI(m) || !InHall(m))
                continue;
            if (!host || m->GetGUID() < host->GetGUID())
                host = m;
        }
        return host;
    }

    // One tick's worth of the hall, read live.
    struct Hall
    {
        InstanceScript* inst{nullptr};
        uint32 event{0};
        uint32 phase{0};
        DcChess::Side ourSide{DcChess::Side::Alliance};
        Creature* echo{nullptr};
        GameObject* chest{nullptr};
        std::vector<Creature*> pieces;                            // alive and on the board
        std::unordered_map<uint64, Creature*> byGuid;
        std::array<std::array<Creature*, DcChess::N>, DcChess::N> triggers{};  // REAL cells
        int fires{0};
        std::vector<Player*> members;
        DcChess::Board board;
    };

    void ReadHall(Player* leader, Hall& h)
    {
        h.inst = leader->GetInstanceScript();
        if (!h.inst)
            return;
        h.event = h.inst->GetData(DATA_CHESS_EVENT);
        h.phase = h.inst->GetData(DATA_CHESS_GAME_PHASE);
        // The raid's colour is the team of whoever talked to Medivh. Before a game
        // it is unset; the leader is the one who will talk to him.
        uint32 const team = h.phase == CHESS_PHASE_NOT_STARTED ? static_cast<uint32>(leader->GetTeamId())
                                                               : h.inst->GetData(DATA_CHESS_TEAM);
        h.ourSide = team == TEAM_HORDE ? DcChess::Side::Horde : DcChess::Side::Alliance;
        h.echo = leader->FindNearestCreature(NPC_ECHO_OF_MEDIVH, SCAN_YD, /*alive*/ true);
        h.chest = leader->FindNearestGameObject(GO_DUST_COVERED_CHEST, SCAN_YD);
        h.members = Members(leader);

        std::unordered_map<uint64, bool> ourBots;
        for (Player* m : h.members)
            ourBots[m->GetGUID().GetRawValue()] = true;

        h.board.Clear(h.ourSide == DcChess::Side::Horde);

        std::list<Creature*> found;
        leader->GetCreatureListWithEntryInGrid(
            found, std::vector<uint32>(PIECE_ENTRIES.begin(), PIECE_ENTRIES.end()), SCAN_YD);
        for (Creature* c : found)
        {
            // A dead piece is not dead: HandlePieceJustDied teleports it off the
            // board, respawns it and flags it NOT_SELECTABLE.
            if (!c || !c->IsAlive() || c->HasUnitFlag(UNIT_FLAG_NOT_SELECTABLE))
                continue;
            DcChess::PieceInfo const* info = DcChess::InfoOf(c->GetEntry());
            if (!info)
                continue;
            // The core's board moves a piece when its move is ACCEPTED; the body
            // follows at walking pace. Place a walking piece on its destination.
            float x = c->GetPositionX(), y = c->GetPositionY();
            bool moving = false;
            if (c->isMoving() && c->movespline && !c->movespline->Finalized())
            {
                G3D::Vector3 const d = c->movespline->FinalDestination();
                x = d.x;
                y = d.y;
                moving = true;
            }
            int8 r, col;
            if (!DcChess::CellOf(x, y, r, col))
                continue;
            DcChess::Piece p;
            p.guid = c->GetGUID().GetRawValue();
            p.entry = c->GetEntry();
            p.kind = info->kind;
            p.ours = info->side == h.ourSide;
            p.hp = c->GetHealth();
            p.maxHp = c->GetMaxHealth();
            p.buffed = c->HasAura(DcChess::SPELL_HAND_OF_MEDIVH);
            p.controlled = c->IsCharmed() && ourBots.count(c->GetCharmerGUID().GetRawValue());
            p.moving = moving;
            p.shielded = c->HasAura(info->util.spell) && info->util.shape == DcChess::Shape::SelfBuff;
            p.weakened = c->HasAura(SPELL_STOMP) || c->HasAura(SPELL_HOWL);
            if (h.board.Add(p, r, col, DcChess::SnapFacing(c->GetOrientation())))
            {
                h.pieces.push_back(c);
                h.byGuid[p.guid] = c;
            }
        }

        std::list<Creature*> triggers;
        leader->GetCreatureListWithEntryInGrid(triggers, DcChess::NPC_MOVE_TRIGGER, SCAN_YD);
        for (Creature* t : triggers)
        {
            int8 r, col;
            if (t && DcChess::CellOf(t->GetPositionX(), t->GetPositionY(), r, col))
                h.triggers[r][col] = t;
        }

        std::list<Creature*> fires;
        leader->GetCreatureListWithEntryInGrid(fires, DcChess::NPC_FIRE, SCAN_YD);
        for (Creature* f : fires)
        {
            int8 r, col;
            if (f && f->IsAlive() && DcChess::CellOf(f->GetPositionX(), f->GetPositionY(), r, col))
            {
                h.board.SetBurningReal(r, col);
                ++h.fires;
            }
        }
    }

    bool PieceOfGuid(DcChess::Board const& b, uint64 guid, DcChess::Piece const*& out)
    {
        for (DcChess::Piece const& p : b.pieces)
            if (p.guid == guid)
            {
                out = &p;
                return true;
            }
        return false;
    }

    std::string PieceLabel(Creature* c, DcChess::Board const& b)
    {
        DcChess::Piece const* p = nullptr;
        if (!c)
            return "?";
        if (PieceOfGuid(b, c->GetGUID().GetRawValue(), p))
            return c->GetName() + "(" + std::to_string(b.RealRow(p->r)) + "," + std::to_string(p->c) + ")";
        return c->GetName();
    }

    // --- the conductor, one tick -------------------------------------------------

    void Transition(Player* leader, DcRunState& st, State from, DcChessConductor::Step const& step,
                    Hall const& h, uint32 now)
    {
        DcChessRunState& ch = st.chess;
        State const to = step.next;
        bool const newGame = (to == State::TakeKing || to == State::Play) &&
                             (from == State::Idle || from == State::Setup || from == State::Start);
        if (newGame)
        {
            ++ch.attempts;
            ch.gameStartMs = now;
            ch.lastKillMs = now;
            ch.ledger = DcChess::Ledger{};
            ch.pending.clear();
            ch.marched.clear();
            ch.ourCount = ch.theirCount = -1;
        }
        if (to == State::Lost && (from == State::Play || from == State::TakeKing))
            ++ch.losses;
        if (to == State::Restart)
            ++ch.restarts;
        if (to == State::Lost || to == State::Setup)
        {
            ch.assign.clear();
            ch.refused.clear();
        }
        if (to == State::Blocked)
        {
            // Give the pieces back and lift the pacify: a game still on the board
            // must not hold the raid (the rung stands down on a terminal
            // conductor), and a controller released by a lost game had Game In
            // Session re-cast on it by OnCharmed(false) AFTER the event's reset
            // removed it — with no duration, it would outlive the game.
            for (Player* m : h.members)
            {
                if (ControlledPiece(m))
                    m->StopCastingCharm();
                m->RemoveAurasDueToSpell(DcChess::SPELL_GAME_IN_SESSION);
            }
            ch.blockedWhy = step.why;
            LOG_WARN("playerbots.dungeonclear",
                     "DC_CHESS [{}] BLOCKED: {} (attempts={} losses={} restarts={})",
                     leader->GetName(), step.why, ch.attempts, ch.losses, ch.restarts);
        }
        LOG_INFO("playerbots.dungeonclear",
                 "DC_CHESS [{}] {} -> {}: {} (event={} phase={} attempt={} game={}s)", leader->GetName(),
                 DcChessConductor::StateName(from), DcChessConductor::StateName(to), step.why, h.event, h.phase,
                 ch.attempts, ch.gameStartMs ? GetMSTimeDiffToNow(ch.gameStartMs) / 1000 : 0);
        if (to == State::Loot)
            LOG_INFO("playerbots.dungeonclear",
                     "DC_CHESS [{}] WON: attempt {}, {}s of game, losses={} restarts={} cheats h/f/b={}/{}/{}",
                     leader->GetName(), ch.attempts, ch.gameStartMs ? GetMSTimeDiffToNow(ch.gameStartMs) / 1000 : 0,
                     ch.losses, ch.restarts, ch.cheatHeal, ch.cheatFire, ch.cheatBuff);
        ch.state = static_cast<uint8>(to);
        ch.stateMs = now;
        st.BumpEventProgress();
    }

    void PublishExtras(DcRunState& st)
    {
        DcChessRunState const& ch = st.chess;
        st.SetTestExtraNum("attempts", ch.attempts);
        st.SetTestExtraNum("losses", ch.losses);
        st.SetTestExtraNum("restarts", ch.restarts);
        st.SetTestExtraNum("gameTimeS", ch.gameStartMs ? GetMSTimeDiffToNow(ch.gameStartMs) / 1000.0 : 0.0);
        st.SetTestExtraNum("ourMaterial", std::max(ch.ourCount, 0));
        st.SetTestExtraNum("theirMaterial", std::max(ch.theirCount, 0));
        st.SetTestExtraNum("cheatHeal", ch.cheatHeal);
        st.SetTestExtraNum("cheatFire", ch.cheatFire);
        st.SetTestExtraNum("cheatBuff", ch.cheatBuff);
        st.SetTestExtraNum("controllerChurn", ch.churn);
        st.SetTestExtraNum("handoffs", ch.handoffs);
        st.SetTestExtraNum("pawnsMarched", static_cast<uint32>(ch.marched.size()));
        st.SetTestExtraNum("ordersIssued", ch.ordersIssued);
        st.SetTestExtraNum("ordersFailed", ch.ordersFailed);
        st.SetTestExtraNum("movesRejected", ch.movesRejected);
        st.SetTestExtraNum("looted", ch.chestOpened ? 1 : 0);
        st.SetTestExtra("chessState", DcChessConductor::StateName(static_cast<State>(ch.state)));
        if (!ch.blockedWhy.empty())
            st.SetTestExtra("blocked", ch.blockedWhy);
    }

    // Seats in a stable order (GUID, the leader last), the sideline slot of each,
    // and the kernel's view of them.
    std::vector<DcChess::Seat> SeatRaid(Player* leader, Hall const& h, DcChessRunState& ch)
    {
        std::vector<Player*> sorted = h.members;
        std::sort(sorted.begin(), sorted.end(), [leader](Player* a, Player* b)
        {
            if ((a == leader) != (b == leader))
                return b == leader;
            return a->GetGUID() < b->GetGUID();
        });
        ch.seats.clear();
        std::vector<DcChess::Seat> seats;
        for (Player* m : sorted)
        {
            ch.seats.push_back(m->GetGUID().GetRawValue());
            DcChess::Seat s;
            s.bot = m->GetGUID().GetRawValue();
            s.leader = m == leader;
            s.available = true;
            if (Creature* held = ControlledPiece(m))
                s.current = held->GetGUID().GetRawValue();
            else
                for (DcChess::Assignment const& a : ch.assign)
                    if (a.bot == s.bot)
                        s.current = a.piece;
            seats.push_back(s);
        }
        return seats;
    }

    // Execute the policy's orders. The conductor casts on the pieces itself — see
    // the header note — through the polite (untriggered) cast first and the
    // triggered one only if that is refused, the Razorgore shape.
    void Execute(DcRunState& st, Hall& h, DcChess::Decision const& d, uint32 now)
    {
        DcChessRunState& ch = st.chess;
        DcChess::Board const& b = h.board;
        for (DcChess::Order const& o : d.orders)
        {
            auto it = h.byGuid.find(o.piece);
            if (it == h.byGuid.end())
                continue;
            Creature* piece = it->second;
            DcChess::PieceInfo const* info = DcChess::InfoOf(piece->GetEntry());
            DcChess::Piece const* bp = nullptr;
            if (!info || !piece->IsCharmed() || !PieceOfGuid(b, o.piece, bp))
                continue;

            if (o.kind == DcChess::OrderKind::Move || o.kind == DcChess::OrderKind::Face)
            {
                int const realRow = b.RealRow(o.r);
                Creature* trigger = DcChess::OnBoard(realRow, o.c) ? h.triggers[realRow][o.c] : nullptr;
                if (!trigger)
                    continue;
                uint32 const spell = o.kind == DcChess::OrderKind::Move ? info->moveSpell : DcChess::SPELL_CHANGE_FACING;
                SpellCastResult r = piece->CastSpell(trigger, spell, false);
                bool triggered = false;
                if (r != SPELL_CAST_OK)
                {
                    r = piece->CastSpell(trigger, spell, true);
                    triggered = true;
                }
                ++ch.ordersIssued;
                if (r != SPELL_CAST_OK)
                {
                    // Refused outright (out of range, bad target): the core's own
                    // cooldown was never cast, so neither is ours — and there is
                    // nothing to wait for. The next tick decides again.
                    ++ch.ordersFailed;
                    LOG_INFO("playerbots.dungeonclear", "DC_CHESS order {} {} -> ({},{}) why={} REFUSED result={}",
                             PieceLabel(piece, b), o.kind == DcChess::OrderKind::Move ? "move" : "face", realRow,
                             o.c, DcChess::WhyName(o.why), uint32(r));
                    continue;
                }
                ch.ledger.OnAction(o.piece, now);
                DcChessPending pend;
                pend.piece = o.piece;
                pend.kind = static_cast<uint8>(o.kind);
                pend.realRow = static_cast<int8>(realRow);
                pend.col = o.c;
                pend.wantFacing = DcChess::OriOfStep(realRow - b.RealRow(bp->r), o.c - bp->c);
                pend.issuedMs = now;
                ch.pending.push_back(pend);
                LOG_INFO("playerbots.dungeonclear",
                         "DC_CHESS order {} {} -> ({},{}) why={}{}", PieceLabel(piece, b),
                         o.kind == DcChess::OrderKind::Move ? "move" : "face", realRow, o.c,
                         DcChess::WhyName(o.why), triggered ? " (triggered)" : "");
                continue;
            }

            if (o.kind == DcChess::OrderKind::Cast)
            {
                Unit* target = nullptr;
                if (o.target)
                {
                    auto t = h.byGuid.find(o.target);
                    if (t == h.byGuid.end())
                        continue;
                    target = t->second;
                }
                SpellCastResult const r = piece->CastSpell(target, o.spell, false);
                ch.ledger.OnCast(o.piece, o.spell, *info, now);
                ++ch.ordersIssued;
                if (r != SPELL_CAST_OK)
                    ++ch.ordersFailed;
                LOG_DEBUG("playerbots.dungeonclear", "DC_CHESS cast {} spell {} on {} result={}",
                          PieceLabel(piece, b), o.spell, target ? target->GetName() : std::string("-"),
                          uint32(r));
            }
        }
    }

    // Did the moves and turns we issued land? The core's own cooldown is cast
    // only on an accepted move, so a refused one is refunded to the ledger.
    void ConfirmPending(DcRunState& st, Hall const& h, uint32 now)
    {
        DcChessRunState& ch = st.chess;
        DcChess::Board const& b = h.board;
        std::vector<DcChessPending> keep;
        for (DcChessPending const& p : ch.pending)
        {
            uint32 const age = now - p.issuedMs;
            if (age < CONFIRM_AFTER_MS)
            {
                keep.push_back(p);
                continue;
            }
            DcChess::Piece const* bp = nullptr;
            if (!PieceOfGuid(b, p.piece, bp))
                continue;  // it died; nothing to confirm
            bool landed;
            if (p.kind == static_cast<uint8>(DcChess::OrderKind::Move))
                landed = b.RealRow(bp->r) == p.realRow && bp->c == p.col;
            else
                landed = b.RealOri(bp->facing) == p.wantFacing;
            if (landed)
            {
                st.BumpEventProgress();
                continue;
            }
            if (age < CONFIRM_MAX_MS)
            {
                keep.push_back(p);
                continue;
            }
            ch.ledger.RefundAction(p.piece);
            ++ch.movesRejected;
            auto it = h.byGuid.find(p.piece);
            LOG_INFO("playerbots.dungeonclear", "DC_CHESS order {} {} -> ({},{}) did NOT land; refunded",
                     it != h.byGuid.end() ? PieceLabel(it->second, b) : std::string("?"),
                     p.kind == static_cast<uint8>(DcChess::OrderKind::Move) ? "move" : "face", p.realRow, p.col);
        }
        ch.pending.swap(keep);
    }

    // H2: a charm (CHARM_TYPE_CHARM) makes the piece follow its charmer unless the
    // charmer's seer is the piece — Control Piece's Bind Sight (effect 0) should
    // set that before the charm (effect 2) applies. If it ever does not, the
    // piece walks off its cell after the bot and the core's board no longer
    // matches the world. Stop it where it stands.
    void StopFollowing(Hall const& h)
    {
        for (Creature* c : h.pieces)
            if (c->IsCharmed() && c->GetMotionMaster()->GetCurrentMovementGeneratorType() == FOLLOW_MOTION_TYPE)
            {
                c->GetMotionMaster()->Clear();
                c->GetMotionMaster()->MoveIdle();
                LOG_WARN("playerbots.dungeonclear", "DC_CHESS {} was following its controller — stopped (H2)",
                         c->GetName());
            }
    }

    void Track(DcRunState& st, Hall const& h, uint32 now)
    {
        DcChessRunState& ch = st.chess;
        int ours = 0, theirs = 0, buffed = 0;
        uint32 theirKingPct = 100;
        for (DcChess::Piece const& p : h.board.pieces)
        {
            (p.ours ? ours : theirs) += 1;
            if (!p.ours && p.buffed)
                ++buffed;
            if (!p.ours && p.kind == DcChess::Kind::King)
                theirKingPct = static_cast<uint32>(p.HpPct());
        }
        if ((ch.ourCount >= 0 && ours < ch.ourCount) || (ch.theirCount >= 0 && theirs < ch.theirCount))
        {
            ch.lastKillMs = now;
            st.BumpEventProgress();
            LOG_INFO("playerbots.dungeonclear", "DC_CHESS material {}v{} (was {}v{})", ours, theirs,
                     ch.ourCount, ch.theirCount);
        }
        if (h.fires > ch.lastFires)
            ++ch.cheatFire;
        if (buffed > ch.lastBuffed)
            ++ch.cheatBuff;
        if (theirKingPct >= 99 && ch.theirKingPct < 95)
            ++ch.cheatHeal;
        ch.ourCount = ours;
        ch.theirCount = theirs;
        ch.lastFires = h.fires;
        ch.lastBuffed = buffed;
        ch.theirKingPct = theirKingPct;
    }

    void Status(DcRunState& st, Hall const& h, uint32 now, bool log)
    {
        DcChessRunState& ch = st.chess;
        State const s = static_cast<State>(ch.state);
        int ourKingPct = -1;
        int controllers = 0;
        for (DcChess::Piece const& p : h.board.pieces)
        {
            if (p.ours && p.kind == DcChess::Kind::King)
                ourKingPct = static_cast<int>(p.HpPct());
            controllers += p.controlled ? 1 : 0;
        }
        char buf[200];
        std::snprintf(buf, sizeof(buf), "Chess: attempt %u, %s, %dv%d, our K %d%%, their K %u%%",
                      uint32(ch.attempts),
                      s == State::Play ? DcChess::PostureName(static_cast<DcChess::Posture>(ch.posture))
                                       : DcChessConductor::StateName(s),
                      std::max(ch.ourCount, 0), std::max(ch.theirCount, 0), ourKingPct, ch.theirKingPct);
        ch.status = buf;
        if (!log)
            return;
        LOG_INFO("playerbots.dungeonclear",
                 "DC_CHESS state={} phase={} event={} posture={} attempt={} material={}v{} ourK={}% theirK={}% "
                 "cheats=h{}/f{}/b{} controllers={}/{} orders={} failed={} rejected={} fires={} game={}s",
                 DcChessConductor::StateName(s), h.phase, h.event,
                 DcChess::PostureName(static_cast<DcChess::Posture>(ch.posture)), ch.attempts,
                 std::max(ch.ourCount, 0), std::max(ch.theirCount, 0), ourKingPct, ch.theirKingPct,
                 ch.cheatHeal, ch.cheatFire, ch.cheatBuff, controllers, h.members.size(), ch.ordersIssued,
                 ch.ordersFailed, ch.movesRejected, h.fires,
                 ch.gameStartMs ? (now - ch.gameStartMs) / 1000 : 0);
    }

    // The leader's own errands for the conductor: the Echo gossip and the chest.
    // True while the leader's body is spoken for.
    bool LeaderErrand(Player* leader, PlayerbotAI* leaderAI, DcChessRunState& ch, Hall const& h, Act act,
                      uint32 now)
    {
        if (act == Act::StartGame || act == Act::RestartGame)
        {
            // H8: Echo's ordinal 0 means a different thing in every phase. Start
            // only at NOT_STARTED; Restart only at WARMUP/INPROGRESS; never at
            // PVE_FINISHED, where it starts PvP.
            bool const phaseOk = act == Act::StartGame
                ? h.phase == CHESS_PHASE_NOT_STARTED
                : (h.phase == CHESS_PHASE_PVE_WARMUP || h.phase == CHESS_PHASE_INPROGRESS_PVE);
            if (!h.echo || !phaseOk)
                return false;
            if (!Reach(leader, leaderAI, h.echo->GetPositionX(), h.echo->GetPositionY(), h.echo->GetPositionZ(),
                       INTERACT_YD))
                return true;
            if (ch.lastGossipMs && now - ch.lastGossipMs < ECHO_GOSSIP_FLOOR_MS)
                return true;
            ch.lastGossipMs = now;
            bool const sent = DungeonEventExecutor::SelectGossip(leader, h.echo, 0);
            LOG_INFO("playerbots.dungeonclear", "DC_CHESS [{}] Echo of Medivh: {} (phase {}) {}",
                     leader->GetName(), act == Act::StartGame ? "start the game" : "RESTART the game", h.phase,
                     sent ? "sent" : "menu not ready");
            return true;
        }

        if (act == Act::Loot)
        {
            if (!h.chest || !h.chest->isSpawned())
                return false;
            if (!Reach(leader, leaderAI, h.chest->GetPositionX(), h.chest->GetPositionY(), h.chest->GetPositionZ(),
                       INTERACT_YD))
                return true;
            if (h.chest->getLootState() != GO_READY)
            {
                ch.chestOpened = true;
                return false;
            }
            if (leader->IsNonMeleeSpellCast(false))
                return true;
            if (ch.lastGossipMs && now - ch.lastGossipMs < OPEN_FLOOR_MS)
                return true;
            ch.lastGossipMs = now;
            SpellCastResult const r = leader->CastSpell(h.chest, SPELL_OPENING, false);
            LOG_INFO("playerbots.dungeonclear", "DC_CHESS [{}] opening the Dust Covered Chest: result={}",
                     leader->GetName(), uint32(r));
            return true;
        }
        return false;
    }

    // The conductor's tick. Returns whether the leader's body is on an errand
    // (Echo, the chest) this tick, so its own seat stays put.
    bool ConductorTick(Player* leader, PlayerbotAI* leaderAI, DcRunState& st)
    {
        DcChessRunState& ch = st.chess;
        uint32 const now = getMSTime();
        if (!st.enabled || st.paused || !ch.armed)
            return false;
        if (ch.lastTickMs && now - ch.lastTickMs < TICK_MS)
            return ch.state == static_cast<uint8>(State::Start) || ch.state == static_cast<uint8>(State::Restart) ||
                   ch.state == static_cast<uint8>(State::Loot);
        ch.lastTickMs = now;

        Hall h;
        ReadHall(leader, h);
        if (!h.inst)
            return false;

        if (h.phase != ch.lastPhase)
        {
            if (ch.lastPhase != 0xFFFFFFFFu)
                st.BumpEventProgress();
            ch.lastPhase = h.phase;
        }

        // The View.
        std::vector<DcChess::Seat> const seats = SeatRaid(leader, h, ch);
        DcChessConductor::View v;
        v.armed = ch.armed;
        v.event = h.event;
        v.phase = h.phase;
        bool atSideline = true;
        for (size_t i = 0; i < h.members.size(); ++i)
        {
            Player* m = h.members[i];
            v.anyRecentlyInGame |= m->HasAura(DcChess::SPELL_RECENTLY_IN_GAME);
            v.anyCharmed |= ControlledPiece(m) != nullptr;
            float sx, sy;
            StandPoint(sx, sy);
            if (m != leader && !ControlledPiece(m) && m->GetExactDist2d(sx, sy) > 3.0f)
                atSideline = false;
        }
        v.raidAtSideline = atSideline;
        v.stateAgeMs = now - ch.stateMs;
        v.gameAgeMs = ch.gameStartMs ? now - ch.gameStartMs : 0;
        v.sinceKillMs = ch.lastKillMs ? now - ch.lastKillMs : 0;
        v.attempts = ch.attempts;
        v.chestPresent = h.chest && h.chest->isSpawned();
        v.chestOpened = ch.chestOpened || (v.chestPresent && h.chest->getLootState() != GO_READY);

        // No game on the board and none about to start: nobody should still be
        // pacified (OnCharmed(false) re-casts Game In Session after the reset).
        if (h.phase == CHESS_PHASE_NOT_STARTED && static_cast<State>(ch.state) != State::Start)
            for (Player* m : h.members)
                if (m->HasAura(DcChess::SPELL_GAME_IN_SESSION) && !ControlledPiece(m))
                    m->RemoveAurasDueToSpell(DcChess::SPELL_GAME_IN_SESSION);

        State const from = static_cast<State>(ch.state);
        DcChessConductor::Step const step = DcChessConductor::Decide(from, v);
        if (step.next != from)
            Transition(leader, st, from, step, h, now);
        if (step.next == State::Done)
            ch.chestOpened = ch.chestOpened || v.chestOpened;

        // Stamp while there is anything to drive (the freshness the members' rung
        // and the watchdogs read). A terminal state stops stamping, which is what
        // releases everybody.
        if (!DcChessConductor::Terminal(step.next))
            ch.drivingMs = now;

        // Who takes what.
        switch (step.act)
        {
            case Act::TakeKing:
            case Act::Play:
            {
                DcChess::NoteMarched(h.board, ch.marched);
                std::vector<DcChess::Assignment> all =
                    DcChess::Assign(h.board, seats, ch.refused, DcChess::Tunables{}.controlCentrePawns, ch.marched);
                if (step.act == Act::TakeKing)
                {
                    // Only the King is takeable in WARMUP — and taking it is what
                    // starts the game, so nobody else walks anywhere yet.
                    all.erase(std::remove_if(all.begin(), all.end(), [&](DcChess::Assignment const& a)
                    {
                        DcChess::Piece const* p = nullptr;
                        return !PieceOfGuid(h.board, a.piece, p) || p->kind != DcChess::Kind::King;
                    }), all.end());
                }
                for (DcChess::Assignment const& a : all)
                {
                    bool const isNew = std::none_of(ch.assign.begin(), ch.assign.end(),
                                                    [&](DcChess::Assignment const& o)
                                                    { return o.bot == a.bot && o.piece == a.piece; });
                    if (isNew)
                    {
                        ++ch.churn;
                        if (Player* p = ObjectAccessor::FindPlayer(ObjectGuid(a.bot)))
                            if (Creature* held = ControlledPiece(p))
                                if (held->GetGUID().GetRawValue() != a.piece)
                                    ++ch.handoffs;
                        auto it = h.byGuid.find(a.piece);
                        LOG_INFO("playerbots.dungeonclear", "DC_CHESS assign {} -> {}",
                                 ObjectAccessor::FindPlayer(ObjectGuid(a.bot))
                                     ? ObjectAccessor::FindPlayer(ObjectGuid(a.bot))->GetName()
                                     : std::to_string(a.bot),
                                 it != h.byGuid.end() ? PieceLabel(it->second, h.board) : std::string("?"));
                    }
                }
                ch.assign = std::move(all);
                break;
            }
            case Act::Gather:
            case Act::WaitReset:
            case Act::StartGame:
            case Act::RestartGame:
            case Act::Loot:
            case Act::None:
                ch.assign.clear();
                break;
        }

        // The game itself.
        if (step.act == Act::Play && h.phase == CHESS_PHASE_INPROGRESS_PVE)
        {
            StopFollowing(h);
            Track(st, h, now);
            ConfirmPending(st, h, now);
            DcChess::Clock clk;
            clk.nowMs = now;
            clk.gameStartMs = ch.gameStartMs;
            clk.lastKillMs = ch.lastKillMs;
            DcChess::Decision const d = DcChess::Decide(h.board, ch.ledger, clk);
            if (static_cast<uint8>(d.posture) != ch.posture)
                LOG_INFO("playerbots.dungeonclear", "DC_CHESS posture {} -> {}",
                         DcChess::PostureName(static_cast<DcChess::Posture>(ch.posture)),
                         DcChess::PostureName(d.posture));
            ch.posture = static_cast<uint8>(d.posture);
            Execute(st, h, d, now);
        }

        bool const log = !ch.lastLogMs || now - ch.lastLogMs >= LOG_MS || step.next != from;
        if (log)
            ch.lastLogMs = now;
        Status(st, h, now, log);
        PublishExtras(st);

        return LeaderErrand(leader, leaderAI, ch, h, step.act, now);
    }

    // --- one member's seat -------------------------------------------------------

    // Take the piece the conductor gave this bot: walk up to it (never onto its
    // cell centre — a player on a burning cell is safe from Burning Flames, whose
    // conditions only accept pieces, but a body on the board is still clutter),
    // gossip "Control <name>" once per 3s, and give up on it after five tries so
    // the conductor can hand it to someone else.
    bool TakePiece(Player* bot, PlayerbotAI* botAI, Creature* piece, uint32 phase)
    {
        DcChessRunState& me = DcRun::Of(botAI).chess;
        bool const king = piece->GetEntry() == DcChess::NPC_KING_A || piece->GetEntry() == DcChess::NPC_KING_H;
        bool const phaseOk = phase == CHESS_PHASE_INPROGRESS_PVE || (king && phase == CHESS_PHASE_PVE_WARMUP);
        if (!phaseOk || !piece->IsAlive() || piece->HasUnitFlag(UNIT_FLAG_NOT_SELECTABLE) || piece->IsCharmed() ||
            bot->HasAura(DcChess::SPELL_RECENTLY_IN_GAME))
            return false;

        uint64 const guid = piece->GetGUID().GetRawValue();
        if (me.myPiece != guid)
        {
            me.myPiece = guid;
            me.gossipTries = 0;
        }

        float const d = bot->GetExactDist2d(piece);
        if (d > INTERACT_YD)
        {
            // A point 2.5yd short of the piece, on the bot's side of it.
            float const k = std::max(0.0f, (d - 2.5f) / d);
            float const x = bot->GetPositionX() + (piece->GetPositionX() - bot->GetPositionX()) * k;
            float const y = bot->GetPositionY() + (piece->GetPositionY() - bot->GetPositionY()) * k;
            Walk(bot, botAI, x, y, piece->GetPositionZ());
            return true;
        }
        if (bot->isMoving())
            DcMovement::StopBot(bot, DcMovement::Stop::Hold);
        if (DcRun::Of(botAI).Throttled(DcThrottle::ChessGossip, PIECE_GOSSIP_FLOOR_MS))
            return true;

        // Anything in the bot's hands would eat the Control Piece cast (a cast in
        // progress refuses a non-triggered one).
        if (bot->IsNonMeleeSpellCast(false))
            bot->InterruptNonMeleeSpells(false);
        bool const sent = DungeonEventExecutor::SelectGossip(bot, piece, 0);
        ++me.gossipTries;
        LOG_INFO("playerbots.dungeonclear", "DC_CHESS {} takes {} (try {}{})", bot->GetName(), piece->GetName(),
                 uint32(me.gossipTries), sent ? "" : ", menu not ready");
        if (me.gossipTries >= PIECE_GOSSIP_TRIES)
        {
            LOG_WARN("playerbots.dungeonclear", "DC_CHESS {} gives up on {} after {} tries", bot->GetName(),
                     piece->GetName(), uint32(me.gossipTries));
            DcLeaderSignal::ReportChessRefusal(bot, piece->GetGUID());
            me.gossipTries = 0;
        }
        return true;
    }

    void NeverFight(Player* bot, PlayerbotAI* botAI, bool keepCast)
    {
        if (bot->GetVictim())
            bot->AttackStop();
        if (botAI->GetAiObjectContext()->GetValue<Unit*>(DcKey::Stock::CurrentTarget)->Get())
            botAI->GetAiObjectContext()->GetValue<Unit*>(DcKey::Stock::CurrentTarget)->Set(nullptr);
        if (!keepCast && bot->IsNonMeleeSpellCast(false))
            bot->InterruptNonMeleeSpells(false);
        // H6: a pet must not fight the pieces either. Control Piece dismisses it
        // (SPELL_ATTR1_DISMISS_PET_FIRST), but until then it stays put and passive.
        if (Pet* pet = bot->GetPet())
        {
            if (pet->GetReactState() != REACT_PASSIVE)
                pet->SetReactState(REACT_PASSIVE);
            if (pet->GetVictim())
                pet->AttackStop();
            if (CharmInfo* ci = pet->GetCharmInfo())
                if (!ci->HasCommandState(COMMAND_STAY))
                {
                    ci->SetCommandState(COMMAND_STAY);
                    ci->SetIsCommandAttack(false);
                }
        }
    }
}

namespace DcKarazhan
{
    bool KaraChessLive(Map* map)
    {
        if (!map || map->GetId() != MAP)
            return false;
        InstanceMap* inst = map->ToInstanceMap();
        InstanceScript* script = inst ? inst->GetInstanceScript() : nullptr;
        if (!script)
            return false;
        uint32 const phase = script->GetData(DATA_CHESS_GAME_PHASE);
        return phase == CHESS_PHASE_PVE_WARMUP || phase == CHESS_PHASE_INPROGRESS_PVE;
    }

    // The run's conductor, read straight off the run owner — no freshness window.
    // Used for the owner's OWN liveness (it is the one that ticks the conductor,
    // so gating it on its own stamp would freeze the game the first time a tick
    // was missed) and to decide whether a live game belongs to this run at all.
    static DcChessRunState const* OwnerConductor(Player* bot, Player** ownerOut = nullptr)
    {
        Player* owner = DcLeaderSignal::FindRunOwner(bot);
        if (ownerOut)
            *ownerOut = owner;
        PlayerbotAI* ownerAI = owner ? GET_PLAYERBOT_AI(owner) : nullptr;
        if (!ownerAI)
            return nullptr;
        DcRunState const& st = DcRun::Of(ownerAI);
        if (!st.enabled || !st.chess.armed ||
            DcChessConductor::Terminal(static_cast<State>(st.chess.state)))
            return nullptr;
        return &st.chess;
    }

    // A game on the board only holds the raid while this run's conductor is
    // active: a game left behind by a conductor that gave up (Blocked), or by a
    // run switched off mid-game, must not hold anybody or blind the watchdogs.
    bool ChessIsOn(Player* bot)
    {
        if (!bot || bot->GetMapId() != MAP)
            return false;
        return OwnerConductor(bot) != nullptr &&
               (KaraChessLive(bot->GetMap()) || DcLeaderSignal::IsLeaderChessArmed(bot));
    }

    bool ChessRungLive(Player* bot)
    {
        if (!bot || bot->GetMapId() != MAP || !bot->IsAlive() || !InHall(bot))
            return false;
        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        if (!botAI)
            return false;

        Player* owner = nullptr;
        DcChessRunState const* conductor = OwnerConductor(bot, &owner);
        if (!conductor)
            return false;

        // The host (the owner, or its stand-in — ConductorHost): live for as long
        // as the conductor is armed and not finished, paused excepted — its own
        // tick is what stamps the freshness the members read, so it must never
        // depend on that stamp.
        if (ConductorHost(owner) == bot)
        {
            PlayerbotAI* ownerAI = GET_PLAYERBOT_AI(owner);
            if (!ownerAI || DcRun::Of(ownerAI).paused)
                return false;
            State const s = static_cast<State>(conductor->state);
            if (DcChessConductor::HoldsTheRaid(s) || KaraChessLive(bot->GetMap()))
                DcRun::Of(botAI).chess.heldStampMs = getMSTime() | 1u;
            return true;
        }

        bool held = KaraChessLive(bot->GetMap());
        uint8 raw = 0;
        if (DcLeaderSignal::IsLeaderChessArmed(bot, &raw) &&
            DcChessConductor::HoldsTheRaid(static_cast<State>(raw)))
            held = true;
        if (held)
            DcRun::Of(botAI).chess.heldStampMs = getMSTime() | 1u;
        return held;
    }

    bool ChessHoldsTheBot(Player* bot, PlayerbotAI* botAI)
    {
        if (!bot || !botAI || bot->GetMapId() != MAP)
            return false;
        uint32 const stamp = DcRun::Of(botAI).chess.heldStampMs;
        return stamp && GetMSTimeDiffToNow(stamp) < HELD_STAMP_MS;
    }

    bool ChessRungTick(Player* bot, PlayerbotAI* botAI)
    {
        if (!bot || !botAI)
            return false;
        Player* owner = DcLeaderSignal::FindRunOwner(bot);
        PlayerbotAI* ownerAI = owner ? GET_PLAYERBOT_AI(owner) : nullptr;
        bool const isHost = ownerAI && ConductorHost(owner) == bot;

        bool errand = false;
        if (isHost)
        {
            DcRunState& st = DcRun::Of(ownerAI);
            uint64 const me = bot->GetGUID().GetRawValue();
            if (st.chess.hostGuid != me)
            {
                if (st.chess.hostGuid)
                    LOG_INFO("playerbots.dungeonclear", "DC_CHESS conductor handed to {} (owner {} {})",
                             bot->GetName(), owner->GetName(), owner->IsAlive() ? "out of the hall" : "dead");
                st.chess.hostGuid = me;
            }
            errand = ConductorTick(bot, botAI, st);
        }

        uint8 raw = 0;
        bool armed = DcLeaderSignal::IsLeaderChessArmed(bot, &raw);
        if (isHost)
            if (DcChessRunState const* c = OwnerConductor(bot))
            {
                armed = true;
                raw = c->state;
            }
        State const s = static_cast<State>(raw);
        bool const loot = armed && s == State::Loot;

        NeverFight(bot, botAI, /*keepCast*/ isHost && loot);
        if (errand)
            return true;
        if (loot)
            return false;  // the chest is open: hand the tick to the loot pipeline

        // Holding a piece already: stand on the sideline and keep it — unless the
        // conductor has given this bot a better one (a pawn that has marched is
        // left where it stands). Let go only when that piece is there to take;
        // the core puts the bot on the waiting spot under Recently In Game (10s),
        // and the take below walks it to the new piece.
        InstanceScript* inst = bot->GetInstanceScript();
        uint32 const phase = inst ? inst->GetData(DATA_CHESS_GAME_PHASE) : 0;
        if (Creature* held = ControlledPiece(bot))
        {
            ObjectGuid const assigned = DcLeaderSignal::GetChessAssignment(bot);
            // Never while the piece is walking to its cell: RemoveCharmedBy stops
            // it where it stands, between two cells, and a piece off every cell
            // is off our board for the rest of the game (tr-20260926-175548-5:
            // four let go mid-march, 16 pieces to 13 in a second).
            bool const walking = held->isMoving() || (held->movespline && !held->movespline->Finalized());
            if (phase == CHESS_PHASE_INPROGRESS_PVE && !walking && !assigned.IsEmpty() &&
                assigned != held->GetGUID())
                if (Creature* next = ObjectAccessor::GetCreature(*bot, assigned))
                    if (next->IsAlive() && !next->IsCharmed() && !next->HasUnitFlag(UNIT_FLAG_NOT_SELECTABLE))
                    {
                        LOG_INFO("playerbots.dungeonclear", "DC_CHESS {} lets go of {} for {}", bot->GetName(),
                                 held->GetName(), next->GetName());
                        bot->StopCastingCharm();
                    }
        }
        if (!ControlledPiece(bot))
        {
            ObjectGuid const assigned = DcLeaderSignal::GetChessAssignment(bot);
            if (assigned.IsEmpty())
            {
                // Between assignments (a new game, a reset): the next piece — even
                // the same one, whose GUID survives the reset — starts at try one.
                DcRun::Of(botAI).chess.myPiece = 0;
                DcRun::Of(botAI).chess.gossipTries = 0;
            }
            else
                if (Creature* piece = ObjectAccessor::GetCreature(*bot, assigned))
                    if (TakePiece(bot, botAI, piece, phase))
                        return true;
        }

        float x, y;
        StandPoint(x, y);

        // A controller does not walk back across the board: the moment the charm
        // lands it is put on its seat, the way the core puts a released one on
        // the waiting spot. Under 100yd, so TeleportTo strips no aura and the
        // charm holds. A bot without a piece still walks (it is going to one).
        // Landed SEAT_DROP_YD above the floor: a z at the floor put bots under it.
        if (ControlledPiece(bot) && bot->GetExactDist2d(x, y) > SLOT_ARRIVE_YD)
        {
            if (!DcRun::Of(botAI).Throttled(DcThrottle::ChessSeatTeleport, SEAT_TELEPORT_FLOOR_MS))
            {
                float cx, cy;
                BoardCentre(cx, cy);
                bot->GetMotionMaster()->Clear();
                bot->NearTeleportTo(x, y, HALL_FLOOR_Z + SEAT_DROP_YD, bot->GetAbsoluteAngle(cx, cy));
                LOG_DEBUG("playerbots.dungeonclear", "DC_CHESS {} holds its piece: to seat {}", bot->GetName(),
                          DcLeaderSignal::GetChessSeat(bot));
            }
            return true;
        }
        Reach(bot, botAI, x, y, HALL_FLOOR_Z, SLOT_ARRIVE_YD);
        return true;
    }
}

// --- the event's hooks ----------------------------------------------------------

namespace
{
    // Hook 43 — arm the conductor. The rung does the gathering (pets passive,
    // everyone to the sideline) once it is armed, so this step is done at once.
    // Asked to arm a conductor that has already finished while the instance says
    // chess is NOT done — an instance reload forgot the unsaved win and the
    // objective came round again — it starts over from a clean block.
    ObjectiveArriveResult KarazhanChessSetup(Player* bot, AiObjectContext* context, DungeonBossInfo const& /*info*/)
    {
        DcRunState& st = DcRun::Of(context);
        InstanceScript* inst = bot->GetInstanceScript();
        uint32 const event = inst ? inst->GetData(DATA_CHESS_EVENT) : 0;
        if (static_cast<State>(st.chess.state) == State::Done && event != CHESS_EVENT_DONE)
        {
            LOG_INFO("playerbots.dungeonclear", "DC_CHESS [{}] chess reads not done again — a fresh conductor",
                     bot->GetName());
            st.ClearChess();
        }
        if (!st.chess.armed)
        {
            uint32 const now = getMSTime();
            st.chess.armed = true;
            st.chess.stateMs = now;
            st.chess.drivingMs = now;
            LOG_INFO("playerbots.dungeonclear", "DC_CHESS [{}] armed (event={} phase={})", bot->GetName(), event,
                     inst ? inst->GetData(DATA_CHESS_GAME_PHASE) : 0);
        }
        return ObjectiveArriveResult::Done;
    }

    // Hook 42 — the game. It only reports: the conductor runs in the rung, which
    // owns the leader's tick for as long as there is a game to play, so this is
    // called before the game (Running) and after it (Done / Blocked).
    ObjectiveArriveResult KarazhanChessPlay(Player* bot, AiObjectContext* context, DungeonBossInfo const& info)
    {
        DcRunState& st = DcRun::Of(context);
        if (!st.chess.armed)
            KarazhanChessSetup(bot, context, info);
        switch (static_cast<State>(st.chess.state))
        {
            case State::Done:
                return ObjectiveArriveResult::Done;
            case State::Blocked:
                return ObjectiveArriveResult::Blocked;
            default:
                // Keep the conductor fresh whenever this is reached: the rung is
                // not live on the owner right now (paused, or out of the hall),
                // and the members read the stamp.
                st.chess.drivingMs = getMSTime();
                return ObjectiveArriveResult::Running;
        }
    }
}

void RegisterKarazhanChessHooks(ObjectiveHookRegistry::HookTable& out)
{
    ObjectiveHookRegistry::AddHook(out, HOOK_KZ_CHESS_PLAY, &KarazhanChessPlay);
    ObjectiveHookRegistry::AddHook(out, HOOK_KZ_CHESS_SETUP, &KarazhanChessSetup);
}
