/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "Util/DcBotProvisioning.h"

#include "Item.h"
#include "Player.h"

#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotFactory.h"
#include "Playerbots.h"

namespace DcBotProvisioning
{
    char const* ClassToken(std::uint8_t classId)
    {
        switch (classId)
        {
            case 1:  return "warrior";
            case 2:  return "paladin";
            case 3:  return "hunter";
            case 4:  return "rogue";
            case 5:  return "priest";
            case 6:  return "deathknight";
            case 7:  return "shaman";
            case 8:  return "mage";
            case 9:  return "warlock";
            case 11: return "druid";
        }
        return "unknown";
    }

    // Premade-spec template index for the wanted spec name — exact match
    // first, then substring fallback ("prot" catches a renamed "prot pve").
    // -1 when the class has no matching template.
    int ResolveSpecNo(std::uint8_t classId, char const* exact, char const* fallback,
                      std::string* pickedName)
    {
        for (int pass = 0; pass < 2; ++pass)
            for (int i = 0; i < MAX_SPECNO; ++i)
            {
                std::string const& name = sPlayerbotAIConfig.premadeSpecName[classId][i];
                if (name.empty())
                    break;
                bool const hit = pass == 0 ? name == exact
                                           : name.find(fallback) != std::string::npos;
                if (hit)
                {
                    if (pickedName)
                        *pickedName = name;
                    return i;
                }
            }
        return -1;
    }

    // Destroy every equipped item so the factory re-gears from an empty sheet.
    // PlayerbotFactory::ClearAllItems is private to the factory, and its public
    // ClearEverything() drags in a level/talent/skill reset we do not want here,
    // so do the one thing that matters — the equipped set — directly.
    void StripEquipment(Player* bot)
    {
        for (std::uint8_t slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
            if (bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                bot->DestroyItem(INVENTORY_SLOT_BAG_0, slot, true);
    }

    void Roll(Player* bot, std::uint32_t level, std::uint32_t quality,
              std::uint32_t gearScoreLimit, int specNo)
    {
        // Full roll at the target level first (Randomize includes GiveLevel and
        // re-picks talents), then force the role spec and re-gear for it — the
        // same sequence the `talents spec` chat command uses.
        PlayerbotFactory factory(bot, level, quality, gearScoreLimit);

        // Strip the equipped set first. Randomize() only wipes items when
        // AiPlayerbot.EquipAndSpecPersistence is off (it defaults on), and
        // InitEquipment leaves a slot alone when no candidate passes the filters — so
        // a pool bot geared by an earlier run under a looser ceiling would keep those
        // pieces and the new limit would look ignored. Every test bot starts bare and
        // is geared from scratch, which is the `autogear`-on-a-stripped-bot behaviour
        // a run needs to be reproducible.
        StripEquipment(bot);
        factory.Randomize(false);
        if (specNo >= 0)
        {
            PlayerbotFactory::InitTalentsBySpecNo(bot, specNo, true);
            factory.InitEquipment(false, true);
            factory.InitGlyphs(false);

            // Gear first, enchants/gems second — the order the `autogear` then
            // `maintenance` chat commands run in. Randomize() already ends with an
            // ApplyEnchantAndGemsNew() pass, but the spec re-gear above swaps those
            // enchanted/gemmed items out for freshly rolled bare ones, so without
            // this second pass every spec-forced bot (i.e. every tank and healer in
            // a test run) fights with no enchants and empty sockets. Cheap relative
            // to Randomize, and it only touches what is currently equipped.
            if (bot->GetLevel() >= sPlayerbotAIConfig.minEnchantingBotLevel)
                factory.ApplyEnchantAndGemsNew();
        }
        if (bot->getClass() == CLASS_HUNTER)
            factory.InitPet();

        // AMMO LAST, FOR EVERY CLASS — for the same reason the enchant pass above runs
        // last, and it was the same oversight one line further down.
        //
        // Randomize() ends with its own InitAmmo(), which loads ammo for the ranged
        // weapon IT rolled. The spec re-gear then replaces that weapon, and a gun and a
        // bow do not take the same projectile — so a bot whose random roll gave it a bow
        // and whose prot re-gear gave it a gun ends up holding a rifle loaded with
        // arrows. Re-running InitAmmo() only for hunters left every other class stranded
        // on whatever the pre-re-gear weapon needed.
        //
        // Not a cosmetic mismatch. PLAYER_AMMO_ID is set, so every "do I have ammo" test
        // passes, and the failure only surfaces where it counts: the server rejects the
        // Shoot cast itself, silently, and the bot has no ranged opener at all. A warrior
        // has no class opener either (Heroic Throw is level 71), so the tank has nothing
        // — which for a scripted pull means standing on the stand spot for the whole leg
        // budget and then walking into the room. Live: Erinerice and Moge, both prot
        // warriors, both holding Rifle of the Stoic Guardian with Timeless Arrows loaded,
        // failed the Selin stage that way in tr-20260803-154419-13 and -17 while every
        // druid and paladin tank in the same plan pulled normally on a class opener.
        //
        // InitAmmo() self-gates to hunter/rogue/warrior and re-derives the projectile
        // class from the CURRENTLY equipped weapon, so calling it unconditionally is both
        // safe and the whole fix.
        factory.InitAmmo();

        if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
            botAI->ResetStrategies();
    }
}
