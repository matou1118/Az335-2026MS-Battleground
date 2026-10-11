/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DungeonQueueFill/DcDungeonQueueFillJob.h"

#include <algorithm>
#include <ctime>
#include <functional>
#include <utility>

#include "Group.h"
#include "LFGMgr.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Opcodes.h"
#include "Player.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include "PlayerbotAI.h"
#include "PlayerbotFactory.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"

#include "Chat.h"
#include "DBCStores.h"
#include "Map.h"

#include "DcStrategyGate.h"

#include "Ai/Dungeon/DungeonClear/Data/DungeonWingRegistry.h"
#include "Ai/Dungeon/DungeonClear/Settings/DcSettings.h"
#include "Ai/Dungeon/DungeonClear/Util/DcRunWing.h"
#include "DungeonQueueFill/DcDungeonQueueFillManager.h"
#include "TestRun/DcTestDungeonRegistry.h"
#include "TestRun/DcTestGearTiers.h"
#include "Util/DcBotProvisioning.h"
#include "Util/DcProvisionBudget.h"
#include "Util/DcDungeonAccess.h"
#include "Util/DcPoolBots.h"

using namespace lfg;

namespace
{
    std::string MakeFillId()
    {
        static std::uint32_t counter = 0;
        std::time_t const now = std::time(nullptr);
        std::tm tmBuf{};
        localtime_r(&now, &tmBuf);
        char buf[32];
        std::strftime(buf, sizeof(buf), "qf-%Y%m%d-%H%M%S", &tmBuf);
        return std::string(buf) + "-" + std::to_string(++counter);
    }

    std::uint32_t SetupTimeoutMs()
    {
        return DcSettings::GetUInt(ObjectGuid::Empty, "DungeonQueueFill.SetupTimeoutSec") * 1000;
    }

    std::string RoleMaskText(std::uint8_t mask)
    {
        std::string out;
        if (mask & DcDungeonQueueFillPlanner::kRoleTank)
            out += "tank";
        if (mask & DcDungeonQueueFillPlanner::kRoleHealer)
            out += out.empty() ? "heal" : "+heal";
        if (mask & DcDungeonQueueFillPlanner::kRoleDamage)
            out += out.empty() ? "dps" : "+dps";
        return out.empty() ? "none" : out;
    }
}

char const* DcDungeonQueueFillJob::StageName(Stage s)
{
    switch (s)
    {
        case Stage::Observed:     return "observed";
        case Stage::Planning:     return "planning";
        case Stage::Claiming:     return "claiming";
        case Stage::LoggingIn:    return "logging_in";
        case Stage::Evicting:     return "evicting";
        case Stage::Provisioning: return "provisioning";
        case Stage::Sanitizing:   return "sanitizing";
        case Stage::Queueing:     return "queueing";
        case Stage::Waiting:      return "waiting";
        case Stage::Formed:       return "formed";
        case Stage::Releasing:    return "releasing";
        case Stage::Done:         return "done";
    }
    return "?";
}

std::unique_ptr<DcDungeonQueueFillJob> DcDungeonQueueFillJob::Observe(
    Player* player, std::uint8_t roles, std::set<std::uint32_t> const& dungeons)
{
    if (!player)
        return nullptr;

    std::unique_ptr<DcDungeonQueueFillJob> job(new DcDungeonQueueFillJob());
    job->_id = MakeFillId();
    job->_playerGuid = player->GetGUID();
    job->_playerName = player->GetName();
    job->_level = player->GetLevel();
    job->_isAlliance = player->GetTeamId(true) == TEAM_ALLIANCE;
    job->_requestedRoles = roles;
    job->_requestedDungeonCount = dungeons.size();

    Group const* grp = player->GetGroup();
    job->_groupGuid = grp ? grp->GetGUID() : ObjectGuid::Empty;
    job->_lfgGuid = grp ? grp->GetGUID() : player->GetGUID();

    LOG_INFO("playerbots.dungeonclear",
             "QUEUEFILL {} observed {} (level {}, {}) queueing for {} dungeon(s) as {}",
             job->_id, job->_playerName, job->_level, job->_isAlliance ? "Alliance" : "Horde",
             dungeons.size(), RoleMaskText(roles));

    return job;
}

Player* DcDungeonQueueFillJob::FindPlayer() const
{
    // FindConnectedPlayer, not FindPlayer: a player mid-teleport is on a
    // loading screen — not in world, but very much still logged in — and
    // reading that as a logout would drop the fill for no reason.
    return ObjectAccessor::FindConnectedPlayer(_playerGuid);
}

void DcDungeonQueueFillJob::EnterStage(Stage s)
{
    _stage = s;
    _stageMs = 0;
}

void DcDungeonQueueFillJob::Release(std::string const& reason, bool notifyPlayer)
{
    if (_stage == Stage::Releasing || _stage == Stage::Done)
        return;
    _releaseReason = reason;
    _notifyPlayer = notifyPlayer;
    EnterStage(Stage::Releasing);
}

void DcDungeonQueueFillJob::Tick(std::uint32_t diff)
{
    if (_stage == Stage::Done)
        return;

    _stageMs += diff;
    _totalMs += diff;

    // The player is the whole point of the fill: without them there is no
    // party to complete and no proposal our bots could legally be in.
    if (_stage != Stage::Releasing && !FindPlayer())
    {
        Release("player logged out");
        return;
    }

    // Setup can take a few seconds, and the player is free to change their
    // mind inside it. Watch for that in every stage between the plan and the
    // match — Waiting does its own, richer version of this, and Formed is past
    // the point either applies.
    if (_stage >= Stage::Claiming && _stage <= Stage::Queueing)
    {
        Player* const player = FindPlayer();
        if (Group const* grp = player->GetGroup())
            if (grp->isLFGGroup())
            {
                Release("player matched with other players before the fill was ready");
                return;
            }
        if (sLFGMgr->GetState(_lfgGuid) == lfg::LFG_STATE_NONE)
        {
            Release("player left the queue");
            return;
        }
    }

    switch (_stage)
    {
        case Stage::Observed:     TickObserved();     break;
        case Stage::Planning:     TickPlanning();     break;
        case Stage::Claiming:     TickClaiming();     break;
        case Stage::LoggingIn:    TickLoggingIn();    break;
        case Stage::Evicting:     TickEvicting();     break;
        case Stage::Provisioning: TickProvisioning(); break;
        case Stage::Sanitizing:   TickSanitizing();   break;
        case Stage::Queueing:     TickQueueing();     break;
        case Stage::Waiting:      TickWaiting();      break;
        case Stage::Formed:       TickFormed();       break;
        case Stage::Releasing:    TickReleasing();    break;
        case Stage::Done:         break;
    }
}

