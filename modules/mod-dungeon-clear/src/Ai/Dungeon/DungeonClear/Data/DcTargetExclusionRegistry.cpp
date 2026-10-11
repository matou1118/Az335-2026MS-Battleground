/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DcTargetExclusionRegistry.h"

#include "Creature.h"
#include "GameObject.h"
#include "InstanceScript.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Script/Playerbots.h"
#include "Ai/Dungeon/DungeonClear/DcValueKeys.h"
#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"
#include "Ai/Dungeon/DungeonClear/Util/DcChessBoard.h"
#include "Ai/Dungeon/DungeonClear/Util/DcCombatPurge.h"

#include <list>
#include <unordered_set>

namespace
{
    // Blackwing Lair: Razorgore, for as long as an egg is standing.
    //
    // Phase is read from the instance's own DATA_EGG_EVENT, which flips to DONE
    // the instant the thirtieth egg breaks. The obvious alternative — "are there
    // eggs near me" — is wrong in the one direction that matters: a scan is
    // bounded by range, so a raid that has drifted away from the last few eggs
    // reads phase 2, opens up on the boss, and wipes on 20038. The scan survives
    // only as the fallback for a map with no instance script, where it is better
    // than nothing.
    bool RazorgoreEggPhase(Player* bot)
    {
        if (!bot)
            return false;

        if (InstanceScript* inst = bot->GetInstanceScript())
            return inst->GetData(DcBlackwingLair::DATA_EGG_EVENT) != DONE;

        std::list<GameObject*> eggs;
        bot->GetGameObjectListWithEntryInGrid(eggs, DcBlackwingLair::GO_BLACK_DRAGON_EGG,
                                              DcBlackwingLair::ROOM_SCAN);
        for (GameObject* egg : eggs)
            if (egg && egg->isSpawned() && egg->GetGoState() == GO_STATE_READY)
                return true;
        return false;
    }

    // Blackwing Lair: the drake hall bosses, while Broodlord stands.
    //
    // Broodlord's state comes from the instance's own boss slot rather than a
    // scan: Firemaw is the near end of a hall the raid does not reach for another
    // 340yd, so there is nothing local to look at, and BROODLORD_ENCOUNTER_INDEX
    // is authored from instance_blackwing_lair's enum rather than derived from the
    // roster (see the note on that constant). No instance script — never in a real
    // run of this map — reads as "still standing", the safe direction.
    //
    // THE SECOND CLAUSE IS AN OPERATOR ESCAPE, AND IT USED TO BE GEOMETRY. It was
    // `bot->GetPositionZ() >= 445`: below the drake hall's floor you cannot be in a
    // real fight with something standing on it, so the bar lifted once the raid was
    // up there and could not outlive its reason if an operator typed `dc skip` on
    // Broodlord. That reasoning is right about the APPROACH — anchors 6-11 run at
    // z 424.5, 24.6yd directly beneath Firemaw — and wrong about everything after
    // it, because the geometry does not separate the cases the way it looked:
    //
    //   Firemaw    (-7520.2, -1025.8, 449.1)
    //   Broodlord  (-7574.0, -1034.4, 449.3)   <- 54yd away, SAME FLOOR
    //   Chromaggus (-7515.3, -1029.6, 476.7)   <- 27yd straight up from Firemaw
    //
    // The upper suppression room and Broodlord's own standoff are z 449 too, so
    // the raid spends the entire legitimate second half of the gauntlet above the
    // threshold with the bar lifted and Firemaw 54yd away. Measured on
    // tp-20260828-132333-1: two of five raids killed him out of order from there.
    // No z threshold can work here, and no plan-view bound can either at 54yd.
    //
    // So the escape is asked directly instead: has the OPERATOR skipped Broodlord?
    // `dc skip` inserts the boss entry into DcKey::Skipped, which is exactly the
    // question the floor test was approximating. It also degrades safely — a bot
    // with no AI context reads "not skipped", keeping the bar up.
    bool BroodlordSkipped(Player* bot)
    {
        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        if (!botAI)
            return false;
        AiObjectContext* ctx = botAI->GetAiObjectContext();
        if (!ctx)
            return false;
        std::unordered_set<uint32> const& skipped =
            ctx->GetValue<std::unordered_set<uint32>&>(DcKey::Skipped)->Get();
        return skipped.find(DcBlackwingLair::NPC_BROODLORD_LASHLAYER) != skipped.end();
    }

