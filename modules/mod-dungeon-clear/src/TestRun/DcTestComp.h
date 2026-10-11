/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCTESTCOMP_H
#define _PLAYERBOT_DCTESTCOMP_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

// A test run fields a 5-bot party: one tank, one healer, three DPS. Which
// class/spec fills each role is randomised per run (BuildComp) so successive
// runs of the same dungeon exercise different pull/heal/threat paths and shake
// out class-specific edge cases — the earlier fixed prot-warr / holy-priest /
// mage-rogue-hunter comp only ever tested one shape.
//
// Every spec is named by its playerbots premade-spec template name
// (AiPlayerbot.PremadeSpecName.<class>.<n>) and resolved to a specNo at
// provisioning time; a spec with no matching template fails the run loudly
// (tank/heal) rather than silently rolling a random build, so only specs that
// exist as templates appear in the pools below. All-Alliance because the
// addclass pool is picked per team and entrances are faction-agnostic
// teleports.
//
// Death knights are drawn for WotLK runs only (`Roster`), which is the one
// place a class is gated by the dungeon rather than by its role.
//
// Randomisation is seeded (BuildComp(seed)) and the seed is recorded, so a
// comp that trips a bug can be replayed exactly via `.dc test start <d> seed=N`.

namespace DcTestComp
{
    struct Slot
    {
        std::uint8_t classId;      // 1=warr 2=pala 3=hunt 4=rogue 5=priest 6=dk 7=sham 8=mage 9=lock 11=druid
        char const* specName;      // premade-spec template to force
        char const* fallbackSpec;  // substring to match if the exact name is absent
        char const* role;          // "tank" | "heal" | "dps" (for the record/UI)
    };

    inline constexpr std::size_t kPartySize = 5;

    // Which classes a run may field. Death knights arrived with Wrath and
    // their whole kit — abilities, talents, presences — starts at level 55, so
    // a classic or TBC run must never draw one: the factory levels every test
    // bot to the dungeon's own level, and a level-24 death knight has nothing
    // to press. WotLK rows all sit at 70 or above, so there the class is
    // simply available.
    enum class Roster : std::uint8_t
    {
        NoDeathKnights,    // classic and TBC rows, and any run levelled under
                           // kDeathKnightMinLevel
        WithDeathKnights,  // WotLK rows at kDeathKnightMinLevel or above
    };

    // The level a death knight starts at on 3.3.5a. The caller gates on the
    // run's LEVEL as well as its expansion, because `level=N` can drop a WotLK
    // run under the floor.
    inline constexpr std::uint32_t kDeathKnightMinLevel = 55;

    // Every class/spec that can legitimately fill each role. BuildComp draws
    // from these; a class appears once per role even when it offers several
    // specs for it (e.g. priest disc + holy) so both get exercised over time.
    //
    // Death-knight rows sit LAST in every pool they appear in. Filtering them
    // out for a pre-Wrath run then leaves the remaining entries in exactly the
    // order they had before the class existed here, so an old seed still
    // replays the comp it originally rolled.
    inline constexpr Slot kTankPool[] = {
        { 1,  "prot pve",  "prot",  "tank" },  // warrior
        { 2,  "prot pve",  "prot",  "tank" },  // paladin
        { 11, "bear pve",  "bear",  "tank" },  // druid
        // Blood is the tank tree in this playerbots build: AiFactory hands
        // tab 0 the "blood"/"tank assist" strategies and IsTank(bySpec) reads
        // tab 0 as a tank, while frost and unholy both come back as DPS.
        { 6,  "blood pve", "blood", "tank" },  // death knight (WotLK only)
    };

    inline constexpr Slot kHealPool[] = {
        { 5,  "holy pve",  "holy",  "heal" },  // priest
        { 5,  "disc pve",  "disc",  "heal" },  // priest
        { 2,  "holy pve",  "holy",  "heal" },  // paladin
        { 7,  "resto pve", "resto", "heal" },  // shaman
        { 11, "resto pve", "resto", "heal" },  // druid
    };

    inline constexpr Slot kDpsPool[] = {
        { 1,  "arms pve",     "arms",     "dps" },  // warrior
        { 1,  "fury pve",     "fury",     "dps" },  // warrior
        { 2,  "ret pve",      "ret",      "dps" },  // paladin
        { 3,  "bm pve",       "bm",       "dps" },  // hunter
        { 3,  "mm pve",       "mm",       "dps" },  // hunter
        { 3,  "surv pve",     "surv",     "dps" },  // hunter
        { 4,  "as pve",       "as",       "dps" },  // rogue
        { 4,  "combat pve",   "combat",   "dps" },  // rogue
        { 4,  "subtlety pve", "subtlety", "dps" },  // rogue
        { 5,  "shadow pve",   "shadow",   "dps" },  // priest
        { 7,  "ele pve",      "ele",      "dps" },  // shaman
        { 7,  "enh pve",      "enh",      "dps" },  // shaman
        { 8,  "arcane pve",   "arcane",   "dps" },  // mage
        { 8,  "fire pve",     "fire",     "dps" },  // mage
        { 8,  "frost pve",    "frost",    "dps" },  // mage
        { 9,  "affli pve",    "affli",    "dps" },  // warlock
        { 9,  "demo pve",     "demo",     "dps" },  // warlock
        { 9,  "destro pve",   "destro",   "dps" },  // warlock
        { 11, "balance pve",  "balance",  "dps" },  // druid
        { 11, "cat pve",      "cat",      "dps" },  // druid
        { 6,  "frost pve",    "frost",    "dps" },  // death knight (WotLK only)
        { 6,  "unholy pve",   "unholy",   "dps" },  // death knight (WotLK only)
    };

    // Size bounds for a run (raid-support Plan D): a 5-man is the default, a
    // raid run asks for any size up to 40 (`size=N`, presets 10/25).
    inline constexpr std::size_t kMinPartySize = 2;
    inline constexpr std::size_t kMaxPartySize = 40;

    // Role quotas derived from the party size. 5-mans keep the classic
    // 1/1/3; raids scale tanks stepwise and healers at ~N/4.
    struct Quota
    {
        std::size_t tanks;
        std::size_t healers;
        std::size_t dps;
    };
    Quota RoleQuota(std::size_t size);

    // Deterministically pick a party for the given seed: one tank, one healer,
    // three DPS, all five on DISTINCT classes (maximises class diversity and
    // keeps the addclass-pool draw from needing several chars of one class).
    // Pure — no globals, no I/O — so the same seed always yields the same comp.
    // `roster` is the only thing that changes which classes are on the table.
    std::array<Slot, kPartySize> BuildComp(std::uint32_t seed, Roster roster);

    // Sized form: RoleQuota(size) slots, seed-deterministic. Distinct classes
    // while the pools allow, then duplicates spread as evenly as possible (9
    // classes — 10 with death knights — < 25 slots; 6 healers over 4 heal
    // classes force duplicates) — the least-used class of the role pool is
    // always preferred. size is clamped to [kMinPartySize, kMaxPartySize].
    std::vector<Slot> BuildComp(std::uint32_t seed, std::size_t size, Roster roster);

    // The pool backing a role token ("tank" | "heal" | "dps"), for callers that
    // must substitute an alternative class when the drawn one has no available
    // pool character. Empty for an unknown token. Takes `roster` for the same
    // reason BuildComp does: a substitution must not smuggle a death knight
    // into a run the draw itself would never have given one to.
    std::vector<Slot> RolePool(std::string_view role, Roster roster);
}

#endif  // _PLAYERBOT_DCTESTCOMP_H