// Wait for the core to CONFIRM the queue before touching a single bot.
//
// This is what makes the observer safe. The intent hook fires at the very top
// of LFGMgr::JoinLfg, before RBAC, deserter, dungeon-cooldown, lock-map,
// battleground and party-size validation have run — so the arguments it carries
// are a request, not a fact. By the time the state reaches LFG_STATE_QUEUED
// every one of those checks has passed and the dungeon set has been filtered to
// what this party may actually enter. Anything else inside the window (the join
// refused, the player cancelling, a rolecheck failing) drops the intent
// silently: no bot was spawned, so there is nothing to undo.
void DcDungeonQueueFillJob::TickObserved()
{
    LfgState const state = sLFGMgr->GetState(_lfgGuid);
    if (state == LFG_STATE_QUEUED)
    {
        EnterStage(Stage::Planning);
        return;
    }

    // A group queue passes through LFG_STATE_ROLECHECK first, so that is a
    // legitimate intermediate state and not a reason to give up.
    if (state == LFG_STATE_ROLECHECK)
        return;

    if (state != LFG_STATE_NONE)
    {
        // PROPOSAL / DUNGEON / FINISHED_DUNGEON / RAIDBROWSER: the player is
        // already past the point a fill could help.
        Release(std::string("queue did not settle into QUEUED (state ") +
                std::to_string(static_cast<int>(state)) + ")");
        return;
    }

    if (_stageMs >= SetupTimeoutMs())
        Release("queue never reached QUEUED", /*notifyPlayer*/ true);
}

std::vector<DcDungeonQueueFillPlanner::Human> DcDungeonQueueFillJob::ReadHumans() const
{
    std::vector<DcDungeonQueueFillPlanner::Human> humans;

    Player* const player = FindPlayer();
    Group const* const grp = player ? player->GetGroup() : nullptr;
    if (!grp)
    {
        DcDungeonQueueFillPlanner::Human h;
        h.roleMask = sLFGMgr->GetRoles(_playerGuid);
        humans.push_back(h);
        return humans;
    }

    for (GroupReference const* itr = grp->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* const member = itr->GetSource();
        if (!member)
            continue;
        DcDungeonQueueFillPlanner::Human h;
        h.roleMask = sLFGMgr->GetRoles(member->GetGUID());
        humans.push_back(h);
    }
    return humans;
}

std::uint32_t DcDungeonQueueFillJob::ReadDungeonExpansion() const
{
    // Default to Wrath so an id the store does not know cannot silently ban
    // the death-knight rows from a WotLK fill; the roster is a preference,
    // and every other gate here is the core's.
    std::uint32_t lowest = DcTestDungeonRegistry::kExpansionWrath;
    for (std::uint32_t const dungeon : _dungeons)
    {
        // The low 24 bits are the dungeon id; the top byte is the LFG type the
        // client packs alongside it.
        LFGDungeonEntry const* const entry = sLFGDungeonStore.LookupEntry(dungeon & 0x00FFFFFF);
        if (entry && entry->ExpansionLevel < lowest)
            lowest = entry->ExpansionLevel;
    }
    return lowest;
}

// Every map the queue could drop this party into, resolved once.
//
// The LFG lock map is not only about levels and gear: LFGMgr::InitializeLockedDungeons
// walks the destination's `dungeon_access_requirements` rows too, and stamps
// LFG_LOCKSTATUS_QUEST_NOT_COMPLETED on any dungeon whose attunement quest the
// character has not been rewarded. A pool bot has run nothing, so it is missing
// every one of them. On WotLK content that is Pit of Saron (map 658, quest
// 24499/24511 "Echoes of Tortured Souls") and Halls of Reflection (668,
// 24710/24712 "Deliverance from the Pit") — the Forge of Souls (632) has no
// quest row, which is exactly why fills for it always worked and fills for its
// two successors never did.
//
// Read through GetLFGDungeon, never off the DBC row: LoadLFGDungeons rewrites
// `map` from the entrance areatrigger for the entries whose DBC map is the
// outdoor one, and the areatrigger's target is the map the access rows key on.
std::vector<std::pair<std::uint32_t, std::uint8_t>>
DcDungeonQueueFillJob::ReadAccessTargets() const
{
    std::vector<std::pair<std::uint32_t, std::uint8_t>> targets;

    auto add = [&targets](LFGDungeonData const* data)
    {
        if (!data || !data->map)
            return;
        std::pair<std::uint32_t, std::uint8_t> const target{data->map,
                                                            std::uint8_t(data->difficulty)};
        if (std::find(targets.begin(), targets.end(), target) == targets.end())
            targets.push_back(target);
    };

    for (std::uint32_t const dungeon : _dungeons)
    {
        // The low 24 bits are the dungeon id; the top byte is the LFG type the
        // client packs alongside it.
        LFGDungeonData const* const data = sLFGMgr->GetLFGDungeon(dungeon & 0x00FFFFFF);
        if (!data)
            continue;

        if (data->type != LFG_TYPE_RANDOM)
        {
            add(data);
            continue;
        }

        // LFGMgr::GetDungeonsByRandom is private, so rebuild its answer the way
        // LoadLFGDungeons built the cache: every non-random dungeon sharing the
        // random entry's group id.
        for (std::uint32_t i = 0; i < sLFGDungeonStore.GetNumRows(); ++i)
        {
            LFGDungeonEntry const* const row = sLFGDungeonStore.LookupEntry(i);
            if (!row)
                continue;
            LFGDungeonData const* const member = sLFGMgr->GetLFGDungeon(row->ID);
            if (member && member->type != LFG_TYPE_RANDOM && member->group == data->group)
                add(member);
        }
    }

    return targets;
}

