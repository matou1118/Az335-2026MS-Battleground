/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "TestRun/DcTestPlanManager.h"

#include <algorithm>
#include <ctime>
#include <random>

#include "Chat.h"
#include "DBCStores.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "StringFormat.h"
#include "World.h"

#include "Ai/Dungeon/DungeonClear/Settings/DcSettings.h"

#include "TestRun/DcTestDriver.h"
#include "TestRun/DcTestDungeonRegistry.h"
#include "TestRun/DcTestPlanSummary.h"
#include "TestRun/DcTestRunManager.h"

namespace
{
    uint64 NowUnixMs()
    {
        return static_cast<uint64>(std::time(nullptr)) * 1000;
    }

    std::string MakePlanId()
    {
        static uint32 counter = 0;
        std::time_t const now = std::time(nullptr);
        std::tm tmBuf{};
        localtime_r(&now, &tmBuf);
        char buf[32];
        std::strftime(buf, sizeof(buf), "tp-%Y%m%d-%H%M%S", &tmBuf);
        return std::string(buf) + "-" + std::to_string(++counter);
    }

    // How many consecutive transient Start rejections (with nothing in flight
    // to unblock us) before the plan is declared stalled and aborted.
    constexpr uint32 kMaxTransientStreak = 3;

    // A pool plan outlives any one bad entry: a hard Start refusal skips that
    // launch instead of aborting the plan, and only this many in a row (every
    // entry refusing, most likely) pauses it for a human to look at.
    constexpr uint32 kMaxRefusalStreak = 5;

    // An endless plan's summary lists only its most recent runs.
    constexpr std::size_t kEndlessRunIdCap = 200;

    // Canonicalize each entry's token, apply the heroic gate, and fill the
    // catalogue-derived fields. raidDefaultSize: a pool= plan launches raid
    // entries at the catalogue default size (size 0 would field the classic
    // 5-man comp); a positional plan keeps the size= it was given.
    bool ResolvePool(std::vector<DcTestPlan::PoolEntry>& pool, bool raidDefaultSize,
                     std::string* err)
    {
        for (DcTestPlan::PoolEntry& e : pool)
        {
            DcTestDungeonRegistry::Row const* row = DcTestDungeonRegistry::Find(e.token);
            if (!row)
            {
                *err = "unknown dungeon '" + e.token + "' — see .dc test list";
                return false;
            }
            e.token = row->token;  // canonicalize a mapId argument to the token

            // Same gate as DcTestRunManager::Start — reject up front so the
            // plan doesn't burn its launch budget on runs that can never start.
            if (e.heroic && row->heroicLevel == 0)
            {
                *err = "'" + std::string(row->token) +
                       "' has no heroic mode (classic dungeons have none)";
                return false;
            }

            MapEntry const* mapEntry = sMapStore.LookupEntry(row->mapId);
            bool const raid = mapEntry && mapEntry->IsRaid();
            if (raid && raidDefaultSize)
                e.size = std::min(DcTestDungeonRegistry::kRaidDefaultSize,
                                  DcTestDungeonRegistry::RaidSizeMax(
                                      DcTestDungeonRegistry::MaxPlayers(*row)));
            e.resetSensitive = e.heroic || raid;
        }

        // Two spellings of one dungeon ("rfc" and its mapId) collapse only
        // after canonicalization.
        for (std::size_t i = 0; i < pool.size(); ++i)
            for (std::size_t j = i + 1; j < pool.size(); ++j)
                if (pool[i].Key() == pool[j].Key())
                {
                    *err = "duplicate pool entry '" + pool[i].Key() + "'";
                    return false;
                }
        return true;
    }

    uint32 ClampConcurrent(uint32 requested, DcTestPlan::Spec const& spec)
    {
        // Default + clamp the plan's concurrency to the run manager's global cap so
        // the scheduler isn't asking for launches Start would always reject. With
        // MaxConcurrent at its 0 = unlimited default nothing is clamped: an omitted
        // concurrent= still defaults to a modest 5, but an explicit concurrent=N is
        // honoured for any N — the machine's bot budget is the only ceiling.
        uint32 const maxConcurrent =
            DcSettings::GetUInt(ObjectGuid::Empty, "TestRun.MaxConcurrent");
        uint32 c = requested;
        if (c == 0)
            c = std::min<uint32>(5, maxConcurrent ? maxConcurrent : 5);
        if (maxConcurrent != 0)
            c = std::min(c, maxConcurrent);
        c = std::max<uint32>(c, 1);
        if (!spec.endless)
            c = std::min(c, spec.total);
        return c;
    }

