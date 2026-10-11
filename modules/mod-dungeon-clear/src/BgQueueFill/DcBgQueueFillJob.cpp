/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "BgQueueFill/DcBgQueueFillJob.h"

#include <algorithm>
#include <ctime>
#include <functional>
#include <utility>

#include "Battleground.h"
#include "BattlegroundMgr.h"
#include "BattlegroundQueue.h"
#include "BattlegroundUtils.h"
#include "Chat.h"
#include "DBCStores.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Opcodes.h"
#include "Player.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include "PlayerbotAI.h"
#include "PlayerbotFactory.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"

#include "Ai/Dungeon/DungeonClear/Settings/DcSettings.h"
#include "BgQueueFill/DcBgQueueFillManager.h"
#include "DungeonQueueFill/DcDungeonQueueFillPlanner.h"
#include "TestRun/DcTestGearTiers.h"
#include "Util/DcBotProvisioning.h"
#include "Util/DcDungeonAccess.h"
#include "Util/DcPoolBots.h"
#include "Util/DcProvisionBudget.h"

namespace
{
    using DcDungeonQueueFillPlanner::NextRand;

    // Deserter. Cast on anyone who leaves a running battleground — which is
    // what a released bot does — and it makes the next join refuse
    // (ERR_GROUP_JOIN_BATTLEGROUND_DESERTERS, on the BOT's session, silently).
    constexpr std::uint32_t kDeserterSpell = 26013;

    // Logins started but not yet in world, per fill. A 40v40 fill is 79 bots;
    // starting them all at once would put the start-critical ones behind the
    // rest in the login queue. The provisioning ration (one roll per tick) is
    // the real pace-setter, so this only has to stay ahead of it.
    constexpr std::uint32_t kMaxLoginsInFlight = 16;

    // The invite safety net (see TickInvites): how long playerbots' own `bg
    // status` handler gets to accept before the fill does, and how often the
    // fill re-sends inside the 60s invite window.
    constexpr std::uint32_t kInviteGraceMs = 3000;
    constexpr std::uint32_t kPortRetryMs = 5000;

    // A match with no real player in it for this long is released. Long enough
    // to ride out a loading screen or a quick reconnect.
    constexpr std::uint32_t kNoHumanGraceMs = 30000;

    // After the match ends the core walks everyone out on TIME_TO_AUTOREMOVE
    // (120s). Past that the fill pulls whatever is left itself.
    constexpr std::uint32_t kEndedGraceMs = 150000;

    // Cadence of the queue nudge (see TickNudge).
    constexpr std::uint32_t kQueueNudgeMs = 2000;

    // Times a queued bot that falls out of the queue (an invite it missed, a
    // decline because it was in combat) is put back before it is dropped.
    constexpr std::uint8_t kMaxRequeues = 2;

    std::string MakeFillId()
    {
        static std::uint32_t counter = 0;
        std::time_t const now = std::time(nullptr);
        std::tm tmBuf{};
        localtime_r(&now, &tmBuf);
        char buf[32];
        std::strftime(buf, sizeof(buf), "bf-%Y%m%d-%H%M%S", &tmBuf);
        return std::string(buf) + "-" + std::to_string(++counter);
    }

    std::uint32_t SetupTimeoutMs()
    {
        return DcSettings::GetUInt(ObjectGuid::Empty, "BgQueueFill.SetupTimeoutSec") * 1000;
    }

    std::uint32_t MatchTimeoutMs()
    {
        return DcSettings::GetUInt(ObjectGuid::Empty, "BgQueueFill.MatchTimeoutSec") * 1000;
    }

    char const* BgName(std::uint32_t bgTypeId)
    {
        switch (bgTypeId)
        {
            case BATTLEGROUND_AV: return "Alterac Valley";
            case BATTLEGROUND_WS: return "Warsong Gulch";
            case BATTLEGROUND_AB: return "Arathi Basin";
            case BATTLEGROUND_EY: return "Eye of the Storm";
            case BATTLEGROUND_SA: return "Strand of the Ancients";
            case BATTLEGROUND_IC: return "Isle of Conquest";
            case BATTLEGROUND_RB: return "Random Battleground";
            default:              return "battleground";
        }
    }

    // The map strategy AiFactory installs for a bot inside this battleground,
    // or nullptr where there is none to check (Strand of the Ancients).
    char const* MapStrategy(std::uint32_t concreteBgTypeId)
    {
        switch (concreteBgTypeId)
        {
            case BATTLEGROUND_WS: return "warsong";
            case BATTLEGROUND_AB: return "arathi";
            case BATTLEGROUND_AV: return "alterac";
            case BATTLEGROUND_EY: return "eye";
            case BATTLEGROUND_IC: return "isle";
            default:              return nullptr;
        }
    }

    void ResurrectAndCalm(Player* bot)
    {
        if (bot->IsInCombat())
            bot->CombatStop(true);
        if (!bot->IsAlive())
        {
            bot->ResurrectPlayer(1.0f);
            bot->SpawnCorpseBones();
        }
    }
}

char const* DcBgQueueFillJob::StageName(Stage s)
{
    switch (s)
    {
        case Stage::Observed:  return "observed";
        case Stage::Planning:  return "planning";
        case Stage::Filling:   return "filling";
        case Stage::Waiting:   return "waiting";
        case Stage::InBattle:  return "in_battle";
        case Stage::Releasing: return "releasing";
        case Stage::Done:      return "done";
    }
    return "?";
}

char const* DcBgQueueFillJob::SlotStageName(SlotStage s)
{
    switch (s)
    {
        case SlotStage::Claiming:     return "claiming";
        case SlotStage::LoggingIn:    return "logging_in";
        case SlotStage::Evicting:     return "evicting";
        case SlotStage::Provisioning: return "provisioning";
        case SlotStage::Sanitizing:   return "sanitizing";
        case SlotStage::Queueing:     return "queueing";
        case SlotStage::Queued:       return "queued";
        case SlotStage::Dropped:      return "dropped";
    }
    return "?";
}

