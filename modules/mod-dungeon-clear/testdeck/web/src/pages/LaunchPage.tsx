/* The home page and the golden path: pick a dungeon, hit Start. The drawer
 * closes and you stay here — launching several dungeons in a row is the
 * common case, and Live is one click away when you want to watch. Plans and
 * roster launches are tabs in the same drawer so a tester never hunts for a
 * second form.
 *
 * The catalogue is ~60 rows and growing, so the list is shelved: filter
 * pills (expansion, or raids-only) over compact one-line rows grouped into
 * titled sections. Raids always sit in their own sections — a raid launch
 * fields a different party and is never picked by accident.
 *
 * SCENARIOS (a registry row with `scenario: true`) are slices of a parent
 * dungeon — Karazhan's chess event, say — and never stand on their own: each
 * section carries a "Scenarios" shelf under its grid for the scenarios whose
 * parent sits in that section, labelled "Karazhan · Chess". */

import { useEffect, useMemo, useRef, useState } from "react";
import { Link } from "react-router-dom";
import { api, ApiError } from "../api/client";
import { usePoll } from "../api/hooks";
import { startRosterRun, startRun } from "../api/launch";
import type { Catalogue, CommandReply, Dungeon, SavedRoster } from "../api/types";
import { EXPANSION_NAME, expansionOfRow, QUALITY_CHOICES } from "../data/wow";
import {
  EmptyState,
  Field,
  FIELD,
  NumberBox,
  SELECT,
  Spinner,
  useModal,
  useToast,
} from "../components/ui";
import { useStatus } from "../layout/AppShell";

type Mode = "quick" | "plan" | "roster";

/* The three modes are the one thing a new tester has to be told, and the tab
 * labels alone do not say it: "Plan" and "Roster" both read like settings
 * rather than like different kinds of launch. One line under the tabs is
 * cheaper than a help page nobody opens. */
const MODE_HELP: Record<Mode, string> = {
  quick: "One run, right now, with a freshly rolled 5-bot party.",
  plan: "The same run over and over — a campaign, for measuring a success rate.",
  roster: "A saved party of real characters instead of pool bots.",
};

export type Shelf = "all" | "classic" | "tbc" | "wotlk" | "raids";

const SHELF_KEY = "tdeck.launch.shelf";
export const SHELVES: [Shelf, string][] = [
  ["all", "All"],
  ["classic", "Classic"],
  ["tbc", "Burning Crusade"],
  ["wotlk", "Wrath"],
  ["raids", "Raids"],
];

export const EXP_SHELF = ["classic", "tbc", "wotlk"] as const;

/* Six buckets — dungeons and raids per expansion — rendered as titled
 * sections. A shelf is a filter over the buckets, so "Classic" includes the
 * classic raids and "Raids" spans every expansion, and either agrees with
 * the section a row sits under on "All". */
export function sectionsFor(dungeons: Dungeon[], shelf: Shelf, byToken: Map<string, Dungeon>) {
  const buckets: Dungeon[][] = [[], [], [], [], [], []];
  const scenarios: Dungeon[][] = [[], [], [], [], [], []];
  const bucketOf = (d: Dungeon) => expansionOfRow(d) + (d.raid ? 3 : 0);
  for (const d of dungeons) {
    if (d.scenario) {
      /* Shelved by the PARENT's bucket, so a scenario always sits under the
         dungeon it is a slice of. An orphan (stale sidecar) is dropped — the
         Deck's own launch route would refuse it anyway. */
      const parent = byToken.get(d.scenarioOf ?? "");
      if (parent) scenarios[bucketOf(parent)].push(d);
      continue;
    }
    buckets[bucketOf(d)].push(d);
  }
  const titles = [
    ...EXPANSION_NAME,
    ...EXPANSION_NAME.map((n) => `${n} — raids`),
  ];
  const order =
    shelf === "all"
      ? [0, 1, 2, 3, 4, 5]
      : shelf === "raids"
        ? [3, 4, 5]
        : [EXP_SHELF.indexOf(shelf), EXP_SHELF.indexOf(shelf) + 3];
  return order
    .filter((i) => buckets[i].length || scenarios[i].length)
    .map((i) => ({ title: titles[i], items: buckets[i], scenarios: scenarios[i] }));
}