    bool BwlDrakeHallOutOfOrder(Player* bot)
    {
        if (!bot)
            return false;

        if (BroodlordSkipped(bot))
            return false;

        InstanceScript* inst = bot->GetInstanceScript();
        return !inst || inst->GetBossState(DcBlackwingLair::BROODLORD_ENCOUNTER_INDEX) != DONE;
    }

    // Gundrak: a Drakkari Raider the combat purge has just dropped.
    //
    // The purge ends an unendable fight (DcCombatPurge) — but ending it is not
    // enough on its own, because the mob is still standing there, still hostile,
    // and still a legal target. The clear's pickers re-select it the next tick,
    // walk the party at water they cannot cross, and re-open the same fight: a
    // revolving door that purges once a window forever and never progresses.
    //
    // So the purge arms a short bar on the entry and this row carries it into the
    // stock combat engine's own target selection, which is the only place that
    // actually points the party's damage. `alsoTank`, because the tank walking at
    // it is the specific harm — the followers go where the tank goes.
    //
    // Windowed by construction: the bar expires with the purge clock, so a raider
    // that landed on solid ground and IS fightable is barred at most for that
    // window and never permanently. This is the mirror image of the Razorgore row
    // — same mechanism, opposite direction: there the world decides the window,
    // here the failsafe does.
    bool GundrakRaiderJustPurged(Player* bot)
    {
        return DcCombatPurge::IsBarred(bot, 29982);
    }

    // Halls of Reflection: the Lich King, for the whole of the escape.
    //
    // THIS IS THE ONLY ROW IN THE TABLE WHOSE SUBJECT IS A FULLY LEGAL TARGET.
    // Creature 36954 has unit_flags 0, no immunities of any kind, faction 2102
    // and a real health bar — AttackersValue::IsPossibleTarget accepts him
    // without reservation, and stock TankAssistTrigger / NotDpsTargetActiveTrigger
    // acquire him within about a second of the escape gossip. Every other row here
    // bars something the party COULD kill; this one bars something the party WILL
    // attack unless stopped.
    //
    // WHY IT LOSES THE RUN, in the order the costs arrive:
    //
    //   1. HE HEALS. npc_hor_lich_kingAI::UpdateAI sets him back to 75% whenever
    //      he drops below 70, with a HealthModifier of 2000 behind that. No
    //      quantity of damage does anything at all.
    //   2. ATTACKING HIM MEANS STANDING IN HIM. He carries Remorseless Winter for
    //      the whole escape — 7068 +/- 863 frost per second inside 10yd — so a
    //      melee bot that closes on him is taking more damage per second than the
    //      entire wall batch deals.
    //   3. AND IT FREEZES THE PARTY'S MOVEMENT, which is the one that actually
    //      ends runs. He is NullCreatureAI and never calls Attack, so he is in
    //      nobody's getAttackers() and DcCombatFlag::IsEngaged reads false for the
    //      party even though everyone is combat-FLAGGED (he SetInCombatWithZone()s
    //      once a second). That distinction is what lets every MayDrive rung keep
    //      working through an escape nobody can leave combat during. One bot
    //      taking him as a VICTIM destroys it: IsEngaged goes true, AnyPartyEngagement
    //      fans it to the party, and the follow-tank rung stops moving anybody
    //      while a Lich King walks at them at 1.4 yd/s. The party is then caught
    //      by geometry rather than by damage.
    //
    // `alsoTank`, and here that is not the usual "the tank walking at it is the
    // harm" — it is that THERE MUST BE NO HOLDER AT ALL. Razorgore needs an
    // off-tank between mind controls; this creature must be untouched by every
    // member of the party, so the hold-fire rung's tank exemption is keyed off
    // this same flag (see DungeonClearHoldFireTrigger).
    //
    // WINDOWED ON THE ESCAPE, not permanent, for the reason every row here is
    // windowed: outside it the bar is pointless (he is frozen, immune and
    // unreachable before the gossip) and a permanent row would be a claim about
    // the map rather than about the encounter. Note this also leaves the row inert
    // for the entire first two thirds of the dungeon, which is where the map's
    // other work is.
    bool HorEscapeRunning(Player* bot)
    {
        if (!bot || bot->GetMapId() != DcHallsOfReflection::MAP_ID)
            return false;

        InstanceScript* inst = bot->GetInstanceScript();
        return inst &&
               inst->GetBossState(DcHallsOfReflection::DATA_LICH_KING) == IN_PROGRESS;
    }