std::unique_ptr<DcBgQueueFillJob> DcBgQueueFillJob::Observe(Player* player, std::uint32_t bgTypeId,
                                                            bool demoted)
{
    if (!player)
        return nullptr;

    std::unique_ptr<DcBgQueueFillJob> job(new DcBgQueueFillJob());
    job->_id = MakeFillId();
    job->_playerGuid = player->GetGUID();
    job->_playerName = player->GetName();
    job->_level = player->GetLevel();
    // GetTeamId(), not the "real" team: it is the one AddGroup files the
    // player's queue entry under.
    job->_team = player->GetTeamId() == TEAM_ALLIANCE ? DcBgQueueFillPlanner::kTeamAlliance
                                                      : DcBgQueueFillPlanner::kTeamHorde;
    job->_bgTypeId = bgTypeId;
    job->_queueTypeId = BattlegroundMgr::BGQueueTypeId(BattlegroundTypeId(bgTypeId), 0);
    job->_isRandom = bgTypeId == BATTLEGROUND_RB;
    job->_demoted = demoted;

    LOG_INFO("playerbots.dungeonclear",
             "BGQUEUEFILL {} observed {} (level {}, {}) queueing for {}{}", job->_id,
             job->_playerName, job->_level, DcBgQueueFillPlanner::TeamName(job->_team),
             BgName(bgTypeId), demoted ? " — premade group moved to the normal queue" : "");

    return job;
}

Player* DcBgQueueFillJob::FindPlayer() const
{
    // Connected, not in-world: a player on a loading screen into the match is
    // very much still here.
    return ObjectAccessor::FindConnectedPlayer(_playerGuid);
}

void DcBgQueueFillJob::EnterStage(Stage s)
{
    _stage = s;
    _stageMs = 0;
}

void DcBgQueueFillJob::EnterSlotStage(Slot& slot, SlotStage s)
{
    slot.stage = s;
    slot.stageMs = 0;
}

void DcBgQueueFillJob::Release(std::string const& reason, bool notifyPlayer, bool force)
{
    if (_stage == Stage::Releasing || _stage == Stage::Done)
        return;
    _releaseReason = reason;
    _notifyPlayer = notifyPlayer;
    _forceRelease = force;
    EnterStage(Stage::Releasing);
}

bool DcBgQueueFillJob::HoldsBot(ObjectGuid guid) const
{
    if (!guid)
        return false;
    for (Slot const& slot : _slots)
        if (slot.guid == guid && slot.stage != SlotStage::Dropped)
            return true;
    return false;
}

std::vector<ObjectGuid> DcBgQueueFillJob::BotGuids() const
{
    std::vector<ObjectGuid> out;
    out.reserve(_slots.size());
    for (Slot const& slot : _slots)
        if (slot.guid)
            out.push_back(slot.guid);
    return out;
}

std::uint32_t DcBgQueueFillJob::InFlightFor(std::uint32_t queueTypeId, std::uint32_t bracketId,
                                            std::uint8_t side) const
{
    if (queueTypeId != _queueTypeId || bracketId != _bracketId)
        return 0;
    if (_stage == Stage::Releasing || _stage == Stage::Done)
        return 0;

    std::uint32_t n = 0;
    for (Slot const& slot : _slots)
        if (slot.plan.team == side && slot.stage != SlotStage::Queued &&
            slot.stage != SlotStage::Dropped)
            ++n;
    return n;
}

std::uint32_t DcBgQueueFillJob::CountSlots(SlotStage s) const
{
    std::uint32_t n = 0;
    for (Slot const& slot : _slots)
        if (slot.stage == s)
            ++n;
    return n;
}

bool DcBgQueueFillJob::AnyQueued() const
{
    return CountSlots(SlotStage::Queued) > 0;
}

bool DcBgQueueFillJob::AllCriticalQueued() const
{
    for (Slot const& slot : _slots)
        if (slot.plan.startCritical && slot.stage != SlotStage::Queued)
            return false;
    return true;
}

bool DcBgQueueFillJob::AllSettled() const
{
    for (Slot const& slot : _slots)
        if (slot.stage != SlotStage::Queued && slot.stage != SlotStage::Dropped)
            return false;
    return true;
}

bool DcBgQueueFillJob::PlayerHasMovedOn() const
{
    if (_stage == Stage::Releasing || _stage == Stage::Done)
        return true;
    Player* const player = FindPlayer();
    if (!player)
        return true;
    if (_stage == Stage::InBattle)
        return player->GetBattlegroundId() != _bgInstanceId;
    return !player->InBattlegroundQueueForBattlegroundQueueType(
               BattlegroundQueueTypeId(_queueTypeId)) &&
           !player->InBattleground();
}

Battleground* DcBgQueueFillJob::FindPlayersBattleground() const
{
    Player* const player = FindPlayer();
    if (!player)
        return nullptr;
    // create=true only skips GetBattleground's "map exists yet" test — see
    // TickPlayer for why that test must not be applied here.
    if (Battleground* bg = player->GetBattleground(true))
        return bg;

    GroupQueueInfo ginfo;
    BattlegroundQueue& queue =
        sBattlegroundMgr->GetBattlegroundQueue(BattlegroundQueueTypeId(_queueTypeId));
    if (!queue.GetPlayerGroupInfoData(_playerGuid, &ginfo) || !ginfo.IsInvitedToBGInstanceGUID)
        return nullptr;
    // TYPE_NONE searches every type: a Random instance is filed under the
    // Random type, not the map it rolled.
    return sBattlegroundMgr->GetBattleground(ginfo.IsInvitedToBGInstanceGUID, BATTLEGROUND_TYPE_NONE);
}

void DcBgQueueFillJob::Tick(std::uint32_t diff)
{
    if (_stage == Stage::Done)
        return;

    _stageMs += diff;
    _totalMs += diff;
    for (Slot& slot : _slots)
        slot.stageMs += diff;

    // The player is the whole point: no player, no match worth filling.
    if (_stage != Stage::Releasing && !FindPlayer())
        Release("player logged out");

    switch (_stage)
    {
        case Stage::Observed:
            TickObserved();
            break;
        case Stage::Planning:
            TickPlanning();
            break;
        case Stage::Filling:
        case Stage::Waiting:
            TickPlayer();
            break;
        case Stage::InBattle:
            TickInBattle(diff);
            break;
        case Stage::Releasing:
            TickReleasing();
            return;
        case Stage::Done:
            return;
    }

    // The slots keep moving in every live stage, InBattle included: that is
    // how the late ones stream into a match that popped at its minimum. Not
    // once the match has ended — a bot walking out of it is not a bot to put
    // back in the queue.
    if (_stage == Stage::Filling || _stage == Stage::Waiting ||
        (_stage == Stage::InBattle && !_endedMs))
    {
        TickSlots();
        if (_stage != Stage::Releasing)
        {
            TickInvites(diff);
            TickNudge(diff);
        }
    }

    if (_stage == Stage::Releasing)
        TickReleasing();
}

