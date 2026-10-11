/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DcHazardRegistry.h"

#include "Ai/Dungeon/DungeonClear/Util/DungeonClearMath.h"

#include <array>
#include <cmath>

namespace
{
    // ---- the table ------------------------------------------------------
    //
    // The Arcatraz (map 552), entry 20869 "Arcatraz Sentinel". Mechanical elite.
    // SmartAI resets it to REACT_AGGRESSIVE at 40% HP, and it carries three addon
    // auras: 11838 (threat-to-zero, so it does not aggro from threat while idle),
    // 31261 (Permanent Feign Death — the ROOT that pins a DORMANT one in place,
    // REMOVED on aggro so it then chases and melees), and 36716 "Energy Discharge"
    // — SPELL_AURA_PERIODIC_TRIGGER_SPELL, period 1000ms, firing 36717 for 563-937
    // at EffectRadiusIndex 18 = 15.0yd. Nothing removes 36716, so the 15yd pulse
    // runs dormant AND in combat. Heroic (21586) swaps 36716->38828->38829,
    // 938-1562, same 15yd/1s. Five spawns, dormant coords:
    //     (255.498, 158.914, 22.362)   (253.942, 131.881, 22.395)
    //     (264.287, -61.321, 22.453)   (336.514,  27.427, 48.426)
    //     (395.413,  18.195, 48.296)
    // radius 22 = the 15yd pulse plus 7yd of drift margin. Below the 30yd caster
    // range on purpose, so a ranged bot can still hold a line past one.
    //
    // The live Sentinel's <=10% "Explode" (36719 -> 36722, ~5000 in 10yd) is NOT
    // registered as a threat: 36719 also MOD_STUNs the sentinel for its own 6s
    // wind-up, so the party simply bursts the helpless 10%-HP mob down before it
    // detonates. Pulling DPS off it to dodge would keep it alive long enough to
    // actually explode — a self-inflicted wound. So no explode handling here.
    //
    // Entry 21761 "Destroyed Sentinel" — the run-wiper the live one summons on
    // death (event 6 -> spell 37394, which summons the fixed creature 21761 for
    // both normal and heroic). NOT_SELECTABLE (unit_flags 33555200) so the party
    // cannot target/kill it, hostile, and it carries the permanent 36716 -> 36717
    // pulse (15yd, 1s, 563-937). It spawns right where the party just killed the
    // Sentinel and ticks until it despawns, often after combat has ended — so
    // vacateRadius drives an active retreat in BOTH engines. Its `radius` (the
    // camp/route keep-out consumed by PointIsHot) is the RAW 15yd pulse, NOT the
    // padded 22 the live fixture uses: the retreat aims pulse+slack = 19yd, and a
    // padded radius would make PointIsHot reject that point as inside this very
    // emitter's cylinder, so the retreat could never find a clear spot.
    //
    // Entries 21303 "Defender Corpse" / 21304 "Warder Corpse" — proximity bombs.
    // SmartAI event 10 (OOC line of sight, param2 = 8) or event 4 (on aggro) fires
    // actionlist 2130400: cast 36599 "Bloody Explosion" + 36593 "Corpse Burst",
    // then despawn. One-shot, avoidance-only. Tighter 12 = 8yd trigger plus 4yd
    // margin, no route penalty box (12yd is inside ordinary pathing jitter).
    //
    // NOT REGISTERED: the Eredar room's two 45yd auras. Its three spawn points are
    // `creature_multispawn`, each rolling Eredar Soul-Eater (20879, "Entropic
    // Aura" 36784 — 45yd, -25% haste and speed, no damage) or Eredar Deathbringer
    // (20880, "Unholy Aura" 27987/38844 -> 27988/38845 — 45yd, 450 normal / 750
    // heroic SCHOOL_DAMAGE every 2s to every enemy in range), re-rolled on every
    // respawn.
    //
    // The Deathbringer's pulse is genuinely lethal, and a keep-out is still the
    // wrong tool for it: 45yd is wider than the ranges the party works at, so
    // sized honestly it refuses every route through the wing, and sized to obey
    // the threat-1 rule (below caster range) it is a lie — at 30yd you take full
    // damage. There is no standing-off from it and no routing around it. The only
    // answer is to cross and kill, which is a set-piece: ArcatrazEvents.cpp event
    // 2, a ClearRadius over the three spawns filtered to both entries.

