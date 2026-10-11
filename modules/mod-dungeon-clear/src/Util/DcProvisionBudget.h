/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCPROVISIONBUDGET_H
#define _PLAYERBOT_DCPROVISIONBUDGET_H

// ONE PlayerbotFactory::Randomize per world tick, across every subsystem that
// provisions bots.
//
// Randomize is a full gear/spell/talent roll and several of them in one tick is
// a visible server stall, so the test harness has always rationed itself to one
// per tick. That ration used to be a local `bool provisionBudget` in
// DcTestRunManager::Tick, handed round-robin to its runs — which bounds the
// harness against itself and nothing else. The RDF queue-fill subsystem
// provisions from the same factory on the same tick, so a live fill running
// while a test plan is in flight would roll two.
//
// Hoisted here so the ration is realm-wide: Reset() once per world tick from
// the module's global tick, Take() from every provisioner. First caller of the
// tick wins; everyone else retries next tick, bounded by their own stage
// timeout.
//
// World-thread only by construction — the only Reset() call site is the world
// tick and every Take() call site is reached from it, so a plain static bool is
// the whole implementation. No lock, no atomic: making it threadsafe would
// imply it may be called from a map-update thread, which it may not.
namespace DcProvisionBudget
{
    // Re-arm the tick's single roll. Exactly one call site: the module's
    // per-world-tick hook, before any provisioner ticks.
    void Reset();

    // Claim this tick's roll. True at most once between two Reset() calls;
    // false means "another provisioner already spent it — retry next tick".
    bool Take();

    // Peek without claiming, for status/diagnostic text only.
    bool Available();
}

#endif  // _PLAYERBOT_DCPROVISIONBUDGET_H
