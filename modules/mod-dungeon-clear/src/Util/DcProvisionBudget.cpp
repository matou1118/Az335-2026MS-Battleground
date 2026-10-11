/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "Util/DcProvisionBudget.h"

namespace
{
    bool g_available = true;
}

namespace DcProvisionBudget
{
    void Reset() { g_available = true; }

    bool Take()
    {
        if (!g_available)
            return false;
        g_available = false;
        return true;
    }

    bool Available() { return g_available; }
}