// The join hook fires after AddGroup, so by now the player is either in the
// queue or already out of it again. There is no window to wait through.
void DcBgQueueFillJob::TickObserved()
{
    Player* const player = FindPlayer();
    BattlegroundQueueTypeId const qt = BattlegroundQueueTypeId(_queueTypeId);

    // The queue update the join scheduled can invite the player on the spot
    // when a running instance of this battleground has a free slot. Nothing
    // to fill then — the player has a match.
    if (player->IsInvitedForBattlegroundQueueType(qt) || player->InBattleground())
    {
        Release("player was invited straight away (a running battleground had room)");
        return;
    }

    if (!player->InBattlegroundQueueForBattlegroundQueueType(qt))
    {
        Release("player is not in the queue");
        return;
    }

    EnterStage(Stage::Planning);
}

// Read the queue, the template and the pool, and plan every slot.
//
// Present per side is the NORMAL pool only: a premade on the player's side
// would never be matched with our solo-queued bots, and the player's own group
// has already been moved out of the premade pool if it was ever in it.
void DcBgQueueFillJob::TickPlanning()
{
    Player* const player = FindPlayer();

    Battleground* const tmpl = sBattlegroundMgr->GetBattlegroundTemplate(BattlegroundTypeId(_bgTypeId));
    if (!tmpl)
    {
        Release("no battleground template for type " + std::to_string(_bgTypeId));
        return;
    }

    // The same lookup the join handler made, so it is the same bracket.
    PvPDifficultyEntry const* const bracket =
        GetBattlegroundBracketByLevel(tmpl->GetMapId(), player->GetLevel());
    if (!bracket)
    {
        Release("no battleground bracket for level " + std::to_string(player->GetLevel()));
        return;
    }

    _bracketId = bracket->GetBracketId();
    _bracketMinLevel = bracket->minLevel;
    _bracketMaxLevel = std::min<std::uint32_t>(bracket->maxLevel,
                                               sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL));
    _minPerTeam = GetMinPlayersPerTeam(tmpl, bracket);
    if (sBattlegroundMgr->isTesting())
        _minPerTeam = 1;
    _maxPerTeam = tmpl->GetMaxPlayersPerTeam();

    // Seeded off the job id and the level, exactly as the dungeon fill is: two
    // fills in the same second differ, one fill replays from its log line.
    _seed = static_cast<std::uint32_t>(std::hash<std::string>{}(_id)) ^ (_level * 2654435761u);
    _drawState = _seed ^ 0x2545f491u;

    if (std::vector<ObjectGuid> const* last =
            DcBgQueueFillManager::Instance().RecentBotsFor(_playerGuid))
        _avoidGuids = *last;

    DcBgQueueFillPlanner::Request req;
    req.minPerTeam = _minPerTeam;
    req.maxPerTeam = _maxPerTeam;
    req.bracketMinLevel = _bracketMinLevel;
    req.bracketMaxLevel = _bracketMaxLevel;
    req.playerLevel = _level;
    req.levelSpread = DcSettings::GetUInt(ObjectGuid::Empty, "BgQueueFill.LevelSpread");
    req.healersPerSide = DcSettings::GetUInt(ObjectGuid::Empty, "BgQueueFill.HealersPerSide");
    req.playerTeam = _team;
    req.seed = _seed;

    BattlegroundQueue& queue =
        sBattlegroundMgr->GetBattlegroundQueue(BattlegroundQueueTypeId(_queueTypeId));
    for (std::uint8_t t = 0; t < DcBgQueueFillPlanner::kTeams; ++t)
    {
        req.sides[t].humansQueued = queue.GetPlayersCountInGroupsQueue(
            BattlegroundBracketId(_bracketId), BattlegroundQueueGroupTypes(BG_QUEUE_NORMAL_ALLIANCE + t));
        req.sides[t].botsInFlight =
            DcBgQueueFillManager::Instance().InFlightBots(_queueTypeId, _bracketId, t, this);
        req.sides[t].poolFree = DcPoolBots::CountFree(t == DcBgQueueFillPlanner::kTeamAlliance, nullptr);
    }

    DcBgQueueFillPlanner::Result const plan = DcBgQueueFillPlanner::Plan(req);
    if (plan.kind == DcBgQueueFillPlanner::Kind::NothingToDo)
    {
        Release("the queue already holds a full match (" + plan.detail + ")");
        return;
    }
    if (plan.kind != DcBgQueueFillPlanner::Kind::Ok)
    {
        Release(plan.detail + " — pre-seed with `.playerbots addclass`", /*notifyPlayer*/ true);
        return;
    }

    _slots.clear();
    _slots.reserve(plan.slots.size());
    std::uint32_t critical[DcBgQueueFillPlanner::kTeams] = {0, 0};
    std::uint32_t healers[DcBgQueueFillPlanner::kTeams] = {0, 0};
    for (DcBgQueueFillPlanner::Slot const& s : plan.slots)
    {
        Slot slot;
        slot.plan = s;
        _slots.push_back(slot);
        if (s.startCritical)
            ++critical[s.team];
        if (std::string(s.role) == "heal")
            ++healers[s.team];
    }

    auto const [lo, hi] = DcBgQueueFillPlanner::LevelWindow(req);
    LOG_INFO("playerbots.dungeonclear",
             "BGQUEUEFILL {} plan: {} bracket {}-{}, {}v{} (pops at {}), levels {}-{}; Alliance {} "
             "present + {} in flight, +{} bot(s) ({} start-critical, {} healers); Horde {} present "
             "+ {} in flight, +{} bot(s) ({} start-critical, {} healers); shortfall A{} H{} (seed {})",
             _id, BgName(_bgTypeId), _bracketMinLevel, _bracketMaxLevel, _maxPerTeam, _maxPerTeam,
             _minPerTeam, lo, hi, req.sides[0].humansQueued, req.sides[0].botsInFlight,
             plan.planned[0], critical[0], healers[0], req.sides[1].humansQueued,
             req.sides[1].botsInFlight, plan.planned[1], critical[1], healers[1],
             plan.shortfall[0], plan.shortfall[1], _seed);

    // Gear ceiling, resolved once: a conf reloaded mid-fill must not give one
    // side one ceiling and the other another.
    DcTestGearTiers::Spec spec;
    spec.ilvl = static_cast<std::int32_t>(DcSettings::GetUInt(ObjectGuid::Empty, "BgQueueFill.GearIlvl"));
    spec.quality = DcSettings::GetUInt(ObjectGuid::Empty, "BgQueueFill.GearQuality");
    DcTestGearTiers::Resolved const gear = DcTestGearTiers::Resolve(
        spec, sPlayerbotAIConfig.autoGearScoreLimit, sPlayerbotAIConfig.autoGearQualityLimit);
    _gearIlvl = gear.ilvl;
    _gearQuality = gear.quality;
    _gearScoreLimit = gear.ilvl == 0 ? 0 : PlayerbotFactory::CalcMixedGearScore(gear.ilvl, gear.quality);

    EnterStage(Stage::Filling);
}

