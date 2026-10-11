/* Continuous mode: pick a set of dungeons and a concurrency, press Start, and
 * the harness keeps that many runs in flight — drawn from the set — until
 * told to stop. Hours later every failure is still here with its evidence.
 *
 * Two states. With no session running: the setup form (a shelved picker like
 * Launch's, a sticky tray of what is picked, presets, concurrency). With one
 * running: its header and controls, a per-dungeon table, failure clusters,
 * and the failures list — paged, never truncated. A past session opens the
 * same running view, read-only, at /continuous/<soakId>. */

import { useCallback, useEffect, useMemo, useState } from "react";
import { Link, useParams } from "react-router-dom";
import { api } from "../api/client";
import { fmtDuration, timeAgo, usePoll } from "../api/hooks";
import type {
  Catalogue,
  Dungeon,
  RunRecord,
  SoakCluster,
  SoakConfig,
  SoakEvidence,
  SoakIndex,
  SoakPerDungeon,
  SoakPoolEntry,
  SoakPreset,
  SoakRunRow,
  SoakRunsPage,
  SoakView,
} from "../api/types";
import { expansionOfRow, QUALITY_CHOICES } from "../data/wow";
import {
  Card,
  CardTitle,
  ConfirmButton,
  CopyButton,
  EmptyState,
  ExpandToggle,
  Field,
  FIELD,
  NumberBox,
  ResultPill,
  SELECT,
  Spinner,
  useModal,
  useToast,
} from "../components/ui";
import {
  EXP_SHELF,
  scenarioLabel,
  sectionsFor,
  Segmented,
  SHELVES,
  type Shelf,
} from "./LaunchPage";
import { RunDetail } from "./HistoryPage";

export const SOAK_SEEN_KEY = "testdeck.soak.seen";

type Difficulty = "normal" | "heroic" | "both";
type Selection = Map<string, Difficulty>;

const keyOf = (e: { token: string; heroic?: boolean }) =>
  e.token + (e.heroic ? ":heroic" : "");

function toPool(sel: Selection): SoakPoolEntry[] {
  const out: SoakPoolEntry[] = [];
  for (const [token, d] of sel) {
    if (d !== "heroic") out.push({ token, heroic: false });
    if (d !== "normal") out.push({ token, heroic: true });
  }
  return out;
}

function fromPool(pool: SoakPoolEntry[]): Selection {
  const sel: Selection = new Map();
  for (const e of pool) {
    const prev = sel.get(e.token);
    const now: Difficulty = e.heroic ? "heroic" : "normal";
    sel.set(e.token, prev && prev !== now ? "both" : now);
  }
  return sel;
}

/* Remember which session's failures this browser has seen, for the tab-title
   badge AppShell draws. */
export function markSoakSeen(soakId: string, fail: number) {
  try {
    localStorage.setItem(SOAK_SEEN_KEY, JSON.stringify({ soakId, fail }));
    window.dispatchEvent(new Event("testdeck-soak-seen"));
  } catch {
    /* storage unavailable: the badge just stays */
  }
}

function fmtBytes(n: number) {
  if (n < 1 << 10) return `${n} B`;
  if (n < 1 << 20) return `${(n / 1024).toFixed(0)} KB`;
  if (n < 1 << 30) return `${(n / (1 << 20)).toFixed(1)} MB`;
  return `${(n / (1 << 30)).toFixed(2)} GB`;
}

const pct = (a: number, b: number) => (b ? Math.round((a / b) * 100) : 0);

export default function ContinuousPage() {
  const { soakId } = useParams();
  const { data, error, refresh } = usePoll(
    () => api.get<SoakIndex>("/api/soak"),
    4000,
  );

  if (error)
    return (
      <EmptyState icon="⚠️" title="Cannot reach the server">
        {error}
      </EmptyState>
    );
  if (!data) return <Spinner label="loading…" />;

  if (soakId && soakId !== data.active?.soakId)
    return <PastSoak soakId={soakId} index={data} />;

  if (data.active)
    return <RunningView soak={data.active} index={data} onChange={refresh} />;

  return <SetupView index={data} onStarted={refresh} />;
}

/* ======================================================================= */
/* setup                                                                   */
/* ======================================================================= */