    // Two corpse clusters overlap Sentinels — the (264.3,-61.3) Sentinel sits
    // beside a Defender Corpse at (272.1,-59.0), and the (395.4,18.2) Sentinel
    // sits inside the corpse pair (392.1,24.9)/(395.1,27.6) — so routing through
    // either takes the pulse AND trips a bomb. Both are covered by the Sentinel
    // penalty boxes in DcNavPenaltyRegistry.
    //
    // Maraudon (map 349), entry 12222 "Creeping Sludge" — 24 spawns, the single
    // biggest source of wipes in the instance. It carries a PERMANENT
    // creature_template_addon aura, 22638 "Poison Shock": passive, infinite
    // duration, SPELL_AURA_PERIODIC_TRIGGER_SPELL on a 2000ms period firing
    // 22595, which is SPELL_EFFECT_SCHOOL_DAMAGE for 181-221 nature at
    // EffectRadiusIndex 8 = 5.0yd against TARGET_UNIT_DEST_AREA_ENEMY around the
    // sludge. Nothing removes it, so the ~100 dps sphere runs while the sludge is
    // idle as well as in combat — merely walking a follower within 5yd of a
    // sleeping one costs ~200 and pulls it.
    //
    // The sludge is what makes a keep-out the RIGHT tool rather than a lie: at
    // speed_run 0.285714 it moves ~2.0 yd/s, under a third of a player's 7.0, so
    // a bot that stays out of the sphere can never be dragged back into it. That
    // is the opposite of the Eredar Deathbringer note below, where the aura is
    // wider than the party's working ranges and standing off is impossible.
    //
    // radius 8 = the 5yd pulse plus 3yd of drift margin, the same sizing as the
    // Noxious Cloud pool below and for the same reason: wide enough that a camp
    // anchor or a fan-out slot is not planted on the rim, tight enough that 24 of
    // them do not sterilise the corridors the party still has to clear.
    //
    // MELEE NEVER TRADE WITH THIS MOB. That is how the sludge is meant to be
    // fought and it is what the vacate bands encode: melee reach is 3D < 4.75yd
    // against a 5.0yd pulse, so "in melee" and "in the aura" are the same place —
    // there is no stance from which a melee bot can hit it without eating ~100 dps,
    // and against 4-8 of them at once that is 400-800 dps on the melee cluster. The
    // sludge is slow enough that nobody has to accept that trade: the party leaves
    // it standing, the ranged shoot it down, and it never catches anyone.
    //
    // Hence vacateRadius 5 with holdBand 6 (in danger inside 11yd) and
    // retreatSlack 9 (flee to 14yd). The wide hold band is the whole point — it
    // has to outlast the bot's own MoveChase, which will keep pulling it back to
    // 4.75yd. At holdBand 2 the bot would read clear at 7yd, get chased back to
    // 4.75, and oscillate THROUGH the aura taking roughly half the damage for none
    // of the benefit. At 6 the chase never gets it closer than 11 before the
    // retreat pushes it back out to 14, so it orbits the pack and takes nothing.
    //
    // First attempt at this row (S1799) set vacateRadius 0 — avoidance only, on the
    // reasoning that melee must be able to swing. tr-20260815-134844-3 and -5 are
    // what that cost: the tank stood in the sphere for the whole fight and died on
    // the first sludge pull of both runs, ending them at 4m24s and 3m41s.
    //
    // No DcNavPenaltyRegistry counterpart: every Creeping Sludge spawn has
    // MovementType != 0 with wander_distance 1-5, so there is no author-time
    // position to box off. Like the ground pools, the live predicates are the whole
    // route defence here.
    //
    // NOT registered: entry 12221 "Noxious Slime" (27 spawns, same instance).
    // Its creature_template_addon auras column is NULL — it carries no permanent
    // pulse at all, and it runs at normal speed. Its only ground threat is the
    // Noxious Cloud it shares with the sludge, which is a pool row, not a creature
    // row. Giving it a creature keep-out would fence off a mob that is not actually
    // emitting anything.
    // Utgarde Keep (map 574), entry 23997 "Ingvar Throw Dummy" — Ingvar the
    // Plunderer's thrown axe, phase 2 only. boss_ingvar_the_plunderer casts 42749
    // "Throw Axe", which SUMMONS this dummy and (JustSummoned) sends it to a
    // RANDOM party member's position; the dummy carries a permanent
    // creature_template_addon aura 42750, SPELL_AURA_PERIODIC_TRIGGER_SPELL on a
    // 1000ms period firing 42751 — SPELL_EFFECT_SCHOOL_DAMAGE for 1750-2250
    // shadow at EffectRadiusIndex 5.0yd around the dummy — until EVENT_AXE_PICKUP
    // despawns it ~10s later.
    //
    // ~2000 dps in 5yd, dropped ON somebody, in the middle of the boss fight. It
    // is a threat-2 emitter by construction and there is no version of "fight it":
    // creature_template unit_flags 33554432 is UNIT_FLAG_NOT_SELECTABLE and its
    // AIName is NullCreatureAI, so nothing can target it and nothing it does can
    // be interrupted. Leaving is the whole answer.
    //
    // The bands are the Destroyed Sentinel's "leave, then carry on" pair (hold 2,
    // slack 6), NOT Maraudon's wide stay-out pair, and that is deliberate: the
    // party is mid-encounter with a live boss it must keep tanking, the dummy
    // despawns on its own in ~10s, and a wide hold band would walk the melee off
    // Ingvar for a hazard that is about to delete itself. Danger band is
    // 5 + 2 = 7yd; the retreat aims at 5 + 6 = 11yd, outside this row's own 7yd
    // placement radius so PointIsHot cannot reject the landing spot.
    //
    // radius 7 = the 5yd pulse plus 2yd of margin, and no wider: the dummy lands
    // on the floor the party is actively fighting on, so an over-wide keep-out
    // would sterilise Ingvar's own arena for placement.
    //
    // No DcNavPenaltyRegistry counterpart — the dummy has no author-time position
    // at all (it lands wherever a random member was standing), so the live
    // predicates are the whole defence, exactly as for the ground pools.

    // --- Halls of Stone (map 599), the Tribunal of Ages: TWO EMITTERS ------
    //
    // These are the rows the plan for this dungeon got WRONG, and the correction
    // is worth stating because it is a whole-table mistake rather than a tuning
    // one. Both hazards were sketched as DcGroundHazard rows keyed on the cast
    // spell. `DcGroundHazard::spellId` is what DynamicObject::GetSpellId()
    // returns, i.e. it only ever matches a spell with a
    // SPELL_EFFECT_PERSISTENT_AREA_AURA (27) effect — and NEITHER of these has
    // one, so both rows would have sat in the table matching nothing, forever,
    // looking exactly like coverage. Read from Spell.dbc:
    //
    //   51136 Searing Gaze  eff0 APPLY_AURA(6)/DUMMY on the trigger itself,
    //                       eff1 APPLY_AURA(6)/PERIODIC_TRIGGER_SPELL amplitude
    //                       500ms -> 51125, whose SCHOOL_DAMAGE radius is 5.0yd.
    //   51012 Dark Matter   eff0/eff1 APPLY_AURA(6) debuffs + eff2
    //                       SCHOOL_DAMAGE, all at radius 5.0yd. A one-shot nova
    //                       at the chaser's detonation point, not a pool at all.
    //
    // So both are CREATURES carrying/casting an aura, which is precisely what
    // DcHazardEmitter is for, and both are keyed on the trigger's entry instead.
    //
    // 28265 SEARING GAZE is the textbook threat-2 emitter: brann_bronzebeard
    // summons it AT A RANDOM PLAYER'S EXACT POSITION (TEMPSUMMON_TIMED_DESPAWN
    // 10000) and it casts 51136 on itself, so it materialises under someone's feet
    // and ticks 5yd of damage twice a second for its whole life. It is
    // NOT_SELECTABLE, so there is nothing to kill and no reason to stay: vacate 5
    // (the measured radius), keep-out 8 for placement drift, and the DEFAULT 2/6
    // bands — a fixed patch of ground you step past once and then carry on.
    //
    // 28237 DARK MATTER TARGET is the same shape with a delay: it spawns at
    // (899.843, 355.271, 214.301), waits ~5s, then MOVES to a random player's
    // position and detonates. Vacating it is therefore worth more than vacating a
    // static pool — the bot is walking out of the path of something still
    // travelling — and it costs nothing when it detonates elsewhere. Same 5yd
    // measured radius, same bands.
    //
    // The keep-out radii are deliberately MODEST (8, not 15). Both of these land
    // on top of the party in the middle of a 300-second defend the party may not
    // leave: the hold point is 25yd from Brann and the whole intercept line is
    // ~10yd wide, so an over-wide keep-out would sterilise the one piece of ground
    // the encounter requires the party to stand on and push it off the line to
    // dodge a puddle that expires in ten seconds.
    //
    // DELIBERATELY ABSENT, all three checked against Spell.dbc rather than assumed:
    //
    //   50988 GLARE OF THE TRIBUNAL — no row is possible. It is a single-target
    //     SCHOOL_DAMAGE beam on a random player within 100yd every 1.5s for ~250s.
    //     There is no radius and nowhere to stand; it is sustained raid damage the
    //     healer covers, and the reason to confirm a resto healer sustains normal
    //     before attempting heroic.
    //   50840 / 59848 / 51849 SJONNIR'S LIGHTNING RING — a row here would be
    //     actively harmful. All three are self-auras on Sjonnir that periodically
    //     trigger 50841/59849, a 10.0yd nova CENTRED ON HIM. A 10yd keep-out
    //     around a melee boss is a keep-out around the tank's own position: it
    //     would push the melee out of the fight for the whole encounter. This is
    //     healed through, not dodged.
    //   52341 / 59038 ELECTRICAL OVERLOAD — the Lightning Construct's on-death
    //     10yd nova. One instant SCHOOL_DAMAGE with no aura, no DynamicObject and
    //     no duration: by the time anything could react it has already resolved.
    //     There is no persistent volume for the placement or vacate machinery to
    //     act on.