// Read back the AUTHORITATIVE, post-validation queue data — never the hook's
// raw arguments.
//
// GetSelectedDungeons is what LFGMgr itself will match on: for a random queue
// JoinLfg deliberately stores the single rDungeonId there, which is exactly the
// id the bots must queue for, and for a specific queue it is the list already
// filtered to what this party can enter. Copying it verbatim is the only way
// the bots land in the same queue bucket as the player.
void DcDungeonQueueFillJob::TickPlanning()
{
    _dungeons = sLFGMgr->GetSelectedDungeons(_playerGuid);
    if (_dungeons.empty())
    {
        Release("no selected dungeons after validation");
        return;
    }

    _expansion = ReadDungeonExpansion();
    _accessTargets = ReadAccessTargets();

    std::vector<DcDungeonQueueFillPlanner::Human> const humans = ReadHumans();

    // Seed the class draw off the job id's counter suffix and the level so two
    // fills in the same second do not field identical parties, while a single
    // fill stays reproducible from its log line. Every later draw in this job
    // — the pool character per slot, the order substitutions are tried in —
    // runs off the same stream, so the whole fill replays from this one number.
    _seed = static_cast<std::uint32_t>(std::hash<std::string>{}(_id)) ^ (_level * 2654435761u);
    _drawState = _seed ^ 0x2545f491u;

    // What this player last ran with. An independent draw is not enough on its
    // own: the tank pool is three classes deep before Wrath, so back-to-back
    // dungeons would hand the player the same tank a third of the time. Copy
    // it out BEFORE the claim overwrites it.
    if (DcDungeonQueueFillManager::RecentParty const* last =
            DcDungeonQueueFillManager::Instance().RecentPartyFor(_playerGuid))
    {
        _avoidClasses = last->classIds;
        _avoidGuids = last->guids;
    }

    DcDungeonQueueFillPlanner::Result const plan =
        DcDungeonQueueFillPlanner::Plan(humans, _level, _expansion, _seed, _avoidClasses);
    if (plan.kind != DcDungeonQueueFillPlanner::Kind::Ok)
    {
        Release(plan.detail);
        return;
    }

    _slots.clear();
    _slots.reserve(plan.slots.size());
    std::string wanted;
    for (DcDungeonQueueFillPlanner::Slot const& s : plan.slots)
    {
        Slot slot;
        slot.plan = s;
        _slots.push_back(slot);
        if (!wanted.empty())
            wanted += ", ";
        wanted += std::string(s.role) + "(class " + std::to_string(s.classId) + " " + s.specName + ")";
    }

    std::string ids;
    for (std::uint32_t d : _dungeons)
        ids += (ids.empty() ? "" : "/") + std::to_string(d);

    // The role the humans were GIVEN, not the roles they ticked: a player who
    // queued as "tank or dps" plays one of them, and which one decides the
    // whole deficit below. Print it or the next short-looking fill is a mystery.
    std::string played;
    for (std::uint8_t const mask : plan.humanRoles)
        played += (played.empty() ? "" : ", ") + RoleMaskText(mask);

    // Name what the draw was steering AWAY from as well as what it landed on:
    // "why did I get the same tank again" is answered by seeing whether the
    // previous party was known at all when the plan was made.
    std::string avoided;
    for (std::uint8_t const classId : _avoidClasses)
    {
        if (!avoided.empty())
            avoided += "/";
        avoided += DcBotProvisioning::ClassToken(classId);
    }

    LOG_INFO("playerbots.dungeonclear",
             "QUEUEFILL {} plan: {} human(s) playing {}, {} dungeon(s) [{}], needs {} bot(s) — {}"
             " (seed {}, avoiding last party {})",
             _id, humans.size(), played, _dungeons.size(), ids, _slots.size(), wanted, _seed,
             avoided.empty() ? "none" : avoided);

    // Gear ceiling, resolved once here rather than per bot: a conf reloaded
    // mid-fill must not give the tank one ceiling and the healer another.
    DcTestGearTiers::Spec spec;
    spec.ilvl = static_cast<std::int32_t>(
        DcSettings::GetUInt(ObjectGuid::Empty, "DungeonQueueFill.GearIlvl"));
    spec.quality = DcSettings::GetUInt(ObjectGuid::Empty, "DungeonQueueFill.GearQuality");
    DcTestGearTiers::Resolved const gear = DcTestGearTiers::Resolve(
        spec, sPlayerbotAIConfig.autoGearScoreLimit, sPlayerbotAIConfig.autoGearQualityLimit);
    _gearIlvl = gear.ilvl;
    _gearQuality = gear.quality;
    _gearScoreLimit =
        gear.ilvl == 0 ? 0 : PlayerbotFactory::CalcMixedGearScore(gear.ilvl, gear.quality);

    EnterStage(Stage::Claiming);
}

bool DcDungeonQueueFillJob::HoldsBot(ObjectGuid guid) const
{
    for (Slot const& slot : _slots)
        if (slot.guid == guid)
            return true;
    return false;
}

std::vector<ObjectGuid> DcDungeonQueueFillJob::BotGuids() const
{
    std::vector<ObjectGuid> out;
    out.reserve(_slots.size());
    for (Slot const& slot : _slots)
        if (slot.guid)
            out.push_back(slot.guid);
    return out;
}

// Draw one free offline addclass-pool character of `classId` for the fill's
// faction — the `addclass` command's own selection rules, plus the two
// cross-subsystem exclusions.
//
// The LFG queue is partitioned by TeamId (LFGMgr::GetQueue), so a Horde bot
// could never match an Alliance player's proposal however well provisioned: the
// faction is not a nicety here, it is the whole reason the fill works.
ObjectGuid DcDungeonQueueFillJob::ClaimPoolCharacter(std::uint8_t classId)
{
    return DcPoolBots::ClaimPoolCharacter(_isAlliance, classId, _drawState, _avoidGuids,
                                          [this](ObjectGuid guid) { return HoldsBot(guid); });
}

