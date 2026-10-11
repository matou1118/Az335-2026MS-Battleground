/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DcNeverTargetRegistry.h"

namespace
{
    // ---- the table ------------------------------------------------------
    //
    // The Nexus (576) — Crystalline Frayer (26793), 44 spawns filling the
    // south-west garden the party crosses to reach Ormorok the Tree-Shaper.
    // `npc_crystalline_frayer` (instance_nexus.cpp) makes it unkillable until
    // Ormorok is dead, and then kills every one of them itself:
    //
    //     void JustEngagedWith(Unit*) override
    //     {
    //         _allowDeath = instance->GetBossState(DATA_ORMOROK_EVENT) == DONE;
    //     }
    //     void DamageTaken(Unit*, uint32& damage, ...) override
    //     {
    //         if (damage >= me->GetHealth() && !_allowDeath)
    //         {
    //             damage = 0;            // <-- the killing blow is discarded
    //             EnterSeedPod();
    //         }
    //     }
    //
    // EnterSeedPod parks it for NINETY SECONDS — REACT_PASSIVE, threat cleared,
    // NOT_SELECTABLE | IMMUNE_TO_PC | IMMUNE_TO_NPC, scale 0.6, and an Aura of
    // Regeneration (57056) ticking it back up — and LeaveSeedPod then returns it
    // to full health, REACT_AGGRESSIVE and roaming. Ormorok's death runs
    // `instance_nexus::KillAllFrayers()`, which strips those flags off every
    // frayer and `Unit::Kill`s it outright.
    //
    // So the clear's view of a frayer is binary and needs no instance-data read:
    // while one is ALIVE it cannot be killed, and the moment it can be killed it
    // is already dead. There is no window in which fighting one is progress.
    //
    // The dormant half of that cycle is already invisible to the clear —
    // IsPossibleTarget rejects NOT_SELECTABLE and IMMUNE_TO_PC — which is exactly
    // why filtering only the seed pod would not have fixed anything: the bots
    // wedge on the AWAKE frayer, whose only observable difference from ordinary
    // trash is that its health bar refills every 90 seconds. Hence a flat row
    // rather than an aura test.
    // ---------------------------------------------------------------------
    //
    // Ahn'kahet (619) — Jedoga Shadowseeker's ritual STAGING. Two entries, one
    // failure: the party walks off the ritual floor mid-encounter and the
    // encounter resets behind it.
    //
    // WHAT BREAKS. At 55% HP boss_jedoga_shadowseeker enters PHASE_RITUAL:
    //
    //     me->SetCombatMovement(false);
    //     me->InterruptNonMeleeSpells(false);
    //     me->AttackStop();
    //     me->SetReactState(REACT_PASSIVE);
    //     me->SetUnitFlag(UNIT_FLAG_NOT_SELECTABLE | UNIT_FLAG_NON_ATTACKABLE);
    //
    // and `damage = 0` for the rest of the phase. She takes off, hovers 13.7yd
    // above the floor and sacrifices a volunteer. For those seconds the party has
    // no boss to hit and no boss hitting it, so the CLEAR'S NON-COMBAT LADDER
    // TAKES OVER — and the corridor scan finds the ring of staging mobs the
    // encounter has just placed around the arena. Live (tr-20260825-224456-8,
    // tank Wieron, 23:02:48-23:03:33):
    //
    //     pull target vetoed — Jedoga Shadowseeker (untargetable)
    //     blocking-trash: 3 candidate(s) in band -> Entry: 30111 at 52.6yd   x25
    //
    // The tank walked 52yd off the ritual floor. Jedoga is REACT_PASSIVE with
    // combat movement off, so `CreatureAI::UpdateVictim` takes the passive
    // branch, her threat list empties behind the departing party, and she
    // EnterEvadeMode(NO_HOSTILES)s — `BossAI::_EnterEvadeMode` then
    // `summons.DespawnAll()`s the whole staging set and `Reset()` re-summons the
    // fifteen Twilight Initiates at full strength. The clear cannot recover from
    // that: the initiate objective (map 619 event 3) latched Done minutes ago and
    // is not Repeatable, so nothing clears them a second time and the party is
    // left standing at a boss that will never come down.
    //
    // 30111 TWILIGHT WORSHIPPER. Ten of them are summoned by
    // `JustEngagedWith` -> SummonCreatureGroup(SUMMON_GROUP_IC_WORSHIPPERS), set
    // kneeling, at z -17.95 and up to 65yd from the ritual floor. They are
    // scenery — kneeling congregation watching the sacrifice — but nothing marks
    // them as such: they are plain hostile SmartAI casters, so the corridor scan
    // reads them as a pack standing on the route out.
    //
    // The row also covers the SIX DB spawns in the lower chamber (z -31.6) the
    // party crosses on the way in, and that is accepted rather than worked
    // around: they aggro on their own (every 30111 first-contact in the live logs
    // is one of them opening the fight from 0.0yd off its spawn), and this table
    // only removes the clear's decision to go LOOKING — a worshipper that pulls
    // the party is still fought normally. There is no position in a
    // (mapId, entry) row to separate the two sets, and letting the clear seek the
    // approach six is not worth an unrecoverable reset at the boss.
    //
    // 30385 TWILIGHT VOLUNTEER. Twenty-five are summoned when the last initiate
    // dies, at z -31.6, and walk to a ring around the arena up to 36yd out.
    // Twenty-four of them are NOT_SELECTABLE | NON_ATTACKABLE, `SetImmuneToAll`
    // and `UNIT_STATE_STUNNED` for the whole encounter — they fit this table's
    // original criterion exactly, in that they cannot be killed at all. Only the
    // one Jedoga picks as `sacrificeTargetGUID` has those flags removed
    // (`npc_twilight_volunteer::DoAction(ACTION_RITUAL_BEGIN)`), and it then
    // walks to (373.5, -706.0, -16.2) — INTO the party — so nobody ever needs to
    // travel to reach it.
    //
    // Chasing it is what the clear was doing instead: `DcTargeting::
    // LeaderFightAnchor` resolves the regroup anchor off the leader's victim, so
    // the moment the tank picked the volunteer at its ring position the whole
    // party's standoff anchor moved with it — "regroup: moving to standoff
    // (36.3yd, anchor=Entry: 30385)", three members at once, in the same window.
    //
    // KILLING IT IS STILL THE RIGHT PLAY and this row does not stop it. Denying
    // Jedoga `SPELL_GIFT_OF_THE_HERALD` (56219) means killing the volunteer
    // before she does, and mod-playerbots' own `wotlk-ok` strategy does exactly
    // that (JedogaVolunteerTrigger / AttackJedogaVolunteerAction, which scan
    // `possible targets no los` by entry). That is the COMBAT engine, which this
    // table leaves untouched. All that is removed is the CLEAR's decision to walk
    // the tank out to the arena's edge for it.
    // Drak'Tharon Keep (600) — Novos Summon Target (27583). THE softlock on the
    // map, and the one row here whose failure mode is unrecoverable.
    //
    // WHAT IT IS. boss_novos' JustEngagedWith summons two of them into the Novos
    // chamber's opposite corners, at (-341.31, -724.40, 28.57) and
    // (-408.87, -730.21, 28.58). Its template reads `unit_flags 0`,
    // `flags_extra 128` (TRIGGER), faction 14, rank 1 (elite), level 74 — i.e.
    // it is a "trigger" only by convention. It carries NONE of the flags that
    // hide a trigger from the clear: not NOT_SELECTABLE, not NON_ATTACKABLE, not
    // IMMUNE_TO_PC. AttackersValue::IsPossibleTarget accepts it, and there is no
    // IsTrigger() test anywhere in mod-playerbots or mod-dungeon-clear. So to the
    // corridor scan it is simply a fresh elite standing 30-40yd away, in the room
    // the party is already fighting in.
    //
    // WHY KILLING IT ENDS THE RUN. The four Crystal Handlers are spawned by
    // `target->CastSpell(target, SPELL_SUMMON_CRYSTAL_HANDLER, ...)` on me->
    // m_Events at 16s / 32s / 48s / 64s, alternating between these two targets.
    // A dead unit cannot cast. Killing one therefore permanently prevents one or
    // both of its two handlers from ever spawning — and a handler's death is the
    // ONLY thing that removes a Beam Channel (52106) from Novos. The 70s gate
    // task tests `me->HasAura(SPELL_BEAM_CHANNEL)` and repeats every 2s FOREVER
    // while it holds, with no timeout escape, so Novos never becomes attackable.
    // The encounter is unwinnable until the instance resets: not a wipe, not a
    // stall the human can unstick at the keyboard, a dead run.
    //
    // This is class 2 of the two above (killing it is NEGATIVE progress) in its
    // purest form: it is encounter staging, placed by the script, and the party
    // has no reason to travel to it and every reason not to.
    //
    // NOT LISTED, on purpose: Darkweb Victim (27909), the six cocooned civilians
    // in the corridor between Trollgore and Novos. Killing one rolls 49958/49959
    // and hands the party a free level-76 elite at the corpse, so it is genuine
    // negative progress — but only one of the six ((-287.1, -701.2)) is within
    // ~10yd of the route and the rest are 20yd+ east of it, off-path. This table
    // is meant to stay small and justified; add the row if run data shows the
    // clear actually detouring to them.
    // The Violet Hold (map 608) — the Azure Saboteur (31079).
    //
    // Class 1: killing it is not merely useless, it is IMPOSSIBLE. Its template
    // carries unit_flags 768 = IMMUNE_TO_PC | IMMUNE_TO_NPC and the script never
    // clears them, so the party literally cannot damage it. But it is faction
    // 1720, red-name and fully SELECTABLE, and on waves 6 and 12 it walks the
    // ENTIRE room — the middle-of-the-room portal to whichever prisoner's cell the
    // instance rolled, up to 88yd away — casting Shield Disruption at the far end.
    //
    // AttackersValue::IsPossibleTarget does already reject IMMUNE_TO_PC, so the
    // combat engine will not pick it. This row is about the CLEAR, which is a
    // different question with a different answer: the saboteur is the only moving
    // hostile on the field during a boss-release wave, exactly when the wave
    // driver is otherwise idle at the door camp, and it is the shape that pulls a
    // clear across an arena (the Ahn'kahet Twilight Volunteer, one map over,
    // failed in precisely this way — LeaderFightAnchor resolved the party's
    // standoff onto a mob nobody could kill). Belt and braces on a mob whose whole
    // job in the encounter is to be walked past.
    //
    // NOT LISTED: the Prison Door Seal (30896), Defense Dummy Target (30857) and
    // Defense System (30837). All three are NOT_SELECTABLE as well as immune, so
    // they are invisible to every selector on both sides of the question.
    // Blackwing Lair (469) — the four Corrupted Whelps (14022 red / 14023 green /
    // 14024 blue / 14025 bronze), and the whole of class 3.
    //
    // THE ARITHMETIC, which is the only argument this row has and the only one it
    // needs. The two Suppression Rooms between Vaelastrasz and Broodlord Lashlayer
    // hold 160 of them — 79 on the lower floor (z 440), 81 on the upper (z 449) —
    // on a THIRTY-SECOND respawn. That is a spawn rate of 5.3/s across the
    // complex and ~3.3/s for the hundred that sit within 20yd of the 375yd route
    // line the clear has to cross. A 40-bot raid can out-DPS that. It cannot
    // out-TRAVEL it, because DcCombatFlag::MayDrive is false for as long as
    // anything in the party is engaged and Advance is registered only in the
    // NON-combat engine — so with the whelps up the clear has no driver at all.
    // Over the 268 seconds the crossing takes at the Suppression Devices' -80%
    // move speed the route-adjacent hundred respawn NINE times: ~900 whelp kills
    // to cross one leg, against a party moving at 1.4yd/s.
    //
    // WHAT THE CLEAR WAS DOING WITH THEM, which is the part this row removes. The
    // corridor blocking-trash scan, the en-route pack sweep and the map-wide
    // stalled fallback all read a whelp as a legitimate target, and they pick the
    // NEAREST one — so the tank is continuously walked BACK into the room it has
    // just crossed for a mob that will be replaced in thirty seconds. Half of
    // "they just kill trash forever" is that walk, and it is pure loss: the whelps
    // are level-60 normals with no AI, no script and no formation, worth nothing
    // to a raid that only has to get past them.
    //
    // NOT PACIFISM, and this matters more here than anywhere else in the table.
    // The stock combat engine is untouched, so a whelp that aggros a bot is still
    // fought and still cleaved down — which on a 375yd walk through a hundred of
    // them is free AoE and exactly what the transit wants. All that is removed is
    // the clear's decision to go LOOKING.
    //
    // NOT LISTED, deliberately: the Death Talon Hatchers (12468) and Blackwing
    // Taskmasters (12458) in the same two rooms. They are elites on a TEN-MINUTE
    // respawn, thirteen of the twenty are within 25yd of the route, and six
    // Taskmasters stand on the only ramp between the two rooms — killing those is
    // real progress that stays bought, and the transit driver stands and fights
    // them on purpose. Class 3 is about a spawn rate, not about difficulty.
    // --- Halls of Stone (599) — Sjonnir's Earthen Dwarves (27980) -----------
    //
    // CLASS 2: killing it is NEGATIVE PROGRESS, and here that is not a figure of
    // speech — these fight ON YOUR SIDE.
    //
    // boss_sjonnir's 25% health-check cancels the previous add spawner and starts
    // one that summons THREE Earthen Dwarves every 10-20s, and for each one does
    // `if (Player* plr = SelectTargetFromPlayerList(100.0f)) dwarf->SetFaction(
    // plr->GetFaction());` before `dwarf->AI()->AttackStart(me)`. They are handed
    // a player's own faction and sent at the boss: free DPS for the last quarter
    // of the fight, and the reason achievement 2155 exists.
    //
    // The reason a row is needed at all rather than leaning on the faction copy is
    // the `if`. SelectTargetFromPlayerList(100.0f) returns nullptr when nobody is
    // inside 100yd — a corpse run, a kited tank, a party spread across Sjonnir's
    // 135yd-long room — and the dwarf then keeps its TEMPLATE faction 1868, which
    // is hostile. IsPossibleTarget passes it, the clear's pickers select it, and
    // the party turns away from a boss that is summoning three more every ten
    // seconds to kill an ally that would have despawned on its own in twenty
    // (TEMPSUMMON_CORPSE_TIMED_DESPAWN, 20000ms).
    //
    // NOT PACIFISM: the stock combat engine is untouched, so a dwarf that somehow
    // attacks a bot is still fought. All this removes is the clear's decision to
    // go looking.
    //
    // DELIBERATELY ABSENT — the rest of map 599, and it is a long list, because
    // AttackersValue::IsPossibleTarget already rejects UNIT_FLAG_NOT_SELECTABLE
    // and UNIT_FLAG_IMMUNE_TO_PC and every other candidate is one or the other:
    //   30898 Kaddrak / 30897 Marnak / 30899 Abedneum  unit_flags 33554436
    //   28235 Dark Matter / 28237 Dark Matter Target   unit_flags 33554432
    //   28265 Searing Gaze / 28824 Brann Flying Machine        "
    //   28130 Invis Lightning Stalker / 28055 Channel Target   "
    //   22515 World Trigger                            unit_flags 33555200
    //   28149 Earthen Protector                        unit_flags 768 (IMMUNE_TO_PC|NPC)
    //   30535 Elder Yurauk                             faction 35, friendly
    // Adding them would be dead rows that read as a safety net and are not one.
    // Sjonnir's own infinite pipe adds (27979/27981/27982) are NOT here either:
    // they are ordinary hostile trash that dies, and each HP threshold REPLACES
    // the previous spawner rather than stacking, so the stream stops at his death
    // — that is difficulty, not a fixed point the party can never move.
    DcNeverTargetRow const kRows[] =
    {
        { 576, 26793 },  // The Nexus — Crystalline Frayer (seed pod; unkillable until Ormorok dies)
        { 619, 30111 },  // Ahn'kahet — Twilight Worshipper (Jedoga's kneeling congregation, 65yd out)
        { 619, 30385 },  // Ahn'kahet — Twilight Volunteer (24/25 permanently unattackable; the 25th walks in)
        { 600, 27583 },  // Drak'Tharon Keep — Novos Summon Target (killing one softlocks the Novos gate)
        { 608, 31079 },  // The Violet Hold — Azure Saboteur (IMMUNE_TO_PC bait that walks the whole room)
        { 599, 27980 },  // Halls of Stone — Earthen Dwarf (Sjonnir 25%: faction copied from a player; they fight FOR you)
        { 469, 14022 },  // Blackwing Lair — Corrupted Red Whelp    (160 whelps, 30s respawn: infinite)
        { 469, 14023 },  // Blackwing Lair — Corrupted Green Whelp  (see the class-3 note above)
        { 469, 14024 },  // Blackwing Lair — Corrupted Blue Whelp
        { 469, 14025 },  // Blackwing Lair — Corrupted Bronze Whelp

        // --- Halls of Reflection (668) ------------------------------------
        //
        // FOUR ROWS, and only the first of them is load-bearing. This registry
        // answers "is killing this progress" and only filters the CLEAR's own
        // scans; the stock combat engine is untouched. For the Lich King that is
        // NOT enough on its own — he is a fully legal target that stock assist
        // triggers acquire by themselves — which is why he also carries a
        // windowed DcTargetExclusionRegistry row with alsoTank. This row is the
        // other half: it keeps the clear's FarTargets / RoomTrash / BlockingTrash
        // scans from ever proposing him in the first place, so nothing walks the
        // party toward him and nothing marks him.
        //
        // The other three are dead-row insurance of the kind the Halls of Stone
        // note above argues against, with one difference that earns them: they
        // are all NAMED CREATURES STANDING ON THE PARTY'S PATH, and their flags
        // change during the run. The intro Lich King is immune while he walks in
        // and then despawns; Uther stands on the altar the party camps on for
        // nine minutes; the Ice Wall Targets sit exactly where the escape's
        // stand points are. A flag test that is right today is a thinner
        // guarantee than a row for a creature killing which can never be
        // progress under any circumstances.
        { 668, 36954 },  // Halls of Reflection — the Lich King (escape; he heals to 75% and
                         //   holding him freezes the party's movement — see the exclusion row)
        { 668, 37226 },  // Halls of Reflection — the intro Lich King (RP; immune and passive)
        { 668, 37225 },  // Halls of Reflection — Uther (RP ghost on the altar the party camps)
        { 668, 37014 },  // Halls of Reflection — Ice Wall Target (invisible world trigger)

        // --- Trial of the Champion (650) ------------------------------------
        //
        // Both are summons that land in the middle of a fight, and neither passes
        // the flag filters that hide a helper: checked against creature_template,
        // not assumed.
        //
        // 35614 DESECRATION STALKER is the Novos Summon Target shape — a trigger
        // by flags_extra (128) only, with unit_flags 0 and faction 14, so
        // IsPossibleTarget accepts it. It spawns UNDER a player for 15 seconds, so
        // to the clear's scans it is the nearest hostile on the map at 0yd. Killing
        // it is not progress; leaving it is (see its hazard emitter).
        //
        // 35311 FOUNTAIN OF LIGHT, the Argent Priestess's healing fountain:
        // unit_flags PACIFIED only, faction 16. Killing it is fine, and the stock
        // combat engine still will if it comes to that — but it outlives the pack
        // that summoned it, and the clear must not walk the party across the bowl
        // after a fountain between soldier packs.
        { 650, 35614 },  // Trial of the Champion — Desecration Stalker (the Black Knight's ground hazard)
        { 650, 35311 },  // Trial of the Champion — Fountain of Light (the Priestess's summon)
    };
}

bool DcNeverTargetRegistry::IsNeverTarget(uint32 mapId, uint32 entry)
{
    for (DcNeverTargetRow const& r : kRows)
        if (r.mapId == mapId && r.entry == entry)
            return true;
    return false;
}