function SetupView({ index, onStarted }: { index: SoakIndex; onStarted: () => void }) {
  const toast = useToast();
  const { data: catalogue } = usePoll(
    () => api.get<Catalogue>("/api/testdungeons"),
    30000,
  );
  const [sel, setSel] = useState<Selection>(new Map());
  const [concurrent, setConcurrent] = useState("2");
  const [pick, setPick] = useState<"bag" | "random">("bag");
  const [more, setMore] = useState(false);
  const [ilvl, setIlvl] = useState("");
  const [noCap, setNoCap] = useState(false);
  const [quality, setQuality] = useState(0);
  const [level, setLevel] = useState("");
  const [seed, setSeed] = useState("");
  const [autoResume, setAutoResume] = useState(true);
  const [breaker, setBreaker] = useState("5");
  const [busy, setBusy] = useState(false);

  const all = useMemo(() => catalogue?.dungeons ?? [], [catalogue]);
  const byToken = useMemo(() => new Map(all.map((d) => [d.token, d])), [all]);
  const pool = toPool(sel);
  const n = parseInt(concurrent || "0", 10);

  const blocked = !index.supported
    ? "The worldserver predates continuous mode — rebuild mod-dungeon-clear."
    : !pool.length
      ? "Pick at least one dungeon."
      : !(n >= 1 && n <= 100)
        ? "Concurrency must be 1–100."
        : "";

  function start() {
    const cfg: SoakConfig = {
      pool,
      concurrent: n,
      pick,
      level: parseInt(level || "0", 10),
      seed: parseInt(seed || "0", 10),
      ilvl: noCap ? -1 : parseInt(ilvl || "0", 10),
      quality,
      autoResume,
      breaker: parseInt(breaker || "0", 10),
    };
    setBusy(true);
    api
      .post<SoakView>("/api/soak/start", cfg)
      .then((s) => {
        toast("ok", `Continuous session ${s.soakId} started`);
        onStarted();
      })
      .catch((e) => toast("error", e.message))
      .finally(() => setBusy(false));
  }

  if (!catalogue) return <Spinner label="loading catalogue…" />;

  return (
    <div>
      <div className="mb-4">
        <h1 className="text-2xl font-semibold">Continuous</h1>
        <p className="mt-1 max-w-2xl text-sm text-ink-400">
          Keep runs going around the clock: pick dungeons and a concurrency,
          and the harness keeps that many in flight, drawn from your pick,
          until you stop it. Every failure is kept with its logs — even
          across worldserver restarts.
        </p>
      </div>

      {!index.supported && (
        <div className="mb-4 rounded-xl border border-amber-900/50 bg-amber-950/30 px-4 py-3 text-sm text-amber-200/90">
          This worldserver's dungeon catalogue does not advertise pool plans
          (<span className="font-mono">limits.planPool</span>), so it cannot
          run a continuous session. Rebuild and restart it with the current
          mod-dungeon-clear.
        </div>
      )}

      <div className="grid gap-5 xl:grid-cols-[1fr_22rem]">
        <DungeonPicker all={all} byToken={byToken} sel={sel} setSel={setSel} />

        <div className="space-y-4 xl:sticky xl:top-4 xl:self-start">
          <SelectedTray sel={sel} setSel={setSel} byToken={byToken} />
          <PresetsCard pool={pool} setSel={setSel} index={index} />

          <Card>
            <CardTitle>Run</CardTitle>
            <div className="space-y-4">
              <Field label="Concurrent runs">
                <Stepper value={concurrent} onChange={setConcurrent} />
              </Field>
              <BotEstimate pool={pool} byToken={byToken} concurrent={n}
                           pool_size={index.addclassPool}
                           maxConcurrent={catalogue.limits?.maxConcurrent ?? 0} />
              <Field label="Order">
                <Segmented
                  value={pick}
                  onChange={setPick}
                  options={[["bag", "Balanced"], ["random", "Pure random"]]}
                />
                <p className="mt-1.5 text-xs text-ink-500">
                  {pick === "bag"
                    ? "Every dungeon runs once per round, in a shuffled order."
                    : "Each launch is an independent draw — some dungeons may go a while unpicked."}
                </p>
              </Field>

              <button
                type="button"
                onClick={() => setMore(!more)}
                className="text-xs text-ink-500 hover:text-ink-300"
              >
                {more ? "▾" : "▸"} More
              </button>
              {more && (
                <div className="space-y-3">
                  <div className="grid grid-cols-2 gap-3">
                    <Field label="Item level" hint="blank = server">
                      <NumberBox value={noCap ? "" : ilvl} onChange={setIlvl}
                                 placeholder={noCap ? "no cap" : "default"} />
                    </Field>
                    <Field label="Quality">
                      <select className={SELECT} value={quality}
                              onChange={(e) => setQuality(parseInt(e.target.value, 10))}>
                        <option value={0}>server default</option>
                        {QUALITY_CHOICES.map((q) => (
                          <option key={q.v} value={q.v}>{q.label}</option>
                        ))}
                      </select>
                    </Field>
                  </div>
                  <label className="flex items-center gap-2 text-sm text-ink-300">
                    <input type="checkbox" checked={noCap}
                           onChange={(e) => setNoCap(e.target.checked)} />
                    no item-level cap
                  </label>
                  <div className="grid grid-cols-2 gap-3">
                    <Field label="Level" hint="blank = dungeon">
                      <NumberBox value={level} onChange={setLevel} placeholder="default" />
                    </Field>
                    <Field label="Seed" hint="reproducible">
                      <NumberBox value={seed} onChange={setSeed} placeholder="random" />
                    </Field>
                  </div>
                  <label className="flex items-center gap-2 text-sm text-ink-300">
                    <input type="checkbox" checked={autoResume}
                           onChange={(e) => setAutoResume(e.target.checked)} />
                    resume after a worldserver restart
                  </label>
                  <Field label="Circuit breaker"
                         hint="pause after this many setup failures in a row (0 = off)">
                    <NumberBox value={breaker} onChange={setBreaker} />
                  </Field>
                </div>
              )}

              <button
                type="button"
                disabled={!!blocked || busy}
                onClick={start}
                title={blocked}
                className="w-full rounded-xl bg-iris-600 px-4 py-2.5 text-sm font-semibold text-white transition hover:bg-iris-500 disabled:cursor-not-allowed disabled:bg-ink-800 disabled:text-ink-500"
              >
                {busy ? "Starting…" : "Start continuous"}
              </button>
              {blocked && <p className="text-xs text-ink-500">{blocked}</p>}
            </div>
          </Card>
        </div>
      </div>

      <RecentSessions index={index} />
    </div>
  );
}

function Stepper({ value, onChange }: { value: string; onChange: (v: string) => void }) {
  const n = parseInt(value || "0", 10);
  const btn =
    "rounded-lg border border-ink-700 px-3 py-2 text-sm text-ink-300 hover:border-ink-500 disabled:opacity-40";
  return (
    <div className="flex items-center gap-2">
      <button type="button" className={btn} disabled={n <= 1}
              onClick={() => onChange(String(Math.max(1, n - 1)))}>−</button>
      <NumberBox value={value} onChange={onChange} />
      <button type="button" className={btn} disabled={n >= 100}
              onClick={() => onChange(String(Math.min(100, n + 1)))}>+</button>
    </div>
  );
}

/* Bots in flight: Σ party size over what might be running at once, against
   the addclass pool every party is drawn from (harness bots are exempt from
   AiPlayerbot.MaxAddedBots). A raid entry fields its default size, so a
   raid-heavy pool at a high concurrency is where this turns amber. */
function BotEstimate({
  pool,
  byToken,
  concurrent,
  pool_size,
  maxConcurrent,
}: {
  pool: SoakPoolEntry[];
  byToken: Map<string, Dungeon>;
  concurrent: number;
  pool_size: number | null;
  maxConcurrent: number;
}) {
  if (!pool.length || !concurrent) return null;
  const sizes = pool.map((e) => {
    const d = byToken.get(e.token);
    return d?.raid ? d.defaultSize ?? 10 : 5;
  });
  const avg = sizes.reduce((a, b) => a + b, 0) / sizes.length;
  const worst = [...sizes].sort((a, b) => b - a).slice(0, concurrent);
  const peak = worst.reduce((a, b) => a + b, 0) +
    Math.max(0, concurrent - worst.length) * (worst[worst.length - 1] ?? 5);
  const typical = Math.round(avg * concurrent);
  const over = (pool_size !== null && peak > pool_size) || (maxConcurrent > 0 && concurrent > maxConcurrent);
  return (
    <div className={`rounded-lg px-3 py-2 text-xs ${over
      ? "border border-amber-800/60 bg-amber-950/30 text-amber-200/90"
      : "bg-ink-950/50 text-ink-400"}`}>
      ~{typical} bots in flight, up to {peak} when the biggest entries overlap
      {pool_size !== null && <> · addclass pool {pool_size} characters</>}
      {maxConcurrent > 0 && <> · server cap {maxConcurrent} runs</>}
      {over && (
        <div className="mt-1">
          More than the pool can field at once: launches will back off and
          wait for a run to finish (grow it with <code>.playerbots addclass</code>).
        </div>
      )}
    </div>
  );
}

