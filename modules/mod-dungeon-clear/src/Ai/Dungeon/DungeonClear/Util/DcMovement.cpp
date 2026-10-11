/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "Ai/Dungeon/DungeonClear/Util/DcMovement.h"
#include "Ai/Dungeon/DungeonClear/Util/DcPbCompat.h"
#include "Ai/Dungeon/DungeonClear/Util/DcRun.h"

#include <algorithm>

#include "Log.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "Ai/Dungeon/DungeonClear/DcValueKeys.h"

namespace DcMovement
{
    // Drop the escort spline's arbitration block. When the spline was issued,
    // SplinePath recorded it with a duration sized to the window travel time
    // (capped at maxWaitForMove, up to ~5s), and the framework refuses an
    // equal-or-lower priority move for that long — as a delay window before
    // mod-playerbots #2747, as a hold after it. After a glide is halted the bot
    // would otherwise idle for the remainder of that duration before the next
    // move is accepted. DcPbCompat spells the clear for both versions.
    static void ZeroLastMovementWait(Player* bot)
    {
        if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
            if (AiObjectContext* ctx = botAI->GetAiObjectContext())
                DcPbCompat::ClearMovementHold(
                    ctx->GetValue<LastMovement&>(DcKey::Stock::LastMovement)->Get());
    }

    static DcGlideRecord* GlideRecordOf(Player* bot)
    {
        if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
            if (AiObjectContext* ctx = botAI->GetAiObjectContext())
                return &ctx->GetValue<DcGlideRecord&>(DcKey::GlideRecord)->Get();
        return nullptr;
    }

    bool OwnsRunningEscort(Player* bot)
    {
        if (!bot)
            return false;
        MotionMaster* mm = bot->GetMotionMaster();
        if (!mm || mm->GetCurrentMovementGeneratorType() != ESCORT_MOTION_TYPE)
            return false;
        DcGlideRecord const* rec = GlideRecordOf(bot);
        if (!rec || !rec->issued)
            return false;
        Movement::MoveSpline const* spline = bot->movespline;
        if (!spline || !spline->Initialized() || spline->Finalized())
            return false;
        // MovebyPath stores our points verbatim, so the final point is ours to the
        // float; the tolerance only absorbs a z nudge. A stock glide that happens
        // to end within half a yard of ours is going where we asked anyway.
        return (spline->FinalDestination() - rec->end).squaredLength() < 0.25f;
    }

    // Kill an in-flight ESCORT glide of DC's OWN so a forced rebuild/reset starts
    // from a standstill. Without this a now-stale EscortMovementGenerator keeps
    // driving the bot down the OLD route while the rebuilt path is ignored. Never
    // touches a stock spline (see DcGlideRecord); no-op otherwise.
    static void StopActiveSplineGlide(Player* bot)
    {
        if (!bot)
            return;
        if (OwnsRunningEscort(bot))
        {
            bot->StopMoving();
            if (DcGlideRecord* rec = GlideRecordOf(bot))
                rec->issued = false;
        }
        ZeroLastMovementWait(bot);
    }

    void ClearMovementWait(Player* bot)
    {
        if (!bot)
            return;
        ZeroLastMovementWait(bot);
    }

    void ReleaseMoveLock(Player* bot)
    {
        if (!bot)
            return;
        if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
            if (AiObjectContext* ctx = botAI->GetAiObjectContext())
            {
                LastMovement& last = ctx->GetValue<LastMovement&>(DcKey::Stock::LastMovement)->Get();
                DcPbCompat::ClearMovementHold(last);
                last.msTime        = 0;
            }
    }

    void ResolveEscortConflict(Player* bot)
    {
        if (!bot)
            return;
        // Only act when one of OUR escort glides is actually in flight, so a
        // point-move at a site with no glide is not perturbed (in particular the
        // LastMovement block is left untouched there) — and so a stock spline is
        // never cancelled. Live (2026-09-04, Bero): eight
        // "closing to aggro edge" re-issues in 1.5s, 0.8yd apart, each one a
        // StopMoving here followed by a fresh spline from a standstill.
        if (OwnsRunningEscort(bot))
            StopActiveSplineGlide(bot);
    }