// The player side of the fill, while they are queued or holding the popup.
void DcBgQueueFillJob::TickPlayer()
{
    Player* const player = FindPlayer();
    if (!player)
        return;  // Tick's own liveness check already released
    BattlegroundQueueTypeId const qt = BattlegroundQueueTypeId(_queueTypeId);

    // 1. They clicked Enter Battle. The instance id is what InBattle follows.
    //
    // GetBattleground() without create=true returns null until the
    // battleground's MAP exists, and the map is only created when the first
    // player arrives. A player who accepts before any bot — a selfbot's own
    // AI clicks Enter Battle the instant the popup lands — is on that loading
    // screen with InBattleground() already true, and reading the null as "some
    // other battleground" released every bot and left them in it alone.
    if (player->InBattleground())
    {
        Battleground* const bg = player->GetBattleground(true);
        if (!bg)
            return;  // the instance is still being set up; next tick
        if (bg->GetBgTypeID() != _bgTypeId)
        {
            Release("player entered a different battleground");
            return;
        }
        _bgInstanceId = bg->GetInstanceID();
        EnterStage(Stage::InBattle);
        LOG_INFO("playerbots.dungeonclear",
                 "BGQUEUEFILL {} {} entered {} (instance {}) — {} bot(s) queued, {} still setting up",
                 _id, _playerName, BgName(bg->GetBgTypeID(true)), _bgInstanceId,
                 CountSlots(SlotStage::Queued),
                 _slots.size() - CountSlots(SlotStage::Queued) - CountSlots(SlotStage::Dropped));
        MaybeTopUp(bg);
        return;
    }

    // 2. Out of the queue without entering: left it, or let the popup go.
    if (!player->InBattlegroundQueueForBattlegroundQueueType(qt))
    {
        Release(_stage == Stage::Filling ? "player left the queue"
                                         : "player declined the invitation or let it lapse");
        return;
    }

    // 3. The popup is up.
    if (player->IsInvitedForBattlegroundQueueType(qt))
    {
        // Before a single bot of ours was in the queue: real players, or a
        // running instance with room, got there first. A good outcome — the
        // bots go home.
        if (!AnyQueued())
        {
            Release("player matched with other players before the fill was ready");
            return;
        }

        if (_stage == Stage::Filling)
        {
            EnterStage(Stage::Waiting);
            LOG_INFO("playerbots.dungeonclear",
                     "BGQUEUEFILL {} match popped for {} after {}s — {} bot(s) queued", _id,
                     _playerName, _totalMs / 1000, CountSlots(SlotStage::Queued));
        }

        if (_isRandom && !_toppedUp)
            MaybeTopUp(FindPlayersBattleground());
    }
    else if (_stage == Stage::Filling && AllCriticalQueued())
    {
        EnterStage(Stage::Waiting);
        LOG_INFO("playerbots.dungeonclear",
                 "BGQUEUEFILL {} every start-critical bot is queued after {}s — waiting for the pop",
                 _id, _totalMs / 1000);
    }

    if (_stage == Stage::Waiting && _stageMs >= MatchTimeoutMs())
        Release("match timed out", /*notifyPlayer*/ true);
}

// The match is running with the player in it. Release it when it ends, when
// the player leaves it, or when nobody real is left in it.
void DcBgQueueFillJob::TickInBattle(std::uint32_t diff)
{
    Player* const player = FindPlayer();
    if (!player)
        return;

    Battleground* const bg = sBattlegroundMgr->GetBattleground(_bgInstanceId, BATTLEGROUND_TYPE_NONE);
    if (!bg)
    {
        Release("the battleground closed");
        return;
    }

    // Ended. The core walks everyone out on TIME_TO_AUTOREMOVE and stock
    // BGStatusAction takes the bots with it; let that happen, and let the
    // player read the scoreboard, then pull whatever is left.
    if (bg->GetStatus() == STATUS_WAIT_LEAVE)
    {
        if (!_endedMs)
            LOG_INFO("playerbots.dungeonclear", "BGQUEUEFILL {} {} ended after {}s", _id,
                     BgName(bg->GetBgTypeID(true)), _stageMs / 1000);
        _endedMs += std::max<std::uint32_t>(diff, 1);

        bool anyInside = false;
        for (Slot const& slot : _slots)
            if (Player* const bot = ObjectAccessor::FindConnectedPlayer(slot.guid))
                if (bot->GetBattlegroundId() == _bgInstanceId)
                    anyInside = true;

        if (!anyInside || player->GetBattlegroundId() != _bgInstanceId || _endedMs >= kEndedGraceMs)
            Release("match ended");
        return;
    }

    if (player->GetBattlegroundId() != _bgInstanceId)
    {
        Release("player left the battleground");
        return;
    }

    if (DcBgQueueFillManager::CountRealPlayers(bg) == 0)
    {
        _noHumanMs += diff;
        if (_noHumanMs >= kNoHumanGraceMs)
            Release("no real player left in the battleground");
    }
    else
        _noHumanMs = 0;
}

// Every slot's pipeline, one step each, in slot order — which is plan order,
// start-critical first. Logins are capped per fill and factory rolls per realm
// tick, so walking in order is what puts the bots a pop is waiting on ahead of
// the ones that will stream in later.
void DcBgQueueFillJob::TickSlots()
{
    std::uint32_t loggingIn = CountSlots(SlotStage::LoggingIn);
    bool rollSpent = false;

    for (std::size_t i = 0; i < _slots.size() && _stage != Stage::Releasing; ++i)
    {
        switch (_slots[i].stage)
        {
            case SlotStage::Claiming:
                if (loggingIn < kMaxLoginsInFlight)
                    TickSlotClaiming(i, loggingIn);
                break;
            case SlotStage::LoggingIn:
                TickSlotLoggingIn(_slots[i]);
                if (_slots[i].stage == SlotStage::LoggingIn && _slots[i].stageMs >= SetupTimeoutMs() &&
                    !RedrawSlot(i))
                    FailSlot(i, "did not finish logging in (MaxAddedBots cap, or a login failure — "
                                "see the server log)");
                break;
            case SlotStage::Evicting:
                TickSlotEvicting(_slots[i]);
                if (_slots[i].stage == SlotStage::Evicting && _slots[i].stageMs >= SetupTimeoutMs())
                    FailSlot(i, "could not be moved to its capital");
                break;
            case SlotStage::Provisioning:
                if (!rollSpent)
                    rollSpent = TickSlotProvisioning(i);
                break;
            case SlotStage::Sanitizing:
                TickSlotSanitizing(_slots[i]);
                if (_slots[i].stage == SlotStage::Sanitizing && _slots[i].stageMs >= SetupTimeoutMs())
                    FailSlot(i, "sanitising timed out");
                break;
            case SlotStage::Queueing:
                TickSlotQueueing(i);
                break;
            case SlotStage::Queued:
                TickSlotQueued(_slots[i]);
                if (_slots[i].stage == SlotStage::Queued && _slots[i].requeues > kMaxRequeues)
                    FailSlot(i, "kept falling out of the queue");
                break;
            case SlotStage::Dropped:
                break;
        }
    }
}