    // Trial of the Champion: Paletress, while her Memory lives.
    //
    // At 25% she casts Reflective Shield (66515, a 999 999 absorb) and summons a
    // Memory; the Memory's death calls her DoAction(1), which removes the shield.
    // Until then no quantity of damage does anything, and bots on `dps assist`
    // sit on her for the whole window while the Memory — the one thing that ends
    // it — goes unhit.
    //
    // The AURA is the gate, not a scan for the Memory's 25 entries: it IS the
    // unkillable window, to the tick, and it costs one aura lookup per pick. If the
    // shield ever fails to land she is killable and the row rightly stands down.
    // Resolved through the instance's Argent-champion slot, which holds whichever
    // of Eadric or Paletress the click rolled.
    //
    // NOT `alsoTank`: the tank keeps her — she is the one who is hitting people.
    bool PaletressShielded(Player* bot)
    {
        if (!bot || bot->GetMapId() != DcTrialOfTheChampion::MAP_ID)
            return false;

        InstanceScript* inst = bot->GetInstanceScript();
        if (!inst)
            return false;

        Creature* boss = ObjectAccessor::GetCreature(
            *bot, inst->GetGuidData(DcTrialOfTheChampion::DATA_ARGENT_CHAMPION));
        return boss && boss->IsAlive() && boss->GetEntry() == DcTrialOfTheChampion::NPC_PALETRESS &&
               boss->HasAura(DcTrialOfTheChampion::SPELL_REFLECTIVE_SHIELD);
    }