    // ---- Halls of Reflection (668): Remorseless Winter -------------------
    //
    // The escape's Lich King (36954) carries 69780 for the whole run, ticking
    // 69781 every second for 7068 +/- 863 frost to everything within 10 yards.
    // That is the largest per-second number in this table by an order of
    // magnitude, and the row that models it is deliberately the WEAKEST kind
    // here: a placement keep-out with NO vacateRadius.
    //
    // WHY NOT A VACATE ROW, when the damage plainly warrants one. Because
    // DungeonClearHazardVacateAction retreats RADIALLY — a point directly away
    // from the emitter, past its pulse — and on this encounter "away from him" is
    // the single worst direction a bot can move. The path runs -x and -y, he
    // follows the party down it, and every 2 seconds each player whose
    // (p.x - lk.x) + (p.y - lk.y) exceeds 20 takes 10 000 damage AND A KNOCKBACK
    // THAT THROWS THEM FURTHER BEHIND. A radial vacate fired on a bot that is
    // already behind him aims it deeper into that rule, and the rule then
    // reinforces itself. The correct move is FORWARD, along the path, to the
    // party's stand point — which is a different action
    // (DungeonClearHorStayAheadAction, relevance 56) and a different registry's
    // job than this one.
    //
    // So this row does exactly the half a radial answer CAN do correctly: it
    // keeps camp anchors, engage standoffs and skirt legs out of a 12yd cylinder
    // around him — right for a bot that is AHEAD, which is where the whole party
    // is meant to be — and leaves the behind case to the action that knows which
    // way forward is. If DcHazardRegistry ever grows a "vacate toward a point"
    // mode, this row is the first thing to fold into it.
    //
    // NOT WINDOWED, because the table has no mechanism for it and does not need
    // one here: before the escape he is frozen at his spawn 36yd from where the
    // party musters, and after it the run is over. A permanent 12yd keep-out
    // around this creature is correct at every moment of the dungeon.
    // ---- Trial of the Champion (650): Desecration ------------------------
    //
    // The Black Knight's phase-2 Desecration (67778 -> 67779) summons creature
    // 35614 at a player's position for 15 seconds, and the stalker carries the
    // ground aura 67781, radius index 14 = 8 yards. A CREATURE, not a
    // DynamicObject, so it belongs in this table rather than the ground-pool one.
    // It is a "trigger" by flags_extra only — unit_flags 0, faction 14 — so it
    // also carries a never-target row.
    //
    // The ordinary leave-once shape, the Searing Gaze row's: vacate the RAW 8yd
    // pulse, keep-out 11 for placement drift, default 2/6 bands — the retreat
    // aims at 14, outside the 11yd cylinder, so it always finds a spot it
    // accepts. It lands under someone mid-fight in an open bowl, and stepping
    // past it is all it asks.
    //
    // ---- The Oculus (578): Drakos's Unstable Spheres ------------------------
    //
    // Creature 28166, NOT_SELECTABLE | PACIFIED: two every 2s (four more after
    // each Magic Pull), wandering 40yd around (961.29, 1049.0) for 10s, then
    // sitting and pulsing 50757 every 2s for the rest of their 18. A moving
    // creature emitter — the Blaze shape — rather than a ground pool, so it lives
    // here. Vacate 8 aims the retreat at 14, well clear of the 6yd keep-out, and
    // stock `wotlk-occ`'s `avoid unstable sphere` (12yd, sitting spheres only)
    // runs alongside it. If a battery shows the vacate ping-ponging in a dense
    // field, this row is the lever.
    constexpr std::array<DcHazardEmitter, 11> kEmitters = {{
        //                    radius  zBand  vacate  hold  slack
        { 578, 28166, /*Unstable Sphere    (leave once)  */  6.0f,  8.0f,  8.0f, 2.0f, 6.0f },
        { 668, 36954, /*Lich King, Remorseless Winter    */ 12.0f, 10.0f,  0.0f, 2.0f, 6.0f },
        { 650, 35614, /*Desecration stalker (leave once) */ 11.0f, 12.0f,  8.0f, 2.0f, 6.0f },
        { 552, 20869, /*Arcatraz Sentinel  (fought)      */ 22.0f, 12.0f,  0.0f, 2.0f, 6.0f },
        { 552, 21761, /*Destroyed Sentinel (leave once)  */ 15.0f, 12.0f, 15.0f, 2.0f, 6.0f },
        { 552, 21303, /*Defender Corpse                  */ 12.0f,  8.0f,  0.0f, 2.0f, 6.0f },
        { 552, 21304, /*Warder Corpse                    */ 12.0f,  8.0f,  0.0f, 2.0f, 6.0f },
        { 349, 12222, /*Creeping Sludge    (STAY OUT)    */  8.0f,  6.0f,  5.0f, 6.0f, 9.0f },
        { 574, 23997, /*Ingvar Throw Dummy (leave once)  */  7.0f, 10.0f,  5.0f, 2.0f, 6.0f },
        { 599, 28265, /*Searing Gaze trig  (leave once)  */  8.0f,  8.0f,  5.0f, 2.0f, 6.0f },
        { 599, 28237, /*Dark Matter Target (leave once)  */  8.0f, 12.0f,  5.0f, 2.0f, 6.0f },
    }};

