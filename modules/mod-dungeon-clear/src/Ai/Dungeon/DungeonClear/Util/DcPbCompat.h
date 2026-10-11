/*
 * mod-dungeon-clear — DcPbCompat.h
 *
 * The movement-arbitration surface of mod-playerbots, spelled so that this
 * module compiles against both sides of mod-playerbots PR #2747.
 *
 * #2747 replaced the per-move delay window with an explicit hold. The whole
 * version-dependent surface DC touches is three symbols:
 *
 *   before #2747 (V1)                      after #2747 (V2)
 *   ------------------------------------   ------------------------------------
 *   MovementAction::IsWaitingForLastMove   MovementAction::CanOverrideMovement
 *     true  = the arbiter would REFUSE       true  = the arbiter would ALLOW
 *   LastMovement::lastdelayTime            LastMovement::holdStartMs /
 *     float, a window armed by EVERY move    holdDurationMs, armed only by an
 *                                            explicit SetHold()
 *   LastMovement::Set(..., delay, prio)    LastMovement::Set(..., prio)
 *                                            + SetHold(delay, prio)
 *
 * They are not inverses of each other. Under V1 every MoveTo armed a window
 * (up to MaxWaitForMove, 5 s) that refused any later move of EQUAL or lower
 * priority. Under V2 that window is gone — equal priority replaces, newest
 * wins — and only an explicit hold blocks, so on V2 DC is far freer to
 * re-issue movement tick over tick. The predicates below therefore answer the
 * one question every DC caller actually asks — "would the arbiter refuse a
 * move at this priority right now?" — and are named for that question, not
 * for either version's spelling of it.
 *
 * Detection is on LastMovement's public members, which are disjoint between
 * the versions (V1 has lastdelayTime, V2 has holdDurationMs). Nothing here is
 * a preprocessor switch: `if constexpr` over that trait, with the member
 * access made DEPENDENT through a template parameter so the discarded branch
 * is never name-looked-up against headers that do not have it. Callers that
 * need the protected MovementAction members (the two arbitration calls) live
 * on DcMovementAction in DungeonClearActions.h and use the same idiom with a
 * `Self` cast; see there.
 *
 * A CMake-time probe emitting a -D flag was considered and rejected: it would
 * add a cross-repo path assumption to CMakeLists.txt for nothing this header
 * does not already get from the headers it is compiled against.
 */

#ifndef _DUNGEONCLEAR_DCPBCOMPAT_H
#define _DUNGEONCLEAR_DCPBCOMPAT_H

#include <type_traits>
#include <utility>

#include "Ai/Base/Value/LastMovementValue.h"

namespace DcPbCompat
{
    template <class T, class = void>
    struct HasMovementHold : std::false_type {};
    template <class T>
    struct HasMovementHold<T, std::void_t<decltype(std::declval<T&>().holdDurationMs)>>
        : std::true_type {};

    // True when compiling against the post-#2747 movement layer.
    inline constexpr bool kMovementV2 = HasMovementHold<LastMovement>::value;

    // Drop whatever is blocking the NEXT move of equal-or-lower priority: the
    // delay window on V1, the hold on V2. Leaves the recorded destination and
    // priority alone. `LM` is a template parameter only so that the member
    // access is dependent; it is always LastMovement.
    template <class LM = LastMovement>
    inline void ClearMovementHold(LM& lastMove)
    {
        if constexpr (kMovementV2)
        {
            lastMove.holdStartMs = 0;
            lastMove.holdDurationMs = 0;
        }
        else
            lastMove.lastdelayTime = 0.0f;
    }

    // Record a move DC issued through the core MotionMaster directly (the
    // escort-spline glides), so the framework's arbitration sees it. `delayMs`
    // is how long the move is expected to last. On V1 that is the delay window
    // Set() takes; on V2 it becomes a hold at the same priority, which
    // IsHoldActive() gates on `prio <= lastMove.priority` — the same predicate
    // V1's window used, so this is a 1:1 port rather than an approximation.
    template <class LM = LastMovement>
    inline void RecordMovement(LM& lastMove, uint32 mapId, float x, float y, float z, float ori,
                               float delayMs, MovementPriority prio)
    {
        if constexpr (kMovementV2)
        {
            lastMove.Set(mapId, x, y, z, ori, prio);
            lastMove.SetHold(static_cast<uint32>(delayMs), prio);
        }
        else
            lastMove.Set(mapId, x, y, z, ori, delayMs, prio);
    }
}

#endif