// The role pool in a seed-dependent order. Substitutions walked the pool in
// declaration order, which meant a class with no free character always fell
// back to the same neighbour — the determinism this feature is trying to shed,
// one step removed.
std::vector<DcTestComp::Slot> DcDungeonQueueFillJob::ShuffledRolePool(
    char const* role, DcTestComp::Roster roster)
{
    std::vector<DcTestComp::Slot> pool = DcTestComp::RolePool(role, roster);
    // Fisher-Yates off the fill's own stream — never std::shuffle, whose
    // generator would be another source of un-replayable randomness.
    for (std::size_t i = pool.size(); i > 1; --i)
    {
        std::size_t const j = DcDungeonQueueFillPlanner::NextRand(_drawState) % i;
        std::swap(pool[i - 1], pool[j]);
    }
    return pool;
}

bool DcDungeonQueueFillJob::RedrawSlot(std::size_t i)
{
    Slot& slot = _slots[i];
    if (slot.redrawn)
        return false;

    DcTestComp::Roster const roster = DcDungeonQueueFillPlanner::RosterFor(_level, _expansion);
    for (DcTestComp::Slot const& alt : ShuffledRolePool(slot.plan.role, roster))
    {
        if (alt.classId == slot.plan.classId)
            continue;
        ObjectGuid const guid = ClaimPoolCharacter(alt.classId);
        if (!guid)
            continue;

        LOG_INFO("playerbots.dungeonclear", "QUEUEFILL {} re-drawing {} as {} instead of {}",
                 _id, slot.plan.role, DcBotProvisioning::ClassToken(alt.classId),
                 DcBotProvisioning::ClassToken(slot.plan.classId));

        slot.plan.classId = alt.classId;
        slot.plan.specName = alt.specName;
        slot.plan.fallbackSpec = alt.fallbackSpec;
        slot.guid = guid;
        slot.name.clear();
        slot.relocated = false;
        slot.provisioned = false;
        slot.sanitized = false;
        slot.queueSent = false;
        slot.queued = false;
        slot.redrawn = true;
        sRandomPlayerbotMgr.AddPlayerBot(slot.guid, 0);
        RememberRoster();
        return true;
    }
    return false;
}

// Record what this fill is fielding as the player's last party, so their NEXT
// fill draws around it. Recorded when the roster settles rather than when the
// dungeon ends, because a player who cancels and immediately re-queues is
// exactly the back-to-back case this exists for; re-recorded after a redraw so
// the memory names the bots that actually went.
void DcDungeonQueueFillJob::RememberRoster()
{
    std::vector<std::uint8_t> classIds;
    std::vector<ObjectGuid> guids;
    classIds.reserve(_slots.size());
    guids.reserve(_slots.size());
    for (Slot const& slot : _slots)
    {
        classIds.push_back(slot.plan.classId);
        guids.push_back(slot.guid);
    }
    DcDungeonQueueFillManager::Instance().RememberParty(_playerGuid, std::move(classIds),
                                                        std::move(guids));
}

// Claim one pool character per planned slot, then start every login.
//
// Substitution rather than failure when the drawn class has no free character:
// the class draw is cosmetic (it decides which shape of DPS shows up), so
// aborting a fill over an un-seeded warlock would be refusing to do the job for
// no reason. Only a role with no fillable class at all is fatal, and it is
// fatal with the operator-facing wording the harness already uses, because the
// answer is the same: seed the pool.
void DcDungeonQueueFillJob::TickClaiming()
{
    DcTestComp::Roster const roster = DcDungeonQueueFillPlanner::RosterFor(_level, _expansion);
    std::set<std::uint8_t> used;
    for (Slot const& slot : _slots)
        used.insert(slot.plan.classId);

    for (Slot& slot : _slots)
    {
        slot.guid = ClaimPoolCharacter(slot.plan.classId);
        if (slot.guid)
            continue;

        for (DcTestComp::Slot const& alt : ShuffledRolePool(slot.plan.role, roster))
        {
            if (alt.classId == slot.plan.classId || used.count(alt.classId))
                continue;
            ObjectGuid const guid = ClaimPoolCharacter(alt.classId);
            if (!guid)
                continue;
            used.erase(slot.plan.classId);
            used.insert(alt.classId);
            slot.plan.classId = alt.classId;
            slot.plan.specName = alt.specName;
            slot.plan.fallbackSpec = alt.fallbackSpec;
            slot.guid = guid;
            break;
        }

        if (!slot.guid)
        {
            Release(std::string("no available ") + slot.plan.role +
                    " class in the addclass pool — pre-seed with `.playerbots addclass`",
                    /*notifyPlayer*/ true);
            return;
        }
    }

    RememberRoster();

    // MASTERLESS login, exactly as DcTestRunJob does and for the same
    // ownership-gate reason: AddPlayerBot's gate clears only for same-account /
    // same-guild / addclass-pool / linked characters, and masterAccountId 0
    // takes the isRndbot branch that skips it. The party therefore lands in
    // sRandomPlayerbotMgr, which is also where the teardown looks for it.
    //
    // Crucially it does NOT make these bots random-bot-rotation members, so
    // nothing periodically re-Randomizes or relocates them mid-fill.
    for (Slot const& slot : _slots)
        sRandomPlayerbotMgr.AddPlayerBot(slot.guid, 0);

    EnterStage(Stage::LoggingIn);
}

void DcDungeonQueueFillJob::TickLoggingIn()
{
    bool allIn = true;
    for (Slot& slot : _slots)
    {
        Player* const bot = ObjectAccessor::FindPlayer(slot.guid);
        if (!bot || !bot->IsInWorld() || !GET_PLAYERBOT_AI(bot))
        {
            allIn = false;
            continue;
        }
        if (slot.name.empty())
            slot.name = bot->GetName();
    }

    if (allIn)
    {
        EnterStage(Stage::Evicting);
        return;
    }

    if (_stageMs >= SetupTimeoutMs())
        Release("bots did not finish logging in (pool contention, MaxAddedBots cap, "
                "or a login failure — see the server log)", /*notifyPlayer*/ true);
}