    // ---- the ground-pool table ------------------------------------------
    //
    // Scholomance (map 289), spell 17742 "Cloud of Disease". The Diseased Ghoul
    // (10495, 29 spawns, all on this map) runs SmartAI event 6 (on death) ->
    // action 11 cast 17742 on itself, so the pool lands exactly where the party
    // just killed it. From Spell.dbc: Effect[0] = 27 SPELL_EFFECT_PERSISTENT_AREA_AURA
    // applying aura 3 SPELL_AURA_PERIODIC_DAMAGE, 350 nature damage per 1000ms
    // tick, EffectRadiusIndex 8 = 5.0yd, DurationIndex 18 = 20s. That is up to
    // ~7000 damage to anyone who simply stands where they were fighting, at a
    // level bracket where a party member has a few thousand HP — it is the same
    // shape as the Destroyed Sentinel, one tier smaller.
    //
    // There is no creature to key on: a persistent area aura is a DynamicObject,
    // not a unit, which is why this table exists at all. It is also why there are
    // no DcNavPenaltyRegistry boxes to pair with it — the pool's position is
    // unknown until a ghoul dies, so nothing can be hand-authored at author time
    // and the live predicates carry the whole job.
    //
    // radius 8 = the 5yd pool plus 3yd of drift margin: enough that a camp anchor
    // or standoff is not placed on the rim, small enough that it does not sterilise
    // a Scholomance corridor the party still has to walk. vacateRadius is the RAW
    // 5yd pool, NOT the padded 8 — same rule as the Destroyed Sentinel row above:
    // the retreat aims pulse + VacateRetreatSlack = 11yd, which must fall OUTSIDE
    // this row's own PointIsHot cylinder or the retreat can never find a spot it
    // accepts. The 3yd gap between 8 and 11 is the budget for NavmeshSnap pulling
    // the candidate back toward the pool; do not raise `radius` toward 11 without
    // raising VacateRetreatSlack with it.
    //
    // NOT registered elsewhere despite sharing the spell: 17742 is also cast on
    // death by the Silicate Feeder (15333), and Cloud of Disease exists under two
    // other ids — 29047 (Mummified Headhunter, Zul'Gurub) and 41193 (Mutant War
    // Hound, cast in combat rather than on death). None of those sit on a map
    // dungeon-clear runs today. Adding one is a single row here.
    //
    // Maraudon (map 349), spell 21070 "Noxious Cloud" — the same shape one tier
    // down, and the other half of the slime problem. From Spell.dbc: Effect[0] =
    // 27 SPELL_EFFECT_PERSISTENT_AREA_AURA applying aura 3
    // SPELL_AURA_PERIODIC_DAMAGE, BasePoints 150 + DieSides 1 = 151 nature per
    // 1000ms tick, EffectRadiusIndex 8 = 5.0yd, DurationIndex 18 = 20s — ~3000
    // damage to a bot that just stands where it was fighting, against the ~2-3k HP
    // a party member has in the 45-49 bracket.
    //
    // BOTH slimes drop it, and both drop it twice over:
    //   * Noxious Slime (12221, 27 spawns) — SmartAI event 9 SMART_EVENT_RANGE,
    //     rangeMax 5, repeat 10-15s, and event 6 on death.
    //   * Creeping Sludge (12222, 24 spawns) — the same pair, repeat 22-26s.
    // target_type 1 is SMART_TARGET_SELF, so the pool always lands under the mob,
    // which is to say under whoever is meleeing it — and then again on the corpse,
    // under whoever stops to loot. That on-death copy is the Destroyed Sentinel
    // failure exactly: the party has won the fight, combat has dropped, and nothing
    // else in the AI has any reason to move them off 151/s.
    //
    // One row covers both creatures because the pool is keyed on the spell, not on
    // whatever died to make it.
    //
    // Sizing follows the Cloud of Disease row above unchanged — same 5yd aura, same
    // 8/5 split, same 3yd gap to the 11yd retreat aim point. Do not raise `radius`
    // toward 11 without raising VacateRetreatSlack with it.
    //
    // Unlike Scholomance, the vacate here runs against a pool centred on a LIVE mob
    // the party is still fighting. It settles rather than ping-pongs because the
    // DynamicObject stays at the cast point while the mob keeps chasing: melee
    // clear to 11yd, the slime walks off its own pool after them, and they
    // re-engage on clean ground. The Creeping Sludge's 2.0 yd/s makes that
    // separation slow but certain.
    // Azjol-Nerub (map 601), spells 53400 (normal) and 59419 (heroic) "Acid
    // Cloud" — Hadronox's ground pool, and the longest-lived one on the clear.
    // From Spell.dbc: Effect[0] = 27 SPELL_EFFECT_PERSISTENT_AREA_AURA applying
    // aura 3 SPELL_AURA_PERIODIC_DAMAGE, EffectRadiusIndex 8 = 5.0yd,
    // EffectAmplitude 1000ms, DurationIndex 23 = 90 SECONDS, BasePoints+1 = 707
    // nature per tick normal and 1414 heroic. Ninety seconds is 4-6x the two
    // rows above, so unlike a Cloud of Disease this one does not simply expire
    // while the party finishes the pull — it outlives the fight it was cast in.
    //
    // BOTH IDS ARE REGISTERED, and that is not belt-and-braces. boss_hadronox
    // only ever casts 53400; spelldifficulty_dbc row 53400 maps it to 59419 on
    // heroic, and the DynamicObject then reports 59419 from GetSpellId(). A row
    // for 53400 alone leaves the retreat inert on exactly the difficulty where
    // the pool does double damage.
    //
    // She casts it every 25s at a RANDOM party member inside 100yd
    // (EVENT_HADRONOX_ACID -> SelectTarget(Random, 0, 100, false)), so the pool
    // lands on top of whoever it picked rather than under the boss — the
    // Destroyed Sentinel shape, not the Maraudon-slime shape, and the reason
    // vacateRadius carries this row rather than the placement keep-out.
    //
    // Sizing is the two rows above, unchanged: same 5yd aura, same 8/5 split,
    // same 3yd gap to the 11yd retreat aim point (vacate 5 + VacateRetreatSlack).
    // Do not raise `radius` toward 11 without raising VacateRetreatSlack with it.
    //
    // zBand 6 matters more here than anywhere else on the clear. Azjol-Nerub is
    // a vertical shaft: the platform the fight ends on is z ~733, Hadronox's
    // spawn ledge is z ~675, the pit floor is z ~648 and the lower kingdom is
    // z ~289. A pool dropped on one of those decks must not fence off the deck
    // below it, and 6yd is comfortably inside the smallest of those gaps (27yd).
    // Drak'Tharon Keep (map 600) — THREE pools, and the first row on the clear
    // whose aura is bigger than a party's idea of melee range.
    //
    // 47346 ARCANE FIELD, Novos the Summoner, PHASE 1 ONLY. From Spell.dbc:
    // Effect[0] = 27 SPELL_EFFECT_PERSISTENT_AREA_AURA applying aura 3
    // SPELL_AURA_PERIODIC_DAMAGE, EffectRadiusIndex 42 = 11.0yd,
    // EffectAmplitude 1000ms, BasePoints+DieSides = 1665 arcane PER SECOND, at
    // TARGET_DEST_CASTER — so the pool sits on the boss's own feet, which is
    // where the tank has just landed the pull. Effect[1] is a −50% movement-speed
    // leg on the same footprint, which is why walking out of it is slower than
    // walking into it. Novos is rooted (UNIT_FLAG_DISABLE_MOVE from Reset()) and
    // stays rooted, so unlike a boss-carried aura this pool never chases.
    //
    // IT DOES NOT PERSIST INTO PHASE 2, and that matters for the vacate band.
    // 47346's AttributesEx is 0x4 = SPELL_ATTR1_IS_CHANNELED, so the cast
    // occupies CURRENT_CHANNELED_SPELL; the phase-2 flip's
    // `me->InterruptNonMeleeSpells(false)` cancels it and Spell::cancel() ends
    // with `m_caster->RemoveDynObject(m_spellInfo->Id)`. Creature::_EnterEvadeMode
    // and Unit::setDeathState interrupt the same way, so a wipe does not leave
    // the room poisoned either. (Its DurationIndex is 225 = 604800000ms — seven
    // days. That is irrelevant for a channeled spell and is NOT evidence of
    // persistence; it is recorded here so the wrong conclusion is not re-derived
    // from it.) The pool is therefore gone before melee ever need to close, and
    // a vacateRadius this wide cannot strand them at the boss.
    //
    // SIZING IS THE ONE PLACE THIS ROW DEPARTS FROM THE FIVE-YARD POOLS ABOVE.
    // vacateRadius is the RAW 11yd aura, same rule as every other row. The
    // retreat then aims at 11 + retreatSlack 6 = 17yd, so `radius` — the
    // placement keep-out — has to stay below that or the retreat could never find
    // a spot PointIsHot accepts: 14 leaves the same 3yd budget for NavmeshSnap
    // pulling the candidate back toward the pool that the 8/5/11 rows leave. Do
    // not raise `radius` toward 17 without raising retreatSlack with it.
    //
    // The Novos camp (-379.0, -757.0) is 19.3yd from him, i.e. 5.3yd outside this
    // keep-out and 2.3yd past the retreat's aim point — the two agree by
    // construction. See DrakTharonKeepEvents.cpp for why that camp is where it
    // is, and ObjectiveHookRegistry hook 14, which pushes the LEADER out of the
    // same 14yd cylinder on the tick the event driver owns.
    //
    // 49034 BLIZZARD, Novos, PHASE 2, and 49548 POISON CLOUD, The Prophet
    // Tharon'ja's flesh phase. Both are ordinary timed pools and take the
    // standard 10 / 8 shape: effect 27, EffectRadiusIndex 14 = 8.0yd, dropped at
    // a random party member (Blizzard 1665 per 2s for 6s; Poison Cloud 602/s for
    // 10s, every 10s at a player within 35yd). Neither is channeled
    // (AttributesEx 0x88 on both, no channel bit), so they behave like every
    // other DynamicObject pool. radius 10 leaves a 4yd gap to the 14yd retreat
    // aim point.
    //
    // ONE ROW PER SPELL, not two: no spell on map 600 has a SpellDifficulty.dbc
    // row (checked for all three, plus the Crystal Handlers' Flash of Darkness),
    // so the heroic variants that exist are selected by SmartAI event phase and
    // the DynamicObject reports the same id on both difficulties. Contrast
    // Hadronox's Acid Cloud above, which genuinely needs its 59419 twin.
    //
    // zBand 6 for all three: the Novos chamber and Tharon'ja's platform are each
    // a single floor, and the nearest deck to either is more than 6yd away.
    //
    // DELIBERATELY ABSENT — Trollgore's Corpse Explode (49555 -> 49618). It is
    // 3770 damage in a 5yd radius every 15-19s and it is not representable here,
    // because the EMITTER IS A CORPSE: 49555 applies a 3s periodic dummy to a
    // DEAD Drakkari Invader within 10yd of him and tick 2 detonates it. No
    // DynamicObject, no GameObject, no live creature — nothing any of the three
    // tables can key on. It is a healing and spread fact, not a registry row.
    // The Violet Hold (map 608) — 58693 BLIZZARD, Cyanigosa, wave 18. From
    // Spell.dbc: Effect[0] = 27 SPELL_EFFECT_PERSISTENT_AREA_AURA applying aura 3
    // SPELL_AURA_PERIODIC_DAMAGE, EffectRadiusIndex 13 = 10.0yd,
    // EffectAmplitude 2000ms, BasePoints+DieSides = 1500 frost per two seconds,
    // DurationIndex 31 = 8000ms. Effect[1] is a -40% movement-speed leg on the
    // same footprint, which is why walking out of it is slower than walking into
    // it. AttributesEx 0x88 carries no channel bit, so it behaves like every other
    // DynamicObject pool and is NOT interrupted by anything the boss does.
    //
    // She casts it every 5-10s (then repeating) at a RANDOM party member within
    // 45yd — DoCastRandomTarget(SPELL_BLIZZARD, 0, 45.0f) — so the pool lands on
    // top of whoever it picked rather than under her, the Acid Cloud shape and not
    // the Arcane Field shape.
    //
    // SIZING. vacateRadius is the RAW 10yd aura, the same rule every row here
    // follows. The retreat then aims at 10 + retreatSlack 6 = 16yd, so `radius` —
    // the placement keep-out — must stay below that or the retreat could never
    // find a spot PointIsHot accepts: 12 leaves the same 4yd budget for NavmeshSnap
    // pulling a candidate back toward the pool that the 10/8 rows leave. Do not
    // raise `radius` toward 16 without raising retreatSlack with it.
    //
    // ONE ROW, not two: 58693 has no SpellDifficulty.dbc row (checked, as for
    // Arcane Vacuum 58694 and Mana Destruction 59374), so the DynamicObject
    // reports the same id on normal and heroic.
    //
    // zBand 6: the whole fight happens on the arena floor at z ~38.4
    // (MiddleRoomLocation), and the nearest other deck — the door landing at
    // z 44.1 — is 5.7yd up and 37yd away in plan, so it is never inside the
    // cylinder anyway.
    //
    // DELIBERATELY ABSENT — Lavanthor's Cauterizing Flames (59466), which the
    // Violet Hold plan flagged for verification. It does NOT qualify: Spell.dbc
    // gives it Effect[0] = 2 SPELL_EFFECT_SCHOOL_DAMAGE and Effect[1] = 6 apply
    // aura 87 at implicit target 22 (TARGET_UNIT_SRC_AREA_ENEMY), i.e. a one-shot
    // AoE nuke plus a damage-taken debuff. There is no SPELL_EFFECT_PERSISTENT_
    // AREA_AURA leg, so no DynamicObject is ever spawned and there is nothing for
    // this table to key on. It is a healing fact, not a registry row.
    // Gundrak (map 604) — 55627 MOJO PUDDLE, the Living Mojo's pool, and the ONLY
    // PERSISTENT_AREA_AURA anything on this map casts. From Spell.dbc: Effect[0] =
    // 27 SPELL_EFFECT_PERSISTENT_AREA_AURA applying aura 3 SPELL_AURA_PERIODIC_
    // DAMAGE, EffectRadiusIndex 15 = 3.0yd, EffectAmplitude 1000ms, DurationIndex 1
    // = 10000ms. A small, short, ordinary DynamicObject pool, so it takes a shape
    // scaled down from the map-600 rows rather than their 10/8: radius 6 keep-out
    // over a 3yd aura, with the retreat aiming at vacate 3 + slack 6 = 9yd, a
    // comfortable 3yd outside the keep-out cylinder PointIsHot rejects into.
    //
    // WHO ACTUALLY DROPS IT is worth being exact about, because it is NOT the ring
    // the Colossus event pulls. npc_living_mojoAI::UpdateAI opens with
    // `if (me->ToTempSummon() || !UpdateVictim()) return;`, so the five SUMMONED
    // ring mojos never reach their EVENT_MOJO_MOJO_PUDDLE at all — they are inert
    // until informed and despawn 1.2s later. The four PRE-PLACED trash mojos of the
    // west corridor (guids 127076-127079) are the ones that cast it, every 13s, in
    // an ordinary trash fight on the way to the Colossus. See GundrakEvents.cpp.
    //
    // ONE ROW, not two: SpellDifficulty.dbc has NO entry for ANY Gundrak spell
    // (checked against all 25 of them), so the heroic templates cast the same ids
    // and the DynamicObject reports 55627 on both difficulties.
    //
    // zBand 6: the corridor is one floor at z ~143 and the nearest other deck is
    // the moat 33yd below.
    //
    // DELIBERATELY ABSENT — 54888 Elemental Spawn Effect, which has a
    // PERSISTENT_AREA_AURA leg and is the only other candidate on the map. Its
    // radius is EffectRadiusIndex 16 = 1.0yd carrying a DUMMY aura for 1000ms: a
    // spawn visual, not damage. Also absent: 55081 Poison Nova, 55101 Quake and
    // 55142 Ground Tremor are instant 60yd / 15yd novas with nothing to stand
    // outside of (Moorabi's two are the whole room — a healing problem, not a
    // positioning one), and 55250 Whirling Slash, 55292 Stomp, 54956 Impaling
    // Charge and 55218 Stampede are self-auras, charges and summons. There is no
    // GAMEOBJECT_TYPE_TRAP on map 604 and no creature carries a permanent pulsing
    // aura, so map 604 needs neither a DcTrapHazard nor a DcHazardEmitter row.
    // Pit of Saron (map 658) — 69024 / 70274 TOXIC WASTE, the two green poison
    // pools of Krick's arena and the only DAMAGING PERSISTENT_AREA_AURA anything
    // on this map casts. Checked against 88 spell ids: every id in the four Pit of
    // Saron script files, every smart_scripts cast by the map's 34 spawned
    // creature entries, every spell_script_names id that could reach the map, and
    // one level of EffectTriggerSpell out of all of them. Exactly three carry an
    // Effect 27 leg, and the third is not damage — see the icicle note below.
    //
    // Both are the SAME spell wearing two damage numbers. From Spell.dbc:
    // Effect[0] = 27 SPELL_EFFECT_PERSISTENT_AREA_AURA applying aura 89
    // SPELL_AURA_PERIODIC_DAMAGE_PERCENT, EffectRadiusIndex 26 = 4.0yd,
    // EffectAmplitude 2000ms, DurationIndex 1 = 10000ms, EffectImplicitTargetA 53
    // TARGET_DEST_TARGET_ENEMY — so the pool lands under the VICTIM, not under the
    // caster, and ticks a percentage of max health five times over ten seconds.
    // Percent-based is why standing in one is not a healing problem the way an
    // ordinary pool is: 69024 is 15%/2s and 70274 is 10%/2s regardless of gear, so
    // a full ten-second bath is fatal at any item level.
    //
    // WHO CASTS WHICH:
    //
    //   69024 — KRICK, the boss. boss_ick's own AI drives it (Krick rides Ick and
    //   has no combat AI of its own): EVENT_SPELL_TOXIC_WASTE is scheduled 3-5s
    //   into the pull, picks SelectTarget(Random, 0, 40.0f, true) and has Krick
    //   cast on that player, then repeats every 7-10s — or retries in 2.5s if
    //   Krick happens to be mid-cast. So one or two pools are alive at any moment
    //   for the whole Ick & Krick fight, each on top of a random party member
    //   inside 40yd, and the tank is as likely to be picked as anyone.
    //
    //   70274 — the PLAGUEBORN HORROR (36879), trash. SmartAI id 2: event 0
    //   SMART_EVENT_UPDATE_IC every 8s, action 11 cast at target_type 5
    //   SMART_TARGET_HOSTILE_RANDOM. Five spawns, all on the arena floor and its
    //   approach: (826.3, 117.0, 509.5) (790.2, 132.3, 509.7) (805.9, 73.9, 510.0)
    //   (860.6, 116.5, 510.0) (777.2, 88.1, 512.5). Its other two spells need no
    //   row: 69581 Pustulant Flesh is a single-target nuke plus DoT with no
    //   PERSISTENT_AREA_AURA leg, and 69582 Blight Bomb is a one-shot 20yd death
    //   explosion at 15% HP — instant, nothing left on the ground to stand in.
    //
    // SIZING follows the Mojo Puddle row exactly, one step up: vacateRadius is the
    // RAW 4.0yd aura, so the retreat aims 4 + retreatSlack 6 = 10yd, and `radius`
    // — the placement keep-out — is 7, holding the same 3yd budget between keep-out
    // and aim point that every other row here leaves for NavmeshSnap pulling a
    // candidate back toward the pool. Deliberately small relative to the damage,
    // for the Shattered Halls reason: the pool lands ON the party by design
    // (TARGET_DEST_TARGET_ENEMY), the party has to keep fighting in that arena,
    // and stepping off the patch is the whole available answer.
    //
    // TWO ROWS, not four: spelldifficulty_dbc has NO entry for 69024 or 70274
    // (nor for any other Pit of Saron spell), so the heroic templates cast the
    // same ids and the DynamicObject reports 69024 / 70274 on both difficulties.
    // The heroic Plagueborn Horror template (37635) carries no smart_scripts and
    // no AIName of its own and does not need them: Creature::UpdateEntry swaps
    // only m_creatureInfo for the difficulty template and keeps `SetEntry(Entry);
    // // normal entry always`, so a heroic Horror is still entry 36879 running
    // 36879's SmartAI and casting 70274.
    //
    // zBand 6: the entire hazard is one floor. All five Horrors and Ick spawn
    // between z 509.5 and z 512.5, so the widest cylinder reaches z 518.5 — below
    // the z520-531 north-bridge band DcNavPenaltyRegistry fences, and nowhere near
    // the z522-565 ramp above the arena.
    //
    // DELIBERATELY ABSENT — Ick's POISON NOVA (68989), which reads like the
    // obvious third row and is not one. Its Effect[0] is 2 SPELL_EFFECT_SCHOOL_
    // DAMAGE and Effect[1] is 6 apply aura 3 at EffectRadiusIndex 18 = 15.0yd:
    // an instant 15yd nova plus a DoT, with no PERSISTENT_AREA_AURA leg, so no
    // DynamicObject is ever created and there is nothing for this table to key on.
    // It is announced (EMOTE_ICK_POISON_NOVA / SAY_POISON_NOVA) and run out of
    // during its cast, or healed through — the same call Gundrak's Quake and
    // Ground Tremor get. Also absent: 69012 / 69263 Explosive Barrage, which is a
    // periodic-trigger aura summoning Exploding Orb CREATURES (36610 via 69015)
    // whose 69019 detonation is an instant 6yd nuke — a moving one-shot summon,
    // not a patch of ground, and not a permanent pulsing aura either, so it fits
    // none of the three tables here.
    //
    // AND ABSENT FOR A DIFFERENT REASON — 69424 ICICLE, the tunnel telegraph, which
    // IS the map's third SPELL_EFFECT_PERSISTENT_AREA_AURA and still does not
    // belong here. Its Effect[1] applies aura 4 SPELL_AURA_DUMMY at
    // EffectRadiusIndex 0 = 0.0yd with no amplitude: a marker DynamicObject with
    // no radius and no periodic tick, i.e. the ground decal that warns where a
    // Collapsing Icicle is about to land, not the landing. A zero-radius row would
    // be inert anyway, but registering it would read as "the icicles are handled".
    // They are NOT — that call is deliberately deferred, and PitOfSaronEvents.cpp
    // owns the reasoning and the measure-first condition attached to it.
    //
    // There is no GAMEOBJECT_TYPE_TRAP on map 658 and no creature on it carries a
    // permanent aura in creature_template_addon, so map 658 needs neither a
    // DcTrapHazard nor a DcHazardEmitter row.
    constexpr std::array<DcGroundHazard, 12> kGroundHazards = {{
        //                   radius  zBand  vacate  hold  slack
        // Cloud of Disease — the pool a dying Diseased Ghoul (10495) leaves.
        { 289, 17742, 8.0f, 6.0f, 5.0f, 2.0f, 6.0f },
        // Noxious Cloud — dropped in combat AND on death by both Maraudon slimes.
        { 349, 21070, 8.0f, 6.0f, 5.0f, 2.0f, 6.0f },
        // Acid Cloud — Hadronox, normal (707/s) and heroic (1414/s), 90s each.
        { 601, 53400, 8.0f, 6.0f, 5.0f, 2.0f, 6.0f },
        { 601, 59419, 8.0f, 6.0f, 5.0f, 2.0f, 6.0f },
        // Arcane Field — Novos' 11yd / 1665-per-second phase-1 keep-out.
        { 600, 47346, 14.0f, 6.0f, 11.0f, 2.0f, 6.0f },
        // Blizzard — Novos, phase 2, on a random party member.
        { 600, 49034, 10.0f, 6.0f, 8.0f, 2.0f, 6.0f },
        // Poison Cloud — Tharon'ja, flesh phase, on a random party member.
        { 600, 49548, 10.0f, 6.0f, 8.0f, 2.0f, 6.0f },
        // Blizzard — Cyanigosa, on a random party member within 45yd. 10yd aura.
        { 608, 58693, 12.0f, 6.0f, 10.0f, 2.0f, 6.0f },
        // Mojo Puddle — the west-corridor Living Mojo trash. 3yd aura, 10s.
        { 604, 55627, 6.0f, 6.0f, 3.0f, 2.0f, 6.0f },
        // Toxic Waste — Krick, on a random party member within 40yd. 4yd, 15%/2s.
        { 658, 69024, 7.0f, 6.0f, 4.0f, 2.0f, 6.0f },
        // Toxic Waste — Plagueborn Horror trash, every 8s. Same pool, 10%/2s.
        { 658, 70274, 7.0f, 6.0f, 4.0f, 2.0f, 6.0f },
        // Well of Corruption — Marwyn, every 13s, on a random party member within
        // 40yd. A 3yd persistent area aura lasting 8 seconds that applies 72383
        // (+30% shadow damage taken) to anyone standing in it, in a fight whose
        // every other ability is shadow. Sized like the Mojo Puddle row above,
        // which is the same 3yd/short-duration shape: vacate 3 is the aura
        // itself, radius 6 the placement keep-out. Kept tight on purpose — the
        // pool lands under a party that is camped on the altar by a 70.5yd leash
        // and cannot simply relocate, so the answer is a step, not a move.
        { 668, 72362, 6.0f, 6.0f, 3.0f, 2.0f, 6.0f },
    }};