ObjectGuid DcBgQueueFillJob::ClaimFor(Slot& slot)
{
    bool const alliance = slot.plan.team == DcBgQueueFillPlanner::kTeamAlliance;
    auto const held = [this](ObjectGuid guid) { return HoldsBot(guid); };

    ObjectGuid guid = DcPoolBots::ClaimPoolCharacter(alliance, slot.plan.classId, _drawState, _avoidGuids, held);
    if (guid)
        return guid;

    // The class draw is cosmetic: a side short of warlocks can field a mage.
    // Walk the role's pool in a seed-shuffled order so the substitute is not
    // always the same neighbour.
    std::vector<DcTestComp::Slot> pool =
        DcTestComp::RolePool(slot.plan.role, DcBgQueueFillPlanner::RosterFor(slot.plan.level));
    for (std::size_t i = pool.size(); i > 1; --i)
        std::swap(pool[i - 1], pool[NextRand(_drawState) % i]);

    for (DcTestComp::Slot const& alt : pool)
    {
        if (alt.classId == slot.plan.classId)
            continue;
        guid = DcPoolBots::ClaimPoolCharacter(alliance, alt.classId, _drawState, _avoidGuids, held);
        if (!guid)
            continue;
        slot.plan.classId = alt.classId;
        slot.plan.specName = alt.specName;
        slot.plan.fallbackSpec = alt.fallbackSpec;
        return guid;
    }
    return ObjectGuid::Empty;
}

// MASTERLESS login, as the dungeon fill and the test harness do and for the
// same reason: masterAccountId 0 takes AddPlayerBot's isRndbot branch that
// skips the ownership gate, files the bot under sRandomPlayerbotMgr (where the
// release looks for it), and does not make it a random-bot-rotation member.
void DcBgQueueFillJob::TickSlotClaiming(std::size_t i, std::uint32_t& loggingIn)
{
    Slot& slot = _slots[i];
    slot.guid = ClaimFor(slot);
    if (!slot.guid)
    {
        std::string const reason = std::string("no free ") + DcBgQueueFillPlanner::TeamName(slot.plan.team) +
                                   " " + slot.plan.role + " character in the addclass pool — pre-seed "
                                   "with `.playerbots addclass`";
        FailSlot(i, reason);
        if (_stage == Stage::Releasing)
            return;

        // The pool is dry for this side and role, so every slot still waiting
        // to claim one will fail the same way. Drop them together, in one
        // line, rather than one line per seat of a 40-man side.
        std::uint32_t dropped = 0;
        for (Slot& other : _slots)
            if (other.stage == SlotStage::Claiming && other.plan.team == slot.plan.team &&
                std::string(other.plan.role) == slot.plan.role && !other.plan.startCritical)
            {
                EnterSlotStage(other, SlotStage::Dropped);
                ++dropped;
            }
        if (dropped)
            LOG_INFO("playerbots.dungeonclear",
                     "BGQUEUEFILL {} {} {} pool is dry — {} more slot(s) dropped; the match runs short",
                     _id, DcBgQueueFillPlanner::TeamName(slot.plan.team), slot.plan.role, dropped);
        return;
    }

    sRandomPlayerbotMgr.AddPlayerBot(slot.guid, 0);
    ++loggingIn;
    EnterSlotStage(slot, SlotStage::LoggingIn);
}

void DcBgQueueFillJob::TickSlotLoggingIn(Slot& slot)
{
    Player* const bot = ObjectAccessor::FindPlayer(slot.guid);
    if (!bot || !bot->IsInWorld() || !GET_PLAYERBOT_AI(bot))
        return;
    slot.name = bot->GetName();
    EnterSlotStage(slot, SlotStage::Evicting);
}

// Put the bot down in its OWN faction's capital. A pool character logs in
// wherever it was saved — often inside a dungeon — and the battleground port
// records the bot's current spot as its entry point, which is where it goes
// back to when the match ends. A capital is also the one place a bot cannot be
// in combat when the invite arrives, and the port handler refuses in combat.
void DcBgQueueFillJob::TickSlotEvicting(Slot& slot)
{
    Player* const bot = ObjectAccessor::FindPlayer(slot.guid);
    if (!bot || !bot->IsInWorld() || !GET_PLAYERBOT_AI(bot))
        return;  // mid-worldport — the stage timeout bounds it

    if (DcPoolBots::EvictToCapital(bot, slot.plan.team == DcBgQueueFillPlanner::kTeamAlliance,
                                   "BGQUEUEFILL", _id))
        EnterSlotStage(slot, SlotStage::Provisioning);
}

bool DcBgQueueFillJob::TickSlotProvisioning(std::size_t i)
{
    Slot& slot = _slots[i];
    Player* const bot = ObjectAccessor::FindPlayer(slot.guid);
    PlayerbotAI* const botAI = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
    if (!bot || !bot->IsInWorld() || !botAI)
    {
        if (slot.stageMs >= SetupTimeoutMs())
            FailSlot(i, "left the world before it could be provisioned");
        return false;
    }

    std::string pickedSpec;
    int const specNo = DcBotProvisioning::ResolveSpecNo(slot.plan.classId, slot.plan.specName,
                                                        slot.plan.fallbackSpec, &pickedSpec);
    if (specNo < 0 && std::string(slot.plan.role) == "heal")
    {
        // A random roll is a damage spec three times in four. A side that was
        // planned a healer should get one; swap the class rather than ship it.
        if (!RedrawSlot(i))
            FailSlot(i, std::string("no premade spec template matching '") + slot.plan.specName +
                            "' for " + DcBotProvisioning::ClassToken(slot.plan.classId) +
                            " (AiPlayerbot.PremadeSpecName.*)");
        return false;
    }

    // The ration is realm-wide: if somebody else spent this tick's roll, so
    // has every slot behind this one.
    if (!DcProvisionBudget::Take())
        return true;

    DcBotProvisioning::Roll(bot, slot.plan.level, _gearQuality, _gearScoreLimit, specNo);

    LOG_DEBUG("playerbots.dungeonclear",
              "BGQUEUEFILL {} provisioned {} ({} {} {} {}, level {}, gear <= ilvl {})", _id,
              bot->GetName(), DcBgQueueFillPlanner::TeamName(slot.plan.team),
              DcBotProvisioning::ClassToken(slot.plan.classId),
              specNo >= 0 ? pickedSpec : std::string("(random)"), slot.plan.role, bot->GetLevel(),
              _gearIlvl ? std::to_string(_gearIlvl) : std::string("unlimited"));

    EnterSlotStage(slot, SlotStage::Sanitizing);
    return true;
}

