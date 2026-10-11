/*
 * mod-dungeon-clear — BetterLootRollAction.h
 *
 * "Better Loot Rolling", improvement #1: a bot in "bot self" mode (master ==
 * bot — the human's own character running on autopilot) must NOT cast an
 * automatic Need/Greed vote on group loot. The bot and the human share one
 * character GUID, so the bot's vote is counted FOR the player and pre-empts
 * their roll dialog — a double roll. Suppressing the bot vote lets only the
 * player roll.
 *
 * Improvement #2: roll on gear the bot will grow into. Stock rolling asks
 * ItemUsageValue, which rejects any weapon/armor whose RequiredLevel is above
 * the bot's current level (CanUseItem fails on level alone), so the bot greeds or passes
 * on its own future upgrades. Here, when the level requirement is the ONLY
 * thing blocking the item, the vote is computed as if the bot already were
 * that level: Need when the bot will have the proficiency at that level
 * (plate/mail unlocks at 40 included) and the item's stats score for its
 * spec, Greed otherwise. The server's LootNeedRollLevel/LootGreedRollLevel
 * and unique-equipped post-checks still apply, exactly as in stock.
 *
 * Improvement #3 (not in this class): bots roll immediately. Stock reaches
 * "loot roll" only off the "very often" RandomTrigger (a 1-in-3 chance checked
 * at most once per AiPlayerbot.RepeatDelay), so a pending roll sits unanswered
 * for many seconds. DungeonClearLootRollPendingTrigger fires this same action
 * every non-combat tick while a vote is pending — see DungeonClearTriggers.h
 * and the node in DungeonClearStrategy.cpp.
 *
 * Housed in this module (not in mod-playerbots) so the stock module stays
 * unedited and conflict-free on upstream pulls. The wiring is the same override
 * seam DungeonClear already uses for "auto release" (see StayDeadAction.h):
 * DungeonClearActionContext registers the "loot roll" creator name, and because
 * the engine's shared creator map keeps the LAST registration for a given name
 * (SharedNamedObjectContextList::Add) and the DungeonClear contexts are appended
 * AFTER playerbots builds its own, this creator wins for every bot of every
 * class.
 *
 * Gated by the config flag DungeonClear.BetterLootRolling (default off), which
 * leaves this behaving exactly like the stock LootRollAction. With the flag on
 * there are two cases and they do not overlap: a self-bot casts no vote at all
 * (improvement #1), and every other bot gets improvement #2 on the over-level
 * items and stock's own answer on everything else.
 *
 * Execute votes on every pending roll it is given, matching stock since
 * mod-playerbots #2496 — which replaced "one item per Execute" with all of
 * them. Matching matters here because, unlike stock, this action runs off a
 * per-tick trigger (improvement #3): one item per tick would hold the action
 * slot for as many ticks as the boss dropped items.
 */

#ifndef _DUNGEONCLEAR_BETTERLOOTROLLACTION_H
#define _DUNGEONCLEAR_BETTERLOOTROLLACTION_H

#include "LootRollAction.h"

class PlayerbotAI;
class Player;
class Roll;

// The ONE predicate for "this bot has a vote on this roll that the core will
// actually record". Both the per-tick trigger and the action loop above call
// it, and that shared call is the point: when the two drifted apart the run
// died outright.
//
// tr-20260831-123946-18 (Gundrak, tank Olinigo) is the case. The trigger fired
// on any unemitted vote whose item template resolved; the action dutifully
// called Group::CountRollVote, which REFUSED the vote at Group.cpp:1516 —
//
//     if (roll->getLoot())
//         if (roll->getLoot()->items.empty())
//             return false;          // returns BEFORE recording the vote
//
// — so the vote stayed NOT_EMITED_YET, the trigger stayed hot, and the action
// returned true every tick. At DcRel::LootRollPending (95) that outranks the
// whole driving ladder, so `dungeon clear advance` (15) was PUSHED on all 3142
// ticks of the freeze and executed on NONE of them. The party stood still for
// ten minutes with every watchdog reading zero, because the watchdogs live
// inside the action that never ran.
//
// The trigger already mirrored the action's OTHER no-vote path (an item
// template that does not resolve) with the note that such a roll "must not fire
// the trigger every tick forever". This is that same rule, one guard short —
// which is why the guard now lives in one place instead of two.
namespace DcLootRoll
{
    // Mirrors Group::CountRollVote's accept conditions, in ITS dependency order:
    //   * the roll is still valid — CountRollVote resolves it through
    //     Group::GetRoll, which requires isValid() (Group.cpp:2680), and an
    //     invalidated roll stays in the group's list looking answerable, and
    //   * the bot is a voter on this roll and has not voted yet, and
    //   * the roll's loot object, IF it has one, still holds items, and
    //   * the item template resolves (the action skips the roll otherwise).
    // A null Loot* on a VALID roll is deliberately votable — CountRollVote only
    // rejects a loot object that exists and is empty. On an invalidated roll a
    // null Loot* means something else entirely, which is why validity is tested
    // first rather than inferred from the pointer.
    bool IsVotablePendingRoll(Roll const* roll, Player* bot);
}

class DungeonClearBetterLootRollAction : public LootRollAction
{
public:
    DungeonClearBetterLootRollAction(PlayerbotAI* botAI)
        : LootRollAction(botAI, "loot roll") {}

    bool isUseful() override;
    bool Execute(Event event) override;

private:
    bool IsFutureWearable(ItemTemplate const* proto) const;
    RollVote CalculateFutureVote(ItemTemplate const* proto, int32 randomProperty);
};

#endif