    // ---- the trap table --------------------------------------------------
    //
    // The Shattered Halls (map 540), GameObject 181915 "Blaze" — the fire patch
    // the flame-arrow gauntlet rains on the corridor between Nethekurse and
    // O'mrogg. This is THE reason the trap table exists: it is the only hazard
    // shape dungeon-clear meets that is a plain GameObject, and until this row
    // existed "never stand in the fire" was a comment in ShatteredHallsEvents.cpp
    // with nothing behind it.
    //
    // How a Blaze gets there, end to end (boss_porung.cpp + the world DB):
    //   1. A Shattered Hand Archer (17427, two of them at x~514) casts 30952
    //      "Shoot Flame Arrow" — a 2s cast, script effect, whose implicit target
    //      TARGET_UNIT_SRC_AREA_ENTRY is narrowed by `conditions` to creature
    //      entry 17687 "Flame Arrow" (20 invisible trigger spawns wandering the
    //      corridor between x290 and x469).
    //   2. spell_tsh_shoot_flame_arrow::FilterTargets drops every anchor with no
    //      player inside 15yd, every anchor that already has a Blaze inside 6yd,
    //      and the last one used, then RandomResizes to ONE. So a volley lands on
    //      exactly one anchor, and only ever one the party is standing near.
    //   3. The chosen anchor casts 30953 "Explosion" on itself: 657-844 fire in a
    //      10yd radius RIGHT NOW, plus effect 76 SPELL_EFFECT_TRANS_DOOR spawning
    //      GameObject 181915 for its 60s duration.
    //   4. The Blaze is a GAMEOBJECT_TYPE_TRAP: trap.diameter 4 (so a 2yd trigger
    //      circle in GameObject::Update), trap.spellId 30979 "Flames" (875-1126
    //      fire, EffectRadiusIndex 15 = 3.0yd), trap.cooldown 2 (re-arms every
    //      2s). ~1000 damage every two seconds for a minute, to anyone within 3yd.
    //
    // Sizing follows the two pool rows above:
    //   vacateRadius 3.5 = the CAST spell's 3.0yd splash plus half a yard, NOT
    //     the 2yd trigger circle. A bot standing 2.8yd off still eats the splash
    //     when the melee on top of the Blaze sets it off, so the trigger radius
    //     is the wrong number to flee by.
    //   radius 5 (placement keep-out) leaves a 4.5yd gap to the 9.5yd retreat aim
    //     point (vacate 3.5 + slack 6), well clear of the pool rows' 3yd budget —
    //     deliberately generous here because up to ~20 Blazes can be alive at once
    //     (one per volley, volleys every 2-9.75s, each lasting 60s) and a retreat
    //     that cannot find an accepted spot in a 25yd-wide corridor thrashes.
    //   zBand 6 keeps the corridor (z~2) separate from Nethekurse's chamber
    //     (z~-8) ten yards below it, which the route crosses on the way in.
    //
    // The keep-out is deliberately SMALL relative to the damage. The party has to
    // fight its way down this corridor, the fire follows the party by design (an
    // anchor only qualifies with a player within 15yd), and there is no
    // anchor-free standing spot between x~261 and x~497 once the anchors' 12-17yd
    // wander is accounted for. Fencing hard would freeze the run; stepping off the
    // patch is the whole available answer, and it is enough.
    //
    // The real END of the fire is not avoidance at all: FireArrows() returns false
    // once no Shattered Hand Archer is left alive, and killing the far-end Blood
    // Guard (17461 normal, SmartAI on-death SetData 2) or Porung (20923 heroic,
    // boss_porung::JustDied) cancels the scout's whole scheduler — waves and
    // arrows together. See ShatteredHallsEvents.cpp, which sequences exactly that.
    constexpr std::array<DcTrapHazard, 1> kTrapHazards = {{
        //                   radius  zBand  vacate  hold  slack
        // Blaze — the 60s fire patch a flame arrow leaves on the gauntlet floor.
        { 540, 181915, 5.0f, 6.0f, 3.5f, 2.0f, 6.0f },
    }};
}

