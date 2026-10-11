/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCCHESSCONDUCTOR_H
#define _PLAYERBOT_DCCHESSCONDUCTOR_H

#include <string>
#include <vector>

#include "Define.h"
#include "Ai/Dungeon/DungeonClear/Util/DcChessDecision.h"

// PURE state machine of Karazhan's chess CONDUCTOR — the leader-side driver that
// starts the game, hands out pieces, plays it, retries a loss and loots the chest
// (Overrides/KarazhanChessDriver.cpp is the glue). Plan section 3.3.
//
// Everything the FSM needs is read live every tick into a View — the instance's
// two chess values, a few party facts, the clocks — so nothing here can go stale
// across a loss, a restart, or a server restart that wiped the unsaved chess
// state. The glue owns the side effects of each Act.
//
// WHY THE GAME LIVES INSIDE ONE HOOK. Anchored objectives latch forever once they
// complete and Repeatable is conditional-only, so a lost game cannot rewind the
// event to its first step. The whole of the game — start, retries, the win, the
// chest — is therefore one Custom step's worth of state, and a loss is just a
// transition back to Setup here.
namespace DcChessConductor
{
    enum class State : uint8
    {
        Idle,      // not armed yet (hook 43 arms it)
        Setup,     // gather: everyone to the sideline, pets passive
        Start,     // the leader talks to Echo of Medivh: "We want to play a game against you!"
        TakeKing,  // WARMUP: only the King can be taken, and taking it starts the game
        Play,      // the game
        Restart,   // abandon a hopeless game: Echo's ordinal 0 now reads "Restart"
        Lost,      // our King died (or we restarted): wait for the reset to settle
        Loot,      // won: open the Dust Covered Chest
        Done,
        Blocked,
    };

    inline char const* StateName(State s)
    {
        switch (s)
        {
            case State::Idle:     return "Idle";
            case State::Setup:    return "Setup";
            case State::Start:    return "Start";
            case State::TakeKing: return "TakeKing";
            case State::Play:     return "Play";
            case State::Restart:  return "Restart";
            case State::Lost:     return "Lost";
            case State::Loot:     return "Loot";
            case State::Done:     return "Done";
            case State::Blocked:  return "Blocked";
        }
        return "?";
    }

    // States in which the raid's members are held by the game (the member rung is
    // live for every bot in the hall). Loot is the leader's alone.
    inline bool HoldsTheRaid(State s)
    {
        return s == State::Setup || s == State::Start || s == State::TakeKing || s == State::Play ||
               s == State::Restart || s == State::Lost;
    }

    inline bool Terminal(State s) { return s == State::Done || s == State::Blocked; }

    // karazhan.h values the FSM reads (mirrored here so the kernel stays free of
    // game headers). DATA_CHESS_EVENT uses EncounterState; the phase is
    // KarazhanChessGamePhase.
    constexpr uint32 EVENT_DONE = 3;
    constexpr uint32 PHASE_NOT_STARTED = 0;
    constexpr uint32 PHASE_WARMUP = 1;
    constexpr uint32 PHASE_INPROGRESS = 2;

    // Budgets. Each is a ceiling on a wait the world normally ends by itself.
    constexpr uint32 SETUP_MAX_MS     = 30000;   // gather at the sideline
    constexpr uint32 START_MAX_MS     = 90000;   // walk to Echo and start
    constexpr uint32 TAKE_KING_MAX_MS = 90000;   // WARMUP until the King is taken (several bots x 5 gossips)
    constexpr uint32 GAME_MAX_MS      = 15 * 60 * 1000;
    constexpr uint32 NO_KILL_MAX_MS   = 5 * 60 * 1000;
    constexpr uint32 RESTART_MAX_MS   = 60000;
    constexpr uint32 LOST_SETTLE_MS   = 3000;    // let OnCharmed(false) teleport everyone
    constexpr uint32 LOOT_MAX_MS      = 60000;
    // The win respawns the chest (DoRespawnGameObject) in the same tick it sets
    // DONE, and the GO appears on its own next update — so "no chest" only means
    // no chest once it has had a moment to show up.
    constexpr uint32 CHEST_WAIT_MS    = 5000;
    constexpr uint8  MAX_ATTEMPTS     = 3;

