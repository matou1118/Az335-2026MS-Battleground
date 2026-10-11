/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DcRegroupDecision.h"

namespace DcRegroupDecision
{
    RegroupVerdict DecideCombatRegroup(RegroupInputs const& in)
    {
        // Never clip a cast; never fight the CC handler. Same guard set as the
        // heal-reposition trigger. This wins even over the hard tether: a bot
        // mid-cast or stunned cannot move usefully anyway, so hold.
        if (in.casting || in.ccd)
            return RegroupVerdict::None;

        // Drifted past the outer tether: reconnect regardless of the contribution
        // test — the chased-runner-into-nowhere / left-far-behind case. Bypasses
        // debounce at the call site (the emergency path).
        if (in.tankDist2d > in.hardTether)
            return RegroupVerdict::HardTether;

        if (!in.isHealer)
        {
            // DPS (melee & ranged). A non-empty stock LOS-filtered attacker list
            // means the rotation + reach-spell(20)/MoveChase(30) have real work —
            // exactly the emptiness test ShouldAssistCampFight relies on. Empty =>
            // nothing visible to fight from here => reconnect. A ranged DPS whose
            // visible target is merely out of cast range is intentionally None:
            // stock `reach spell` closes toward the TARGET, which regroup-to-tank
            // used to fight (defect D1). A melee chasing a runner keeps a visible
            // attacker the whole chase => None until the hard tether.
            //
            // hasLosTarget is the second half of "can contribute", and it is the one
            // that keeps this rung from MUTING a working DPS. `attackers` only ever
            // contains mobs the bot or a groupmate holds real THREAT on (stock
            // AttackersValue walks GetThreatenedByMeList), so it reads empty in every
            // window where the party is combat-flagged but nobody has landed threat
            // yet — including the one DC manufactures itself, where the camp assist
            // seeds a target and SetInCombatWith()s the bot without any threat
            // relationship. In that window the bot HAS a live, valid, in-LOS target
            // its rotation could shoot, but a Reconnect verdict here latches and the
            // action (rel 29, above reach spell at 20 and the whole rotation at 5-20)
            // eats the tick — so the bot never casts, never gains threat, and the
            // empty-attackers state that armed the rung sustains itself. Standing
            // down whenever something shootable is in sight breaks that loop and
            // costs nothing: the around-the-corner case this rung exists for has no
            // in-LOS target by definition.
            return (in.hasVisibleAttacker || in.hasLosTarget) ? RegroupVerdict::None
                                                              : RegroupVerdict::Reconnect;
        }

        // Healer. HealReposition (rel 41) owns the hurt-target case — do not
        // double-own it. When a heal target is hurt, stand down here.
        if (in.hasHurtHealTarget)
            return RegroupVerdict::None;

        // Healer pre-positioning: nobody is hurt yet, but the healer is parked where
        // it could not heal the tank the moment damage starts (out of LOS, or beyond
        // heal range less a slack band so a step of tank movement doesn't drop it
        // straight back out). Reconnect to a heal-range LOS point; otherwise None.
        if (!in.tankLos || in.tankDist2d > (in.healRange - in.slack))
            return RegroupVerdict::Reconnect;

        return RegroupVerdict::None;
    }
}
