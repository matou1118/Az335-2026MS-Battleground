/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCANCHORDONE_H
#define _PLAYERBOT_DCANCHORDONE_H

#include "InstanceScript.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonBossInfo.h"

// The instance-script half of anchor completion — an anchor's own boss-state
// slot (DungeonBossInfo::doneBossStateIndex) or GetData value
// (doneInstanceDataId / doneInstanceDataValue) — read live off the script. Shared
// by the target picker (NextDungeonBossValue) and the test harness's diagnostic
// snapshot (DcDiagSnapshot), so the clear and its report can never disagree about
// what "done" means. The rule itself is AnchorDoneByInstanceValues, which is pure.
//
// What this does NOT cover, on purpose: the DBC completed-encounter mask and the
// arrival latch. Those are the callers' own first rungs, and both come before this
// one.
inline DcAnchorDoneVia DcAnchorDoneByInstance(DungeonBossInfo const& info, InstanceScript const* inst)
{
    if (!inst || (info.doneBossStateIndex < 0 && info.doneInstanceDataId < 0))
        return DcAnchorDoneVia::None;

    uint32 const bossState = info.doneBossStateIndex >= 0
        ? static_cast<uint32>(inst->GetBossState(static_cast<uint32>(info.doneBossStateIndex)))
        : 0u;
    uint32 const data = info.doneInstanceDataId >= 0
        ? inst->GetData(static_cast<uint32>(info.doneInstanceDataId))
        : 0u;
    return AnchorDoneByInstanceValues(info, bossState, data);
}

inline bool DcAnchorDoneByInstanceScript(DungeonBossInfo const& info, InstanceScript const* inst)
{
    return DcAnchorDoneByInstance(info, inst) != DcAnchorDoneVia::None;
}

#endif  // _PLAYERBOT_DCANCHORDONE_H