bool DcHazardRegistry::HasEmitters(uint32 mapId)
{
    for (auto const& e : kEmitters)
        if (e.mapId == mapId)
            return true;
    return false;
}

bool DcHazardRegistry::HasGroundHazards(uint32 mapId)
{
    for (auto const& g : kGroundHazards)
        if (g.mapId == mapId)
            return true;
    return false;
}

bool DcHazardRegistry::HasTrapHazards(uint32 mapId)
{
    for (auto const& t : kTrapHazards)
        if (t.mapId == mapId)
            return true;
    return false;
}

bool DcHazardRegistry::HasAnyHazard(uint32 mapId)
{
    return HasEmitters(mapId) || HasGroundHazards(mapId) || HasTrapHazards(mapId);
}

DcHazardEmitter const* DcHazardRegistry::Find(uint32 mapId, uint32 creatureEntry)
{
    for (auto const& e : kEmitters)
        if (e.mapId == mapId && e.creatureEntry == creatureEntry)
            return &e;
    return nullptr;
}

DcGroundHazard const* DcHazardRegistry::FindGround(uint32 mapId, uint32 spellId)
{
    for (auto const& g : kGroundHazards)
        if (g.mapId == mapId && g.spellId == spellId)
            return &g;
    return nullptr;
}

DcTrapHazard const* DcHazardRegistry::FindTrap(uint32 mapId, uint32 goEntry)
{
    for (auto const& t : kTrapHazards)
        if (t.mapId == mapId && t.goEntry == goEntry)
            return &t;
    return nullptr;
}