    void StopBot(Player* bot, Stop strength)
    {
        if (!bot)
            return;
        MotionMaster* mm = bot->GetMotionMaster();

        switch (strength)
        {
            case Stop::Soft:
                if (bot->isMoving())
                    bot->StopMoving();
                return;

            case Stop::Hold:
            {
                // Follower path: tear down a persistent FOLLOW generator. A
                // self-bot cannot self-heal a leftover follow (its own follow
                // no-ops without clearing), so any hold must clear it explicitly.
                if (mm && mm->GetCurrentMovementGeneratorType() == FOLLOW_MOTION_TYPE)
                {
                    if (bot->isMoving())
                        bot->StopMoving();
                    mm->Clear();
                }

                bool const escortGlide =
                    mm && mm->GetCurrentMovementGeneratorType() == ESCORT_MOTION_TYPE;
                if (!escortGlide && !bot->isMoving())
                    return;
                StopActiveSplineGlide(bot);
                bot->StopMovingOnCurrentPos();
                return;
            }

            case Stop::HardPin:
                // No early-out: under CC the bot is not "moving" yet a queued
                // MOVEMENT_COMBAT MoveTo lurks under the CC generator and would
                // resume the moment the impairment clears.
                StopActiveSplineGlide(bot);
                bot->StopMovingOnCurrentPos();
                return;
        }
    }

    bool DcMovementAllowed(PlayerbotAI* botAI)
    {
        if (!botAI)
            return false;
        AiObjectContext* ctx = botAI->GetAiObjectContext();
        if (!ctx)
            return false;
        return !DcRun::Of(ctx).paused;
    }