/* "Karazhan: Chess" under parent "Karazhan" reads "Karazhan · Chess". */
export function scenarioLabel(d: Dungeon, parent?: Dungeon) {
  if (!parent) return d.name;
  const tail = d.name.startsWith(parent.name)
    ? d.name.slice(parent.name.length).replace(/^[\s:·—-]+/, "")
    : d.name;
  return `${parent.name} · ${tail || d.name}`;
}

export default function LaunchPage() {
  const { data: catalogue, error } = usePoll(
    () => api.get<Catalogue>("/api/testdungeons"),
    30000,
  );
  const status = useStatus();
  const [query, setQuery] = useState("");
  const [shelf, setShelfState] = useState<Shelf>(() => {
    const s = localStorage.getItem(SHELF_KEY);
    return SHELVES.some(([v]) => v === s) ? (s as Shelf) : "all";
  });
  const [selected, setSelected] = useState<Dungeon | null>(null);

  function setShelf(v: Shelf) {
    setShelfState(v);
    setQuery("");
    localStorage.setItem(SHELF_KEY, v);
  }

  const all = useMemo(() => catalogue?.dungeons ?? [], [catalogue]);
  const byToken = useMemo(() => new Map(all.map((d) => [d.token, d])), [all]);
  const q = query.trim().toLowerCase();

  /* A typed search suspends the shelf filter — a tester hunting a name
   * should not also have to remember which shelf it lives on. Picking a
   * shelf pill clears the search, so the two never silently fight. */
  const sections = useMemo(() => {
    const matched = q
      ? all.filter(
          (d) =>
            d.name.toLowerCase().includes(q) ||
            d.token.toLowerCase().includes(q),
        )
      : all;
    return sectionsFor(matched, q ? "all" : shelf, byToken);
  }, [all, q, shelf, byToken]);

  /* Scenarios are shelved under their parent, so the pill counts are
     dungeons only. */
  const counts = useMemo(() => {
    const dungeons = all.filter((d) => !d.scenario);
    const c: Record<Shelf, number> = {
      all: dungeons.length,
      classic: 0,
      tbc: 0,
      wotlk: 0,
      raids: 0,
    };
    for (const d of dungeons) {
      c[EXP_SHELF[expansionOfRow(d)]]++;
      if (d.raid) c.raids++;
    }
    return c;
  }, [all]);

  if (error) {
    return (
      <EmptyState icon="⚠️" title="Cannot reach the server">
        {error}
      </EmptyState>
    );
  }
  if (!catalogue) return <Spinner label="loading catalogue…" />;
  if (!catalogue.dungeons?.length) {
    return (
      <EmptyState icon="🌙" title="No dungeon catalogue yet">
        The worldserver writes its dungeon list shortly after startup
        {status?.realm === "ONLINE"
          ? " — it should appear here within a minute."
          : " — and it does not look like the realm is up right now."}
      </EmptyState>
    );
  }

  return (
    <div>
      <div className="mb-4 flex flex-wrap items-end justify-between gap-4">
        <div>
          <h1 className="text-2xl font-semibold">Launch a test run</h1>
          <p className="mt-1 text-sm text-ink-400">
            Pick a dungeon — a full bot party spawns, clears it, and reports
            back.
          </p>
        </div>
        <input
          className={`${FIELD} max-w-xs sm:w-64`}
          placeholder="Search dungeons…"
          value={query}
          onChange={(e) => setQuery(e.target.value)}
        />
      </div>

      <div className="mb-5 flex flex-wrap gap-1.5">
        {SHELVES.map(([v, label]) => (
          <button
            key={v}
            type="button"
            onClick={() => setShelf(v)}
            className={`rounded-full border px-3 py-1.5 text-sm transition ${
              shelf === v && !q
                ? "border-iris-500/40 bg-iris-500/15 text-iris-100"
                : "border-ink-800 bg-ink-900/60 text-ink-400 hover:border-ink-700 hover:text-ink-200"
            }`}
          >
            {label}
            <span
              className={`ml-1.5 text-xs ${
                shelf === v && !q ? "text-iris-300/70" : "text-ink-600"
              }`}
            >
              {counts[v]}
            </span>
          </button>
        ))}
      </div>

      {sections.map((s) => (
        <section key={s.title} className="mb-6">
          <h2 className="mb-2 flex items-baseline gap-2 text-xs font-semibold uppercase tracking-wider text-ink-500">
            {s.title}
            <span className="font-normal text-ink-600">{s.items.length}</span>
          </h2>
          <div className="grid grid-cols-1 gap-1.5 sm:grid-cols-2 xl:grid-cols-3">
            {s.items.map((d) => (
              <DungeonRow
                key={d.token + d.wing}
                d={d}
                onPick={() => setSelected(d)}
              />
            ))}
          </div>
          {s.scenarios.length > 0 && (
            <div className="mt-2.5">
              <h3 className="mb-1.5 text-[11px] font-semibold uppercase tracking-wider text-teal-400/70">
                Scenarios
              </h3>
              <div className="grid grid-cols-1 gap-1.5 sm:grid-cols-2 xl:grid-cols-3">
                {s.scenarios.map((d) => (
                  <DungeonRow
                    key={d.token}
                    d={d}
                    parent={byToken.get(d.scenarioOf ?? "")}
                    onPick={() => setSelected(d)}
                  />
                ))}
              </div>
            </div>
          )}
        </section>
      ))}
      {!sections.length && (
        <EmptyState icon="🔍" title={`No dungeon matches “${query}”`} />
      )}

      {selected && (
        <LaunchDrawer
          dungeon={selected}
          parent={selected.scenario ? byToken.get(selected.scenarioOf ?? "") : undefined}
          catalogue={catalogue}
          onClose={() => setSelected(null)}
        />
      )}
    </div>
  );
}