    std::string PoolText(std::vector<DcTestPlan::PoolEntry> const& pool)
    {
        std::string out;
        for (DcTestPlan::PoolEntry const& e : pool)
        {
            if (!out.empty())
                out += ',';
            out += e.Key();
        }
        return out;
    }
}

DcTestPlanManager& DcTestPlanManager::Instance()
{
    static DcTestPlanManager instance;
    return instance;
}

bool DcTestPlanManager::Start(DcTestPlan::Spec spec, Player* gm, std::string* msg)
{
    auto fail = [&](std::string const& why) -> bool
    {
        if (msg)
            *msg = "Test plan not started: " + why;
        return false;
    };

    // One launch path: a single-dungeon plan is a one-entry pool.
    if (!spec.isPool)
    {
        DcTestPlan::PoolEntry only;
        only.token = spec.dungeonToken;
        only.heroic = spec.heroic;
        only.size = spec.size;
        spec.pool = {only};
    }
    std::string err;
    if (!ResolvePool(spec.pool, spec.isPool, &err))
        return fail(err);
    if (!spec.isPool)
        spec.dungeonToken = spec.pool.front().token;

    uint32 const maxPlans = DcSettings::GetUInt(ObjectGuid::Empty, "TestRun.MaxPlans");
    if (maxPlans != 0 && _plans.size() >= maxPlans)
        return fail("max active test plans reached (" + std::to_string(maxPlans) +
                    ") — .dc test plan stop <planId> first");

    // 0 = unlimited (the default): the plan's size is the caller's call. An
    // operator who capped plan size has also ruled out plans with no size.
    uint32 const maxTotal = DcSettings::GetUInt(ObjectGuid::Empty, "TestRun.Plan.MaxTotal");
    if (maxTotal != 0 && spec.endless)
        return fail("total=0 (run until stopped) is not allowed while "
                    "DungeonClear.TestRun.Plan.MaxTotal caps plans at " +
                    std::to_string(maxTotal));
    if (maxTotal != 0 && spec.total > maxTotal)
        return fail("total=" + std::to_string(spec.total) + " exceeds the cap (" +
                    std::to_string(maxTotal) + ", DungeonClear.TestRun.Plan.MaxTotal)");

    spec.concurrent = ClampConcurrent(spec.concurrent, spec);
    spec.planId = MakePlanId();

    Plan plan;
    plan.spec = spec;
    if (gm)
        plan.gmGuid = gm->GetGUID();
    plan.startedAtMs = NowUnixMs();
    plan.lastCheckpointMs = plan.startedAtMs;
    plan.paused = spec.startPaused;
    if (spec.endless)
        plan.acc = DcTestPlanSummary::Accumulator(kEndlessRunIdCap);
    DcTestPlan::SeedPicker(plan.picker, spec.seedBase, std::random_device{}());
    for (DcTestPlan::PoolEntry const& e : spec.pool)
    {
        EntryStats& es = plan.entries[e.Key()];
        es.token = e.token;
        es.heroic = e.heroic;
        es.size = e.size;
        if (spec.endless)
            es.acc = DcTestPlanSummary::Accumulator(kEndlessRunIdCap);
    }
    _plans.push_back(std::move(plan));

    std::string const target = spec.isPool ? "pool=" + PoolText(spec.pool) : spec.dungeonToken;
    std::string const totalText = spec.endless ? std::string("endless") : std::to_string(spec.total);

    LOG_INFO("playerbots.dungeonclear",
             "TESTPLAN START {} dungeon={} total={} concurrent={} level={} heroic={} seedBase={} "
             "gear={} gm={}",
             spec.planId, target, totalText, spec.concurrent, spec.level,
             spec.heroic ? 1 : 0, spec.seedBase, DcTestGearTiers::Describe(spec.gear),
             gm ? gm->GetName() : "<pending test driver>");

    if (msg)
        *msg = Acore::StringFormat("Test plan started: {} {}{} total={} concurrent={} gear={}{}{}{}",
                                   spec.planId, target,
                                   spec.heroic ? std::string(" (heroic)") : std::string(),
                                   totalText, spec.concurrent,
                                   DcTestGearTiers::Describe(spec.gear),
                                   spec.isPool ? std::string(spec.pick == DcTestPlan::PickMode::Bag
                                                                 ? " pick=bag"
                                                                 : " pick=random")
                                               : std::string(),
                                   spec.seedBase ? Acore::StringFormat(" seedBase={}", spec.seedBase)
                                                 : std::string(),
                                   spec.startPaused
                                       ? std::string(" — paused: .dc test plan resume to launch")
                                       : gm ? std::string()
                                            : std::string(" — first run launches once the test "
                                                          "driver finishes logging in"));
    return true;
}