std::vector<uint32> DcHazardRegistry::TrapEntries(uint32 mapId)
{
    std::vector<uint32> entries;
    for (auto const& t : kTrapHazards)
        if (t.mapId == mapId)
            entries.push_back(t.goEntry);
    return entries;
}

bool DcHazardRegistry::PointInCylinder(float radius, float zBand,
                                       float ex, float ey, float ez,
                                       float px, float py, float pz)
{
    if (radius <= 0.0f)
        return false;
    if (std::fabs(pz - ez) > zBand)
        return false;

    float const dx = px - ex;
    float const dy = py - ey;
    return dx * dx + dy * dy < radius * radius;
}

bool DcHazardRegistry::SegmentClipsCylinder(float radius, float zBand,
                                            float ex, float ey, float ez,
                                            float ax, float ay, float az,
                                            float bx, float by, float bz)
{
    if (radius <= 0.0f)
        return false;

    // Reject only when both endpoints are out of band ON THE SAME SIDE. Two
    // endpoints out of band on OPPOSITE sides means the leg descends straight
    // THROUGH the band — a ramp from the Arcatraz z48 upper tier down toward
    // Zereketh's z-10 chamber passes the emitter's exact z with |dz| large at
    // both ends, and a naive `both out => clean` test would wave it through.
    float const da = az - ez;
    float const db = bz - ez;
    if (std::fabs(da) > zBand && std::fabs(db) > zBand && (da > 0.0f) == (db > 0.0f))
        return false;

    float const clipSq = DungeonClearMath::DistSqToSegment2D(ex, ey, ax, ay, bx, by);
    return clipSq < radius * radius;
}

