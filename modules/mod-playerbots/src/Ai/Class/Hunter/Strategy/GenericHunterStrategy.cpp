/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "GenericHunterStrategy.h"

class GenericHunterStrategyActionNodeFactory : public NamedObjectFactory<ActionNode>
{
public:
    GenericHunterStrategyActionNodeFactory()
    {
        creators["rapid fire"] = &rapid_fire;
        creators["mongoose bite"] = &mongoose_bite;
        creators["raptor strike"] = &raptor_strike;
        creators["explosive trap"] = &explosive_trap;
    }

private:
    static ActionNode* rapid_fire([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode("rapid fire",
                              /*P*/ {},
                              /*A*/ { NextAction("readiness") },
                              /*C*/ {});
    }
    static ActionNode* mongoose_bite([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode("mongoose bite",
                              /*P*/ {},
                              /*A*/ { NextAction("raptor strike") },
                              /*C*/ {});
    }
    static ActionNode* raptor_strike([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode("raptor strike",
                              /*P*/ { NextAction("melee") },
                              /*A*/ {},
                              /*C*/ {});
    }
    static ActionNode* explosive_trap([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode("explosive trap",
                              /*P*/ {},
                              /*A*/ { NextAction("immolation trap") },
                              /*C*/ {});
    }
};

GenericHunterStrategy::GenericHunterStrategy(PlayerbotAI* botAI) : CombatStrategy(botAI)
{
    actionNodeFactories.Add(new GenericHunterStrategyActionNodeFactory());
}

void GenericHunterStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    CombatStrategy::InitTriggers(triggers);

    // Mark/Ammo/Mana Triggers
    triggers.push_back(new TriggerNode("no ammo", { NextAction("equip upgrades packet action", 30.0f) }));
    triggers.push_back(new TriggerNode("hunter's mark", { NextAction("hunter's mark", 29.5f) }));
    triggers.push_back(new TriggerNode("rapid fire", { NextAction("rapid fire", 29.0f) }));
    triggers.push_back(new TriggerNode("aspect of the viper", { NextAction("aspect of the viper", 28.0f) }));

    // Pet care. "hunters pet dead", "pet not happy", "revive pet", "feed pet",
    // "call pet" and "aspect of the wild" were all registered in
    // HunterAiObjectContext, but nothing in any hunter strategy ever referenced
    // them - so a hunter bot would fight on with no beast at all, or with a dead
    // one, until it was told otherwise by hand. Priority is deliberately above the
    // damage rotation: a hunter with no pet is doing a fraction of its damage.
    triggers.push_back(new TriggerNode("hunters pet dead", { NextAction("revive pet", 99.0f) }));
    triggers.push_back(new TriggerNode("pet not happy", { NextAction("feed pet", 98.0f) }));
    triggers.push_back(new TriggerNode("feed pet", { NextAction("feed pet", 97.0f) }));
    triggers.push_back(new TriggerNode("call pet", { NextAction("call pet", 96.0f) }));

    // Aggro/Threat/Defensive Triggers
    triggers.push_back(new TriggerNode("has aggro", { NextAction("concussive shot", 20.0f) }));
    triggers.push_back(new TriggerNode("low tank threat", { NextAction("misdirection on main tank", 27.0f) }));
    triggers.push_back(new TriggerNode("low health", { NextAction("deterrence", 35.0f) }));
    triggers.push_back(new TriggerNode("concussive shot on snare target", { NextAction("concussive shot on snare target", 21.0f) }));
    triggers.push_back(new TriggerNode("medium threat", { NextAction("feign death", 35.0f) }));
    triggers.push_back(new TriggerNode("hunters pet medium health", { NextAction("mend pet", 22.0f) }));
    triggers.push_back(new TriggerNode("hunters pet low health", { NextAction("mend pet", 21.0f) }));

    // Dispel Triggers
    triggers.push_back(new TriggerNode("tranquilizing shot enrage", { NextAction("tranquilizing shot", 61.0f) }));
    triggers.push_back(new TriggerNode("tranquilizing shot magic", { NextAction("tranquilizing shot", 61.0f) }));

    // Ammunition. Both are registered in HunterAiObjectContext but nothing fired
    // them, so a hunter that ran dry just stood there doing nothing until the
    // generic restock happened to catch it.
    triggers.push_back(new TriggerNode("has ammo", { NextAction("auto shot", 31.0f) }));
    triggers.push_back(new TriggerNode("low ammo", { NextAction("equip upgrades packet action", 30.5f) }));
    triggers.push_back(new TriggerNode("no ammo", { NextAction("equip upgrades packet action", 31.5f) }));

    // Steady-shot weave. Explosive Trap is registered and the "explosive trap"
    // trigger exists in the factory, but no strategy ever mounted the trap that
    // the weave needs - without it Survival had no steady-shot source at all.
    triggers.push_back(new TriggerNode("trap weave", { NextAction("explosive trap", 32.0f) }));
    triggers.push_back(new TriggerNode("immolation trap no cd", { NextAction("immolation trap", 32.5f) }));
    triggers.push_back(new TriggerNode("freezing trap", { NextAction("freezing trap", 33.0f) }));

    // Pet stings. serpent/scorpid/wyvern were all registered; only serpent was
    // wired up, so a hunter whose serpent buff wore off had nothing to reapply
    // it with. Viper Sting's own condition lives in CastViperStingAction.
    triggers.push_back(new TriggerNode("no stings", { NextAction("wyvern sting", 26.5f) }));
    triggers.push_back(new TriggerNode("serpent sting on attacker", { NextAction("serpent sting on attacker", 26.0f) }));

    // Ranged-based Triggers
    triggers.push_back(new TriggerNode("enemy within melee", { NextAction("explosive trap", 37.0f),
                                                               NextAction("mongoose bite", 22.0f),
                                                               NextAction("wing clip", 21.0f) }));

    triggers.push_back(new TriggerNode("enemy too close for auto shot", { NextAction("disengage", 35.0f),
                                                                          NextAction("flee", 34.0f) }));
}

// ===== AoE Strategy, 2/3+ enemies =====
AoEHunterStrategy::AoEHunterStrategy(PlayerbotAI* botAI) : CombatStrategy(botAI) {}

void AoEHunterStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode("volley channel check", { NextAction("cancel channel", 23.0f) }));
    triggers.push_back(new TriggerNode("medium aoe", { NextAction("volley", 22.0f) }));
    triggers.push_back(new TriggerNode("light aoe", { NextAction("multi-shot", 21.0f) }));
}

void HunterCcStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode("scare beast", { NextAction("scare beast on cc", 23.0f) }));
    triggers.push_back(new TriggerNode("freezing trap", { NextAction("freezing trap on cc", 23.0f) }));
}

void HunterTrapWeaveStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode("immolation trap no cd", { NextAction("reach melee", 23.0f) }));
}