// Put every bot down in its faction's capital before anything else touches it.
//
// A pool character logs in exactly where it was saved, and for this pool that is
// very often INSIDE a dungeon — still standing in the copy the last run left it
// in. Three separate things go wrong from there, and one trip out to the capital
// defuses all three:
//
//   * m_InstanceValid. Group::_homebindIfInstance clears it for any member
//     pulled out of a group while standing in a dungeon, which is exactly what
//     TickSanitizing's `grp->RemoveMember` does to every recycled bot that
//     logged in inside one. The only place that ever sets it back is
//     WorldSession::HandleMoveWorldportAckOpcode, and only when the DESTINATION
//     is not instanced. Carry it into the run and Player::UpdateHomebindTime
//     repops the bot at the entrance graveyard 60 seconds after the party
//     zones in — the fill hands the player a member who walks out of the
//     dungeon on a timer.
//
//   * The leftover temp bind. That same worldport ack is what calls
//     PlayerUnbindInstance for the map being left, so a bot that never leaves
//     keeps a bind that InstanceSaveMgr::PlayerGetDestinationInstanceId may
//     answer a later entry with.
//
//   * The open world itself. A bot dropped anywhere it can be attacked can be
//     in combat or dead when the proposal lands, and LfgAcceptAction answers
//     DENY for either — which does not just cost that bot, it takes the whole
//     party's proposal down and stamps the bot with a 150s cooldown (see
//     RequeueDroppedBots). A capital is the one place on the map where neither
//     can happen.
//
// So this is unconditional, not a rescue: every bot goes to the capital, not
// only the ones that happen to be in a dungeon. A bot already standing there is
// teleported anyway — Player::TeleportTo's near branch makes that nearly free —
// because "is it close enough" is a judgement call that buys nothing.
//
// The wait is on IsBeingTeleported, not on the destination coordinates: a far
// teleport is a two-step handshake (the semaphore here, HandleMoveWorldportAck
// on the bot's own AI tick), and the bot is neither in the old map nor the new
// one in between.
void DcDungeonQueueFillJob::TickEvicting()
{
    bool allPlaced = true;
    for (Slot& slot : _slots)
    {
        if (slot.relocated)
            continue;

        Player* const bot = ObjectAccessor::FindPlayer(slot.guid);
        if (!bot || !bot->IsInWorld() || !GET_PLAYERBOT_AI(bot))
        {
            allPlaced = false;
            continue;  // mid-worldport, or still arriving — the stage timeout bounds it
        }

        if (DcPoolBots::EvictToCapital(bot, _isAlliance, "QUEUEFILL", _id))
            slot.relocated = true;
        else
            allPlaced = false;
    }

    if (allPlaced)
    {
        LOG_INFO("playerbots.dungeonclear", "QUEUEFILL {} placed {} bot(s) in {}", _id,
                 _slots.size(), DcPoolBots::CapitalName(_isAlliance));
        EnterStage(Stage::Provisioning);
        return;
    }

    if (_stageMs >= SetupTimeoutMs())
        Release("bots could not be moved out to a capital city (a bot stuck mid-teleport, or a "
                "world DB with no Stormwind/Orgrimmar game_tele row)",
                /*notifyPlayer*/ true);
}

// One factory roll per world tick, under the realm-wide ration shared with the
// `.dc test` harness. Four bots is therefore four ticks plus their logins,
// which is the "seconds, not minutes" the feature promises.
void DcDungeonQueueFillJob::TickProvisioning()
{
    if (_stageMs >= SetupTimeoutMs())
    {
        Release("provisioning timed out", /*notifyPlayer*/ true);
        return;
    }

    if (_provisionIdx >= _slots.size())
    {
        EnterStage(Stage::Sanitizing);
        return;
    }

    Slot& slot = _slots[_provisionIdx];
    Player* const bot = ObjectAccessor::FindPlayer(slot.guid);
    PlayerbotAI* const botAI = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
    if (!bot || !bot->IsInWorld() || !botAI)
        return;  // transient (mid world-add) — the stage timeout bounds the wait

    std::string pickedSpec;
    int const specNo = DcBotProvisioning::ResolveSpecNo(slot.plan.classId, slot.plan.specName,
                                                        slot.plan.fallbackSpec, &pickedSpec);
    if (specNo < 0 && DcDungeonQueueFillPlanner::MaskForRole(slot.plan.role) !=
                          DcDungeonQueueFillPlanner::kRoleDamage)
    {
        // A random-rolled tank or healer would be handed to a real player as
        // THEIR tank or healer. Swap the class rather than ship it.
        if (RedrawSlot(_provisionIdx))
        {
            EnterStage(Stage::LoggingIn);
            return;
        }
        Release(std::string("no premade spec template matching '") + slot.plan.specName +
                "' for " + DcBotProvisioning::ClassToken(slot.plan.classId) +
                " (AiPlayerbot.PremadeSpecName.*) — cannot force the " + slot.plan.role,
                /*notifyPlayer*/ true);
        return;
    }

    if (!DcProvisionBudget::Take())
        return;

    DcBotProvisioning::Roll(bot, _level, _gearQuality, _gearScoreLimit, specNo);

    LOG_INFO("playerbots.dungeonclear",
             "QUEUEFILL {} provisioned {} ({} {} {}, level {}, gear <= ilvl {})", _id,
             bot->GetName(), DcBotProvisioning::ClassToken(slot.plan.classId),
             specNo >= 0 ? pickedSpec : std::string("(random)"), slot.plan.role, bot->GetLevel(),
             _gearIlvl ? std::to_string(_gearIlvl) : std::string("unlimited"));

    slot.provisioned = true;
    ++_provisionIdx;
}

