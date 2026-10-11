/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCBOTPROVISIONING_H
#define _PLAYERBOT_DCBOTPROVISIONING_H

#include <cstdint>
#include <string>

class Player;

// Rolling an addclass-pool character into a party member of a wanted level,
// spec and gear ceiling.
//
// Extracted from DcTestRunJob so the RDF instant fill rolls its bots through
// exactly the same sequence a `.dc test` bot goes through. The sequence is
// order-sensitive in three places that each cost a live run to find (the spec
// re-gear dropping enchants, the re-gear swapping the ranged weapon out from
// under its ammo, the equipped set surviving a tightened gear ceiling), so a
// second copy of it in a second subsystem would drift.
//
// Callers keep their own spec-resolution policy, failure handling and logging:
// a test run treats an unresolvable tank spec as a fatal setup failure, while a
// fill just draws a different class. Only the factory sequence is shared.
namespace DcBotProvisioning
{
    // "warrior" / "paladin" / ... — the class token used in logs and records.
    char const* ClassToken(std::uint8_t classId);

    // Premade-spec template index for the wanted spec name — exact match first,
    // then substring fallback ("prot" catches a renamed "prot pve"). -1 when
    // the class has no matching template.
    int ResolveSpecNo(std::uint8_t classId, char const* exact, char const* fallback,
                      std::string* pickedName);

    // Destroy every equipped item so the factory re-gears from an empty sheet.
    // PlayerbotFactory::ClearAllItems is private to the factory, and its public
    // ClearEverything() drags in a level/talent/skill reset we do not want here.
    void StripEquipment(Player* bot);

    // The one heavyweight factory pass: strip, full Randomize at `level`, force
    // `specNo` (-1 = keep the random roll) and re-gear/re-glyph/re-enchant for
    // it, then pet and ammo, then ResetStrategies.
    //
    // Costs one PlayerbotFactory::Randomize, so the caller must have claimed
    // DcProvisionBudget::Take() for this tick before calling.
    //
    // `quality` and `gearScoreLimit` are the resolved ceiling (0 on either =
    // inherit the AiPlayerbot.AutoGear* server settings, which is what the
    // factory itself does with a 0).
    void Roll(Player* bot, std::uint32_t level, std::uint32_t quality,
              std::uint32_t gearScoreLimit, int specNo);
}

#endif  // _PLAYERBOT_DCBOTPROVISIONING_H
