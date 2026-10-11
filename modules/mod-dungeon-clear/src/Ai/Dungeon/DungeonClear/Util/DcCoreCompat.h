/*
 * mod-dungeon-clear — DcCoreCompat.h
 *
 * The seams where the playerbots FORK core and the upstream-shaped core it is
 * being folded back into have no API in common, so DC has to pick one at
 * compile time.
 *
 * mod-playerbots #2765 / #2792 / #2820 (test-staging, Sep 2026) move playerbots
 * off the fork-only core hooks and onto upstream AzerothCore's, and the fork
 * core deletes the fork-only half as it syncs:
 *
 *   fork core (old)                     upstream-shaped core (new)
 *   ------------------------------      --------------------------------------
 *   PlayerbotScript                     gone
 *     OnPlayerbotUpdate(diff)             -> WorldScript::OnUpdate (no detection
 *                                            needed: WorldScript is on both)
 *     OnPlayerbotPacketSent(p, pkt)       -> ServerScript::OnPacketSent(s, pkt)
 *   WorldSession::IsBot()               WorldSession::IsHeadless()
 *   IWorld::AddQueryHolderCallback      moved to WorldSession
 *
 * Everything that has a spelling valid on both cores uses it and is not
 * mentioned here. What remains is detected by mod-dungeon-clear.cmake, which
 * greps the exact token out of the core header (an enum value and a virtual
 * member cannot be tested by the preprocessor):
 *
 *   DC_CORE_HAS_ON_PACKET_SENT       ServerScript.h declares SERVERHOOK_ON_PACKET_SENT
 *   DC_CORE_IWORLD_HAS_QUERYHOLDER   IWorld.h still declares AddQueryHolderCallback
 *
 * Every #if on these is deletable, along with this header, once no supported
 * core lacks the upstream hooks.
 */

#ifndef _DUNGEONCLEAR_DCCORECOMPAT_H
#define _DUNGEONCLEAR_DCCORECOMPAT_H

#include "WorldSession.h"

namespace DcCoreCompat
{
    namespace Detail
    {
        template <typename S>
        bool IsClientless(S const* session)
        {
            if constexpr (requires { session->IsHeadless(); })
                return session->IsHeadless();
            else
                return session->IsBot();
        }
    }

    // A session with no game client behind it: a playerbot. Headless on the
    // new core; the fork core spells it IsBot(). Where both exist, IsBot() is
    // the one to avoid: #2792 stops passing is_bot, so it reads false for bots.
    inline bool IsClientless(WorldSession const* session)
    {
        return session && Detail::IsClientless(session);
    }
}

#endif