std::vector<DcTestPlanManager::Plan*> DcTestPlanManager::Select(std::string const& selector,
                                                               std::string* msg)
{
    std::vector<Plan*> targets;
    if (_plans.empty())
    {
        if (msg)
            *msg = "no test plan active";
        return targets;
    }

    if (selector.empty())
    {
        if (_plans.size() > 1)
        {
            if (msg)
            {
                *msg = "multiple test plans active — specify a planId or 'all':";
                for (Plan const& plan : _plans)
                    *msg += "\n  " + StatusLine(plan);
            }
            return targets;
        }
        targets.push_back(&_plans.front());
    }
    else if (selector == "all")
    {
        for (Plan& plan : _plans)
            targets.push_back(&plan);
    }
    else
    {
        for (Plan& plan : _plans)
            if (plan.spec.planId == selector)
                targets.push_back(&plan);
        if (targets.empty() && msg)
        {
            *msg = "no plan matches '" + selector + "' — active plans:";
            for (Plan const& plan : _plans)
                *msg += "\n  " + StatusLine(plan);
        }
    }
    return targets;
}

bool DcTestPlanManager::Stop(std::string const& selector, std::string* msg)
{
    std::vector<Plan*> const targets = Select(selector, msg);
    if (targets.empty())
        return false;

    std::string acc;
    for (Plan* plan : targets)
    {
        plan->stopping = true;
        if (plan->result.empty())
            plan->result = "stopped";

        // Abort the live children through the run manager's own selector path;
        // their outcomes flow back via OnRunFinished and the drain finalizes.
        for (ActiveRun const& run : plan->activeRuns)
        {
            std::string ignored;
            DcTestRunManager::Instance().Stop(run.runId, &ignored);
        }

        if (!acc.empty())
            acc += '\n';
        acc += "stopping " + plan->spec.planId + " (" + PlanLabel(*plan) + ", " +
               std::to_string(plan->counters.activeNow) + " run(s) draining)";
    }
    if (msg)
        *msg = acc;
    return true;
}

bool DcTestPlanManager::SetPaused(std::string const& selector, bool paused, std::string* msg)
{
    std::vector<Plan*> const targets = Select(selector, msg);
    if (targets.empty())
        return false;

    std::string acc;
    for (Plan* plan : targets)
    {
        plan->paused = paused;
        plan->pauseReason.clear();
        if (!paused)
        {
            // A fresh start for the refusal breaker and any pending backoff.
            plan->refusalStreak = 0;
            plan->transientStreak = 0;
            plan->backoffMs = 0;
        }
        LOG_INFO("playerbots.dungeonclear", "TESTPLAN {} {}", plan->spec.planId,
                 paused ? "paused" : "resumed");
        if (!acc.empty())
            acc += '\n';
        acc += (paused ? "paused " : "resumed ") + plan->spec.planId + " (" +
               std::to_string(plan->counters.activeNow) + " run(s) in flight)";
    }
    if (msg)
        *msg = acc;
    return true;
}