function DungeonPicker({
  all,
  byToken,
  sel,
  setSel,
}: {
  all: Dungeon[];
  byToken: Map<string, Dungeon>;
  sel: Selection;
  setSel: (s: Selection) => void;
}) {
  const [shelf, setShelf] = useState<Shelf>("all");
  const [query, setQuery] = useState("");
  const q = query.trim().toLowerCase();
  const sections = useMemo(() => {
    const matched = q
      ? all.filter((d) => d.name.toLowerCase().includes(q) || d.token.toLowerCase().includes(q))
      : all;
    return sectionsFor(matched, q ? "all" : shelf, byToken);
  }, [all, q, shelf, byToken]);
  const counts = useMemo(() => {
    const c: Record<Shelf, number> = { all: 0, classic: 0, tbc: 0, wotlk: 0, raids: 0 };
    for (const d of all) {
      if (d.scenario) continue;
      c.all++;
      c[EXP_SHELF[expansionOfRow(d)]]++;
      if (d.raid) c.raids++;
    }
    return c;
  }, [all]);

  const toggle = (d: Dungeon) => {
    const next = new Map(sel);
    if (next.has(d.token)) next.delete(d.token);
    else next.set(d.token, "normal");
    setSel(next);
  };
  const setDiff = (d: Dungeon, v: Difficulty) => {
    const next = new Map(sel);
    next.set(d.token, v);
    setSel(next);
  };
  const selectAll = (items: Dungeon[]) => {
    const next = new Map(sel);
    const allIn = items.every((d) => next.has(d.token));
    for (const d of items) {
      if (allIn) next.delete(d.token);
      else if (!next.has(d.token)) next.set(d.token, "normal");
    }
    setSel(next);
  };

  return (
    <div className="min-w-0">
      <div className="mb-3 flex flex-wrap items-center gap-1.5">
        {SHELVES.map(([v, label]) => (
          <button
            key={v}
            type="button"
            onClick={() => {
              setShelf(v);
              setQuery("");
            }}
            className={`rounded-full border px-3 py-1.5 text-sm transition ${
              shelf === v && !q
                ? "border-iris-500/40 bg-iris-500/15 text-iris-100"
                : "border-ink-800 bg-ink-900/60 text-ink-400 hover:border-ink-700 hover:text-ink-200"
            }`}
          >
            {label}
            <span className="ml-1.5 text-xs text-ink-600">{counts[v]}</span>
          </button>
        ))}
        <input
          className={`${FIELD} ml-auto max-w-xs sm:w-56`}
          placeholder="Search dungeons…"
          value={query}
          onChange={(e) => setQuery(e.target.value)}
        />
      </div>

      {sections.map((s) => {
        const rows = [...s.items, ...s.scenarios];
        const allIn = rows.length > 0 && rows.every((d) => sel.has(d.token));
        return (
          <section key={s.title} className="mb-5">
            <h2 className="mb-2 flex items-baseline gap-2 text-xs font-semibold uppercase tracking-wider text-ink-500">
              {s.title}
              <span className="font-normal text-ink-600">{s.items.length}</span>
              <button
                type="button"
                onClick={() => selectAll(rows)}
                className="ml-auto font-normal normal-case tracking-normal text-iris-300/80 hover:text-iris-200"
              >
                {allIn ? "clear section" : "select all in section"}
              </button>
            </h2>
            <div className="grid grid-cols-1 gap-1.5 sm:grid-cols-2">
              {rows.map((d) => (
                <PickRow
                  key={d.token}
                  d={d}
                  label={d.scenario ? scenarioLabel(d, byToken.get(d.scenarioOf ?? "")) : d.name}
                  value={sel.get(d.token)}
                  onToggle={() => toggle(d)}
                  onDifficulty={(v) => setDiff(d, v)}
                />
              ))}
            </div>
          </section>
        );
      })}
      {!sections.length && <EmptyState icon="🔍" title={`No dungeon matches “${query}”`} />}
    </div>
  );
}

function PickRow({
  d,
  label,
  value,
  onToggle,
  onDifficulty,
}: {
  d: Dungeon;
  label: string;
  value?: Difficulty;
  onToggle: () => void;
  onDifficulty: (v: Difficulty) => void;
}) {
  const on = value !== undefined;
  return (
    <div
      className={`flex items-center gap-2 rounded-xl border px-3 py-2 transition ${
        on ? "border-iris-500/50 bg-iris-500/10" : "border-ink-800 bg-ink-900/60 hover:border-ink-700"
      }`}
    >
      <label className="flex min-w-0 flex-1 cursor-pointer items-center gap-2">
        <input type="checkbox" checked={on} onChange={onToggle} />
        <span className="min-w-0 flex-1 truncate text-sm text-ink-100">{label}</span>
        <span className="hidden shrink-0 font-mono text-xs text-ink-600 sm:inline">{d.token}</span>
        {d.raid && d.defaultSize ? (
          <span className="shrink-0 rounded-full bg-ink-800 px-2 py-0.5 text-xs text-ink-400">
            {d.defaultSize}-man
          </span>
        ) : null}
      </label>
      {on && d.heroicLevel > 0 && (
        <div className="flex shrink-0 gap-0.5 rounded-lg border border-ink-800 bg-ink-950/70 p-0.5">
          {(["normal", "heroic", "both"] as Difficulty[]).map((v) => (
            <button
              key={v}
              type="button"
              onClick={() => onDifficulty(v)}
              className={`rounded-md px-1.5 py-0.5 text-[11px] capitalize ${
                value === v
                  ? v === "normal"
                    ? "bg-iris-500/20 text-iris-100"
                    : "bg-fuchsia-500/20 text-fuchsia-200"
                  : "text-ink-500 hover:text-ink-300"
              }`}
            >
              {v}
            </button>
          ))}
        </div>
      )}
    </div>
  );
}

function SelectedTray({
  sel,
  setSel,
  byToken,
}: {
  sel: Selection;
  setSel: (s: Selection) => void;
  byToken: Map<string, Dungeon>;
}) {
  const pool = toPool(sel);
  return (
    <Card>
      <CardTitle
        right={
          pool.length > 0 && (
            <button type="button" onClick={() => setSel(new Map())}
                    className="text-xs text-ink-500 hover:text-ink-300">
              clear
            </button>
          )
        }
      >
        Pool · {pool.length}
      </CardTitle>
      {!pool.length ? (
        <p className="text-sm text-ink-500">Nothing picked yet.</p>
      ) : (
        <div className="flex max-h-56 flex-wrap gap-1.5 overflow-y-auto">
          {pool.map((e) => (
            <button
              key={keyOf(e)}
              type="button"
              title="remove"
              onClick={() => {
                const next = new Map(sel);
                const cur = next.get(e.token);
                if (cur === "both") next.set(e.token, e.heroic ? "normal" : "heroic");
                else next.delete(e.token);
                setSel(next);
              }}
              className={`rounded-full px-2.5 py-1 text-xs ${
                e.heroic ? "bg-fuchsia-500/15 text-fuchsia-200" : "bg-iris-500/15 text-iris-100"
              } hover:line-through`}
            >
              {byToken.get(e.token)?.name ?? e.token}
              {e.heroic && " (H)"}
            </button>
          ))}
        </div>
      )}
    </Card>
  );
}