    struct View
    {
        bool   armed{false};
        uint32 event{0};              // GetData(DATA_CHESS_EVENT)
        uint32 phase{0};              // GetData(DATA_CHESS_GAME_PHASE)
        bool   anyRecentlyInGame{false};  // a member still has Recently In Game (30529)
        bool   anyCharmed{false};     // a member still holds a piece
        bool   raidAtSideline{false}; // every member is at its sideline slot
        uint32 stateAgeMs{0};
        uint32 gameAgeMs{0};
        uint32 sinceKillMs{0};
        uint8  attempts{0};           // games started so far
        bool   chestPresent{false};   // the Dust Covered Chest is spawned
        bool   chestOpened{false};    // ...and has been opened (its loot state left READY)
    };

    enum class Act : uint8
    {
        None,
        Gather,      // members to the sideline, no pieces assigned
        StartGame,   // the leader to Echo, gossip ordinal 0 — ONLY at phase 0 (H8)
        TakeKing,    // publish the King's assignment alone
        Play,        // the whole game: assignments, the policy, the orders
        RestartGame, // the leader to Echo, gossip ordinal 0 — ONLY at phase 1/2
        WaitReset,   // nobody takes anything; the reset is the core's
        Loot,        // the leader to the chest and open it
    };

    struct Step
    {
        State next{State::Idle};
        Act act{Act::None};
        char const* why{""};
    };

    // One tick. Total and ordered: the WIN first, from any state — so a game won
    // while we were deciding to restart it is a win and never a Restart gossip,
    // which after the win would start PvP (H8).
    inline Step Decide(State s, View const& v)
    {
        if (!v.armed)
            return { State::Idle, Act::None, "not armed" };

        if (v.event == EVENT_DONE && s != State::Loot && !Terminal(s))
            return { State::Loot, Act::Loot, "Medivh's King is dead" };

        switch (s)
        {
            case State::Idle:
            case State::Setup:
                // A game already running (DC restarted mid-game, or somebody
                // started it by hand) is adopted, never re-started.
                if (v.phase == PHASE_WARMUP)
                    return { State::TakeKing, Act::TakeKing, "adopt a game in warmup" };
                if (v.phase == PHASE_INPROGRESS)
                    return { State::Play, Act::Play, "adopt a game in progress" };
                if (s == State::Idle)
                    return { State::Setup, Act::Gather, "armed" };
                if (v.raidAtSideline || v.stateAgeMs >= SETUP_MAX_MS)
                    return { State::Start, Act::StartGame,
                             v.raidAtSideline ? "raid at the sideline" : "gather budget spent" };
                return { State::Setup, Act::Gather, "gathering" };

            case State::Start:
                if (v.phase == PHASE_WARMUP)
                    return { State::TakeKing, Act::TakeKing, "Medivh accepted" };
                if (v.phase == PHASE_INPROGRESS)
                    return { State::Play, Act::Play, "game already on" };
                if (v.attempts >= MAX_ATTEMPTS)
                    return { State::Blocked, Act::None, "chess lost 3x" };
                if (v.stateAgeMs >= START_MAX_MS)
                    return { State::Blocked, Act::None, "could not start the game" };
                if (v.phase != PHASE_NOT_STARTED || v.anyRecentlyInGame || v.anyCharmed)
                    return { State::Start, Act::WaitReset, "waiting for the last game to clear" };
                return { State::Start, Act::StartGame, "starting" };

            case State::TakeKing:
                if (v.phase == PHASE_INPROGRESS)
                    return { State::Play, Act::Play, "the King is taken" };
                if (v.phase == PHASE_NOT_STARTED)
                    return { State::Lost, Act::WaitReset, "reset during warmup" };
                // Echo offers no gossip at all in WARMUP (sGossipHello lists
                // options at phases 0, 2, 4 and 6 only), so a warmup cannot be
                // restarted — and nothing but taking the King ever ends it. The
                // members hand the King on after five failed gossips each; if
                // nobody has taken it in the budget, stop honestly.
                if (v.stateAgeMs >= TAKE_KING_MAX_MS)
                    return { State::Blocked, Act::None, "nobody could take the King" };
                return { State::TakeKing, Act::TakeKing, "taking the King" };

            case State::Play:
                if (v.phase == PHASE_NOT_STARTED)
                    return { State::Lost, Act::WaitReset, "our King died" };
                if (v.gameAgeMs >= GAME_MAX_MS)
                    return { State::Restart, Act::RestartGame, "game over 15 minutes" };
                if (v.sinceKillMs >= NO_KILL_MAX_MS)
                    return { State::Restart, Act::RestartGame, "no kill for 5 minutes" };
                return { State::Play, Act::Play, "" };

            case State::Restart:
                if (v.phase == PHASE_WARMUP)
                    return { State::Blocked, Act::None, "cannot restart a warmup" };
                if (v.phase == PHASE_NOT_STARTED)
                    return { State::Lost, Act::WaitReset, "restarted" };
                if (v.stateAgeMs >= RESTART_MAX_MS)
                    return { State::Blocked, Act::None, "could not restart the game" };
                return { State::Restart, Act::RestartGame, "restarting" };

            case State::Lost:
                if (v.anyCharmed || v.anyRecentlyInGame || v.stateAgeMs < LOST_SETTLE_MS)
                    return { State::Lost, Act::WaitReset, "settling" };
                if (v.attempts >= MAX_ATTEMPTS)
                    return { State::Blocked, Act::None, "chess lost 3x" };
                return { State::Setup, Act::Gather, "re-arming" };

            case State::Loot:
                if (!v.chestPresent)
                    return v.stateAgeMs >= CHEST_WAIT_MS ? Step{ State::Done, Act::None, "no chest to loot" }
                                                         : Step{ State::Loot, Act::None, "waiting for the chest" };
                if (v.chestOpened)
                    return { State::Done, Act::None, "chest looted" };
                if (v.stateAgeMs >= LOOT_MAX_MS)
                    return { State::Done, Act::None, "chest loot timed out" };
                return { State::Loot, Act::Loot, "looting" };

            case State::Done:
            case State::Blocked:
                return { s, Act::None, "" };
        }
        return { s, Act::None, "" };
    }
}