bool DcTestPlanManager::Edit(DcTestPlan::EditSpec const& edit, std::string* msg)
{
    auto fail = [&](std::string const& why) -> bool
    {
        if (msg)
            *msg = "Test plan not edited: " + why;
        return false;
    };

    Plan* plan = nullptr;
    for (Plan& p : _plans)
        if (p.spec.planId == edit.planId)
            plan = &p;
    if (!plan)
        return fail("no plan '" + edit.planId + "'");
    if (plan->stopping)
        return fail(edit.planId + " is stopping");

    if (edit.hasPool || edit.hasAdd)
    {
        if (!plan->spec.isPool)
            return fail("pool= / add= only apply to a plan started with pool=");
        std::vector<DcTestPlan::PoolEntry> incoming = edit.hasPool ? edit.pool : edit.add;
        std::string err;
        if (!ResolvePool(incoming, true, &err))
            return fail(err);
        std::vector<DcTestPlan::PoolEntry> pool;
        if (edit.hasAdd)
        {
            pool = plan->spec.pool;
            for (DcTestPlan::PoolEntry const& e : incoming)
            {
                for (DcTestPlan::PoolEntry const& have : pool)
                    if (have.Key() == e.Key())
                        return fail("'" + e.Key() + "' is already in the pool");
                pool.push_back(e);
            }
        }
        else
            pool = std::move(incoming);
        plan->spec.pool = std::move(pool);
        for (DcTestPlan::PoolEntry const& e : plan->spec.pool)
        {
            auto [it, inserted] = plan->entries.try_emplace(e.Key());
            if (inserted)
            {
                it->second.token = e.token;
                it->second.heroic = e.heroic;
                it->second.size = e.size;
                if (plan->spec.endless)
                    it->second.acc = DcTestPlanSummary::Accumulator(kEndlessRunIdCap);
            }
        }
        DcTestPlan::ResetBag(plan->picker);
    }
    if (edit.hasConcurrent)
        plan->spec.concurrent = ClampConcurrent(edit.concurrent, plan->spec);

    LOG_INFO("playerbots.dungeonclear", "TESTPLAN {} edited: pool={} concurrent={}",
             plan->spec.planId, PoolText(plan->spec.pool), plan->spec.concurrent);
    if (msg)
        *msg = Acore::StringFormat("edited {}: {} entr{}, concurrent={}", plan->spec.planId,
                                   plan->spec.pool.size(),
                                   plan->spec.pool.size() == 1 ? "y" : "ies",
                                   plan->spec.concurrent);
    return true;
}

void DcTestPlanManager::StopAll(std::string const& reason)
{
    for (Plan& plan : _plans)
    {
        plan.stopping = true;
        if (plan.result.empty())
        {
            plan.result = "stopped";
            plan.abortReason = reason;
        }
    }
}

std::string DcTestPlanManager::PlanLabel(Plan const& plan)
{
    if (!plan.spec.isPool)
        return plan.spec.dungeonToken + (plan.spec.heroic ? " (heroic)" : "");
    return "pool=" + PoolText(plan.spec.pool);
}

std::string DcTestPlanManager::StatusLine(Plan const& plan)
{
    DcTestPlan::Counters const& c = plan.counters;
    std::string const state =
        plan.stopping         ? ", draining"
        : plan.paused         ? (plan.pauseReason.empty() ? ", paused"
                                                          : ", paused: " + plan.pauseReason)
        : plan.holdingForReset ? ", holding for instance reset"
        : plan.backoffMs      ? ", backoff"
                              : "";
    return Acore::StringFormat("{} {} {}/{} done ({} ok, {} fail), {} active (max {}){}",
                               plan.spec.planId, PlanLabel(plan), c.succeeded + c.failed,
                               plan.spec.endless ? std::string("∞")
                                                 : std::to_string(plan.spec.total),
                               c.succeeded, c.failed, c.activeNow, plan.spec.concurrent, state);
}

std::string DcTestPlanManager::StatusText() const
{
    if (_plans.empty())
        return "no test plans active";

    std::string out = std::to_string(_plans.size()) +
                      (_plans.size() == 1 ? " test plan active:" : " test plans active:");
    for (Plan const& plan : _plans)
        out += "\n  " + StatusLine(plan);
    return out;
}

