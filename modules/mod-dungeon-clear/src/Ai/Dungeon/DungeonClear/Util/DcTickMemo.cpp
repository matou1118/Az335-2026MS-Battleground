/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DcTickMemo.h"

#include <cmath>

#include "DcEngageGeometry.h"
#include "DcPartyState.h"
#include "DungeonClearTuning.h"   // DC_ENGAGE_RANGE, DC_Z_LEVEL_TOLERANCE
#include "Ai/Dungeon/DungeonClear/Data/ScriptedPullRegistry.h"
#include "Player.h"
#include "Timer.h"
#include "Unit.h"
#include "AiObjectContext.h"
#include "Value.h"
#include "Ai/Dungeon/DungeonClear/DcValueKeys.h"

bool DcTickMemo::MemoValid(std::uint32_t stampMs, std::uint32_t now)
{
    if (stampMs == 0)
        return false;
    return getMSTimeDiff(stampMs, now) <= kMemoWindowMs;
}

void DcTickMemo::EnsureFresh(std::uint32_t now)
{
    if (MemoValid(stampMs, now))
        return;
    *this = DcTickMemo{};       // clear all cached fields
    stampMs = now ? now : 1;    // never stamp 0 (== "never filled")
}

namespace
{
    DcTickMemo& Memo(AiObjectContext* ctx)
    {
        DcTickMemo& m =
            ctx->GetValue<DcTickMemo&>(DcKey::TickMemo)->Get();
        m.EnsureFresh(getMSTime());
        return m;
    }
}

bool DcTickMemoAccess::AtBossEngage(Player* bot, AiObjectContext* ctx,
                                    DungeonBossInfo const& next)
{
    if (!bot || !ctx)
        return DcEngageGeometry::IsAtBossEngage(bot, ctx, next, DC_ENGAGE_RANGE);

    DcTickMemo& m = Memo(ctx);
    if (m.atBossEngage < 0)
        m.atBossEngage =
            DcEngageGeometry::IsAtBossEngage(bot, ctx, next, DC_ENGAGE_RANGE) ? 1 : 0;
    return m.atBossEngage == 1;
}

bool DcTickMemoAccess::BetweenPullsReady(Player* bot, AiObjectContext* ctx,
                                         bool requireNoLoot)
{
    if (!bot || !ctx)
        return DcPartyState::IsBetweenPullsReady(bot, ctx, requireNoLoot);

    DcTickMemo& m = Memo(ctx);
    std::int8_t& slot =
        requireNoLoot ? m.betweenPullsReadyStrict : m.betweenPullsReadyLoose;
    if (slot < 0)
        slot = DcPartyState::IsBetweenPullsReady(bot, ctx, requireNoLoot) ? 1 : 0;
    return slot == 1;
}

ScriptedPullStage const* DcTickMemoAccess::ScriptedStage(Player* bot, AiObjectContext* ctx)
{
    if (!bot || !ctx)
        return nullptr;

    DcTickMemo& m = Memo(ctx);
    if (m.scriptedStage == -2)
    {
        ScriptedPullStage const* const due = ScriptedPullRegistry::DueStage(bot, ctx);
        m.scriptedStage = due ? static_cast<std::int32_t>(due->order) : -1;
        return due;
    }
    // Only the ORDER is memoised (the pointer would be just as stable, but storing
    // an int keeps the memo a plain trivially-copyable POD that EnsureFresh can
    // reset by assignment). Re-resolving it is a two-row table walk.
    return ScriptedPullRegistry::Find(bot->GetMapId(), m.scriptedStage);
}

bool DcTickMemoAccess::LevelReachable(Player* bot, AiObjectContext* ctx, Unit* u)
{
    if (!bot || !u)
        return false;

    // The same-level answer is one fabs, and it is the answer on nearly every
    // tick in nearly every dungeon — never worth a memo slot. (This mirrors
    // IsLevelReachable's own fast path deliberately: taking it here is what keeps
    // the fixed slot table from filling with candidates that never cost anything.)
    if (std::fabs(u->GetPositionZ() - bot->GetPositionZ()) <= DC_Z_LEVEL_TOLERANCE)
        return true;

    if (!ctx)
        return DcEngageGeometry::IsLevelReachable(bot, u);

    DcTickMemo& m = Memo(ctx);
    std::uint64_t const key = u->GetGUID().GetRawValue();
    for (std::uint8_t i = 0; i < m.levelReachableCount; ++i)
        if (m.levelReachable[i].guid == key)
            return m.levelReachable[i].reachable != 0;

    bool const reachable = DcEngageGeometry::IsLevelReachable(bot, u);
    if (m.levelReachableCount < DcTickMemo::kLevelReachableSlots)
        m.levelReachable[m.levelReachableCount++] =
            { key, static_cast<std::uint8_t>(reachable ? 1 : 0) };
    return reachable;
}

void DcTickHeartbeat::Stamp(AiObjectContext* ctx)
{
    if (!ctx)
        return;
    std::uint32_t const now = getMSTime();
    // Never store 0 — that is the "never ran" sentinel, and getMSTime() really
    // does return 0 for one millisecond every wrap.
    ctx->GetValue<uint32>(DcKey::LastTickMs)->Set(now ? now : 1u);
}

std::uint32_t DcTickHeartbeat::LastMs(AiObjectContext* ctx)
{
    if (!ctx)
        return 0;
    return ctx->GetValue<uint32>(DcKey::LastTickMs)->Get();
}