// The step with the sharpest teeth.
//
// A recycled pool character carries whatever the last thing it did left on it,
// and two of those silently refuse the queue rather than failing loudly:
// LFG_SPELL_DUNGEON_COOLDOWN (JoinLfg -> LFG_JOIN_RANDOM_COOLDOWN for any
// random queue) and LFG_SPELL_DUNGEON_DESERTER (LFG_JOIN_DESERTER for any
// queue at all). This is the single likeliest cause of "the fill just doesn't
// happen sometimes", so it is dealt with before a packet is sent, not
// diagnosed afterwards.
//
// The rest is what LfgAcceptAction demands at the LAST moment: it DECLINES a
// proposal outright if the bot is in combat or dead, which would fail the match
// after the player has already seen the popup.
void DcDungeonQueueFillJob::TickSanitizing()
{
    if (_stageMs >= SetupTimeoutMs())
    {
        Release("sanitizing timed out", /*notifyPlayer*/ true);
        return;
    }

    bool allReady = true;
    for (Slot& slot : _slots)
    {
        Player* const bot = ObjectAccessor::FindPlayer(slot.guid);
        if (!bot || !bot->IsInWorld() || !GET_PLAYERBOT_AI(bot))
        {
            allReady = false;
            continue;
        }

        bot->RemoveAurasDueToSpell(lfg::LFG_SPELL_DUNGEON_DESERTER);
        bot->RemoveAurasDueToSpell(lfg::LFG_SPELL_DUNGEON_COOLDOWN);

        // A death knight who never finished the Ebon Hold chain is locked out
        // of every dungeon in the finder, which on this server is every death
        // knight in the pool. Credit the chain the way the deserter aura is
        // stripped: it is the one thing standing between the class and the
        // queue, and the refusal it causes is silent (JoinLfg answers the
        // BOT's session, so the fill just sits in queueing until it times out).
        if (DcPoolBots::CreditDeathKnightChain(bot))
            LOG_INFO("playerbots.dungeonclear",
                     "QUEUEFILL {} credited {} with the death-knight starting chain (LFG lock)",
                     _id, bot->GetName());

        // The same lock one layer out, and for the same reason: the pool
        // character has never run the dungeon it is being queued for, so it
        // satisfies none of that map's access rows either. Satisfy them the
        // way DcTestRunJob does before it teleports a party in — the refusal
        // is just as silent here, and it cost two Pit of Saron fills their
        // whole setup budget before anyone could see why.
        if (!slot.attuned)
        {
            for (auto const& [mapId, difficulty] : _accessTargets)
                DcDungeonAccess::GrantEntry(bot, mapId, Difficulty(difficulty));
            slot.attuned = true;
        }

        // Out of any leftover LFG queue and group, alive, out of combat.
        if (!DcPoolBots::SanitizeCommon(bot, slot.sanitized))
        {
            allReady = false;
            continue;
        }

        // Refresh the bot's own lock map so GetCompatibleDungeons judges it on
        // the level and gear it has NOW. Randomize's GiveLevel fires the level
        // hook that does this, but only when the level actually changed — a
        // pool character already at the player's level would keep a stale map.
        sLFGMgr->InitializeLockedDungeons(bot, nullptr);
    }

    if (allReady)
        EnterStage(Stage::Queueing);
}

// Put the bots in the queue the same way a client would.
//
// CMSG_LFG_JOIN through the session, NEVER sLFGMgr->JoinLfg directly: playerbots
// says in as many words that JoinLfg is not threadsafe, and the packet path is
// also the only one that runs the handler's own slot masking and
// UpdateLFGChannel. The wire layout is LfgJoinAction::JoinLFG's, which is the
// layout WorldPackets::LFG::LFGJoin::Read expects.
//
// The dungeon set is the player's, copied verbatim — for a random queue that is
// the single rDungeonId, which is what puts the bots in the same queue bucket.
void DcDungeonQueueFillJob::TickQueueing()
{
    bool allQueued = true;
    for (std::size_t i = 0; i < _slots.size(); ++i)
    {
        Slot& slot = _slots[i];
        if (slot.queued)
            continue;

        Player* const bot = ObjectAccessor::FindPlayer(slot.guid);
        if (!bot || !bot->IsInWorld())
        {
            allQueued = false;
            continue;
        }

        // Only OUR packet counts. Stock playerbots gives a random bot the
        // `lfg` strategy, whose "random" tick can put it in the queue for its
        // own choice of dungeons in the window between Sanitizing leaving the
        // queue and this packet going out — and a bot queued for the wrong
        // dungeon set will never match our player. Sending ours anyway is
        // harmless: JoinLfg's LFG_STATE_QUEUED case removes the old entry and
        // re-adds with the dungeons we asked for.
        if (slot.queueSent && sLFGMgr->GetState(slot.guid) == lfg::LFG_STATE_QUEUED)
        {
            slot.queued = true;
            continue;
        }

        if (!slot.queueSent)
        {
            WorldPacket* data = new WorldPacket(CMSG_LFG_JOIN);
            *data << static_cast<uint32>(slot.plan.roleMask);
            *data << static_cast<bool>(false);   // NoPartialClear
            *data << static_cast<bool>(false);   // Achievements
            *data << static_cast<uint8>(_dungeons.size());
            for (std::uint32_t const dungeon : _dungeons)
                *data << static_cast<uint32>(dungeon);
            *data << static_cast<uint8>(3) << static_cast<uint8>(0)
                  << static_cast<uint8>(0) << static_cast<uint8>(0);   // Needs
            *data << std::to_string(GET_PLAYERBOT_AI(bot)
                                        ? GET_PLAYERBOT_AI(bot)->GetEquipGearScore(bot)
                                        : 0);                          // comment
            bot->GetSession()->QueuePacket(data);
            slot.queueSent = true;
        }
        allQueued = false;
    }

    if (allQueued)
    {
        LOG_INFO("playerbots.dungeonclear", "QUEUEFILL {} all {} bot(s) queued for {}", _id,
                 _slots.size(), _playerName);
        EnterStage(Stage::Waiting);
        return;
    }

    if (_stageMs >= SetupTimeoutMs())
    {
        // A bot that will not queue has usually had every dungeon in the set
        // killed by its own lock map. One swap for a different draw, then give
        // up gracefully — the player keeps their place in the real queue.
        for (std::size_t i = 0; i < _slots.size(); ++i)
            if (!_slots[i].queued && RedrawSlot(i))
            {
                EnterStage(Stage::LoggingIn);
                return;
            }
        Release("bots would not enter the queue (their own lock maps refused every dungeon)",
                /*notifyPlayer*/ true);
    }
}

bool DcDungeonQueueFillJob::BotIsSomebodyElses(Player* bot) const
{
    Group const* const grp = bot ? bot->GetGroup() : nullptr;
    if (!grp || !grp->isLFGGroup())
        return false;
    return !_formedGroupGuid || grp->GetGUID() != _formedGroupGuid;
}

