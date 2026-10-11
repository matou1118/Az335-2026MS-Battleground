/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DcModuleEnable.h"

#include <atomic>

#include "Log.h"
#include "ObjectGuid.h"

#include "Ai/Dungeon/DungeonClear/Settings/DcSettings.h"

namespace
{
    // -1 = not resolved yet, 0 = disabled, 1 = enabled. One word, relaxed: the
    // value is written once (the first world tick, before any bot exists) and
    // only read afterwards, so no ordering beyond atomicity is needed even
    // though map-update worker threads read it.
    std::atomic<int> g_state{-1};

    bool ReadConf()
    {
        return DcSettings::GetBool(ObjectGuid::Empty, "Enable");
    }
}

namespace DcModule
{
    void LatchValue(bool enabled)
    {
        // First writer wins — a second call (a `.reload config`, a stray early
        // reader) must never move the answer under a bot that already built its
        // context list against it.
        int expected = -1;
        g_state.compare_exchange_strong(expected, enabled ? 1 : 0,
                                        std::memory_order_relaxed);
    }

    bool IsEnabled()
    {
        int const state = g_state.load(std::memory_order_relaxed);
        if (state >= 0)
            return state != 0;

        // A read that beat LatchFromConf (should not happen: the registrar
        // latches on the first world tick, before any player or bot can log in).
        // Resolve and latch rather than guessing, so this reader and every later
        // one see the same answer.
        LatchValue(ReadConf());
        return g_state.load(std::memory_order_relaxed) != 0;
    }

    void LatchFromConf()
    {
        LatchValue(ReadConf());

        if (IsEnabled())
            return;

        LOG_INFO("module",
                 "mod-dungeon-clear: DISABLED (DungeonClear.Enable = 0). No "
                 "strategies, actions, triggers or values are registered with "
                 "mod-playerbots, no bot is given the dungeon-clear strategies, "
                 "and every `.dc` command, addon message and hook is inert. Set "
                 "DungeonClear.Enable = 1 and restart the worldserver to enable "
                 "it.");
    }

    void WarnIfConfDiffersFromLatch()
    {
        int const state = g_state.load(std::memory_order_relaxed);
        if (state < 0)
            return;  // nothing latched yet — the first world tick will read it

        bool const conf = ReadConf();
        if (conf == (state != 0))
            return;

        LOG_WARN("module",
                 "mod-dungeon-clear: DungeonClear.Enable is now {} in the config, "
                 "but this worldserver started with the module {}. Registration "
                 "into mod-playerbots' shared contexts happens once at startup "
                 "and cannot be undone or added live, so nothing changed — "
                 "restart the worldserver for the new value to take effect.",
                 conf ? 1 : 0, state != 0 ? "ENABLED" : "DISABLED");
    }

    void ResetLatchForTests()
    {
        g_state.store(-1, std::memory_order_relaxed);
    }
}