// Everything that would make the join handler or the port handler say no —
// and both say no silently, on the bot's own session.
void DcBgQueueFillJob::TickSlotSanitizing(Slot& slot)
{
    Player* const bot = ObjectAccessor::FindPlayer(slot.guid);
    if (!bot || !bot->IsInWorld() || !GET_PLAYERBOT_AI(bot))
        return;

    // Deserter: the join handler refuses on it, and a pool character that was
    // released out of a running match by an earlier fill is wearing it.
    bot->RemoveAurasDueToSpell(kDeserterSpell);

    // The join handler refuses a death knight standing on Acherus without
    // Death Gate; the chain credit and the travel unlock clear both halves.
    if (DcPoolBots::CreditDeathKnightChain(bot))
        LOG_DEBUG("playerbots.dungeonclear", "BGQUEUEFILL {} credited {} with the death-knight chain",
                  _id, bot->GetName());
    DcDungeonAccess::UnlockTravel(bot);

    // A battleground or a queue we did not ask for. Leave, once, and wait.
    if (bot->InBattleground())
    {
        if (!slot.bgLeaveSent)
        {
            bot->LeaveBattleground();
            slot.bgLeaveSent = true;
        }
        return;
    }
    if (bot->InBattlegroundQueue())
    {
        if (!slot.bgLeaveSent)
        {
            for (std::uint32_t q = 0; q < PLAYER_MAX_BATTLEGROUND_QUEUES; ++q)
                if (BattlegroundQueueTypeId const qt = bot->GetBattlegroundQueueTypeId(q))
                    DcBgQueueFillManager::QueuePortPacket(bot, BattlegroundMgr::BGArenaType(qt),
                                                          BattlegroundMgr::BGTemplateId(qt), 0);
            slot.bgLeaveSent = true;
        }
        return;
    }

    if (!DcPoolBots::SanitizeCommon(bot, slot.lfgLeaveSent))
        return;

    EnterSlotStage(slot, SlotStage::Queueing);
}

// Queue the bot the way a client would: CMSG_BATTLEMASTER_JOIN, SOLO, for the
// type the player queued for. Solo is what keeps every bot out of the premade
// pool (the handler only marks a GROUP join premade), and the handler itself
// schedules the queue update that pops the match.
void DcBgQueueFillJob::TickSlotQueueing(std::size_t i)
{
    Slot& slot = _slots[i];
    Player* const bot = ObjectAccessor::FindPlayer(slot.guid);
    BattlegroundQueueTypeId const qt = BattlegroundQueueTypeId(_queueTypeId);

    if (bot && slot.queueSent &&
        (bot->InBattlegroundQueueForBattlegroundQueueType(qt) || bot->InBattleground()))
    {
        EnterSlotStage(slot, SlotStage::Queued);
        return;
    }

    if (bot && bot->IsInWorld() && !slot.queueSent)
    {
        WorldPacket* data = new WorldPacket(CMSG_BATTLEMASTER_JOIN, 8 + 4 + 4 + 1);
        *data << ObjectGuid::Empty;             // battlemaster: not checked
        *data << std::uint32_t(_bgTypeId);
        *data << std::uint32_t(0);              // instance id: first available
        *data << std::uint8_t(0);               // join as group: never
        bot->GetSession()->QueuePacket(data);
        slot.queueSent = true;
        return;
    }

    if (slot.stageMs >= SetupTimeoutMs() && !RedrawSlot(i))
        FailSlot(i, "would not enter the queue (see the bot's join refusal in the server log)");
}

// A queued bot that is neither in the queue nor in a match any more has been
// thrown out — an invite it missed, or one the port handler refused because it
// was in combat. Put it back, a bounded number of times.
void DcBgQueueFillJob::TickSlotQueued(Slot& slot)
{
    Player* const bot = ObjectAccessor::FindConnectedPlayer(slot.guid);
    if (!bot)
    {
        slot.requeues = kMaxRequeues + 1;  // gone: TickSlots drops it
        return;
    }
    if (bot->IsBeingTeleported() || !bot->IsInWorld())
        return;

    BattlegroundQueueTypeId const qt = BattlegroundQueueTypeId(_queueTypeId);
    if (bot->InBattlegroundQueueForBattlegroundQueueType(qt) || bot->InBattleground())
        return;

    if (++slot.requeues > kMaxRequeues)
        return;

    LOG_INFO("playerbots.dungeonclear",
             "BGQUEUEFILL {} {} fell out of the queue — re-queueing (attempt {} of {})", _id,
             slot.name.empty() ? bot->GetName() : slot.name, slot.requeues, kMaxRequeues);
    ResurrectAndCalm(bot);
    slot.queueSent = false;
    slot.bgLeaveSent = false;
    slot.lfgLeaveSent = false;
    slot.invitedMs = 0;
    slot.portRetryMs = 0;
    EnterSlotStage(slot, SlotStage::Sanitizing);
}