function PresetsCard({
  pool,
  setSel,
  index,
}: {
  pool: SoakPoolEntry[];
  setSel: (s: Selection) => void;
  index: SoakIndex;
}) {
  const toast = useToast();
  const { data, refresh } = usePoll(
    () => api.get<{ presets: SoakPreset[] }>("/api/soak-presets"),
    30000,
  );
  const [name, setName] = useState("");
  const presets = data?.presets ?? [];
  const withFails = index.recent.filter((s) => s.fail > 0).slice(0, 10);

  function save() {
    api
      .post("/api/soak-presets", { name: name.trim(), pool })
      .then(() => {
        toast("ok", `Saved preset “${name.trim()}”`);
        setName("");
        refresh();
      })
      .catch((e) => toast("error", e.message));
  }

  function rerunFailures(soakId: string) {
    api
      .get<SoakView>(`/api/soak/${soakId}`)
      .then((s) => {
        const failed = (s.stats?.perDungeon ?? []).filter((p) => p.fail > 0);
        setSel(fromPool(failed.map((p) => ({ token: p.dungeon, heroic: p.heroic }))));
        toast("ok", `Picked the ${failed.length} dungeon(s) that failed in ${soakId}`);
      })
      .catch((e) => toast("error", e.message));
  }

  return (
    <Card>
      <CardTitle>Presets</CardTitle>
      <div className="space-y-1">
        {presets.map((p) => (
          <div key={p.name} className="flex items-center gap-2 text-sm">
            <button type="button" onClick={() => setSel(fromPool(p.pool))}
                    className="min-w-0 flex-1 truncate text-left text-ink-200 hover:text-iris-200">
              {p.name}
              <span className="ml-2 text-xs text-ink-600">{p.pool.length}</span>
            </button>
            <span className="text-xs text-ink-600">{p.owner}</span>
            {p.writable && (
              <ConfirmButton
                label="✕"
                message={`Delete preset “${p.name}”?`}
                className="text-xs text-ink-600 hover:text-red-300"
                onConfirm={() =>
                  api
                    .del(`/api/soak-presets/${encodeURIComponent(p.name)}`)
                    .then(refresh)
                    .catch((e) => toast("error", e.message))
                }
              />
            )}
          </div>
        ))}
        {!presets.length && <p className="text-xs text-ink-500">No saved pools yet.</p>}
      </div>
      <div className="mt-3 flex gap-2">
        <input className={FIELD} placeholder="Save current pool as…" value={name}
               onChange={(e) => setName(e.target.value)} maxLength={60} />
        <button type="button" disabled={!name.trim() || !pool.length} onClick={save}
                className="rounded-lg border border-ink-700 px-3 text-sm text-ink-300 hover:border-iris-500/50 disabled:opacity-40">
          Save
        </button>
      </div>
      {withFails.length > 0 && (
        <div className="mt-3">
          <select
            className={SELECT}
            value=""
            onChange={(e) => e.target.value && rerunFailures(e.target.value)}
          >
            <option value="">Re-run failures from…</option>
            {withFails.map((s) => (
              <option key={s.soakId} value={s.soakId}>
                {s.soakId} — {s.fail} failed
              </option>
            ))}
          </select>
        </div>
      )}
    </Card>
  );
}

function RecentSessions({ index }: { index: SoakIndex }) {
  if (!index.recent.length) return null;
  return (
    <div className="mt-8">
      <h2 className="mb-2 text-xs font-semibold uppercase tracking-wider text-ink-500">
        Past sessions
      </h2>
      <SessionList sessions={index.recent} />
    </div>
  );
}

export function SessionList({ sessions }: { sessions: SoakView[] }) {
  return (
    <div className="space-y-1.5">
      {sessions.map((s) => (
        <Link
          key={s.soakId}
          to={`/continuous/${s.soakId}`}
          className="flex flex-wrap items-center gap-x-3 gap-y-1 rounded-xl border border-ink-800 bg-ink-900/60 px-4 py-2.5 text-sm hover:border-iris-500/40"
        >
          <span className="font-mono text-xs text-ink-500">{s.soakId}</span>
          <StatusPill status={s.status} restarts={0} />
          <span className="text-ink-300">
            {s.config.pool.length} dungeon{s.config.pool.length === 1 ? "" : "s"}
          </span>
          <span className="text-ink-400">{s.runs} runs</span>
          <span className="text-emerald-300">{pct(s.ok, s.runs)}% ok</span>
          {s.fail > 0 && <span className="text-red-300">{s.fail} failed</span>}
          <span className="ml-auto text-xs text-ink-600">
            {s.owner} · {timeAgo(s.createdAtMs)}
            {s.stoppedAtMs
              ? ` · ran ${fmtDuration((s.stoppedAtMs - s.createdAtMs) / 1000)}`
              : ""}
          </span>
        </Link>
      ))}
    </div>
  );
}

/* ======================================================================= */
/* running (and read-only past sessions)                                   */
/* ======================================================================= */

function PastSoak({ soakId, index }: { soakId: string; index: SoakIndex }) {
  const { data, error, refresh } = usePoll(
    () => api.get<SoakView>(`/api/soak/${soakId}`),
    15000,
  );
  if (error)
    return (
      <EmptyState icon="🗂️" title={`No session ${soakId}`}>
        {error} · <Link className="text-iris-300" to="/continuous">back</Link>
      </EmptyState>
    );
  if (!data) return <Spinner label="loading session…" />;
  return <RunningView soak={data} index={index} onChange={refresh} readOnly />;
}

function StatusPill({ status, restarts }: { status: string; restarts: number }) {
  const tone =
    status === "running"
      ? "bg-emerald-500/15 text-emerald-300"
      : status === "stopped"
        ? "bg-ink-700/40 text-ink-300"
        : status === "server down"
          ? "bg-red-500/15 text-red-300"
          : "bg-amber-500/15 text-amber-300";
  return (
    <span className={`rounded-full px-2.5 py-0.5 text-xs font-medium ${tone}`}>
      {status}
      {restarts > 0 && status !== "stopped" && ` · resumed ×${restarts}`}
    </span>
  );
}