std::vector<DcTestRunLive::PlanSnapshot> DcTestPlanManager::Snapshots() const
{
    std::vector<DcTestRunLive::PlanSnapshot> out;
    out.reserve(_plans.size());
    uint64 const nowMs = NowUnixMs();
    for (Plan const& plan : _plans)
    {
        DcTestRunLive::PlanSnapshot s;
        s.planId = plan.spec.planId;
        s.dungeon = plan.spec.isPool ? std::string("pool") : plan.spec.dungeonToken;
        s.total = plan.spec.total;
        s.launched = plan.counters.launched;
        s.succeeded = plan.counters.succeeded;
        s.failed = plan.counters.failed;
        s.activeNow = plan.counters.activeNow;
        s.concurrent = plan.spec.concurrent;
        s.heroic = plan.spec.heroic;
        s.state = plan.stopping          ? "draining"
                  : plan.paused          ? (plan.pauseReason.empty() ? "paused"
                                                                     : "paused: " + plan.pauseReason)
                  : plan.driverWaitSinceMs ? "waiting for test driver"
                  : plan.holdingForReset ? "holding for instance reset"
                  : plan.backoffMs       ? "backoff"
                                         : "running";
        s.elapsedS = static_cast<uint32>((nowMs - plan.startedAtMs) / 1000);
        s.endless = plan.spec.endless;
        s.paused = plan.paused;
        s.resetHoldUntil = plan.resetGuardUntilS;
        if (plan.spec.isPool)
        {
            for (DcTestPlan::PoolEntry const& e : plan.spec.pool)
            {
                DcTestRunLive::PlanPoolEntry pe;
                pe.token = e.token;
                pe.heroic = e.heroic;
                pe.size = e.size;
                auto const it = plan.entries.find(e.Key());
                if (it != plan.entries.end())
                {
                    pe.launched = it->second.launched;
                    pe.succeeded = it->second.acc.Succeeded();
                    pe.failed = it->second.acc.Failed();
                }
                s.pool.push_back(std::move(pe));
            }
            std::int32_t const next = DcTestPlan::PeekNext(
                plan.picker, plan.spec,
                DcTestPlan::EligibleEntries(plan.spec, plan.resetGuardUntilS != 0));
            if (next >= 0)
                s.nextPick = plan.spec.pool[static_cast<std::size_t>(next)].Key();
        }
        out.push_back(std::move(s));
    }
    return out;
}

void DcTestPlanManager::Tick(uint32 diff)
{
    if (_plans.empty())
        return;

    for (Plan& plan : _plans)
        TickPlan(plan, diff);

    // Endless plans only summarize when stopped, so they checkpoint as they
    // go: a worldserver crash then loses at most one interval of aggregate.
    uint32 const checkpointMin =
        DcSettings::GetUInt(ObjectGuid::Empty, "TestRun.Plan.CheckpointMin");
    uint64 const nowMs = NowUnixMs();
    for (Plan& plan : _plans)
    {
        if (!plan.spec.endless || checkpointMin == 0 || plan.stopping)
            continue;
        if (nowMs - plan.lastCheckpointMs < static_cast<uint64>(checkpointMin) * 60 * 1000)
            continue;
        plan.lastCheckpointMs = nowMs;
        WriteSummary(plan, true, nowMs);
    }

    for (auto it = _plans.begin(); it != _plans.end();)
    {
        if (DcTestPlan::IsFinished(it->spec, it->counters, it->stopping))
        {
            Finalize(*it);
            it = _plans.erase(it);
        }
        else
            ++it;
    }
}

