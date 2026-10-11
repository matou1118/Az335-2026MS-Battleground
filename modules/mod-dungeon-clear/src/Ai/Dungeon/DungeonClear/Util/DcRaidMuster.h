/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCRAIDMUSTER_H
#define _PLAYERBOT_DCRAIDMUSTER_H

class AiObjectContext;
class Player;
class PlayerbotAI;
struct DungeonBossInfo;

// Glue for Plan C's pre-boss RAID muster: the snapshots (staged / topped /
// rebuff-pending walks), the clocks in DcRunState, the ForceRebuff drive and
// the announcements around the pure kernel in DcRaidMusterDecision.h.
//
// It lives out here rather than inside DungeonClearEngageBossAction because the
// engage action is the WRONG and ONLY place it used to be called from. Every
// budget the muster owns — rest, rebuff, whole-muster ceiling — is compared
// against getMSTime() on the tick it is evaluated, so a muster that is only
// evaluated when the at-boss trigger lets the engage action run has no budget at
// all: the trigger's last gate is DcPartyState::IsBetweenPullsReady, and the
// muster's own 100/100 rest override made that gate unsatisfiable for a 40-man.
// Live at Vaelastrasz (2026-08-28): three evaluations in 126s against a 60s
// ceiling, and four sibling raids at 77s / 89s / 116s / 218s.
//
// So DungeonClearAtBossTrigger now ticks this EVERY tick on a raid boss anchor
// and gates on its verdict — the muster is the readiness gate there, and it is
// the one that is timeout-bounded. The engage action still calls it as the
// action-side half of the same guard (the trigger/action pairing this module
// uses everywhere); by then the phase reads Ready and it passes straight
// through.
namespace DcRaidMuster
{
    // True while the boss engage must hold this tick. Advances the phase machine
    // and owns its side effects; safe to call more than once per tick.
    bool Holds(Player* bot, PlayerbotAI* botAI, AiObjectContext* context,
               DungeonBossInfo const& next);
}

#endif  // _PLAYERBOT_DCRAIDMUSTER_H