function RunningView({
  soak,
  index,
  onChange,
  readOnly = false,
}: {
  soak: SoakView;
  index: SoakIndex;
  onChange: () => void;
  readOnly?: boolean;
}) {
  const toast = useToast();
  const stats = soak.stats;
  const running = !readOnly && soak.status !== "stopped";
  const canControl = running && (index.admin || index.me === soak.owner);
  const [dungeonFilter, setDungeonFilter] = useState("");
  const [clusterFilter, setClusterFilter] = useState("");
  const [editing, setEditing] = useState(false);
  const { data: catalogue } = usePoll(() => api.get<Catalogue>("/api/testdungeons"), 60000);
  const names = useMemo(
    () => new Map((catalogue?.dungeons ?? []).map((d) => [d.token, d.name])),
    [catalogue],
  );

  /* This browser has now seen every failure so far. */
  useEffect(() => {
    if (!readOnly && !document.hidden) markSoakSeen(soak.soakId, soak.fail);
  }, [soak.soakId, soak.fail, readOnly]);

  const act = (path: string, body?: unknown, done?: string) =>
    api
      .post(`/api/soak/${soak.soakId}/${path}`, body)
      .then(() => {
        if (done) toast("ok", done);
        onChange();
      })
      .catch((e) => toast("error", e.message));

  const plan = soak.plan;
  const elapsed = stats?.elapsedS ?? 0;
  const paused = soak.status === "paused";
  const holdUntil = plan?.resetHoldUntil
    ? new Date(plan.resetHoldUntil * 1000).toLocaleTimeString([], {
        hour: "2-digit",
        minute: "2-digit",
      })
    : "";

  return (
    <div>
      <div className="mb-4 flex flex-wrap items-end justify-between gap-3">
        <div>
          <h1 className="text-2xl font-semibold">
            Continuous{" "}
            <span className="font-mono text-base text-ink-500">{soak.soakId}</span>{" "}
            <CopyButton text={soak.soakId} />
          </h1>
          <p className="mt-1 text-sm text-ink-400">
            started by {soak.owner} {timeAgo(soak.createdAtMs)}
            {readOnly && (
              <>
                {" "}· <Link className="text-iris-300 hover:text-iris-200" to="/continuous">
                  back to Continuous
                </Link>
              </>
            )}
          </p>
        </div>
        {!running && (index.admin || index.me === soak.owner) && soak.status === "stopped" && (
          <ConfirmButton
            label="Delete session"
            message={`Delete ${soak.soakId} and all of its evidence (${fmtBytes(stats?.diskBytes ?? 0)})? This cannot be undone.`}
            className="rounded-lg border border-ink-800 px-3 py-1.5 text-xs text-ink-500 hover:border-red-900 hover:text-red-300"
            onConfirm={() =>
              api
                .del(`/api/soak/${soak.soakId}`)
                .then(() => {
                  toast("ok", `Deleted ${soak.soakId}`);
                  window.history.pushState({}, "", "/continuous");
                  window.dispatchEvent(new PopStateEvent("popstate"));
                })
                .catch((e) => toast("error", e.message))
            }
          />
        )}
      </div>

      <Card className="mb-4">
        <div className="flex flex-wrap items-center gap-x-5 gap-y-2">
          <StatusPill status={soak.status} restarts={soak.restarts} />
          {soak.status === "holding" && holdUntil && (
            <span className="text-xs text-amber-300/90">holding for instance reset until {holdUntil}</span>
          )}
          {soak.status !== "holding" && holdUntil && running && (
            <span className="text-xs text-amber-300/80">
              heroics &amp; raids held until {holdUntil} (instance reset)
            </span>
          )}
          <Stat label="elapsed" value={fmtDuration(elapsed)} />
          <Stat label="runs" value={String(soak.runs)} />
          <Stat label="success" value={`${pct(soak.ok, soak.runs)}%`} tone="ok" />
          <Stat label="failed" value={String(soak.fail)} tone={soak.fail ? "bad" : ""} />
          {soak.lost > 0 && <Stat label="lost" value={String(soak.lost)} tone="bad" />}
          <Stat label="runs / h" value={stats?.runsPerHour ? String(stats.runsPerHour) : "–"} />
          {running && (
            <Stat label="active" value={`${plan?.active ?? soak.liveRuns.length} / ${soak.config.concurrent}`} />
          )}
          {running && plan?.nextPick && <Stat label="next" value={plan.nextPick} />}
          <Stat label="disk" value={fmtBytes(stats?.diskBytes ?? 0)} />

          {canControl && (
            <div className="ml-auto flex flex-wrap items-center gap-2">
              <button
                type="button"
                onClick={() => act(paused ? "resume" : "pause", undefined, paused ? "Resumed" : "Paused — live runs finish")}
                className="rounded-lg border border-ink-700 px-3 py-1.5 text-sm text-ink-200 hover:border-iris-500/50"
              >
                {paused ? "Resume" : "Pause"}
              </button>
              <button
                type="button"
                disabled={!soak.currentPlanId}
                onClick={() => setEditing(true)}
                className="rounded-lg border border-ink-700 px-3 py-1.5 text-sm text-ink-200 hover:border-iris-500/50 disabled:opacity-40"
              >
                Edit pool
              </button>
              <ConfirmButton
                label="Finish & stop"
                confirmLabel="Finish current runs"
                message={`Stop launching and let the ${soak.liveRuns.length} run(s) in flight finish, then end the session?`}
                className="rounded-lg border border-red-900/60 px-3 py-1.5 text-sm text-red-300 hover:bg-red-950/40"
                onConfirm={() => act("stop", { mode: "drain" }, "Draining — the session ends when live runs finish")}
              />
              <ConfirmButton
                label="Stop now"
                confirmLabel="Abort and stop"
                message={`Abort the ${soak.liveRuns.length} run(s) in flight and end the session now? They are recorded as aborted.`}
                className="rounded-lg px-2 py-1.5 text-xs text-ink-500 hover:text-red-300"
                onConfirm={() => act("stop", { mode: "now" }, "Stopping")}
              />
            </div>
          )}
        </div>
        {soak.statusDetail && (
          <div className="mt-2 text-sm text-amber-300/90">{soak.statusDetail}</div>
        )}
        <div className="mt-3 flex flex-wrap gap-1.5 text-xs text-ink-500">
          <span>pool:</span>
          {soak.config.pool.map((e) => (
            <span key={keyOf(e)} className={`rounded px-1.5 py-0.5 ${e.heroic ? "bg-fuchsia-500/10 text-fuchsia-200/80" : "bg-ink-800 text-ink-300"}`}>
              {keyOf(e)}
            </span>
          ))}
          <span className="ml-2">· {soak.config.pick === "random" ? "pure random" : "balanced"}</span>
          {soak.planIds.length > 0 && (
            <span className="ml-2 font-mono">· {soak.planIds.join(", ")}</span>
          )}
        </div>
      </Card>

      {running && soak.liveRuns.length > 0 && <LiveStrip runs={soak.liveRuns} />}

      <div className="grid gap-4 xl:grid-cols-[3fr_2fr]">
        <PerDungeonTable
          rows={stats?.perDungeon ?? []}
          names={names}
          active={dungeonFilter}
          onPick={(k) => {
            setDungeonFilter(k === dungeonFilter ? "" : k);
            setClusterFilter("");
          }}
        />
        <Clusters
          clusters={stats?.clusters ?? []}
          active={clusterFilter}
          onPick={(c) => {
            setClusterFilter(c === clusterFilter ? "" : c);
            setDungeonFilter("");
          }}
        />
      </div>

      <FailuresList
        soakId={soak.soakId}
        dungeon={dungeonFilter}
        cluster={clusterFilter}
        version={soak.fail}
        canCapture={index.admin || index.me === soak.owner}
        onClear={() => {
          setDungeonFilter("");
          setClusterFilter("");
        }}
      />

      {editing && (
        <EditPoolDialog soak={soak} onClose={() => setEditing(false)} onSaved={onChange} />
      )}
    </div>
  );
}

function Stat({ label, value, tone = "" }: { label: string; value: string; tone?: "" | "ok" | "bad" }) {
  const cls = tone === "ok" ? "text-emerald-300" : tone === "bad" ? "text-red-300" : "text-ink-100";
  return (
    <div className="leading-tight">
      <div className="text-[10px] uppercase tracking-wider text-ink-600">{label}</div>
      <div className={`text-sm font-medium tabular-nums ${cls}`}>{value}</div>
    </div>
  );
}

function LiveStrip({ runs }: { runs: SoakView["liveRuns"] }) {
  return (
    <div className="mb-4 grid grid-cols-[repeat(auto-fill,minmax(13rem,1fr))] gap-2">
      {runs.map((r) => (
        <Link
          key={r.runId}
          to="/live"
          className="min-w-0 rounded-xl border border-ink-800 bg-ink-900/60 px-3 py-2 text-xs hover:border-iris-500/40"
        >
          <div className="flex items-center gap-1.5">
            <span className={`h-2 w-2 rounded-full ${r.wiped ? "bg-red-400" : r.inCombat ? "bg-amber-400" : "bg-emerald-400"}`} />
            <span className="truncate font-medium text-ink-100">{r.dungeonName || r.dungeon}</span>
            {r.heroic && <span className="text-fuchsia-300">H</span>}
            <span className="ml-auto shrink-0 whitespace-nowrap text-ink-500">{fmtDuration(r.elapsedS)}</span>
          </div>
          <div className="mt-1 truncate text-ink-400">
            {r.bossesKilled ?? 0}/{r.bossesTotal ?? "?"} · {r.stall || r.bossName || r.stage || r.state}
          </div>
        </Link>
      ))}
    </div>
  );
}