void DcTestPlanManager::TickPlan(Plan& plan, uint32 diff)
{
    plan.backoffMs = plan.backoffMs > diff ? plan.backoffMs - diff : 0;

    if (plan.stopping)
        return;

    // Reset guard (pool plans only — a single-dungeon plan keeps today's
    // behaviour): refreshed every tick, paused or not, so the heartbeat always
    // says whether heroics / raids are being held back.
    std::vector<bool> eligible;
    if (plan.spec.isPool)
    {
        uint64 const nowS = NowUnixMs() / 1000;
        plan.resetGuardUntilS = DcTestPlan::ResetGuardUntil(
            nowS, sWorld->getIntConfig(CONFIG_INSTANCE_RESET_TIME_HOUR),
            DcSettings::GetUInt(ObjectGuid::Empty, "TestRun.Plan.ResetGuardMin"));
        eligible = DcTestPlan::EligibleEntries(plan.spec, plan.resetGuardUntilS != 0);
        plan.holdingForReset =
            plan.resetGuardUntilS != 0 &&
            std::none_of(eligible.begin(), eligible.end(), [](bool b) { return b; });
    }

    if (plan.paused || plan.holdingForReset)
        return;

    uint32 const wanted = DcTestPlan::LaunchesWanted(
        plan.spec, plan.counters, DcTestRunManager::Instance().CapHeadroom(), plan.backoffMs);
    if (wanted == 0)
        return;

    // At most one launch per world tick per plan: each accepted Start feeds
    // five async bot logins into the shared provisioning budget, and spreading
    // the starts keeps the world tick smooth. The next tick launches the next.
    //
    // Issuer resolution happens per launch, not per plan: the plan may have
    // been started in-game (issuer = that GM) or from the console (issuer =
    // the headless driver). Either way, when the stored issuer is gone the
    // driver takes over — a GM can start a 20-run plan and log off.
    Player* gm = ObjectAccessor::FindConnectedPlayer(plan.gmGuid);
    uint32 const backoffCfg =
        DcSettings::GetUInt(ObjectGuid::Empty, "TestRun.Plan.BackoffMs");
    if (!gm)
    {
        // No issuing GM: either the plan came from the console (there never was
        // one) or the GM logged off mid-campaign. Either way the headless
        // driver takes over — and since a console start registers the plan on
        // the same click that kicks the login off, waiting here is the normal
        // first-tick path, not an error.
        std::string why;
        DcTestDriver::Readiness const ready = DcTestDriver::Ensure(&why);
        if (ready == DcTestDriver::Readiness::Ready)
        {
            gm = DcTestDriver::Get();
            plan.driverWaitSinceMs = 0;
        }
        else
        {
            uint64 const nowMs = NowUnixMs();
            if (!plan.driverWaitSinceMs)
                plan.driverWaitSinceMs = nowMs;
            plan.backoffMs = backoffCfg;

            uint32 const waitCap =
                DcSettings::GetUInt(ObjectGuid::Empty, "TestRun.Plan.DriverWaitMs");
            if (DcTestPlan::DriverWaitVerdict(
                    ready == DcTestDriver::Readiness::PendingLogin,
                    static_cast<uint32>(nowMs - plan.driverWaitSinceMs),
                    waitCap) == DcTestPlan::DriverWait::Abort)
                AbortPlan(plan, "no issuing GM: " + why);
            return;
        }
    }

    // Draw the entry only now that a launch will really be attempted, so the
    // bag is never consumed by a tick that didn't launch.
    std::int32_t const pickIdx = DcTestPlan::PickNext(plan.picker, plan.spec, eligible);
    if (pickIdx < 0)
        return;
    DcTestPlan::PoolEntry const entry = plan.spec.pool[static_cast<std::size_t>(pickIdx)];
    // A refused launch gives the entry its turn back (bag mode).
    auto giveBack = [&]()
    {
        if (plan.spec.pick == DcTestPlan::PickMode::Bag)
            plan.picker.bag.insert(plan.picker.bag.begin(), static_cast<uint32>(pickIdx));
    };

    uint32 const seed =
        plan.spec.seedBase ? plan.spec.seedBase + plan.counters.launched : 0;

    std::string msg;
    std::string runId;
    DcTestRunManager::StartErr err = DcTestRunManager::StartErr::None;
    bool const ok = DcTestRunManager::Instance().Start(gm, entry.token,
                                                       plan.spec.level, seed, entry.heroic,
                                                       plan.spec.gear, &msg, plan.spec.planId,
                                                       &err, &runId, entry.size);
    if (ok)
    {
        ++plan.counters.launched;
        ++plan.counters.activeNow;
        plan.activeRuns.push_back({runId, entry.Key()});
        ++plan.entries[entry.Key()].launched;
        plan.transientStreak = 0;
        plan.refusalStreak = 0;
        LOG_INFO("playerbots.dungeonclear", "TESTPLAN {} launched {} {} ({}/{})",
                 plan.spec.planId, runId, entry.Key(), plan.counters.launched,
                 plan.spec.endless ? std::string("∞") : std::to_string(plan.spec.total));
        return;
    }

    switch (err)
    {
        case DcTestRunManager::StartErr::CapHit:
        case DcTestRunManager::StartErr::PoolExhausted:
            giveBack();
            plan.backoffMs = backoffCfg;
            // With anything in flight a rejection resolves itself when a run
            // finishes; only a rejection with the whole harness idle can be a
            // permanent misconfiguration (an empty addclass pool) —
            // count those. The harness-wide check (not just this plan's own
            // children) is what lets plans queue: with MaxPlans unlimited, a
            // plan launched while another is eating the bot budget has nothing
            // active of its own and would otherwise abort itself within three
            // backoffs instead of waiting its turn.
            if (plan.counters.activeNow == 0 && !DcTestRunManager::Instance().IsActive() &&
                ++plan.transientStreak >= kMaxTransientStreak)
            {
                if (plan.spec.endless)
                {
                    // An endless plan is meant to sit through a bad patch; park
                    // it with the reason rather than throw the session away.
                    plan.paused = true;
                    plan.pauseReason = "stalled: " + msg;
                    LOG_INFO("playerbots.dungeonclear", "TESTPLAN {} paused: {}",
                             plan.spec.planId, plan.pauseReason);
                }
                else
                    AbortPlan(plan, "stalled: " + msg);
            }
            break;
        default:
            if (!plan.spec.isPool)
            {
                AbortPlan(plan, msg);
                break;
            }
            // Pool plan: one bad entry must not end the whole campaign. Skip
            // this launch; a run of refusals (every entry refusing) pauses it.
            plan.lastRefusal = entry.Key() + ": " + msg;
            plan.backoffMs = backoffCfg;
            LOG_INFO("playerbots.dungeonclear", "TESTPLAN {} launch refused: {}",
                     plan.spec.planId, plan.lastRefusal);
            if (++plan.refusalStreak >= kMaxRefusalStreak)
            {
                plan.paused = true;
                plan.pauseReason = "launches keep being refused — " + plan.lastRefusal;
                LOG_INFO("playerbots.dungeonclear", "TESTPLAN {} paused: {}",
                         plan.spec.planId, plan.pauseReason);
            }
            break;
    }
}