/* One line per dungeon: name, token, a heroic marker, and the level (raids
 * show their party size instead — level 60/70/80 says less about a raid than
 * how many bots it fields). The old two-line cards made 58 rows into three
 * screens of scrolling. */
function DungeonRow({
  d,
  parent,
  onPick,
}: {
  d: Dungeon;
  parent?: Dungeon;
  onPick: () => void;
}) {
  return (
    <button
      onClick={onPick}
      className="group flex items-center gap-2 rounded-xl border border-ink-800 bg-ink-900/60 px-3 py-2 text-left transition hover:border-iris-500/50 hover:bg-ink-900"
    >
      <span className="min-w-0 flex-1 truncate text-sm font-medium text-ink-100 group-hover:text-iris-200">
        {d.scenario ? scenarioLabel(d, parent) : d.name}
      </span>
      <span className="hidden shrink-0 font-mono text-xs text-ink-600 group-hover:text-ink-500 sm:inline">
        {d.token}
      </span>
      {d.scenario && (
        <span
          title={
            d.success
              ? `Scenario · passes on ${d.success}`
              : "Scenario · passes on all-cleared"
          }
          className="shrink-0 rounded bg-teal-500/15 px-1.5 py-0.5 text-[10px] font-semibold text-teal-300"
        >
          S
        </span>
      )}
      {d.heroicLevel > 0 && (
        <span
          title={`Heroic · lv ${d.heroicLevel}`}
          className="shrink-0 rounded bg-fuchsia-500/15 px-1.5 py-0.5 text-[10px] font-semibold text-fuchsia-300"
        >
          H
        </span>
      )}
      <span className="shrink-0 rounded-full bg-ink-800 px-2 py-0.5 text-xs tabular-nums text-ink-400">
        {d.raid && d.defaultSize ? `${d.defaultSize}-man` : `lv ${d.level}`}
      </span>
    </button>
  );
}