// The matchmaker's turn. We watch, and we have exactly three outcomes to tell
// apart.
//
// The stock compatibility pass (LFGMgr::Update task 1) drains one new queue
// entry per map update, so a fill that got its bots in normally matches within
// a few seconds. Everything here is about the cases where it does not.
std::size_t DcDungeonQueueFillJob::RequeueDroppedBots()
{
    // Two goes per slot. A bot the core keeps ejecting is refusing for a
    // reason re-sending will not fix, and the fill should reach its timeout
    // and say so rather than trade packets with LFGMgr for two minutes.
    constexpr std::uint8_t kMaxRequeues = 2;

    std::size_t touched = 0;
    for (Slot& slot : _slots)
    {
        if (sLFGMgr->GetState(slot.guid) != lfg::LFG_STATE_NONE)
            continue;

        Player* const bot = ObjectAccessor::FindPlayer(slot.guid);
        if (!bot || !bot->IsInWorld() || !GET_PLAYERBOT_AI(bot))
            continue;

        if (slot.requeues >= kMaxRequeues)
            continue;

        ++slot.requeues;
        ++touched;

        // The same sanitising Queueing was reached under. The bot answered
        // DENY because it was in combat or dead, so clearing that is the whole
        // point — putting it back in the queue in the state that got it thrown
        // out would just lose the next proposal too.
        bot->RemoveAurasDueToSpell(lfg::LFG_SPELL_DUNGEON_COOLDOWN);
        bot->RemoveAurasDueToSpell(lfg::LFG_SPELL_DUNGEON_DESERTER);
        if (!bot->IsAlive())
        {
            bot->ResurrectPlayer(1.0f);
            bot->SpawnCorpseBones();
        }
        if (bot->IsInCombat())
            bot->CombatStop(true);
        bot->SetFullHealth();

        slot.queued = false;
        slot.queueSent = false;

        LOG_INFO("playerbots.dungeonclear",
                 "QUEUEFILL {} {} ({}) was dropped from the queue — sanitising and re-queueing "
                 "(attempt {} of {})",
                 _id, slot.name.empty() ? bot->GetName() : slot.name, slot.plan.role,
                 slot.requeues, kMaxRequeues);
    }

    return touched;
}

void DcDungeonQueueFillJob::TickWaiting()
{
    Player* const player = FindPlayer();
    if (!player)
        return;  // Tick's own liveness check already released; belt and braces

    // 1. The group formed. Whose it is decides everything.
    if (Group* grp = player->GetGroup())
    {
        if (grp->isLFGGroup())
        {
            bool oursInside = false;
            for (Slot const& slot : _slots)
                if (grp->IsMember(slot.guid))
                {
                    oursInside = true;
                    break;
                }

            if (oursInside)
            {
                _formedGroupGuid = grp->GetGUID();
                EnterStage(Stage::Formed);
                HandOffGroup(grp);
                return;
            }

            // Real players got there first. That is a GOOD outcome, not a
            // failure: the player has the group they actually queued for, and
            // our bots go home.
            Release("player matched with other players");
            return;
        }
    }

    // 2. A proposal is up, or has just collapsed. A declined/expired proposal
    // re-queues the survivors (AddToQueue(..., true)) and the core tries
    // again, so one round trip is normal; a second is a pattern.
    lfg::LfgState const state = sLFGMgr->GetState(_lfgGuid);
    if (state == lfg::LFG_STATE_PROPOSAL)
    {
        _sawProposal = true;
        _stageMs = 0;  // the proposal has its own timer; do not race it
        return;
    }
    if (_sawProposal && state == lfg::LFG_STATE_QUEUED)
    {
        _sawProposal = false;
        if (++_proposalFailures > 1)
        {
            Release("the proposal failed twice", /*notifyPlayer*/ true);
            return;
        }
        LOG_INFO("playerbots.dungeonclear",
                 "QUEUEFILL {} proposal failed; the core has re-queued — waiting once more", _id);
    }

    // 3. The player left the queue, or was never in it.
    if (state == lfg::LFG_STATE_NONE)
    {
        Release("player left the queue");
        return;
    }

    // 4. Our own bots are still in the queue behind us — see
    // RequeueDroppedBots. Only worth asking while the player is plainly
    // queued: mid-proposal every member sits in LFG_STATE_PROPOSAL, and
    // reading that as "dropped" would tear down a proposal that is still live.
    if (state == lfg::LFG_STATE_QUEUED && RequeueDroppedBots())
        EnterStage(Stage::Queueing);

    if (_stageMs >= DcSettings::GetUInt(ObjectGuid::Empty,
                                        "DungeonQueueFill.MatchTimeoutSec") * 1000)
    {
        Release("match timed out", /*notifyPlayer*/ true);
    }
}

// The one thing this feature must not get wrong.
//
// LFGQueue picks proposal.leader, and MakeNewGroup calls grp->Create() on the
// first player in its role-sorted list — which starts with that leader. So the
// leader can land on one of our bots, and a real player would find themselves
// unable to set loot rules, use the LFG teleport button, or start a vote kick
// in a group they thought was theirs. Hand it straight back.
void DcDungeonQueueFillJob::HandOffGroup(Group* grp)
{
    if (!grp || _handedOff)
        return;
    _handedOff = true;

    bool const leaderIsOurs = HoldsBot(grp->GetLeaderGUID());
    if (leaderIsOurs)
    {
        grp->ChangeLeader(_playerGuid);
        LOG_INFO("playerbots.dungeonclear", "QUEUEFILL {} moved group leader to {}", _id,
                 _playerName);
    }

    // Loot follows the leader. A bot-led group is created with the bot as
    // looter/master looter, and ChangeLeader does not move either — so a
    // master-loot group would leave every drop in a bot's hands.
    if (leaderIsOurs)
    {
        if (grp->GetLootMethod() == MASTER_LOOT)
            grp->SetMasterLooterGuid(_playerGuid);
        grp->SetLooterGuid(_playerGuid);
        grp->SendUpdate();
    }

    LOG_INFO("playerbots.dungeonclear", "QUEUEFILL {} formed: {} + {} bot(s) — handing off", _id,
             _playerName, _slots.size());
}