void DcTestPlanManager::AbortPlan(Plan& plan, std::string const& reason)
{
    plan.stopping = true;
    plan.result = "aborted";
    plan.abortReason = reason;
    LOG_INFO("playerbots.dungeonclear", "TESTPLAN {} aborting: {}", plan.spec.planId, reason);
    for (ActiveRun const& run : plan.activeRuns)
    {
        std::string ignored;
        DcTestRunManager::Instance().Stop(run.runId, &ignored);
    }
}

void DcTestPlanManager::OnRunFinished(std::string const& planId, DcTestPlan::RunOutcome outcome)
{
    for (Plan& plan : _plans)
    {
        if (plan.spec.planId != planId)
            continue;

        std::string entryKey;
        auto it = std::find_if(plan.activeRuns.begin(), plan.activeRuns.end(),
                               [&](ActiveRun const& r) { return r.runId == outcome.runId; });
        if (it != plan.activeRuns.end())
        {
            entryKey = it->entryKey;
            plan.activeRuns.erase(it);
        }
        if (plan.counters.activeNow > 0)
            --plan.counters.activeNow;

        if (outcome.result == "success")
            ++plan.counters.succeeded;
        else
            ++plan.counters.failed;

        if (!entryKey.empty())
        {
            auto const es = plan.entries.find(entryKey);
            if (es != plan.entries.end())
                es->second.acc.Add(outcome);
        }
        plan.acc.Add(outcome);
        return;
    }
}