    DcTargetExclusionRow const kRows[] = {
        // Blackwing Lair — Razorgore the Untamed. Killing him before the last egg
        // breaks casts 20038 (Explosion) and instakills the raid, so phase-1
        // damage into him does not merely go to waste: it ends the run, and with
        // 10-40 bots it takes about as long as the egg run does. Tanks are NOT
        // excluded — an off-tank holding him between mind controls is how the
        // fight is played, and the exclusion types below leave that alone.
        { 469, 12435, &RazorgoreEggPhase },

        // Blackwing Lair — the drake hall, while Broodlord is still alive.
        //
        // This is not a damage-efficiency row, it is a DO NOT GO THERE row. The
        // authored approach to the Suppression Rooms walks the whole raid up a
        // hall 24.7yd directly beneath Firemaw; one bot inside his aggro radius
        // puts every player on the map into combat with him through
        // `DoZoneInCombat`, which tests neither line of sight nor reachability.
        // Nothing in this module can stop that flag. What it CAN stop is the raid
        // answering it: with these rows nobody picks him, so nobody chases him
        // through a ceiling the navmesh has no route across, the raid does not
        // split over two floors for the rest of the run, and Firemaw is not killed
        // out of order and then skipped. Measured in tp-20260828-121941-1: four of
        // five runs, all twenty-five bots in combat with him inside one second at
        // 21.8-61.5yd while he sat on his spawn.
        //
        // `alsoTank` on all four: the tank answering a boss two rooms ahead is
        // precisely how the raid ends up there.
        //
        // The window closes when Broodlord dies OR when an operator skips him —
        // so this can never block the kills it is sequencing, by either route
        // into them.
        { 469, DcBlackwingLair::NPC_FIREMAW,    &BwlDrakeHallOutOfOrder, /*alsoTank*/ true },
        { 469, DcBlackwingLair::NPC_EBONROC,    &BwlDrakeHallOutOfOrder, /*alsoTank*/ true },
        { 469, DcBlackwingLair::NPC_FLAMEGOR,   &BwlDrakeHallOutOfOrder, /*alsoTank*/ true },
        { 469, DcBlackwingLair::NPC_CHROMAGGUS, &BwlDrakeHallOutOfOrder, /*alsoTank*/ true },

        // Gundrak — Drakkari Raider, for the window after a combat purge dropped it.
        { 604, 29982, &GundrakRaiderJustPurged, /*alsoTank*/ true },

        // Halls of Reflection — the Lich King, for the whole of the escape. The
        // one row here whose subject is a perfectly legal, perfectly attackable
        // creature; see HorEscapeRunning above for the three ways attacking him
        // loses the run and why `alsoTank` means "nobody holds him" rather than
        // the usual "the tank must not lead the party there".
        { DcHallsOfReflection::MAP_ID, DcHallsOfReflection::NPC_LICH_KING,
          &HorEscapeRunning, /*alsoTank*/ true },

        // Karazhan — the chess pieces, the move triggers and Medivh's fire, always.
        //
        // The pieces are charmable creatures on the chess factions (1689/1690),
        // and Medivh's are hostile to the raid: the clear's pickers and the stock
        // engine would pick them as targets, and a controller — which loses Game
        // In Session the moment it takes its piece — would cast its own spells at
        // them. That is cheating, and damage from outside the game breaks it. The
        // game is played only through the pieces (the chess rung and conductor).
        // The move triggers (22519) and the fire (22521) are never targets either.
        //
        // Not windowed: there is no moment on this map when attacking a chess
        // piece is progress. `alsoTank`, because nobody holds one.
        { DcKarazhan::MAP, DcChess::NPC_PAWN_A,   nullptr, /*alsoTank*/ true },
        { DcKarazhan::MAP, DcChess::NPC_PAWN_H,   nullptr, /*alsoTank*/ true },
        { DcKarazhan::MAP, DcChess::NPC_ROOK_A,   nullptr, /*alsoTank*/ true },
        { DcKarazhan::MAP, DcChess::NPC_ROOK_H,   nullptr, /*alsoTank*/ true },
        { DcKarazhan::MAP, DcChess::NPC_KNIGHT_A, nullptr, /*alsoTank*/ true },
        { DcKarazhan::MAP, DcChess::NPC_KNIGHT_H, nullptr, /*alsoTank*/ true },
        { DcKarazhan::MAP, DcChess::NPC_BISHOP_A, nullptr, /*alsoTank*/ true },
        { DcKarazhan::MAP, DcChess::NPC_BISHOP_H, nullptr, /*alsoTank*/ true },
        { DcKarazhan::MAP, DcChess::NPC_QUEEN_A,  nullptr, /*alsoTank*/ true },
        { DcKarazhan::MAP, DcChess::NPC_QUEEN_H,  nullptr, /*alsoTank*/ true },
        { DcKarazhan::MAP, DcChess::NPC_KING_A,   nullptr, /*alsoTank*/ true },
        { DcKarazhan::MAP, DcChess::NPC_KING_H,   nullptr, /*alsoTank*/ true },
        { DcKarazhan::MAP, DcChess::NPC_MOVE_TRIGGER, nullptr, /*alsoTank*/ true },
        { DcKarazhan::MAP, DcChess::NPC_FIRE,     nullptr, /*alsoTank*/ true },

        // Trial of the Champion — Paletress, for as long as Reflective Shield is
        // up. DPS and attacker pools only; the tank holds her. See
        // PaletressShielded above.
        { DcTrialOfTheChampion::MAP_ID, DcTrialOfTheChampion::NPC_PALETRESS,
          &PaletressShielded, /*alsoTank*/ false },
    };
}

bool DcTargetExclusionRegistry::HasRowsFor(uint32 mapId)
{
    for (DcTargetExclusionRow const& r : kRows)
        if (r.mapId == mapId)
            return true;
    return false;
}

bool DcTargetExclusionRegistry::IsExcluded(Player* bot, uint32 mapId, uint32 entry, bool forTank)
{
    for (DcTargetExclusionRow const& r : kRows)
        if (r.mapId == mapId && r.entry == entry)
        {
            if (forTank && !r.alsoTank)
                return false;
            return !r.inForce || r.inForce(bot);
        }
    return false;
}