function DotStrip({ results }: { results: string[] }) {
  return (
    <span className="inline-flex gap-0.5">
      {results.map((r, i) => (
        <span
          key={i}
          title={r}
          className={`inline-block h-2 w-2 rounded-full ${
            r === "success" ? "bg-emerald-400" : r === "lost" ? "bg-fuchsia-400" : "bg-red-400"
          }`}
        />
      ))}
    </span>
  );
}

type PerView = "tiles" | "table";
const PER_VIEW_KEY = "testdeck.soak.perView";

/* Worst first: dungeons that have run sort by success rate, then by failure
   count; dungeons still waiting for their first run go last. */
function sortPerDungeon(rows: SoakPerDungeon[], name: (r: SoakPerDungeon) => string) {
  return [...rows].sort(
    (a, b) =>
      (a.runs ? 0 : 1) - (b.runs ? 0 : 1) ||
      (a.runs && b.runs ? a.ok / a.runs - b.ok / b.runs : 0) ||
      b.fail - a.fail ||
      b.runs - a.runs ||
      name(a).localeCompare(name(b)),
  );
}

function PerDungeonTable({
  rows,
  names,
  active,
  onPick,
}: {
  rows: SoakPerDungeon[];
  /* The stats only carry a name once a dungeon has run; the catalogue fills
     in the rest so waiting tiles don't show bare tokens. */
  names: Map<string, string>;
  active: string;
  onPick: (key: string) => void;
}) {
  const [view, setViewState] = useState<PerView>(() => {
    try {
      return localStorage.getItem(PER_VIEW_KEY) === "table" ? "table" : "tiles";
    } catch {
      return "tiles";
    }
  });
  function setView(v: PerView) {
    setViewState(v);
    try {
      localStorage.setItem(PER_VIEW_KEY, v);
    } catch {
      /* per-viewer convenience only */
    }
  }
  const nameOf = useCallback(
    (r: SoakPerDungeon) => r.dungeonName || names.get(r.dungeon) || r.dungeon,
    [names],
  );
  const sorted = useMemo(() => sortPerDungeon(rows, nameOf), [rows, nameOf]);
  const ran = rows.filter((r) => r.runs).length;
  const failing = rows.filter((r) => r.fail).length;

  return (
    <Card className="min-w-0 !p-0">
      <div className="px-5 pt-4">
        <CardTitle
          right={
            <div className="flex items-center gap-3">
              {rows.length > 0 && (
                <span className="text-xs text-ink-500">
                  {ran}/{rows.length} run
                  {failing > 0 && <span className="text-red-300/90"> · {failing} failing</span>}
                </span>
              )}
              <div className="flex gap-0.5 rounded-lg border border-ink-800 bg-ink-950/70 p-0.5">
                {(["tiles", "table"] as PerView[]).map((v) => (
                  <button
                    key={v}
                    type="button"
                    onClick={() => setView(v)}
                    className={`rounded-md px-2 py-0.5 text-xs transition ${
                      view === v ? "bg-iris-500/20 text-iris-100" : "text-ink-500 hover:text-ink-200"
                    }`}
                  >
                    {v}
                  </button>
                ))}
              </div>
            </div>
          }
        >
          Per dungeon
        </CardTitle>
      </div>
      {!rows.length ? (
        <p className="px-5 pb-4 text-sm text-ink-500">No runs yet.</p>
      ) : view === "tiles" ? (
        <div className="grid grid-cols-[repeat(auto-fill,minmax(7.5rem,1fr))] gap-1 px-5 pb-5">
          {sorted.map((r) => (
            <PerDungeonTile key={r.key} r={r} name={nameOf(r)} active={active === r.key} onPick={onPick} />
          ))}
        </div>
      ) : (
        <div className="max-h-[28rem] overflow-auto">
          <table className="w-full text-sm">
            <thead className="sticky top-0 bg-ink-900 text-left text-[10px] uppercase tracking-wider text-ink-600">
              <tr>
                <th className="px-5 py-1.5 font-medium">dungeon</th>
                <th className="px-2 font-medium">runs</th>
                <th className="px-2 font-medium">ok</th>
                <th className="px-2 font-medium">fail</th>
                <th className="px-2 font-medium">rate</th>
                <th className="px-2 font-medium">median</th>
                <th className="px-2 pr-5 font-medium">last 20</th>
              </tr>
            </thead>
            <tbody>
              {sorted.map((r) => {
                const rate = pct(r.ok, r.runs);
                return (
                  <tr
                    key={r.key}
                    onClick={() => onPick(r.key)}
                    className={`cursor-pointer border-t border-ink-800/60 ${active === r.key ? "bg-iris-500/10" : "hover:bg-ink-800/30"}`}
                  >
                    <td className="px-5 py-1.5">
                      <span className="text-ink-100">{nameOf(r)}</span>
                      {r.heroic && <span className="ml-1.5 text-xs text-fuchsia-300">H</span>}
                    </td>
                    <td className="px-2 tabular-nums text-ink-300">{r.runs}</td>
                    <td className="px-2 tabular-nums text-emerald-300">{r.ok}</td>
                    <td className={`px-2 tabular-nums ${r.fail ? "text-red-300" : "text-ink-600"}`}>{r.fail}</td>
                    <td className="px-2">
                      {r.runs ? (
                        <div className="flex items-center gap-1.5">
                          <div className="h-1.5 w-14 overflow-hidden rounded-full bg-red-500/40">
                            <div className="h-full bg-emerald-400" style={{ width: `${rate}%` }} />
                          </div>
                          <span className="text-xs tabular-nums text-ink-400">{rate}%</span>
                        </div>
                      ) : (
                        <span className="text-xs text-ink-600">–</span>
                      )}
                    </td>
                    <td className="px-2 text-xs tabular-nums text-ink-400">
                      {r.medianS ? fmtDuration(r.medianS) : "–"}
                    </td>
                    <td className="px-2 pr-5"><DotStrip results={r.last} /></td>
                  </tr>
                );
              })}
            </tbody>
          </table>
        </div>
      )}
    </Card>
  );
}

/* Tiles are narrow: "Scarlet Monastery: Library" -> "SM: Library",
   "Ahn'kahet: The Old Kingdom" -> "Ahn'kahet", "The Nexus" -> "Nexus". */
function tileLabel(name: string) {
  const [head, wing] = name.split(": ");
  if (wing) {
    const words = head.split(" ");
    return words.length > 1 ? `${words.map((w) => w[0]).join("")}: ${wing}` : head;
  }
  return name.replace(/^The /, "");
}