// The conductor's whole state, one block of DcRunState (`chess`). The CONDUCTOR
// half lives on the run owner (the leader); the MEMBER half on every bot's own
// copy. Dropped wholesale by DcRunState::ClearChess, which the hook calls when a
// finished game has to be played again (an unsaved chess reset by an instance
// reload) and Reset() clears with the rest of the run.
struct DcChessPending
{
    uint64 piece{0};
    uint8  kind{0};             // DcChess::OrderKind
    int8   realRow{-1}, col{-1};
    uint8  wantFacing{0};       // Face: the real orientation the turn should leave
    uint32 issuedMs{0};
};

struct DcChessRunState
{
    // --- conductor (run owner) ---------------------------------------------
    uint8  state{0};            // DcChessConductor::State
    bool   armed{false};
    uint32 stateMs{0};          // getMSTime() the state was entered
    uint32 drivingMs{0};        // last conductor tick with the game on — read cross-bot
    uint32 lastTickMs{0};
    uint64 hostGuid{0};         // the bot ticking it (the owner, or a stand-in while the owner is dead)
    uint32 lastLogMs{0};
    uint32 lastGossipMs{0};     // the leader's Echo gossip / chest open floor
    uint8  attempts{0};         // games started
    uint8  losses{0};
    uint8  restarts{0};
    uint32 gameStartMs{0};
    uint32 lastKillMs{0};
    uint32 lastPhase{0xFFFFFFFFu};
    int    ourCount{-1}, theirCount{-1};
    int    lastFires{0}, lastBuffed{0};
    uint32 theirKingPct{100};
    uint32 cheatHeal{0}, cheatFire{0}, cheatBuff{0};
    uint32 churn{0};            // pieces taken over the whole event
    uint32 ordersIssued{0}, ordersFailed{0}, movesRejected{0};
    bool   chestOpened{false};
    uint8  posture{0};          // DcChess::Posture
    std::string blockedWhy;
    std::string status;         // the panel's one-liner
    DcChess::Ledger ledger;
    std::vector<DcChess::Assignment> assign;   // (bot, piece) raw guids, published
    std::vector<DcChess::Assignment> refused;  // pairs whose gossip never took
    std::vector<uint64> marched;               // our pawns whose march is done (DcChess::NoteMarched)
    uint32 handoffs{0};                        // pieces let go for a better one
    std::vector<uint64> seats;                 // bot raw guids in seat order (sideline slots)
    std::vector<DcChessPending> pending;

    // --- member (every bot's own copy) --------------------------------------
    uint64 myPiece{0};          // the piece this bot is trying to take
    uint8  gossipTries{0};
    uint32 lastTakeMs{0};
    uint32 heldStampMs{0};      // the rung was live for this bot at this time (the multiplier clamp reads it)
};

#endif  // _PLAYERBOT_DCCHESSCONDUCTOR_H