/* A row of mutually-exclusive buttons. Used for the mode tabs and for
 * difficulty — a checkbox for "Heroic" hid the single most consequential knob
 * in the form among four gear dropdowns. */
export function Segmented<T extends string>({
  value,
  onChange,
  options,
}: {
  value: T;
  onChange: (v: T) => void;
  options: [T, string][];
}) {
  return (
    <div className="flex gap-1 rounded-xl border border-ink-800 bg-ink-950/70 p-1">
      {options.map(([v, label]) => (
        <button
          key={v}
          type="button"
          onClick={() => onChange(v)}
          className={`flex-1 rounded-lg px-3 py-1.5 text-sm transition ${
            value === v
              ? "bg-iris-500/20 text-iris-100 ring-1 ring-inset ring-iris-500/40"
              : "text-ink-400 hover:text-ink-200"
          }`}
        >
          {label}
        </button>
      ))}
    </div>
  );
}

function LaunchDrawer({
  dungeon,
  parent,
  catalogue,
  onClose,
}: {
  dungeon: Dungeon;
  parent?: Dungeon;
  catalogue: Catalogue;
  onClose: () => void;
}) {
  /* A scenario runs at its parent's default size (its focus, grace and
     watchdog budget were authored for that raid) and has no heroic mode; the
     Deck's launch route enforces both too. */
  const scenario = !!dungeon.scenario;
  const lockedSize = scenario && dungeon.raid ? dungeon.defaultSize ?? 0 : 0;
  const toast = useToast();
  const [mode, setMode] = useState<Mode>("quick");
  const [heroic, setHeroic] = useState(false);
  /* Every count is held as TEXT so a box can be emptied and retyped; an empty
     box means "the default", which for all four of these is 0. */
  const [levelText, setLevelText] = useState("");
  const [seedText, setSeedText] = useState("");
  /* Raid rows only: party size, prefilled with the catalogue's default so a
     bare launch fields the size the raid plan expects. Blank = classic 5-man
     comp (still legal on a raid map — the harness raid-groups any size). */
  const [sizeText, setSizeText] = useState(
    dungeon.raid && dungeon.defaultSize ? String(dungeon.defaultSize) : "",
  );
  const [ilvl, setIlvl] = useState(0);
  const [quality, setQuality] = useState(0);
  const [totalText, setTotalText] = useState("5");
  const [concurrentText, setConcurrentText] = useState("1");
  const [rosters, setRosters] = useState<SavedRoster[] | null>(null);
  const [rosterName, setRosterName] = useState("");
  const [busy, setBusy] = useState<string | null>(null);
  const [pendingNote, setPendingNote] = useState<string | null>(null);
  const launchRef = useRef<() => void>(() => {});

  const level = Number(levelText || 0);
  const seed = Number(seedText || 0);
  const size = lockedSize || (dungeon.raid ? Number(sizeText || 0) : 0);
  const sizeMin = dungeon.sizeMin ?? 2;
  const sizeMax = dungeon.sizeMax ?? 40;
  const total = Number(totalText);
  const concurrent = Number(concurrentText);

  const canHeroic = dungeon.heroicLevel > 0 && !scenario;
  const ladder = (heroic ? dungeon.gearHeroic : dungeon.gear) ?? dungeon.gear ?? [];
  const maxTotal = catalogue.limits?.planMaxTotal || 0;
  /* Both caps are 0/absent = unlimited, which is the module's own default
     (DungeonClear.TestRun.MaxConcurrent = 0). The old `|| 10` here read that
     "unlimited" as falsy and invented a ceiling of 10 that nothing server-side
     asked for — only mirror a cap the catalogue actually states. */
  const maxConcurrent = catalogue.limits?.maxConcurrent || 0;

  const levelOk = level <= 80;
  const sizeOk =
    size === 0 || (Number.isInteger(size) && size >= sizeMin && size <= sizeMax);
  const totalOk = /^\d+$/.test(totalText) && total >= 1 && (!maxTotal || total <= maxTotal);
  const concurrentOk =
    /^\d+$/.test(concurrentText) && (!maxConcurrent || concurrent <= maxConcurrent);
  const planOk = totalOk && concurrentOk;
  const canLaunch =
    busy === null &&
    levelOk &&
    sizeOk &&
    (mode !== "roster" || !!rosterName) &&
    (mode !== "plan" || planOk);

  useEffect(() => {
    if (mode !== "roster" || rosters !== null) return;
    api
      .get<{ rosters: SavedRoster[] }>("/api/rosters")
      .then((r) => {
        setRosters(r.rosters);
        if (r.rosters.length) setRosterName(r.rosters[0].name);
      })
      .catch(() => setRosters([]));
  }, [mode, rosters]);

  /* ilvl choices differ per difficulty; reset when the ladder changes. */
  useEffect(() => setIlvl(0), [heroic]);

  /* Escape backs out, focus moves into the form and is trapped there, and the
     dungeon grid behind stops scrolling under the overlay — see useModal. */
  const panel = useModal<HTMLDivElement>(onClose);

  async function launch() {
    setBusy("starting…");
    setPendingNote(null);
    try {
      if (mode === "plan") {
        const r = await api.post<CommandReply>("/api/testplans/start", {
          dungeon: dungeon.token,
          total,
          concurrent,
          heroic,
          level,
          seed,
          size,
          ilvl,
          quality,
        });
        if (!r.ok && r.reply.length) throw new ApiError(500, r.reply.join(" "));
        toast("ok", `Plan started: ${total}× ${dungeon.name} — see Live`);
        onClose();
        return;
      }

      if (mode === "roster") {
        const members = rosters?.find((r) => r.name === rosterName)?.members;
        if (!members) throw new ApiError(400, "pick a saved roster first");
        await startRosterRun(dungeon.token, members, heroic);
        toast("ok", `Roster run started at ${dungeon.name} — see Live`);
        onClose();
        return;
      }

      const outcome = await startRun(
        { dungeon: dungeon.token, heroic, level, seed, size, ilvl, quality },
        (attempt, of) => {
          setBusy(`driver logging in — retrying (${attempt}/${of})…`);
          setPendingNote(
            "The test driver is still logging in. If it does not answer, " +
              "this starts as a 1-run plan instead — plans wait for the " +
              "driver automatically.",
          );
        },
      );
      toast(
        "ok",
        outcome === "plan"
          ? `Queued as a 1-run plan at ${dungeon.name} — see Live`
          : `Run started at ${dungeon.name} — see Live`,
      );
      onClose();
    } catch (e) {
      toast("error", e instanceof Error ? e.message : String(e));
    } finally {
      setBusy(null);
    }
  }
  launchRef.current = () => {
    if (canLaunch) void launch();
  };

  const buttonLabel =
    busy ??
    (mode === "plan"
      ? totalOk
        ? `Start ${total} runs`
        : "Start runs"
      : mode === "roster"
        ? "Start roster run"
        : "Start run");

  return (
    <div
      className="fixed inset-0 z-40 flex items-end justify-center bg-ink-950/80 p-0 backdrop-blur-sm sm:items-center sm:p-4"
      onClick={onClose}
    >
      <div
        ref={panel}
        role="dialog"
        aria-modal="true"
        tabIndex={-1}
        aria-label={`Launch ${dungeon.name}`}
        className="flex max-h-[92dvh] w-full max-w-2xl flex-col overflow-hidden rounded-t-2xl border border-ink-700 bg-ink-900 shadow-2xl shadow-ink-950 sm:rounded-2xl"
        onClick={(e) => e.stopPropagation()}
        /* Enter anywhere in the form launches; the selects keep their own
           Enter handling, so exclude them. */
        onKeyDown={(e) => {
          if (e.key !== "Enter") return;
          if ((e.target as HTMLElement).tagName === "SELECT") return;
          e.preventDefault();
          launchRef.current();
        }}
      >
        {/* header */}
        <div className="flex items-start justify-between gap-4 border-b border-ink-800 px-6 py-5">
          <div>
            <h2 className="text-lg font-semibold">
              {scenario ? scenarioLabel(dungeon, parent) : dungeon.name}
            </h2>
            <div className="mt-0.5 text-xs text-ink-500">
              <span className="font-mono">{dungeon.token}</span> · level{" "}
              {heroic ? dungeon.heroicLevel : dungeon.level}
              {dungeon.wing && <> · {dungeon.wing}</>}
            </div>
            {scenario && (
              <div className="mt-1.5 flex flex-wrap gap-x-2 gap-y-1 text-xs text-teal-300/80">
                <span className="rounded bg-teal-500/15 px-1.5 py-0.5 font-semibold text-teal-300">
                  Scenario
                </span>
                <span>of {parent?.name ?? dungeon.scenarioOf}</span>
                {dungeon.focus?.length ? (
                  <span>· focus {dungeon.focus.join(", ")}</span>
                ) : null}
                <span>
                  · passes on {dungeon.success || "all-cleared"}
                  {dungeon.success && dungeon.successGraceS
                    ? ` (+${dungeon.successGraceS}s grace)`
                    : ""}
                </span>
                {dungeon.overallTimeoutS ? (
                  <span>· {Math.round(dungeon.overallTimeoutS / 60)} min cap</span>
                ) : null}
              </div>
            )}
          </div>
          <button
            onClick={onClose}
            aria-label="Close"
            className="rounded-lg px-2 py-1 text-ink-500 transition hover:bg-ink-800 hover:text-ink-200"
          >
            ✕
          </button>
        </div>

        {/* body */}
        <div className="flex-1 space-y-5 overflow-y-auto px-6 py-5">
          <div>
            <Segmented<Mode>
              value={mode}
              onChange={setMode}
              options={[
                ["quick", "Quick run"],
                ["plan", "Plan"],
                ["roster", "Roster"],
              ]}
            />
            <p className="mt-2 text-xs text-ink-500">{MODE_HELP[mode]}</p>
          </div>

          {canHeroic && (
            <Field label="Difficulty">
              <Segmented<"normal" | "heroic">
                value={heroic ? "heroic" : "normal"}
                onChange={(v) => setHeroic(v === "heroic")}
                options={[
                  ["normal", `Normal · lv ${dungeon.level}`],
                  ["heroic", `Heroic · lv ${dungeon.heroicLevel}`],
                ]}
              />
            </Field>
          )}

          {mode === "plan" && (
            <div className="grid grid-cols-1 gap-4 sm:grid-cols-2">
              <Field
                label="Total runs"
                hint={maxTotal ? `1–${maxTotal}` : "1 or more"}
              >
                <NumberBox
                  value={totalText}
                  onChange={setTotalText}
                  invalid={!totalOk}
                  placeholder="5"
                />
              </Field>
              <Field
                label="Concurrent"
                hint={maxConcurrent ? `0–${maxConcurrent}` : "0 = server default"}
              >
                <NumberBox
                  value={concurrentText}
                  onChange={setConcurrentText}
                  invalid={!concurrentOk}
                  placeholder="1"
                />
              </Field>
            </div>
          )}

          {mode === "roster" ? (
            <>
              {rosters === null ? (
                <Spinner label="loading rosters…" />
              ) : rosters.length === 0 ? (
                <p className="rounded-lg border border-ink-800 bg-ink-950/70 px-3 py-2 text-sm text-ink-400">
                  No saved rosters yet — build one on the{" "}
                  {/* Link, not <a>: an <a href> reloads the whole SPA, which
                      throws away the session check and every cached poll for
                      what is a move between two tabs of the same app. */}
                  <Link
                    to="/roster"
                    onClick={onClose}
                    className="text-iris-300 underline"
                  >
                    Roster page
                  </Link>
                  .
                </p>
              ) : (
                <Field label="Saved roster">
                  <select
                    value={rosterName}
                    onChange={(e) => setRosterName(e.target.value)}
                    className={SELECT}
                  >
                    {rosters.map((r) => (
                      <option key={r.name} value={r.name}>
                        {r.name} — {r.members.join(", ")}
                      </option>
                    ))}
                  </select>
                </Field>
              )}
              <p className="text-xs text-ink-500">
                Real characters keep loot, XP and deaths. Gear and level knobs
                don’t apply — the party runs as-is.
              </p>
            </>
          ) : (
            <div className="grid grid-cols-1 gap-4 sm:grid-cols-2">
              {lockedSize > 0 && (
                <Field label="Raid size" hint="locked to the parent raid's default">
                  <div className={`${FIELD} text-ink-400`}>{lockedSize}</div>
                </Field>
              )}
              {dungeon.raid && !lockedSize && (
                <Field
                  label="Raid size"
                  hint={
                    `${sizeMin}–${sizeMax}` +
                    (dungeon.sizePresets?.length
                      ? ` · presets ${dungeon.sizePresets.join("/")}`
                      : "") +
                    " · blank = 5-man comp"
                  }
                >
                  <NumberBox
                    value={sizeText}
                    onChange={setSizeText}
                    invalid={!sizeOk}
                    placeholder={String(dungeon.defaultSize ?? 10)}
                  />
                </Field>
              )}
              <Field label="Bot level" hint="blank = dungeon default">
                <NumberBox
                  value={levelText}
                  onChange={setLevelText}
                  invalid={!levelOk}
                  placeholder={String(heroic ? dungeon.heroicLevel : dungeon.level)}
                />
              </Field>
              <Field label="Comp seed" hint="blank = random">
                <NumberBox
                  value={seedText}
                  onChange={setSeedText}
                  placeholder="random"
                />
              </Field>
              <Field label="Gear item level">
                <select
                  value={ilvl}
                  onChange={(e) => setIlvl(Number(e.target.value))}
                  className={SELECT}
                >
                  <option value={0}>server default</option>
                  {ladder.map((g) => (
                    <option key={g.ilvl} value={g.ilvl}>
                      ilvl {g.ilvl} — {g.label}
                    </option>
                  ))}
                  <option value={-1}>no limit</option>
                </select>
              </Field>
              <Field label="Max quality">
                <select
                  value={quality}
                  onChange={(e) => setQuality(Number(e.target.value))}
                  className={SELECT}
                >
                  <option value={0}>server default</option>
                  {QUALITY_CHOICES.map((q) => (
                    <option key={q.v} value={q.v}>
                      {q.label}
                    </option>
                  ))}
                </select>
              </Field>
              {!levelOk && (
                <p className="text-xs text-rose-300 sm:col-span-2">
                  Bot level must be 80 or lower.
                </p>
              )}
            </div>
          )}

          {pendingNote && (
            <p className="rounded-lg border border-amber-900/50 bg-amber-950/40 px-3 py-2 text-sm text-amber-200">
              {pendingNote}
            </p>
          )}
        </div>

        {/* footer — the action stays on screen however long the form gets */}
        <div className="border-t border-ink-800 bg-ink-900/80 px-6 py-4">
          <button
            disabled={!canLaunch}
            onClick={() => void launch()}
            className="w-full rounded-xl bg-iris-600 px-4 py-2.5 font-medium text-white transition hover:bg-iris-500 disabled:cursor-not-allowed disabled:opacity-40"
          >
            {buttonLabel}
          </button>
        </div>
      </div>
    </div>
  );
}