function PerDungeonTile({
  r,
  name,
  active,
  onPick,
}: {
  r: SoakPerDungeon;
  name: string;
  active: boolean;
  onPick: (key: string) => void;
}) {
  const rate = pct(r.ok, r.runs);
  const tone = !r.runs
    ? "border-ink-800/70 bg-ink-950/30 text-ink-500"
    : !r.fail
      ? "border-emerald-500/25 bg-emerald-500/[0.07] text-ink-100"
      : rate >= 50
        ? "border-amber-500/30 bg-amber-500/[0.08] text-ink-100"
        : "border-red-500/35 bg-red-500/10 text-ink-100";
  const tip = [
    `${name}${r.heroic ? " (heroic)" : ""}`,
    r.runs ? `${r.ok}/${r.runs} ok (${rate}%)` : "not run yet",
    r.medianS ? `median ${fmtDuration(r.medianS)}` : "",
    r.last.length ? `last: ${r.last.join(" ")}` : "",
  ].filter(Boolean).join("\n");
  return (
    <button
      type="button"
      onClick={() => onPick(r.key)}
      title={tip}
      className={`flex min-w-0 items-center gap-1 rounded-md border px-2 py-1 text-left text-xs transition hover:border-iris-500/50 ${tone} ${
        active ? "ring-1 ring-iris-400" : ""
      }`}
    >
      <span className="min-w-0 flex-1 truncate">{tileLabel(name)}</span>
      {r.heroic && <span className="shrink-0 text-fuchsia-300">H</span>}
      {r.runs > 0 && (
        <span className={`shrink-0 tabular-nums ${r.fail ? "text-red-300" : "text-emerald-300"}`}>
          {r.ok}/{r.runs}
        </span>
      )}
    </button>
  );
}

function Clusters({
  clusters,
  active,
  onPick,
}: {
  clusters: SoakCluster[];
  active: string;
  onPick: (label: string) => void;
}) {
  return (
    <Card className="min-w-0">
      <CardTitle>Failure clusters</CardTitle>
      {!clusters.length && <p className="text-sm text-ink-500">No failures yet.</p>}
      <div className="max-h-80 space-y-1 overflow-y-auto">
        {clusters.map((c) => (
          <button
            key={c.label}
            type="button"
            onClick={() => onPick(c.label)}
            title={c.sample}
            className={`flex w-full items-start gap-2 rounded-lg px-2 py-1.5 text-left text-sm ${active === c.label ? "bg-iris-500/10" : "hover:bg-ink-800/40"}`}
          >
            <span className="mt-0.5 shrink-0 rounded bg-red-500/15 px-1.5 text-xs tabular-nums text-red-300">
              ×{c.count}
            </span>
            <span className="min-w-0 flex-1">
              <span className="block truncate text-ink-200">{c.label}</span>
              <span className="block truncate text-xs text-ink-500">
                {c.where.map((w) => `${w.key}×${w.count}`).join(" · ")}
              </span>
            </span>
          </button>
        ))}
      </div>
    </Card>
  );
}

function EvidenceBadge({ state }: { state?: string }) {
  if (!state) return null;
  const map: Record<string, [string, string]> = {
    done: ["evidence ✓", "text-emerald-300/80"],
    queued: ["evidence ⏳", "text-amber-300/80"],
    pending: ["evidence ⏳", "text-amber-300/80"],
    failed: ["evidence ✗", "text-red-300/80"],
  };
  const [label, cls] = map[state] ?? [state, "text-ink-500"];
  return <span className={`text-xs ${cls}`}>{label}</span>;
}

function FailuresList({
  soakId,
  dungeon,
  cluster,
  version,
  canCapture,
  onClear,
}: {
  soakId: string;
  dungeon: string;
  cluster: string;
  version: number;
  canCapture: boolean;
  onClear: () => void;
}) {
  const [rows, setRows] = useState<SoakRunRow[]>([]);
  const [total, setTotal] = useState(0);
  const [next, setNext] = useState<number | null>(null);
  const [loading, setLoading] = useState(false);
  const [open, setOpen] = useState<Set<string>>(new Set());

  const url = useCallback(
    (cursor: number) => {
      const p = new URLSearchParams({ result: "fail", limit: "50", cursor: String(cursor) });
      if (dungeon) p.set("dungeon", dungeon);
      if (cluster) p.set("cluster", cluster);
      return `/api/soak/${soakId}/runs?${p}`;
    },
    [soakId, dungeon, cluster],
  );

  /* Reload the first page when the filter changes or a new failure lands;
     pages already loaded past the first are kept only while the filter is
     unchanged. */
  useEffect(() => {
    let dead = false;
    setLoading(true);
    api
      .get<SoakRunsPage>(url(0))
      .then((r) => {
        if (dead) return;
        setRows((prev) => {
          const fresh = r.runs;
          const seen = new Set(fresh.map((x) => x.runId));
          const keep = prev.length > fresh.length ? prev.filter((x) => !seen.has(x.runId)) : [];
          return [...fresh, ...keep];
        });
        setTotal(r.total);
        setNext(r.nextCursor);
      })
      .finally(() => !dead && setLoading(false));
    return () => {
      dead = true;
    };
  }, [url, version]);

  useEffect(() => {
    setRows([]);
    setOpen(new Set());
  }, [dungeon, cluster]);

  function more() {
    if (next === null) return;
    setLoading(true);
    api
      .get<SoakRunsPage>(url(rows.length))
      .then((r) => {
        setRows((prev) => {
          const seen = new Set(prev.map((x) => x.runId));
          return [...prev, ...r.runs.filter((x) => !seen.has(x.runId))];
        });
        setTotal(r.total);
        setNext(r.nextCursor);
      })
      .finally(() => setLoading(false));
  }

  const toggle = (id: string) =>
    setOpen((s) => {
      const n = new Set(s);
      if (!n.delete(id)) n.add(id);
      return n;
    });

  return (
    <div className="mt-5">
      <div className="mb-2 flex flex-wrap items-center gap-2">
        <h2 className="text-sm font-semibold uppercase tracking-wider text-ink-400">Failures</h2>
        <span className="text-sm text-ink-500">{total}</span>
        {(dungeon || cluster) && (
          <button type="button" onClick={onClear}
                  className="rounded-full border border-iris-500/40 bg-iris-500/10 px-2.5 py-0.5 text-xs text-iris-200">
            {dungeon || cluster} ✕
          </button>
        )}
        {loading && <Spinner />}
      </div>
      {!rows.length && !loading && (
        <EmptyState icon="✨" title="No failures">
          {dungeon || cluster ? "None match this filter." : "Every run so far has succeeded."}
        </EmptyState>
      )}
      <div className="space-y-2">
        {rows.map((r) => {
          const id = r.runId ?? "";
          const expanded = open.has(id);
          return (
            <Card key={id} className="!p-0">
              <div className="flex cursor-pointer flex-wrap items-center gap-x-3 gap-y-1 px-4 py-2.5"
                   onClick={() => toggle(id)}>
                <span className="text-xs tabular-nums text-ink-500">
                  {new Date(r.endedAtMs ?? r.startedAtMs ?? 0).toLocaleString([], {
                    month: "short", day: "numeric", hour: "2-digit", minute: "2-digit",
                  })}
                </span>
                <span className="font-medium">{r.dungeonName ?? r.dungeon}</span>
                {r.heroic && <span className="text-xs text-fuchsia-300">heroic</span>}
                <ResultPill result={r.result} />
                {r.bossesTotal !== undefined && (
                  <span className="text-sm text-ink-400">{r.bossesKilled ?? 0}/{r.bossesTotal}</span>
                )}
                <span className="text-sm text-ink-500">{fmtDuration(r.durationS)}</span>
                {r.failReason && (
                  <span className="max-w-md truncate text-xs text-red-300/80">{r.failReason}</span>
                )}
                <span className="ml-auto flex items-center gap-2">
                  <EvidenceBadge state={r.evidence} />
                  <CopyButton text={r.runId} />
                  <ExpandToggle expanded={expanded} onToggle={() => toggle(id)} label={`run ${id}`} />
                </span>
              </div>
              {expanded && <FailureDetail soakId={soakId} runId={id} canCapture={canCapture} />}
            </Card>
          );
        })}
      </div>
      {next !== null && (
        <button type="button" onClick={more} disabled={loading}
                className="mt-3 w-full rounded-xl border border-ink-800 py-2 text-sm text-ink-400 hover:border-ink-600 hover:text-ink-200">
          Load more ({total - rows.length} older)
        </button>
      )}
    </div>
  );
}