// The invite safety net, and the in-match strategy check.
//
// Playerbots' own `bg status` handler (WorldPacketHandlerStrategy, on every
// bot) accepts WAIT_JOIN and is given the first go. If a bot is still sitting
// on an invite after kInviteGraceMs, the fill answers for it — from the tick,
// never from a packet hook: the status packet is sent from inside
// BattlegroundQueueUpdate's walk of the queue, and accepting there would
// mutate the queue mid-iteration. The port packet is queued and handled on
// the bot's next session update. A second accept (stock and ours both firing)
// is a no-op: the bot is no longer in the queue to answer.
void DcBgQueueFillJob::TickInvites(std::uint32_t diff)
{
    BattlegroundQueueTypeId const qt = BattlegroundQueueTypeId(_queueTypeId);

    for (Slot& slot : _slots)
    {
        if (slot.stage != SlotStage::Queued)
            continue;
        Player* const bot = ObjectAccessor::FindPlayer(slot.guid);
        PlayerbotAI* const botAI = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
        if (!bot || !botAI)
            continue;

        if (bot->InBattleground())
        {
            slot.invitedMs = 0;
            // Arrived. If our accept rather than BGStatusAction's got it here,
            // nothing reset its strategies inside the battleground, and a bot
            // without them stands at the gate. Check once, on the map.
            Battleground* const bg = bot->GetBattleground();
            if (!slot.strategiesChecked && bg && !bot->IsBeingTeleported() && bot->GetMap() &&
                bot->GetMap()->IsBattlegroundOrArena())
            {
                slot.strategiesChecked = true;
                char const* const want = MapStrategy(bg->GetBgTypeID(true));
                if (want && !botAI->HasStrategy(want, BOT_STATE_NON_COMBAT))
                {
                    botAI->ResetStrategies(false);
                    LOG_DEBUG("playerbots.dungeonclear",
                              "BGQUEUEFILL {} reset {}'s strategies inside the battleground", _id,
                              bot->GetName());
                }
            }
            continue;
        }

        if (!bot->IsInvitedForBattlegroundQueueType(qt))
        {
            slot.invitedMs = 0;
            slot.portRetryMs = 0;
            continue;
        }

        slot.invitedMs += diff;
        if (slot.invitedMs < kInviteGraceMs)
            continue;
        if (slot.portRetryMs > diff)
        {
            slot.portRetryMs -= diff;
            continue;
        }
        slot.portRetryMs = kPortRetryMs;

        // The port handler refuses in combat, and resurrects the dead itself
        // but only after that check.
        ResurrectAndCalm(bot);
        DcBgQueueFillManager::QueuePortPacket(bot, 0, _bgTypeId, 1);
        LOG_DEBUG("playerbots.dungeonclear", "BGQUEUEFILL {} accepting the invite for {} ({}s unanswered)",
                  _id, bot->GetName(), slot.invitedMs / 1000);
    }
}

// A normal battleground queue is only re-evaluated when something schedules
// an update — a join, a leave, an expired invite; the 5s periodic pass is for
// rated arenas only. Every bot's own join schedules one, which is what pops the
// match. But the pop takes only enough of the queue to reach each side's
// minimum, so any bot of ours that was already queued and not picked stays
// queued until the NEXT update pulls it into the running instance through its
// free slots — and if it was the last bot to join, there is no next update.
// So while any of ours sits queued and uninvited, schedule one ourselves. The
// scheduler de-duplicates, so this costs one queue pass per cadence at most.
void DcBgQueueFillJob::TickNudge(std::uint32_t diff)
{
    _queueNudgeMs += diff;
    if (_queueNudgeMs < kQueueNudgeMs)
        return;
    _queueNudgeMs = 0;

    BattlegroundQueueTypeId const qt = BattlegroundQueueTypeId(_queueTypeId);
    for (Slot const& slot : _slots)
    {
        if (slot.stage != SlotStage::Queued)
            continue;
        Player* const bot = ObjectAccessor::FindPlayer(slot.guid);
        if (!bot || !bot->InBattlegroundQueueForBattlegroundQueueType(qt) ||
            bot->IsInvitedForBattlegroundQueueType(qt))
            continue;

        sBattlegroundMgr->ScheduleQueueUpdate(0, 0, qt, BattlegroundTypeId(_bgTypeId),
                                              BattlegroundBracketId(_bracketId));
        return;
    }
}

void DcBgQueueFillJob::FailSlot(std::size_t i, std::string const& reason)
{
    Slot& slot = _slots[i];

    // Without a start-critical bot the side cannot reach the minimum, so the
    // match cannot pop: say so and let the player keep their normal queue
    // place. Once the match has popped, no bot is critical any more.
    bool const popped = _stage == Stage::InBattle || FindPlayersBattleground() != nullptr;
    if (slot.plan.startCritical && !popped)
    {
        Release(std::string(DcBgQueueFillPlanner::TeamName(slot.plan.team)) + " bot " + reason,
                /*notifyPlayer*/ true);
        return;
    }

    LOG_INFO("playerbots.dungeonclear", "BGQUEUEFILL {} dropping a {} {} slot{}: {}", _id,
             DcBgQueueFillPlanner::TeamName(slot.plan.team), slot.plan.role,
             slot.name.empty() ? "" : " (" + slot.name + ")", reason);

    if (slot.guid)
        DcBgQueueFillManager::Instance().ReleaseBot(
            slot.guid, DcSettings::GetBool(ObjectGuid::Empty, "BgQueueFill.LogoutOnRelease"), false, _id);
    EnterSlotStage(slot, SlotStage::Dropped);
}

bool DcBgQueueFillJob::RedrawSlot(std::size_t i)
{
    Slot& slot = _slots[i];
    if (slot.redrawn)
        return false;

    ObjectGuid const old = slot.guid;
    std::uint8_t const oldClass = slot.plan.classId;
    slot.guid = ObjectGuid::Empty;
    ObjectGuid const guid = ClaimFor(slot);
    if (!guid)
    {
        slot.guid = old;
        return false;
    }

    LOG_INFO("playerbots.dungeonclear", "BGQUEUEFILL {} re-drawing a {} {} as {} instead of {}", _id,
             DcBgQueueFillPlanner::TeamName(slot.plan.team), slot.plan.role,
             DcBotProvisioning::ClassToken(slot.plan.classId), DcBotProvisioning::ClassToken(oldClass));

    // The character being swapped out is logged in (or on its way): hand it
    // back rather than leave it standing in a capital nobody remembers.
    if (old)
        DcBgQueueFillManager::Instance().ReleaseBot(old, true, false, _id);

    slot.guid = guid;
    slot.name.clear();
    slot.queueSent = false;
    slot.bgLeaveSent = false;
    slot.lfgLeaveSent = false;
    slot.strategiesChecked = false;
    slot.invitedMs = 0;
    slot.portRetryMs = 0;
    slot.redrawn = true;
    sRandomPlayerbotMgr.AddPlayerBot(slot.guid, 0);
    EnterSlotStage(slot, SlotStage::LoggingIn);
    return true;
}

