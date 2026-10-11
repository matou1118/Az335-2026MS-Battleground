/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCRUN_H
#define _PLAYERBOT_DCRUN_H

#include "AiObjectContext.h"
#include "PlayerbotAI.h"
#include "Ai/Dungeon/DungeonClear/DcRunState.h"
#include "Ai/Dungeon/DungeonClear/DcValueKeys.h"

// Typed accessor for the leader-owned DcRunState value. Every read/write of the
// run-level facts (enabled, paused, the pause cluster, the selected-boss override,
// the leader-fight latches) goes through DcRun::Of instead of string-fishing the
// value by hand, so a cross-bot read reaches the leader's run state by a single
// well-named call rather than `leaderCtx->GetValue<DcRunState&>("...")->Get()`
// scattered at every site. Header-only inline; the underlying value is registered
// by DungeonClearValueContext like every other DC value.
namespace DcRun
{
    inline DcRunState& Of(AiObjectContext* ctx)
    {
        return ctx->GetValue<DcRunState&>(DcKey::RunState)->Get();
    }

    inline DcRunState& Of(PlayerbotAI* botAI)
    {
        return Of(botAI->GetAiObjectContext());
    }

    // Is this a raid run? Reads the caller's own run state — meaningful on the
    // LEADER's context (followers' copies stay at defaults, like `enabled`).
    // Cross-bot callers resolve the leader first, exactly as they do for the
    // enabled/paused reads.
    inline bool IsRaid(AiObjectContext* ctx)
    {
        return Of(ctx).raidRun;
    }
}

#endif  // _PLAYERBOT_DCRUN_H
