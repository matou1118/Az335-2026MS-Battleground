/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCOCULUSPLAN_H
#define _PLAYERBOT_DCOCULUSPLAN_H

#include "Define.h"

#include "Ai/Dungeon/DungeonClear/Util/DcOculusDriverDecision.h"

class Player;

// The Oculus party plan (see DcOculusDriverDecision.h) as a member reads it.
//
// The plan lives on the RUN OWNER's DcRunState and is re-derived at most every
// PLAN_MEMO_MS by whichever member asks first — the owner's driver hook or any
// member's rider rung — so it stays current even when the tank is dead and its
// own hook no longer runs. Defined in Overrides/OculusDriver.cpp.
struct DcOculusPlanView
{
    bool                   valid = false;  // map 578, a running (unpaused) run
    DcOculusDriver::Phase  phase = DcOculusDriver::Phase::Idle;
    DcOculusDriver::Action action = DcOculusDriver::Action::Yield;
    uint8                  dest = DcOculus::SITE_NONE;
    bool                   hover = false;
    uint32                 legSeq = 0;
    bool                   tankOnFootOnDest = false;
    bool                   eregosEngaged = false;
    Player*                owner = nullptr;
};

DcOculusPlanView OculusPlanFor(Player* bot);

// This member's lane: 0 for the run owner, then 1.. in GUID order over the rest
// of the group on the map, dead or alive, so a death mid-leg never re-lanes
// anyone.
uint32 OculusLaneOf(Player* bot);

// Revive the party at the portal landing (DcRezRecovery::RegroupAtEntrance, which
// sends map 578 there) — at most once per REGROUP_COOLDOWN_MS for the run.
bool OculusRegroup(Player* bot);

#endif  // _PLAYERBOT_DCOCULUSPLAN_H