function FailureDetail({
  soakId,
  runId,
  canCapture,
}: {
  soakId: string;
  runId: string;
  canCapture: boolean;
}) {
  const toast = useToast();
  const [rec, setRec] = useState<(RunRecord & { lost?: boolean; note?: string }) | null>(null);
  const { data: ev, refresh } = usePoll(
    () => api.get<SoakEvidence>(`/api/soak/${soakId}/evidence/${runId}`),
    8000,
  );
  const [showReport, setShowReport] = useState(false);

  useEffect(() => {
    api
      .get<RunRecord>(`/api/soak/${soakId}/runs/${runId}`)
      .then(setRec)
      .catch((e) => toast("error", e.message));
  }, [soakId, runId, toast]);

  const base = `/api/soak/${soakId}/evidence/${runId}`;
  return (
    <div className="border-t border-ink-800/70">
      <div className="flex flex-wrap items-center gap-2 px-4 py-3 text-xs text-ink-400">
        <code className="rounded bg-ink-950 px-2 py-1 text-ink-300">python3 tools/dc_test_run.py {runId}</code>
        <CopyButton text={`python3 tools/dc_test_run.py ${runId}`} title="copy the post-mortem command" />
        <code className="rounded bg-ink-950 px-2 py-1 text-ink-300">/triage {runId}</code>
        <CopyButton text={`/triage ${runId}`} title="copy the triage prompt" />
      </div>
      {rec?.lost && (
        <div className="mx-4 mb-2 rounded-lg border border-fuchsia-900/50 bg-fuchsia-950/20 px-3 py-2 text-sm text-fuchsia-200/90">
          This run was live when the worldserver went away — it never got a
          record, so this one was synthesized from its last heartbeat and
          accumulated timeline. {rec.note}
        </div>
      )}
      <div className="px-4 pb-3">
        <div className="mb-1 text-xs uppercase tracking-wide text-ink-600">Evidence</div>
        {!ev ? (
          <Spinner />
        ) : ev.state === "done" ? (
          <div className="space-y-2">
            <div className="flex flex-wrap gap-2 text-xs">
              <button type="button" onClick={() => setShowReport(!showReport)}
                      className="rounded border border-ink-700 px-2 py-0.5 text-ink-300 hover:border-iris-500/50">
                {showReport ? "hide report" : "show report"}
              </button>
              {ev.files.map((f) => (
                <a key={f.name} href={`${base}/${encodeURIComponent(f.name)}`}
                   className="rounded border border-ink-800 px-2 py-0.5 text-iris-300 hover:border-iris-500/50"
                   download>
                  {f.name} <span className="text-ink-600">{fmtBytes(f.size)}</span>
                </a>
              ))}
            </div>
            {showReport && (
              <pre className="max-h-[32rem] overflow-auto rounded-lg border border-ink-800 bg-ink-950 p-3 text-[11px] leading-snug text-ink-300">
                {ev.report}
              </pre>
            )}
          </div>
        ) : (
          <div className="flex flex-wrap items-center gap-2 text-sm">
            <EvidenceBadge state={ev.state} />
            {ev.error && <span className="text-xs text-red-300/80">{ev.error}</span>}
            {canCapture && ev.state === "failed" && (
              <button
                type="button"
                onClick={() =>
                  api
                    .post(`${base}/capture`)
                    .then(refresh)
                    .catch((e) => toast("error", e.message))
                }
                className="rounded border border-ink-700 px-2 py-0.5 text-xs text-ink-300"
              >
                capture again
              </button>
            )}
          </div>
        )}
      </div>
      {rec ? <RunDetail r={rec} /> : <div className="px-4 pb-4"><Spinner label="loading record…" /></div>}
    </div>
  );
}

function EditPoolDialog({
  soak,
  onClose,
  onSaved,
}: {
  soak: SoakView;
  onClose: () => void;
  onSaved: () => void;
}) {
  const toast = useToast();
  const panel = useModal<HTMLDivElement>(onClose);
  const { data: catalogue } = usePoll(() => api.get<Catalogue>("/api/testdungeons"), 60000);
  const [sel, setSel] = useState<Selection>(() => fromPool(soak.config.pool));
  const [concurrent, setConcurrent] = useState(String(soak.config.concurrent));
  const all = useMemo(() => catalogue?.dungeons ?? [], [catalogue]);
  const byToken = useMemo(() => new Map(all.map((d) => [d.token, d])), [all]);
  const pool = toPool(sel);

  function save() {
    const body: { pool?: SoakPoolEntry[]; concurrent?: number } = {};
    const before = soak.config.pool.map(keyOf).sort().join(",");
    if (pool.map(keyOf).sort().join(",") !== before) body.pool = pool;
    const n = parseInt(concurrent || "0", 10);
    if (n !== soak.config.concurrent) body.concurrent = n;
    if (!body.pool && body.concurrent === undefined) {
      onClose();
      return;
    }
    api
      .post(`/api/soak/${soak.soakId}/edit`, body)
      .then(() => {
        toast("ok", "Pool updated — takes effect from the next launch");
        onSaved();
        onClose();
      })
      .catch((e) => toast("error", e.message));
  }

  return (
    <div className="fixed inset-0 z-40 flex items-start justify-center overflow-y-auto bg-black/60 p-4" onClick={onClose}>
      <div
        ref={panel}
        role="dialog"
        aria-modal="true"
        aria-label="Edit pool"
        tabIndex={-1}
        className="my-8 w-full max-w-5xl rounded-2xl border border-ink-700 bg-ink-900 p-6"
        onClick={(e) => e.stopPropagation()}
      >
        <div className="mb-4 flex items-center justify-between">
          <h2 className="text-lg font-semibold">Edit pool</h2>
          <button type="button" onClick={onClose} className="text-ink-500 hover:text-ink-200">✕</button>
        </div>
        {!catalogue ? (
          <Spinner label="loading catalogue…" />
        ) : (
          <div className="grid gap-5 lg:grid-cols-[1fr_18rem]">
            <div className="max-h-[60vh] overflow-y-auto pr-1">
              <DungeonPicker all={all} byToken={byToken} sel={sel} setSel={setSel} />
            </div>
            <div className="space-y-4">
              <SelectedTray sel={sel} setSel={setSel} byToken={byToken} />
              <Field label="Concurrent runs" hint="lowering never aborts a live run">
                <Stepper value={concurrent} onChange={setConcurrent} />
              </Field>
              <button
                type="button"
                data-autofocus
                disabled={!pool.length}
                onClick={save}
                className="w-full rounded-xl bg-iris-600 px-4 py-2 text-sm font-semibold text-white hover:bg-iris-500 disabled:bg-ink-800 disabled:text-ink-500"
              >
                Apply
              </button>
            </div>
          </div>
        )}
      </div>
    </div>
  );
}