void DcTestPlanManager::WriteSummary(Plan const& plan, bool checkpoint, uint64 nowMs)
{
    DcTestPlanSummary::Stats const stats = plan.acc.Build();

    DcTestPlanSummary::Header h;
    h.planId = plan.spec.planId;
    if (plan.spec.isPool)
    {
        h.dungeon = "pool";
        h.dungeonName = "Pool: " + PoolText(plan.spec.pool);
    }
    else
    {
        h.dungeon = plan.spec.dungeonToken;
        if (DcTestDungeonRegistry::Row const* row = DcTestDungeonRegistry::Find(plan.spec.dungeonToken))
            h.dungeonName = row->name;
    }
    h.total = plan.spec.total;
    h.concurrent = plan.spec.concurrent;
    h.level = plan.spec.level;
    h.heroic = plan.spec.heroic;
    h.seedBase = plan.spec.seedBase;
    h.size = plan.spec.size;
    h.gearIlvl = plan.spec.gear.ilvl;
    h.gearQuality = plan.spec.gear.quality;
    h.startedAtMs = plan.startedAtMs;
    h.endedAtMs = nowMs;
    h.durationS = static_cast<uint32>((h.endedAtMs - h.startedAtMs) / 1000);
    h.result = checkpoint ? "running" : (plan.result.empty() ? "completed" : plan.result);
    h.abortReason = checkpoint ? plan.pauseReason : plan.abortReason;
    h.endless = plan.spec.endless;
    h.checkpoint = checkpoint;

    std::vector<DcTestPlanSummary::PoolEntryStats> pool;
    if (plan.spec.isPool)
    {
        h.pick = plan.spec.pick == DcTestPlan::PickMode::Bag ? "bag" : "random";
        // Current pool first, in pool order; then anything an edit removed
        // that still has runs to its name.
        for (DcTestPlan::PoolEntry const& e : plan.spec.pool)
        {
            auto const it = plan.entries.find(e.Key());
            if (it == plan.entries.end())
                continue;
            pool.push_back({e.token, e.heroic, e.size, true, it->second.acc.Build()});
        }
        for (auto const& entry : plan.entries)
        {
            std::string const& key = entry.first;
            EntryStats const& es = entry.second;
            bool const inPool = std::any_of(plan.spec.pool.begin(), plan.spec.pool.end(),
                                            [&](DcTestPlan::PoolEntry const& e) { return e.Key() == key; });
            if (!inPool && es.launched)
                pool.push_back({es.token, es.heroic, es.size, false, es.acc.Build()});
        }
    }
    DcTestPlanSummary::Append(h, stats, pool);
}

void DcTestPlanManager::Finalize(Plan& plan)
{
    uint64 const nowMs = NowUnixMs();
    WriteSummary(plan, false, nowMs);

    DcTestPlanSummary::Stats const stats = plan.acc.Build();
    std::string const result = plan.result.empty() ? "completed" : plan.result;
    uint32 const durationS = static_cast<uint32>((nowMs - plan.startedAtMs) / 1000);

    LOG_INFO("playerbots.dungeonclear",
             "TESTPLAN END {} result={} runs={}/{} ok={} fail={} duration={}s{}",
             plan.spec.planId, result, stats.launched,
             plan.spec.endless ? std::string("∞") : std::to_string(plan.spec.total),
             stats.succeeded, stats.failed, durationS,
             plan.abortReason.empty() ? std::string() : (" reason=" + plan.abortReason));

    if (Player* gm = ObjectAccessor::FindConnectedPlayer(plan.gmGuid))
        ChatHandler(gm->GetSession()).SendSysMessage(Acore::StringFormat(
            "Test plan {} {}: {} — {}/{} runs succeeded{}{}",
            plan.spec.planId, PlanLabel(plan), result, stats.succeeded, stats.launched,
            stats.succeeded ? Acore::StringFormat(", median {}s", stats.medianS) : std::string(),
            plan.abortReason.empty() ? std::string() : (" — " + plan.abortReason)));
}
