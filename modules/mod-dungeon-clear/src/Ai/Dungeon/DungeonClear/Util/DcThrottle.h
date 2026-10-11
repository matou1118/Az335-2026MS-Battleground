/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCTHROTTLE_H
#define _PLAYERBOT_DCTHROTTLE_H

#include <cstddef>

#include "Define.h"

// PER-BOT THROTTLE SLOTS, and the replacement for a pattern this module used in
// eleven places:
//
//     thread_local std::unordered_map<uint32, uint32> lastMs;
//     uint32& prev = lastMs[bot->GetGUID().GetCounter()];
//
// Two things are wrong with it, and the second is the one that bites.
//
//   1. NOTHING PRUNES IT. Every map-update thread accumulates a row for every
//      bot it has ever driven and never gives one back — a slow leak across the
//      uptime of a server whose bot roster turns over.
//
//   2. THE THREAD IS NOT A STABLE OWNER. The pattern rests on "a map's bots are
//      updated on one map-update thread, so each thread owns its table", which is
//      true only WITHIN a tick: AzerothCore hands a map to whichever pool thread
//      picks it up next. A map that migrates reads an EMPTY table, and every
//      throttle it holds silently lapses for that tick — a duplicate spline issue
//      on a bot that was deliberately not being re-ordered, a duplicate log line,
//      or (for the keeper/rift target latches that used the same shape) a lock
//      that quietly re-selects.
//
// Per-bot state belongs on the per-bot state. DcRunState carries one slot per id
// below: bounded by the bot, thread-safe by construction because a bot is only
// ever ticked by the thread updating its map, and cleared with the run.
//
// ADDING ONE is a row here plus a call. Keep the id named for the SITE, not for
// what it throttles — the point of the enum is that the set is auditable.
enum class DcThrottle : uint8
{
    // --- movement re-issue floors (destination + time) ---------------------
    TransitIssue,        // DcTransit::TravelTo — the BWL Suppression Rooms crossing
    VhTravelIssue,       // VhTravelTo — the Violet Hold arena
    BmTravelIssue,       // BmTravelTo — the Black Morass arena
    RazorgoreOrbIssue,   // the BWL egg runner's walk between eggs
    DtkCampIssue,        // the Drak'Tharon camp-return point move (time only)
    HosTravelIssue,      // HosTravelTo — the Halls of Stone Tribunal arena
    HorStayAheadIssue,   // the HoR escape's per-follower forward step
    TocMoveIssue,        // the ToC driver's point move (the horse, or the tank on foot)
    OcMoveIssue,         // the Oculus rider's drake move (the vehicle base) and its giver walk
    ChessMoveIssue,      // a Karazhan chess member's walk (sideline, a piece, Echo, the chest)

    // --- log throttles (time only) -----------------------------------------
    TransitLog,          // BwlTransitLog — one crossing telemetry line per 3s
    RazorgoreLog,        // the BWL egg-run step line
    VhTravelLog,         // VhTravelLog
    VhSinclariFlagLog,   // "in reach of Sinclari but she has no gossip flag"
    VhSinclariWalkLog,   // "walking N to Lieutenant Sinclari to start the siege"
    VhRestartLog,        // "instance is back at NOT_STARTED"
    VhWaveLog,           // the Violet Hold wave telemetry line
    BmTravelLog,         // BmTravelLog
    BmWaveLog,           // the Black Morass wave telemetry line
    HosTravelLog,        // HosTravelLog
    HosTribunalLog,      // the Halls of Stone Tribunal garrison line
    HosWaveLog,          // the Halls of Stone wave telemetry line
    RezRefusalLog,       // RezRefusalDiag — why a party rez was refused
    UpHarpoonLog,        // the Utgarde Pinnacle harpoon driver's telemetry line
    UpHarpoonMissingLog, // "in the pocket, and launcher 192175 is not there"
    PosGauntletLog,      // the Pit of Saron gauntlet driver's per-tick line
    PosLedgeLog,         // the Pit of Saron ledge hook's per-state line
    HorWaveLog,          // the Halls of Reflection altar driver's per-tick line
    HorEscapeLog,        // the Halls of Reflection escape driver's per-tick line
    HorEscapeIssue,      // the escape driver's own re-issue floor on the stand point
    HorIntroLog,         // "walking to Jaina/Sylvanas to start the intro"
    HorThroneLog,        // the throne-room gather / forge line
    HorEscapeGoLog,      // "not ready for the point of no return, because ..."
    HorStallWarn,        // the escape's "he is on her and the wall is still shut" WARN
    CosWaveLog,          // the Culling of Stratholme wave driver's per-tick line
                         // (its walk to the wave goes through DcTransit::TravelTo,
                         //  which owns the TransitIssue floor above, so there is no
                         //  movement slot of its own here)
    EscortResumeGossipLog,  // "the resume gossip left his gossip flag up" WARN
    TocTelemetryLog,     // the ToC driver's `DcToc progress=...` line
    TocWarn,             // the ToC driver's never-mounted / Knight-strand WARNs
    TocClick,            // the ToC driver's announcer click (and its log line)
    OcTelemetryLog,      // the Oculus driver's `DcOc ring=...` line
    OcWarn,              // the Oculus WARNs (blocked chord, stalled leg, fabricated essence)
    OcGossip,            // the Oculus rider's giver click (and its log line)
    OcEregosLog,         // the Oculus Eregos hold's per-fight line (hook 39)
    ChessLog,            // the Karazhan chess conductor's DC_CHESS state line
    ChessGossip,         // a chess member's piece gossip (one per 3s)
    ChessSeatTeleport,   // a chess controller's hop to its sideline seat

    // --- action floors (time only) -----------------------------------------
    UpHarpoonFire,       // floor between two Harpoon Launcher clicks (its own
                         // autoclose is 1000ms and GO_FLAG_IN_USE blocks a click
                         // until GameObject::Update clears it, so a faster cadence
                         // can only produce swallowed clicks and log noise)

    Count
};

struct DcThrottleSlot
{
    // The destination last issued, for the movement ids. Unused (and left at 0)
    // by the time-only ones.
    float  x = 0.0f;
    float  y = 0.0f;
    float  z = 0.0f;
    // getMSTime() of the last pass. 0 == never; the stampers below never write 0
    // back, so a wrap can never be read as "never".
    uint32 ms = 0;
};

inline constexpr std::size_t kDcThrottleCount = static_cast<std::size_t>(DcThrottle::Count);

#endif  // _PLAYERBOT_DCTHROTTLE_H
