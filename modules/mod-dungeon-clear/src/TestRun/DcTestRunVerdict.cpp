/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DcTestRunVerdict.h"

namespace DcTestRun
{
    Verdict Classify(Observation const& o, Limits const& l)
    {
        // The run ending on its own beats every watchdog: DisableDungeonClear
        // already knows why it ended, and a timer firing on the same tick must
        // not overwrite a real success with a timeout.
        if (o.disableFired)
        {
            if (o.disableAllCleared)
                return Verdict::Success;
            // A scenario whose objective was already met: whatever disabled
            // the run afterwards happened in the event's tail.
            return o.scenarioSuccess ? Verdict::Success : Verdict::FailDisabled;
        }

        // Scenario predicate held and its grace ran out without all-cleared.
        if (o.scenarioSuccess && o.graceExpired)
            return Verdict::Success;

        if (o.abortRequested || o.leaderMissing || !o.gmOnline)
            return Verdict::FailAborted;

        // Inside a scenario's grace every watchdog below would be failing a run
        // that has already met its objective — the grace exists for the tail,
        // and a tail that goes wrong is still a success (successBy "predicate").
        if (o.scenarioSuccess)
        {
            bool const tripped = (o.partyWiped && o.wipedForMs >= l.wipeGraceMs) ||
                                 (o.paused && o.pausedForMs >= l.pauseGraceMs) ||
                                 (o.stalled && o.stalledForMs >= l.stallGraceMs) ||
                                 o.sinceProgressMs >= l.noProgressMs ||
                                 o.elapsedMs >= l.overallTimeoutMs;
            return tripped ? Verdict::Success : Verdict::Continue;
        }

        // Above pause/stall/no-progress: a corpse party reads as paused, stalled
        // AND frozen all at once, and "wipe" is the only one of the four that
        // says anything useful about the run.
        if (o.partyWiped && o.wipedForMs >= l.wipeGraceMs)
            return Verdict::FailPartyWiped;

        if (o.paused && o.pausedForMs >= l.pauseGraceMs)
            return Verdict::FailPausedTimeout;

        if (o.stalled && o.stalledForMs >= l.stallGraceMs)
            return Verdict::FailStalledTimeout;

        if (o.sinceProgressMs >= l.noProgressMs)
            return Verdict::FailNoProgress;

        if (o.elapsedMs >= l.overallTimeoutMs)
            return Verdict::FailOverallTimeout;

        return Verdict::Continue;
    }

    char const* SuccessBy(Observation const& o, Verdict v)
    {
        if (v != Verdict::Success)
            return "";
        if (o.disableFired && o.disableAllCleared)
            return "allCleared";
        if (o.scenarioSuccess && o.graceExpired)
            return "grace";
        if (o.scenarioSuccess)
            return "predicate";
        return "allCleared";
    }

    char const* VerdictName(Verdict v)
    {
        switch (v)
        {
            case Verdict::Success:            return "success";
            case Verdict::FailDisabled:       return "disabled";
            case Verdict::FailPartyWiped:     return "wipe";
            case Verdict::FailPausedTimeout:  return "paused_timeout";
            case Verdict::FailStalledTimeout: return "stalled_timeout";
            case Verdict::FailNoProgress:     return "no_progress";
            case Verdict::FailOverallTimeout: return "overall_timeout";
            case Verdict::FailAborted:        return "aborted";
            case Verdict::Continue:           break;
        }
        return "continue";
    }
}