// The fill is done; the job now only shadows the group so the bots are released
// at the right moment rather than at a clock.
//
// Nothing is installed on the bots here. LfgAcceptAction calls
// RandomPlayerbotMgr::Refresh + ResetStrategies when it accepts, which would
// wipe anything we set before the proposal; and afterwards it is not needed —
// DcStrategyGate installs the DC strategies for any bot on a dungeon map, and
// the RandomPlayerbotMgr player scan makes the real player their master and
// adds follow. Stock behaviour is exactly what the player wants: bots that
// follow and assist while THEY play the dungeon.
void DcDungeonQueueFillJob::TickFormed()
{
    Player* const player = FindPlayer();
    Group* const grp = player ? player->GetGroup() : nullptr;

    if (!grp || grp->GetGUID() != _formedGroupGuid)
    {
        Release("player left the group / the group disbanded");
        return;
    }

    // The dungeon ended. LFGMgr::FinishDungeon parks the group in
    // LFG_STATE_FINISHED_DUNGEON and leaves it standing (so the party can
    // queue again together), which is exactly the moment the fill's bots stop
    // being wanted — waiting for the group to disband on its own could be
    // another twenty minutes of them following the player around Dalaran.
    if (sLFGMgr->GetState(_formedGroupGuid) == lfg::LFG_STATE_FINISHED_DUNGEON)
    {
        Release("dungeon finished");
        return;
    }

    // Opt-in: run the dungeon for the player instead of with them. Issued once,
    // and only once everyone is actually on the dungeon map — `dc on` before
    // the teleport has no boss roster to work with.
    if (!_autoClearIssued &&
        DcSettings::GetBool(ObjectGuid::Empty, "DungeonQueueFill.AutoClear"))
    {
        for (Slot const& slot : _slots)
        {
            if (DcDungeonQueueFillPlanner::MaskForRole(slot.plan.role) !=
                DcDungeonQueueFillPlanner::kRoleTank)
                continue;
            Player* const tank = ObjectAccessor::FindPlayer(slot.guid);
            PlayerbotAI* const tankAI = tank ? GET_PLAYERBOT_AI(tank) : nullptr;
            if (!tankAI || !tank->GetMap() || !tank->GetMap()->IsDungeon())
                break;
            if (player->GetMapId() != tank->GetMapId())
                break;

            DcStrategyGate::Reconcile(tank);
            // A map whose wing is chosen per run (Blackrock Spire/Depths) takes it
            // from the dungeon the group was actually formed for: LFG 32 is LBRS,
            // 44 UBRS, 30 the BRD Detention Block, 276 the BRD Upper City.
            // Latched before `dc on` so the run clears that wing.
            uint32 const lfgDungeon = sLFGMgr->GetDungeon(_formedGroupGuid);
            if (DungeonWing const* wing =
                    DungeonWingRegistry::WingForLfgDungeon(tank->GetMapId(), lfgDungeon))
            {
                DcRunWing::Set(tank, *wing, DcRunWing::Source::Lfg);
                LOG_INFO("playerbots.dungeonclear",
                         "QUEUEFILL {} AutoClear: LFG dungeon {} -> run wing '{}'", _id,
                         lfgDungeon, wing->token);
            }
            tankAI->DoSpecificAction("dc on", Event("dc", "", player), true);
            _autoClearIssued = true;
            LOG_INFO("playerbots.dungeonclear", "QUEUEFILL {} AutoClear: issued `dc on` to {}",
                     _id, tank->GetName());
            break;
        }
    }
}

// The single funnel every terminal path goes through — timeout, cancel, the
// player logging out, the match landing, the match going to somebody else.
// Leave the queue, leave the group, log out.
//
// The leave packet is queued and the logout follows immediately, which looks
// like a race but is not: WorldSession::LogoutPlayer fires LFGPlayerScript::
// OnPlayerLogout, which calls LeaveLfg AND LeaveAllLfgQueues itself. The packet
// only matters on the LogoutOnRelease=0 path, where nothing else would.
void DcDungeonQueueFillJob::TickReleasing()
{
    bool const logout = DcSettings::GetBool(ObjectGuid::Empty, "DungeonQueueFill.LogoutOnRelease");

    for (Slot const& slot : _slots)
    {
        if (!slot.guid)
            continue;

        Player* const bot = ObjectAccessor::FindPlayer(slot.guid);

        // Never yank a bot the matchmaker put in somebody ELSE's dungeon.
        // Our bots queue solo, so a proposal can legitimately pair one of them
        // with a different player's party — and pulling it out mid-run would
        // break that player's dungeon to tidy up ours.
        if (bot && BotIsSomebodyElses(bot))
        {
            LOG_INFO("playerbots.dungeonclear",
                     "QUEUEFILL {} leaving {} where it is — matched into another group", _id,
                     bot->GetName());
            continue;
        }

        DcPoolBots::ReleaseBot(slot.guid, bot, logout, "QUEUEFILL", _id);
    }

    // Graceful degradation, said out loud: the player is still in the REAL
    // queue, which is a slower version of what they asked for, not a broken
    // one. Silence here is what makes a feature feel unreliable.
    if (_notifyPlayer)
        if (Player* const player = FindPlayer())
            ChatHandler(player->GetSession())
                .PSendSysMessage("|cffff8000[Dungeon Finder]|r Could not fill your group this "
                                 "time — you are still queued normally.");

    LOG_INFO("playerbots.dungeonclear", "QUEUEFILL {} released after {}s — {}", _id,
             _totalMs / 1000, _releaseReason.empty() ? "done" : _releaseReason);
    EnterStage(Stage::Done);
}

std::string DcDungeonQueueFillJob::StatusLine() const
{
    std::string out = _id + " " + _playerName + " [" + StageName(_stage) + "] elapsed " +
                      std::to_string(_totalMs / 1000) + "s";
    if (!_slots.empty())
    {
        out += ", filling";
        for (Slot const& s : _slots)
            out += std::string(" ") + s.plan.role + (s.name.empty() ? "" : ":" + s.name);
    }
    return out;
}