bool DcHazardRegistry::PointInside(DcHazardEmitter const& e,
                                   float ex, float ey, float ez,
                                   float px, float py, float pz)
{
    return PointInCylinder(e.radius, e.zBand, ex, ey, ez, px, py, pz);
}

bool DcHazardRegistry::SegmentClips(DcHazardEmitter const& e,
                                    float ex, float ey, float ez,
                                    float ax, float ay, float az,
                                    float bx, float by, float bz)
{
    return SegmentClipsCylinder(e.radius, e.zBand, ex, ey, ez, ax, ay, az, bx, by, bz);
}

bool DcHazardRegistry::PointInside(DcGroundHazard const& g,
                                   float ex, float ey, float ez,
                                   float px, float py, float pz)
{
    return PointInCylinder(g.radius, g.zBand, ex, ey, ez, px, py, pz);
}

bool DcHazardRegistry::SegmentClips(DcGroundHazard const& g,
                                    float ex, float ey, float ez,
                                    float ax, float ay, float az,
                                    float bx, float by, float bz)
{
    return SegmentClipsCylinder(g.radius, g.zBand, ex, ey, ez, ax, ay, az, bx, by, bz);
}

bool DcHazardRegistry::PointInside(DcTrapHazard const& t,
                                   float ex, float ey, float ez,
                                   float px, float py, float pz)
{
    return PointInCylinder(t.radius, t.zBand, ex, ey, ez, px, py, pz);
}

bool DcHazardRegistry::SegmentClips(DcTrapHazard const& t,
                                    float ex, float ey, float ez,
                                    float ax, float ay, float az,
                                    float bx, float by, float bz)
{
    return SegmentClipsCylinder(t.radius, t.zBand, ex, ey, ez, ax, ay, az, bx, by, bz);
}
