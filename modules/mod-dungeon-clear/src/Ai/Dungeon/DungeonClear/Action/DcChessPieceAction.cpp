/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "Player.h"
#include "PlayerbotAI.h"
#include "Ai/Dungeon/DungeonClear/Action/DungeonClearActions.h"
#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"
#include "Ai/Dungeon/DungeonClear/Trigger/DungeonClearTriggers.h"

// Karazhan's chess rung (plan section 3.4) — the thin engine face of
// Overrides/KarazhanChessDriver.cpp, where the seat logic and the conductor live
// next to the rest of the chess glue.
//
// WHILE LIVE IT CLAIMS EVERY TICK. The game casts Game In Session (pacify +
// silence) on every player, but a controller loses it the moment it takes its
// piece, and nothing then stops its own rotation, a healer's heal, or a pet from
// hitting a piece — which is cheating, and breaks the game (H1, H5, H6). So the
// rung does not hand idle ticks back the way the Oculus rider does; the one
// exception is the run owner once the chest is open, which yields so the loot
// pipeline can store the items. The multiplier clamp (KaraChessClamp) closes the
// rest: stock actions above this rung's relevance (potions, flee, `drop target`).

bool DungeonClearKzChessTrigger::IsActive()
{
    // Map first: registered in both engines on every bot, and everywhere outside
    // Karazhan it must cost one integer compare.
    if (!bot || bot->GetMapId() != DcKarazhan::MAP)
        return false;
    return DcKarazhan::ChessRungLive(bot);
}

bool DungeonClearKzChessAction::Execute(Event /*event*/)
{
    if (!bot || !botAI)
        return false;
    return DcKarazhan::ChessRungTick(bot, botAI);
}