// Random Battleground, phase B.
//
// The Random queue pops at the Random template's 10 a side, and only then does
// the core roll the map (CreateNewBattleground -> GetRandomBG, a weighted draw
// with no hook to steer it). The instance is a copy of the CONCRETE template,
// so its maximum is the real map's — 40 for Alterac Valley — while it stays
// filed under the Random type in the Random free-slot queue. So a second pass
// of bots queued into the SAME Random queue is pulled into this instance by
// FillPlayersToBG until it is full. The 2-minute preparation phase covers the
// logins, and the premature-finish check only runs once the gates are open.
void DcBgQueueFillJob::MaybeTopUp(Battleground* bg)
{
    if (!_isRandom || _toppedUp || !bg)
        return;
    _toppedUp = true;

    std::uint32_t const concrete = bg->GetBgTypeID(true);
    std::uint32_t const max = bg->GetMaxPlayersPerTeam();
    if (max <= _maxPerTeam)
    {
        LOG_INFO("playerbots.dungeonclear",
                 "BGQUEUEFILL {} Random rolled {} — the {}v{} pop is already full size", _id,
                 BgName(concrete), _maxPerTeam, _maxPerTeam);
        return;
    }

    DcBgQueueFillPlanner::Request req;
    req.minPerTeam = bg->GetMinPlayersPerTeam();
    req.maxPerTeam = max;
    req.bracketMinLevel = _bracketMinLevel;
    req.bracketMaxLevel = _bracketMaxLevel;
    req.playerLevel = _level;
    req.levelSpread = DcSettings::GetUInt(ObjectGuid::Empty, "BgQueueFill.LevelSpread");
    req.healersPerSide = DcSettings::GetUInt(ObjectGuid::Empty, "BgQueueFill.HealersPerSide");
    req.playerTeam = _team;
    req.seed = _seed ^ 0x9e3779b9u;

    BattlegroundQueueTypeId const qt = BattlegroundQueueTypeId(_queueTypeId);
    for (std::uint8_t t = 0; t < DcBgQueueFillPlanner::kTeams; ++t)
    {
        // Invited counts everyone holding a popup or already inside.
        req.sides[t].humansQueued = bg->GetInvitedCount(TeamId(t));

        std::uint32_t ownPending = 0;
        std::uint32_t ownHealers = 0;
        for (Slot const& slot : _slots)
        {
            if (slot.plan.team != t || slot.stage == SlotStage::Dropped)
                continue;
            if (std::string(slot.plan.role) == "heal")
                ++ownHealers;
            if (slot.stage != SlotStage::Queued)
            {
                ++ownPending;
                continue;
            }
            Player* const bot = ObjectAccessor::FindConnectedPlayer(slot.guid);
            if (bot && !bot->InBattleground() && !bot->IsInvitedForBattlegroundQueueType(qt))
                ++ownPending;  // queued; FillPlayersToBG will pull it in
        }
        req.sides[t].botsInFlight =
            ownPending + DcBgQueueFillManager::Instance().InFlightBots(_queueTypeId, _bracketId, t, this);
        req.sides[t].healerBots = ownHealers;
        req.sides[t].poolFree = DcPoolBots::CountFree(t == DcBgQueueFillPlanner::kTeamAlliance, nullptr);
    }

    DcBgQueueFillPlanner::Result plan = DcBgQueueFillPlanner::Plan(req);
    // The match is already running: a pool that cannot bring a side to the
    // map's minimum is still worth every bot it CAN bring.
    if (plan.kind == DcBgQueueFillPlanner::Kind::Unresolvable)
    {
        req.minPerTeam = 0;
        plan = DcBgQueueFillPlanner::Plan(req);
    }
    if (plan.kind != DcBgQueueFillPlanner::Kind::Ok)
    {
        LOG_INFO("playerbots.dungeonclear", "BGQUEUEFILL {} Random rolled {} — no top-up: {}", _id,
                 BgName(concrete), plan.detail);
        return;
    }

    for (DcBgQueueFillPlanner::Slot const& s : plan.slots)
    {
        Slot slot;
        slot.plan = s;
        _slots.push_back(slot);
    }
    _minPerTeam = req.minPerTeam;
    _maxPerTeam = max;

    LOG_INFO("playerbots.dungeonclear",
             "BGQUEUEFILL {} Random rolled {} — topping up to {}v{}: Alliance +{}, Horde +{} "
             "(shortfall A{} H{})",
             _id, BgName(concrete), max, max, plan.planned[0], plan.planned[1], plan.shortfall[0],
             plan.shortfall[1]);
}

// The single funnel every terminal path goes through. Each bot goes back
// through the manager, which leaves one sitting in somebody else's live match
// where it is and logs it out once that match is over.
void DcBgQueueFillJob::TickReleasing()
{
    bool const logout = DcSettings::GetBool(ObjectGuid::Empty, "BgQueueFill.LogoutOnRelease");

    std::uint32_t queued[DcBgQueueFillPlanner::kTeams] = {0, 0};
    for (Slot const& slot : _slots)
    {
        if (slot.stage == SlotStage::Queued)
            ++queued[slot.plan.team];
        if (!slot.guid || slot.stage == SlotStage::Dropped)
            continue;
        DcBgQueueFillManager::Instance().ReleaseBot(slot.guid, logout, _forceRelease, _id);
    }

    DcBgQueueFillManager::Instance().RememberBots(_playerGuid, BotGuids());

    // Graceful degradation, said out loud: the player is still in the real
    // queue, which is a slower version of what they asked for, not a broken one.
    if (_notifyPlayer)
        if (Player* const player = FindPlayer())
            ChatHandler(player->GetSession())
                .PSendSysMessage("|cffff8000[Battleground]|r Could not fill your battleground this "
                                 "time — you are still queued normally.");

    LOG_INFO("playerbots.dungeonclear",
             "BGQUEUEFILL {} released after {}s — {} (bots queued: Alliance {}, Horde {})", _id,
             _totalMs / 1000, _releaseReason.empty() ? "done" : _releaseReason, queued[0], queued[1]);
    EnterStage(Stage::Done);
}

std::string DcBgQueueFillJob::StatusLine() const
{
    std::uint32_t planned[DcBgQueueFillPlanner::kTeams] = {0, 0};
    std::uint32_t queued[DcBgQueueFillPlanner::kTeams] = {0, 0};
    std::uint32_t dropped = 0;
    for (Slot const& slot : _slots)
    {
        if (slot.stage == SlotStage::Dropped)
        {
            ++dropped;
            continue;
        }
        ++planned[slot.plan.team];
        if (slot.stage == SlotStage::Queued)
            ++queued[slot.plan.team];
    }

    std::string out = _id + " " + _playerName + " " + BgName(_bgTypeId) + " [" + StageName(_stage) +
                      "] " + std::to_string(_totalMs / 1000) + "s";
    if (!_slots.empty())
    {
        out += " — Alliance " + std::to_string(queued[0]) + "/" + std::to_string(planned[0]) +
               ", Horde " + std::to_string(queued[1]) + "/" + std::to_string(planned[1]) +
               " bots queued (target " + std::to_string(_maxPerTeam) + "v" +
               std::to_string(_maxPerTeam) + ")";
        std::uint32_t const settingUp = static_cast<std::uint32_t>(_slots.size()) - dropped -
                                        queued[0] - queued[1];
        if (settingUp)
            out += ", " + std::to_string(settingUp) + " setting up";
        if (dropped)
            out += ", " + std::to_string(dropped) + " dropped";
    }
    if (_bgInstanceId)
        out += ", instance " + std::to_string(_bgInstanceId);
    return out;
}
