/*
 * mod-dungeon-clear — DcModuleEnable.h
 *
 * The module's master switch: `DungeonClear.Enable` in
 * mod_dungeon_clear.conf (1 by default).
 *
 * With it set to 0 the module is compiled and linked but completely inert:
 *
 *   * NOTHING is registered into mod-playerbots. The registrar's first-world-tick
 *     append of the four DungeonClear contexts (strategy/action/trigger/value)
 *     into the ten per-class shared registries never runs, so no bot's engine
 *     can even name a DC strategy, action, trigger or value.
 *   * No bot ever has "dungeon clear" / "dungeon clear combat" installed —
 *     DcStrategyGate stops reconciling, so the login / map-change / sweep
 *     drivers all no-op.
 *   * Every other hook the module owns (the pull brake, the spectator camera
 *     window and its teardown/death nets, the per-tick reaper that drives the
 *     path worker, status publisher, combat purge and `.dc test` harness, and
 *     the two core-bug workaround scripts) returns immediately.
 *   * Every `.dc` subcommand and every "DC" addon message answers with a
 *     "module is disabled" notice instead of acting; the addon hook does not
 *     consume the chat message.
 *
 * WHY IT IS LATCHED (read once, at the first world tick, and never re-read):
 * registration is a one-way append into `Ctx::sharedStrategyContexts` & friends,
 * and every already-built bot's NamedObjectContextList holds those by reference.
 * There is no supported "unregister" — so a mid-session flip could only ever
 * produce a half-state (contexts registered but strategies refused, or the
 * reverse). Latching makes the answer to "is DC live in this process?" a single
 * constant that every hook, command and gate agrees on. `.reload config` with a
 * changed value logs a warning saying a restart is required, and changes
 * nothing.
 *
 * The switch is server-only in the registry: a master switch is an admin
 * decision, never something the companion addon's per-run override layer may
 * reach.
 */

#ifndef _DC_MODULE_ENABLE_H
#define _DC_MODULE_ENABLE_H

namespace DcModule
{
    // The latched verdict. Cheap enough (one relaxed atomic load) to call from
    // any hook on any tick. If a caller somehow beats the first world tick, the
    // value is resolved from conf on the spot and latched, so the answer is
    // still stable for the rest of the process.
    bool IsEnabled();

    // Resolve `DungeonClear.Enable` from conf and latch it, logging the verdict.
    // Called once from the registrar's first world tick, BEFORE anything is
    // registered into mod-playerbots.
    void LatchFromConf();

    // Latch an explicit value without touching sConfigMgr. No-op once latched —
    // which is the whole point of the latch, and what the headless test pins.
    void LatchValue(bool enabled);

    // `.reload config` net: warn (and change nothing) when the conf line no
    // longer agrees with what the process latched at startup. Call after
    // DcSettings::InvalidateConfCache so the conf read is fresh.
    void WarnIfConfDiffersFromLatch();

    // Test seam. Nothing in the running server may call this: dropping the latch
    // mid-process is exactly the half-state the latch exists to prevent.
    void ResetLatchForTests();
}

#endif  // _DC_MODULE_ENABLE_H