    bool SplinePath(PlayerbotAI* botAI, Movement::PointsArray& pts,
                    MovementPriority recordPrio)
    {
        if (!botAI)
            return false;
        // Re-check the paused flag at the movement-issuance boundary, mirroring
        // DcMovementAction::DcMoveTo. Every caller today sits behind a top-of-Execute
        // pause guard, so this is behaviorally inert now — but it closes the
        // pause-walks-through-door race class at the mechanism (a mid-tick flip of
        // "dungeon clear paused" can no longer launch a fresh escort glide) for every
        // future caller, instead of by hand-pasted convention.
        if (!DcMovementAllowed(botAI))
            return false;
        Player* bot = botAI->GetBot();
        if (!bot || pts.size() < 2)
            return false;
        MotionMaster* mm = bot->GetMotionMaster();
        if (!mm)
            return false;

        if (bot->IsSitState())
            bot->SetStandState(UNIT_STAND_STATE_STAND);
        // Unit::CastStop() interrupts CURRENT_GENERIC/CHANNELED/AUTOREPEAT and lets
        // Spell::cancel() send the real client packets. PlayerbotAI::InterruptSpell()
        // used to be called here too; it is a no-op after CastStop (every slot it
        // scans is already cleared) and its hand-rolled SMSG_SPELL_FAILURE /
        // SMSG_SPELL_FAILED_OTHER would only duplicate what cancel() already sent.
        if (bot->IsNonMeleeSpellCast(true))
            bot->CastStop();

        float windowLen = 0.0f;
        for (size_t i = 1; i < pts.size(); ++i)
            windowLen += (pts[i] - pts[i - 1]).magnitude();

        mm->MoveSplinePath(&pts, FORCED_MOVEMENT_NONE);

        // DID IT ACTUALLY LAUNCH? MotionMaster::MoveSplinePath returns void and has
        // three silent no-ops: it early-returns on UNIT_FLAG_DISABLE_MOVE without
        // creating a generator at all; Mutate defers Initialize when a
        // higher-priority slot (CONTROLLED — root, stun, vehicle) is on top, so the
        // escort never starts; and an escort that does initialize can finalize on
        // the spot if the movement layer rejects the points. In all three this
        // function reported success, so Advance recorded "moving", skipped its
        // MoveTo fallback, and rebuilt the SAME window next tick — a byte-identical
        // re-issue loop with the bot standing still.
        //
        // Live signature (2026-07-26, two tanks independently): "spline issued:
        // 4 pts, 16.0yd, speed=7.0, delay=2286ms" five to six times in a row, ~3s
        // apart, with "advance tick: posDelta=0.00yd moving=0 gen=IDLE(0)" between
        // every pair. Identical window length means an identical seed point, which
        // means the bot never moved; IDLE means no escort was ever in flight. The
        // client animates each spline packet it is sent and is then snapped back by
        // the next position update, which is the short back-and-forth twitch the
        // tank does on approach.
        //
        // Mutate() calls Initialize() synchronously whenever the new slot is the
        // top one, so the state below is already settled by the time we read it.
        MovementGeneratorType const gen = mm->GetCurrentMovementGeneratorType();
        bool const inFlight = gen == ESCORT_MOTION_TYPE &&
                              bot->movespline && !bot->movespline->Finalized();
        if (!inFlight)
        {
            LOG_DEBUG("playerbots.dungeonclear",
                      "[DC:{}] spline REFUSED: {} pts, {:.1f}yd -> gen={} finalized={} "
                      "dur={} disableMove={} root={} stun={} bot=({:.1f},{:.1f},{:.1f}) "
                      "end=({:.1f},{:.1f},{:.1f})",
                      bot->GetName(), pts.size(), windowLen, uint32(gen),
                      bot->movespline ? bot->movespline->Finalized() : true,
                      bot->movespline ? bot->movespline->Duration() : -1,
                      bot->HasUnitFlag(UNIT_FLAG_DISABLE_MOVE),
                      bot->HasUnitState(UNIT_STATE_ROOT),
                      bot->HasUnitState(UNIT_STATE_STUNNED),
                      bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(),
                      pts.back().x, pts.back().y, pts.back().z);
            // Report the truth. The caller's contract is "did I issue movement",
            // and a refused spline is not movement — Advance must fall through to
            // its per-point MoveTo fallback (which walks, slowly) instead of
            // standing still re-issuing a spline the movement layer keeps dropping.
            return false;
        }

        // Remember what we just issued so the stop helpers can recognise it later.
        if (DcGlideRecord* rec = GlideRecordOf(bot))
        {
            rec->issued = true;
            rec->end = pts.back();
        }

        // Record movement so AttackAction::Attack still clears the spline when a
        // patrol aggros mid-glide (its interrupt gate is priority <
        // MOVEMENT_COMBAT). The re-issue guards key off splineRunning, not this
        // duration, so its only remaining job is priority arbitration; sizing it to
        // the window travel time keeps it a faithful "this move lasts ~this long"
        // for the framework's other movement consumers without gating our re-issue.
        // It is recorded as a delay window before mod-playerbots #2747 and as a
        // hold after it (DcPbCompat::RecordMovement) — the same gate either way.
        float const runSpeed = std::max(0.1f, bot->GetSpeed(MOVE_RUN));
        float delay = 1000.0f * (windowLen / runSpeed);
        delay = std::min(delay, static_cast<float>(sPlayerbotAIConfig.maxWaitForMove));
        delay = std::max(delay, static_cast<float>(sPlayerbotAIConfig.reactDelay));

        G3D::Vector3 const& dest = pts.back();
        DcPbCompat::RecordMovement(
            botAI->GetAiObjectContext()->GetValue<LastMovement&>(DcKey::Stock::LastMovement)->Get(),
            bot->GetMapId(), dest.x, dest.y, dest.z, bot->GetOrientation(), delay, recordPrio);

        // The cadence of these lines IS the step-pause signature: consecutive
        // issues spaced ~= their own `delay` mean seamless chaining; a gap much
        // larger than `delay` between a short window's issue and the next is the
        // dead-pause to investigate.
        //
        // `net` is the straight-line displacement the window actually buys —
        // |last - first| — against `windowLen`, the distance walked to get it. A
        // window whose net is far below its length DOUBLES BACK, and one whose
        // net is ~0 is a closed loop: the bot runs out and returns to where it
        // started. That is the open question behind the live "spline issued: 4
        // pts, 15.9yd" repeated five times with posDelta=0.00yd between each —
        // whether the bot never moved, or moved out and back over the same
        // ground. The two look identical from posDelta alone and need opposite
        // fixes, and the refusal check above has already ruled out the third
        // possibility (a spline that never launched).
        float const net = (pts.back() - pts.front()).magnitude();
        LOG_DEBUG("playerbots.dungeonclear",
                  "[DC:{}] spline issued: {} pts, {:.1f}yd, net={:.1f}yd, speed={:.1f}, "
                  "delay={:.0f}ms, from=({:.1f},{:.1f},{:.1f}) to=({:.1f},{:.1f},{:.1f})",
                  bot->GetName(), pts.size(), windowLen, net, runSpeed, delay,
                  pts.front().x, pts.front().y, pts.front().z,
                  pts.back().x, pts.back().y, pts.back().z);
        return true;
    }
}
