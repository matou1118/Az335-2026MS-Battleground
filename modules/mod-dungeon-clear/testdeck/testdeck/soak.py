"""Continuous mode ("soak"): keep N test runs in flight, drawn from a pool of
dungeons, until told to stop — and keep every failure with its evidence.

The worldserver side is one endless pool plan (`.dc test plan start pool=…
total=0`): it owns launching, concurrency and bot-budget backoff, and stamps
its planId on every run record. What a plan cannot do is outlive the
worldserver, so the deck owns the durable part — a SOAK is a deck object that
spans any number of plans (one per worldserver lifetime):

    <data_dir>/soaks/<soakId>/
      state.json      config, status, planIds, ingest offset, counters
      ledger.jsonl    every run record of this soak, copied verbatim as it
                      lands, plus synthesized "lost" rows for runs that died
                      with the worldserver
      evidence/<runId>/   dc_test_run.py report + log slices, per failure

SoakSupervisor.tick() runs every 2 s (loop_soak): mirror the plan's heartbeat,
ingest new run records by byte offset, queue evidence for failures, and
re-issue the plan after a worldserver restart.

Only one soak is active at a time — it owns the whole bot budget.
"""

import asyncio
import hashlib
import json
import os
import re
import shutil
import sys
import time
from collections import Counter, OrderedDict
from pathlib import Path

from .bridge import MAX_CMD_LEN
from .util import parse_jsonl_line as _parse, tail_rows

SOAK_ID_RE = re.compile(r"sk-\d{8}-\d{6}(?:-\d{1,3})?")
RUN_ID_RE = re.compile(r"tr-[A-Za-z0-9-]{1,40}")
PLAN_ID_RE = re.compile(r"tp-[A-Za-z0-9-]{1,40}")
TOKEN_RE = re.compile(r"[a-z0-9][a-z0-9_-]{0,39}")

HEARTBEAT_STALE_S = 15
# A plan missing from a stale heartbeat for this long, with no final summary,
# went away with the worldserver. Longer than any world-thread hitch: a false
# "lost" would be corrected by the real record, but it would still flash.
LOST_AFTER_S = 60
# The plan we asked for must show up in the heartbeat within this long.
START_TIMEOUT_S = 45
START_RETRY_S = 60
MAX_START_ATTEMPTS = 3
BREAKER_DEFAULT = 5
# A pool that does not fit one console line is loaded with `edit add=` chunks.
# Re-send what is still missing after this long (the heartbeat that would show
# the last chunk landing is ~2 s behind), and give up after this many rounds.
ADD_RESEND_MS = 6000
MAX_ADD_ROUNDS = 5
EVIDENCE_TIMEOUT_S = 300
REPORT_CAP_BYTES = 512 << 10

# Ledger rows are the run record verbatim; the index keeps only what the soak
# page lists and aggregates (a full record with its diag snapshot is tens of
# KB — thousands of them are read back one at a time, by offset).
SLIM_KEYS = ("runId", "planId", "dungeon", "dungeonName", "heroic", "size", "result",
             "failReason", "bossesKilled", "bossesTotal", "durationS", "elapsedS",
             "startedAtMs", "endedAtMs", "wipeOpponent", "wipeOnBoss", "lost")

ACTIVE_STATUSES = ("starting", "running", "paused", "holding", "server down",
                   "resuming", "draining", "stopping")


def now_ms():
    return int(time.time() * 1000)


def new_soak_id(root):
    base = time.strftime("sk-%Y%m%d-%H%M%S")
    cand, n = base, 1
    while (root / cand).exists():
        n += 1
        cand = f"{base}-{n}"
    return cand


def entry_key(token, heroic):
    return token + (":heroic" if heroic else "")


def row_key(row):
    return entry_key(row.get("dungeon") or "?", bool(row.get("heroic")))


def catalogue_aliases(path):
    """Retired dungeon token -> its replacement, from the catalogue's "aliases"
    (the module keeps e.g. brs -> lbrs after the Blackrock Spire split). {} when
    the catalogue is missing or predates aliases."""
    try:
        cat = json.loads(Path(path).read_text(encoding="utf-8", errors="replace"))
    except (OSError, ValueError):
        return {}
    aliases = cat.get("aliases") if isinstance(cat, dict) else None
    return {k: v for k, v in (aliases or {}).items()
            if isinstance(k, str) and isinstance(v, str)}


def normalize_pool(pool, aliases):
    """`pool` with retired tokens replaced by their targets, dropping an entry
    that then duplicates an earlier one (a pool holding both brs and lbrs).
    Returns (pool, changed)."""
    out, seen, changed = [], set(), False
    for e in pool or []:
        token = aliases.get(e.get("token"), e.get("token"))
        if token != e.get("token"):
            changed = True
        key = entry_key(token, e.get("heroic"))
        if key in seen:
            changed = True
            continue
        seen.add(key)
        out.append(dict(e, token=token))
    return out, changed


# Failure reasons embed names, numbers and coordinates; strip them so one
# failure mode groups across runs. tools/dc_test_run.py (sk- view) uses the same
# rule.
_NUM_RE = re.compile(r"-?\d+(?:\.\d+)?")
_QUOTED_RE = re.compile(r"'[^']*'|\"[^\"]*\"")
_WS_RE = re.compile(r"\s+")


def cluster_reason(reason):
    s = _QUOTED_RE.sub("'…'", reason or "")
    s = _NUM_RE.sub("#", s)
    s = _WS_RE.sub(" ", s).strip()
    return s[:120] or "(no reason)"


def chunk_keys(prefix, suffix, keys, limit=MAX_CMD_LEN):
    """Split pool keys into runs that each fit `prefix + "a,b,c" + suffix`
    within `limit` characters — the bridge refuses anything longer, and a
    screen console silently drops a line past ~750 characters anyway."""
    chunks, cur = [], []
    for k in keys:
        if cur and len(prefix) + len(",".join(cur + [k])) + len(suffix) > limit:
            chunks.append(cur)
            cur = []
        cur.append(k)
    if cur:
        chunks.append(cur)
    return chunks


def plan_options(cfg):
    opts = " total=0"
    if cfg.get("concurrent"):
        opts += f" concurrent={int(cfg['concurrent'])}"
    if cfg.get("pick") == "random":
        opts += " pick=random"
    if cfg.get("level"):
        opts += f" level={int(cfg['level'])}"
    if cfg.get("seed"):
        opts += f" seed={int(cfg['seed'])}"
    ilvl = int(cfg.get("ilvl") or 0)
    if ilvl:
        opts += " ilvl=none" if ilvl == -1 else f" ilvl={ilvl}"
    if cfg.get("quality"):
        opts += f" quality={int(cfg['quality'])}"
    return opts


def plan_start_cmd(cfg):
    """(start command, pool keys it leaves out). A pool too long for one line
    starts PAUSED with the first chunk; the supervisor adds the rest with
    `edit add=` and resumes once the plan's pool is complete."""
    prefix = ".dc test plan start pool="
    opts = plan_options(cfg)
    keys = [entry_key(e["token"], e.get("heroic")) for e in cfg["pool"]]
    if len(prefix) + len(",".join(keys)) + len(opts) <= MAX_CMD_LEN:
        return prefix + ",".join(keys) + opts, []
    chunks = chunk_keys(prefix, opts + " paused", keys)
    rest = [k for c in chunks[1:] for k in c]
    return prefix + ",".join(chunks[0]) + opts + " paused", rest


def add_cmds(plan_id, keys):
    prefix = f".dc test plan edit {plan_id} add="
    return [prefix + ",".join(c) for c in chunk_keys(prefix, "", keys)]


def read_heartbeat(path):
    """(data, fresh) — the raw heartbeat; fresh = active and recent."""
    try:
        data = json.loads(path.read_text(encoding="utf-8", errors="replace"))
    except (FileNotFoundError, json.JSONDecodeError, OSError):
        return {}, False
    fresh = bool(data.get("active")) and time.time() - data.get("ts", 0) <= HEARTBEAT_STALE_S
    return data, fresh


def final_plan_summary(path, plan_id):
    """The plan's final (non-checkpoint) summary line, or None. Read from the
    end: the line we want is recent."""
    for row in tail_rows(path, 400):
        if row.get("planId") == plan_id and not row.get("checkpoint"):
            return row
    return None


class Soak:
    """One session: its folder, its state.json, and an in-memory index of its
    ledger (slim rows + each row's byte offset)."""

    def __init__(self, root, soak_id):
        self.id = soak_id
        self.dir = root / soak_id
        self.state = {}
        self._index = None          # list of slim rows, ledger order
        self._by_run = None         # runId -> index position (latest wins)

    # -- persistence --------------------------------------------------------

    @property
    def state_file(self):
        return self.dir / "state.json"

    @property
    def ledger_file(self):
        return self.dir / "ledger.jsonl"

    @property
    def evidence_dir(self):
        return self.dir / "evidence"

    def load(self):
        self.state = json.loads(self.state_file.read_text(encoding="utf-8"))
        return self

    def save(self):
        self.dir.mkdir(parents=True, exist_ok=True)
        tmp = self.state_file.with_suffix(".json.tmp")
        tmp.write_text(json.dumps(self.state, indent=1), encoding="utf-8")
        tmp.replace(self.state_file)

    @property
    def active(self):
        return self.state.get("status") in ACTIVE_STATUSES

    # -- ledger -------------------------------------------------------------

    def _load_index(self):
        self._index, self._by_run = [], {}
        try:
            fh = self.ledger_file.open("rb")
        except OSError:
            return
        with fh:
            offset = 0
            for raw in fh:
                row = _parse(raw) if raw.endswith(b"\n") else None
                if row is not None:
                    self._add_slim(row, offset)
                offset += len(raw)

    def _add_slim(self, row, offset):
        slim = {k: row.get(k) for k in SLIM_KEYS if k in row}
        slim["_off"] = offset
        rid = slim.get("runId")
        prev = self._by_run.get(rid)
        # A real record supersedes a synthesized "lost" row for the same run
        # (a worldserver hitch that looked like a crash); otherwise first wins.
        if prev is not None:
            if self._index[prev].get("lost") and not slim.get("lost"):
                self._index[prev] = slim
            return
        self._by_run[rid] = len(self._index)
        self._index.append(slim)

    def index(self):
        if self._index is None:
            self._load_index()
        return self._index

    def has_run(self, run_id):
        self.index()
        return run_id in self._by_run

    def is_lost(self, run_id):
        self.index()
        pos = self._by_run.get(run_id)
        return pos is not None and bool(self._index[pos].get("lost"))

    def append(self, row):
        """Append one row verbatim; returns its slim form."""
        self.index()
        self.dir.mkdir(parents=True, exist_ok=True)
        data = (json.dumps(row, separators=(",", ":")) + "\n").encode("utf-8")
        with self.ledger_file.open("ab") as fh:
            offset = fh.tell()
            fh.write(data)
        self._add_slim(row, offset)
        return self._index[self._by_run[row.get("runId")]]

    def record(self, run_id):
        """The full ledger row for a run (by offset — no full read)."""
        self.index()
        pos = self._by_run.get(run_id)
        if pos is None:
            return None
        try:
            with self.ledger_file.open("rb") as fh:
                fh.seek(self._index[pos]["_off"])
                return _parse(fh.readline())
        except OSError:
            return None

    def rows(self):
        """Slim rows, one per run, ledger order."""
        return [r for r in self.index()]

    # -- evidence -----------------------------------------------------------

    def evidence_state(self, run_id):
        d = self.evidence_dir / run_id
        if (d / "report.txt").is_file():
            return "done"
        if (d / "error.txt").is_file():
            return "failed"
        return "pending"

    def disk_bytes(self):
        total = 0
        for p in self.dir.rglob("*"):
            try:
                if p.is_file():
                    total += p.stat().st_size
            except OSError:
                pass
        return total

    # -- views --------------------------------------------------------------

    def summary(self):
        st = self.state
        c = st.get("counters") or {}
        return {
            "soakId": self.id, "owner": st.get("owner", ""),
            "status": st.get("status", "?"), "statusDetail": st.get("statusDetail", ""),
            "createdAtMs": st.get("createdAtMs"), "stoppedAtMs": st.get("stoppedAtMs"),
            "config": st.get("config") or {}, "planIds": st.get("planIds") or [],
            "currentPlanId": st.get("currentPlanId", ""),
            "restarts": st.get("restarts", 0),
            "runs": c.get("runs", 0), "ok": c.get("ok", 0), "fail": c.get("fail", 0),
            "lost": c.get("lost", 0),
            "plan": st.get("plan") or None,
            "liveRuns": st.get("liveRuns") or [],
            "stopMode": st.get("stopMode", ""),
            "lastRefusal": st.get("lastRefusal", ""),
        }

    def stats(self):
        rows = self.rows()
        by = OrderedDict()
        for e in (self.state.get("config") or {}).get("pool") or []:
            by[entry_key(e["token"], e.get("heroic"))] = []
        for r in rows:
            by.setdefault(row_key(r), []).append(r)

        per = []
        for key, recs in by.items():
            good = [r for r in recs if r.get("result") == "success"]
            durs = sorted(int(r.get("durationS") or 0) for r in good)
            per.append({
                "key": key,
                "dungeon": recs[0].get("dungeon") if recs else key.split(":")[0],
                "dungeonName": next((r.get("dungeonName") for r in recs if r.get("dungeonName")), ""),
                "heroic": key.endswith(":heroic"),
                "runs": len(recs), "ok": len(good), "fail": len(recs) - len(good),
                "medianS": durs[len(durs) // 2] if durs else 0,
                "last": [r.get("result") for r in recs[-20:]],
            })

        clusters = {}
        for r in rows:
            if r.get("result") == "success":
                continue
            label = (r.get("result") or "?") + ": " + cluster_reason(r.get("failReason"))
            c = clusters.setdefault(label, {"label": label, "count": 0, "where": Counter(),
                                            "result": r.get("result"),
                                            "sample": r.get("failReason") or ""})
            c["count"] += 1
            c["where"][row_key(r)] += 1
        cl = sorted(clusters.values(), key=lambda c: -c["count"])[:60]
        for c in cl:
            c["where"] = [{"key": k, "count": n} for k, n in c["where"].most_common()]

        created = self.state.get("createdAtMs") or now_ms()
        end = self.state.get("stoppedAtMs") or now_ms()
        hours = max((end - created) / 3600000, 1e-9)
        ok = sum(1 for r in rows if r.get("result") == "success")
        return {"perDungeon": per, "clusters": cl, "runs": len(rows), "ok": ok,
                "fail": len(rows) - ok,
                "lost": sum(1 for r in rows if r.get("result") == "lost"),
                "runsPerHour": round(len(rows) / hours, 2) if end - created > 60000 else 0,
                "elapsedS": int((end - created) / 1000), "diskBytes": self.disk_bytes()}


class SoakSupervisor:
    """Owns the soaks folder, the active soak, and the per-tick reconcile."""

    def __init__(self, cfg, bridge_getter, timelines_getter=None, spawn=None):
        self.cfg = cfg
        self._bridge = bridge_getter
        self._timelines = timelines_getter or (lambda: None)
        self._spawn = spawn
        self.root = cfg.data_dir / "soaks"
        self._cache = {}
        self._active_id = None
        self._evidence_q = []
        self._evidence_task = None
        self._scanned_active = False
        self.nice = True             # tests turn this off
        self._lock = None

    @property
    def lock(self):
        # Built on first use, inside the running loop: on Python <= 3.9 an
        # asyncio.Lock binds to get_event_loop() at construction.
        if self._lock is None:
            self._lock = asyncio.Lock()
        return self._lock

    # -- lookup -------------------------------------------------------------

    def get(self, soak_id):
        if not SOAK_ID_RE.fullmatch(soak_id or ""):
            return None
        s = self._cache.get(soak_id)
        if s is not None:
            return s
        s = Soak(self.root, soak_id)
        if not s.state_file.is_file():
            return None
        try:
            s.load()
        except (OSError, ValueError):
            return None
        # A session saved before a dungeon token was retired (brs, before the
        # Blackrock Spire split) would reconcile against a plan that reports the
        # new token forever, so its pool is read through the catalogue aliases.
        config = s.state.get("config") or {}
        if config.get("pool"):
            config["pool"], _changed = normalize_pool(
                config["pool"], catalogue_aliases(self.cfg.testdungeons_file))
        self._cache[soak_id] = s
        return s

    def all_ids(self):
        try:
            return sorted((p.name for p in self.root.iterdir()
                           if SOAK_ID_RE.fullmatch(p.name) and (p / "state.json").is_file()),
                          reverse=True)
        except OSError:
            return []

    def active(self):
        if not self._scanned_active:
            self._scanned_active = True
            for sid in self.all_ids():
                s = self.get(sid)
                if s and s.active:
                    self._active_id = sid
                    self._requeue_evidence(s)
                    break
        if self._active_id:
            s = self.get(self._active_id)
            if s and s.active:
                return s
            self._active_id = None
        return None

    def forget(self, soak_id):
        self._cache.pop(soak_id, None)
        if self._active_id == soak_id:
            self._active_id = None

    # -- lifecycle ----------------------------------------------------------

    async def start(self, owner, config):
        """Create the soak and issue its plan. Caller validated config."""
        if self.active():
            raise RuntimeError("a continuous session is already running")
        self.root.mkdir(parents=True, exist_ok=True)
        sid = new_soak_id(self.root)
        s = Soak(self.root, sid)
        s.state = {
            "schema": 1, "soakId": sid, "owner": owner,
            "createdAtMs": now_ms(), "stoppedAtMs": None,
            "status": "starting", "statusDetail": "",
            "config": config, "planIds": [], "currentPlanId": "",
            "ingest": {}, "counters": {"runs": 0, "ok": 0, "fail": 0, "lost": 0},
            "restarts": 0, "setupFailStreak": 0, "breakerTripped": False,
            "stopMode": "", "startAttempts": 0, "startIssuedAtMs": 0,
            "planLastSeenMs": 0, "liveRuns": [], "plan": None,
        }
        # Records already in the history file predate this soak: start the
        # ingest at its current end.
        self._ingest_seek_end(s)
        s.save()
        self._cache[sid] = s
        self._active_id = sid
        self._scanned_active = True
        await self._issue_start(s)
        return s

    async def _issue_start(self, s):
        cmd, rest = plan_start_cmd(s.state["config"])
        s.state["awaitingResume"] = bool(rest)
        s.state["addRounds"] = 0
        s.state["addSentAtMs"] = 0
        s.state["startAttempts"] = s.state.get("startAttempts", 0) + 1
        s.state["startIssuedAtMs"] = now_ms()
        s.state["knownPlanIds"] = self._visible_plan_ids()
        reply = await self._exec(cmd)
        lines = getattr(reply, "lines", None) or []
        text = " / ".join(l for l in lines if l.strip())[-300:]
        if reply is None or not reply.ok:
            s.state["statusDetail"] = f"start command failed: {text or 'bridge error'}"
        elif any("not started" in l for l in lines):
            s.state["status"] = "stopped"
            s.state["statusDetail"] = f"worldserver refused the plan: {text}"
            s.state["stoppedAtMs"] = now_ms()
        # The planId is adopted from the heartbeat (a pool plan nobody else
        # owns), never parsed out of the console reply.
        s.save()
        return reply

    async def _exec(self, cmd):
        print(f"testdeck: soak: {cmd}", flush=True)
        try:
            return await self._bridge().exec(cmd)
        except Exception as e:                    # noqa: BLE001
            print(f"testdeck: soak: bridge error: {e}", flush=True)
            return None

    async def plan_cmd(self, s, verb, extra=""):
        pid = s.state.get("currentPlanId")
        if not pid:
            return None
        return await self._exec(f".dc test plan {verb} {pid}{extra}")

    async def pause(self, s):
        s.state["userPaused"] = True
        await self.plan_cmd(s, "pause")
        if s.state.get("status") in ("running", "holding", "starting", "resuming"):
            s.state["status"] = "paused"
        s.save()

    async def resume(self, s):
        s.state["userPaused"] = False
        s.state["breakerTripped"] = False
        s.state["setupFailStreak"] = 0
        await self.plan_cmd(s, "resume")
        if s.state.get("status") == "paused":
            s.state["status"] = "running"
            s.state["statusDetail"] = ""
        s.save()

    async def edit(self, s, pool=None, concurrent=None):
        """A pool edit sends the first chunk as `pool=` (replacing the old
        pool); the tick's reconcile `add=`s whatever is still missing."""
        extra = ""
        if concurrent is not None:
            extra += f" concurrent={int(concurrent)}"
            s.state["config"]["concurrent"] = int(concurrent)
        if pool is not None:
            prefix = f".dc test plan edit {s.state.get('currentPlanId')} pool="
            keys = [entry_key(e["token"], e.get("heroic")) for e in pool]
            first = chunk_keys(prefix, extra, keys)[0]
            extra = " pool=" + ",".join(first) + extra
            s.state["config"]["pool"] = pool
            s.state["addRounds"] = 0
            # Give the heartbeat time to show the replaced pool before the
            # reconcile diffs against it.
            s.state["addSentAtMs"] = now_ms()
        reply = await self.plan_cmd(s, "edit", extra)
        s.save()
        return reply

    async def stop(self, s, mode):
        s.state["stopMode"] = mode
        if not s.state.get("currentPlanId"):
            self._finish(s, "stopped", "stopped before a plan was running")
            return
        if mode == "now":
            await self.plan_cmd(s, "stop")
            s.state["status"] = "stopping"
        else:
            await self.plan_cmd(s, "pause")
            s.state["status"] = "draining"
        s.save()

    def _finish(self, s, status, detail=""):
        s.state["status"] = status
        s.state["statusDetail"] = detail
        s.state["stoppedAtMs"] = now_ms()
        s.state["liveRuns"] = []
        s.save()
        if self._active_id == s.id:
            self._active_id = None

    def delete(self, s):
        shutil.rmtree(s.dir, ignore_errors=True)
        self.forget(s.id)

    # -- the reconcile tick -------------------------------------------------

    def _visible_plan_ids(self):
        data, _fresh = read_heartbeat(self.cfg.testrun_live_file)
        return [p.get("planId") for p in data.get("plans") or [] if p.get("planId")]

    async def tick(self):
        s = self.active()
        if s is None:
            return
        async with self.lock:
            await self._tick(s)

    async def _tick(self, s):
        st = s.state
        data, fresh = read_heartbeat(self.cfg.testrun_live_file)
        plans = data.get("plans") or [] if fresh else []
        now = now_ms()

        # 1. Adopt the plan we asked for: a pool plan that was not visible when
        #    we issued the start and that no other soak owns.
        if not st.get("currentPlanId") and st.get("status") in ("starting", "resuming"):
            want = {entry_key(e["token"], e.get("heroic")) for e in st["config"]["pool"]}
            known = set(st.get("knownPlanIds") or [])
            for p in plans:
                have = {entry_key(e.get("token", ""), e.get("heroic")) for e in p.get("pool") or []}
                # A long pool arrives in chunks, so the plan may hold only
                # part of it yet: adopt on a non-empty subset.
                if p.get("endless") and p.get("planId") not in known and have and have <= want:
                    st["currentPlanId"] = p["planId"]
                    st["planIds"].append(p["planId"])
                    st["planLastSeenMs"] = now
                    st["startAttempts"] = 0
                    st["status"] = "running"
                    st["statusDetail"] = ("resumed after restart" if st.get("restarts") else "")
                    if st.get("userPaused") or st.get("breakerTripped"):
                        await self._exec(f".dc test plan pause {p['planId']}")
                    break
            else:
                # Nothing adopted yet. Retry a start that never produced a plan
                # (bridge down, worldserver still booting), then give up.
                waited = (now - (st.get("startIssuedAtMs") or now)) / 1000
                if waited > START_RETRY_S:
                    if st.get("startAttempts", 0) >= MAX_START_ATTEMPTS:
                        self._finish(s, "stopped", "the plan never appeared after "
                                     f"{MAX_START_ATTEMPTS} start attempts — "
                                     + (st.get("statusDetail") or "is the worldserver up?"))
                        return
                    await self._issue_start(s)
                elif waited > START_TIMEOUT_S and not st.get("statusDetail"):
                    st["statusDetail"] = "waiting for the worldserver to register the plan"

        # 2. Ingest new run records for any of this soak's plans.
        self._ingest(s)

        pid = st.get("currentPlanId")
        if not pid:
            s.save()
            return

        mine = next((p for p in plans if p.get("planId") == pid), None)
        if mine is not None:
            # 3a. Plan alive: mirror it.
            st["plan"] = mine
            st["planLastSeenMs"] = now
            st["serverDownSinceMs"] = 0
            st["liveRuns"] = [self._live_row(r) for r in data.get("runs") or []
                              if r.get("planId") == pid]
            state = mine.get("state") or ""
            if st["status"] in ("draining", "stopping"):
                if st["status"] == "draining" and not mine.get("active") \
                        and not st.get("drainStopIssued"):
                    st["drainStopIssued"] = True
                    await self._exec(f".dc test plan stop {pid}")
                    st["status"] = "stopping"
            elif state.startswith("paused"):
                st["status"] = "paused"
                if not st.get("breakerTripped") and not st.get("userPaused"):
                    st["statusDetail"] = state[len("paused"):].lstrip(": ") or ""
            elif state.startswith("holding"):
                st["status"] = "holding"
            else:
                st["status"] = "running"
                if st.get("statusDetail", "").startswith("paused"):
                    st["statusDetail"] = ""
            await self._reconcile_pool(s, mine, now)
            self._breaker(s)
            if st.get("breakerPending"):
                st["breakerPending"] = False
                await self._exec(f".dc test plan pause {pid}")
            s.save()
            return

        # 3b. Plan not in the heartbeat. Ended properly (a final summary), or
        #     gone with the worldserver?
        final = final_plan_summary(self.cfg.testplans_file, pid)
        if final is not None:
            self._ingest(s)
            st["plan"] = None
            if st.get("stopMode"):
                self._finish(s, "stopped", "")
            else:
                self._finish(s, "stopped", f"plan {pid} ended ({final.get('result')}"
                             + (f": {final.get('abortReason')}" if final.get("abortReason") else "")
                             + ") outside the deck")
            return

        if st.get("status") in ("server down", "resuming"):
            if st["status"] == "server down" and self._server_back(st):
                if st.get("stopMode"):
                    self._finish(s, "stopped", "worldserver restarted while stopping")
                    return
                if not st["config"].get("autoResume", True):
                    self._finish(s, "stopped", "worldserver restarted (auto-resume off)")
                    return
                st["restarts"] = st.get("restarts", 0) + 1
                st["currentPlanId"] = ""
                st["status"] = "resuming"
                st["startAttempts"] = 0
                st["statusDetail"] = "worldserver is back — re-issuing the plan"
                await self._issue_start(s)
            s.save()
            return

        seen = st.get("planLastSeenMs") or 0
        rebooted = self._catalogue_mtime_ms() > seen > 0
        if rebooted or (now - seen) / 1000 > LOST_AFTER_S:
            self._mark_lost(s, now)
            st["status"] = "server down"
            st["serverDownSinceMs"] = seen or now
            st["statusDetail"] = "the plan vanished with the worldserver"
            st["plan"] = None
        s.save()

    async def _reconcile_pool(self, s, plan, now):
        """Bring the plan's pool up to the session's: `add=` what is missing
        (chunked), then resume a plan that was started paused to be loaded."""
        st = s.state
        pid = st["currentPlanId"]
        want = [entry_key(e["token"], e.get("heroic")) for e in st["config"]["pool"]]
        have = {entry_key(e.get("token", ""), e.get("heroic")) for e in plan.get("pool") or []}
        missing = [k for k in want if k not in have]
        loading = bool(missing) and st.get("addRounds", 0) < MAX_ADD_ROUNDS
        if loading:
            if now - (st.get("addSentAtMs") or 0) > ADD_RESEND_MS:
                st["addSentAtMs"] = now
                st["addRounds"] = st.get("addRounds", 0) + 1
                for cmd in add_cmds(pid, missing):
                    await self._exec(cmd)
            if st.get("awaitingResume") and st["status"] not in ("draining", "stopping"):
                st["status"] = "resuming" if st.get("restarts") else "starting"
                st["statusDetail"] = (f"loading the pool into the plan "
                                      f"({len(want) - len(missing)}/{len(want)})")
            return
        if missing:
            st["statusDetail"] = (f"the worldserver would not take {len(missing)} pool "
                                  f"entr{'y' if len(missing) == 1 else 'ies'}: "
                                  + ", ".join(missing[:8]))
        if st.get("awaitingResume"):
            st["awaitingResume"] = False
            if not missing and st.get("statusDetail", "").startswith("loading the pool"):
                st["statusDetail"] = ""
            if st["status"] not in ("draining", "stopping") and not st.get("userPaused") \
                    and not st.get("breakerTripped"):
                await self._exec(f".dc test plan resume {pid}")
                st["status"] = "running"

    def _live_row(self, r):
        return {k: r.get(k) for k in ("runId", "dungeon", "dungeonName", "heroic", "stage",
                                      "state", "bossName", "bossesKilled", "bossesTotal",
                                      "elapsedS", "stall", "inCombat", "wiped")}

    def _catalogue_mtime_ms(self):
        try:
            return int(self.cfg.testdungeons_file.stat().st_mtime * 1000)
        except OSError:
            return 0

    def _server_back(self, st):
        """The worldserver rewrites the catalogue at its first world tick, so a
        catalogue newer than the moment our plan was last seen is a boot."""
        return self._catalogue_mtime_ms() > (st.get("serverDownSinceMs") or 0)

    # -- lost runs ----------------------------------------------------------

    def _mark_lost(self, s, now):
        """Every run of the plan still live in the last heartbeat died with the
        worldserver: it will never get a record, so synthesize one — crashes
        are the most valuable failures and must not be the ones that vanish."""
        timelines = self._timelines()
        for live in s.state.get("liveRuns") or []:
            rid = live.get("runId")
            if not rid or s.has_run(rid):
                continue
            elapsed = int(live.get("elapsedS") or 0)
            seen = s.state.get("planLastSeenMs") or now
            row = dict(live)
            row.update({
                "schema": "soak-lost", "planId": s.state.get("currentPlanId"),
                "result": "lost", "lost": True,
                "failReason": "worldserver went away",
                "lastStage": live.get("stage"), "lastState": live.get("state"),
                "durationS": elapsed, "startedAtMs": seen - elapsed * 1000, "endedAtMs": seen,
                "note": "no record: the run was live when the worldserver stopped — look "
                        "for a core dump (ac-worldserver-crash-dumps-wsl)",
            })
            if timelines is not None:
                rows = timelines.rows(rid)
                if rows:
                    row["statusTimeline"] = rows
            slim = s.append(row)
            self._count(s, slim)
            self._queue_evidence(s, rid)
        s.state["liveRuns"] = []

    # -- ingest -------------------------------------------------------------

    def _ingest_seek_end(self, s):
        path = self.cfg.testruns_file
        try:
            st = path.stat()
            s.state["ingest"] = {"ino": [st.st_dev, st.st_ino], "offset": st.st_size,
                                 "head": self._head(path)}
        except OSError:
            s.state["ingest"] = {"ino": None, "offset": 0, "head": ""}

    def _ingest(self, s):
        """Read dc_testruns.jsonl from the stored offset; copy rows of our
        plans into the ledger. A different inode or a file shorter than the
        offset means the history was cleared or rotated: rescan from the top
        (rows already in the ledger are skipped by runId)."""
        path = self.cfg.testruns_file
        ing = s.state.setdefault("ingest", {})
        plan_ids = set(s.state.get("planIds") or [])
        try:
            stt = path.stat()
        except OSError:
            return 0
        ino = [stt.st_dev, stt.st_ino]
        offset = int(ing.get("offset") or 0)
        head = self._head(path)
        # Cleared in place and already regrown past our offset: the size
        # check alone cannot see that, the first line's fingerprint can.
        if ing.get("ino") != ino or stt.st_size < offset \
                or (ing.get("head") and head and head != ing["head"]):
            offset = 0
        ing["ino"] = ino
        ing["head"] = head
        if stt.st_size == offset:
            ing["offset"] = offset
            return 0
        added = 0
        try:
            with path.open("rb") as fh:
                fh.seek(offset)
                while True:
                    raw = fh.readline()
                    if not raw or not raw.endswith(b"\n"):
                        break               # EOF, or a half-written line: next tick
                    offset += len(raw)
                    if b'"planId"' not in raw:
                        continue
                    row = _parse(raw)
                    if row is None or row.get("planId") not in plan_ids:
                        continue
                    rid = row.get("runId")
                    if s.has_run(rid) and not s.is_lost(rid):
                        continue
                    slim = s.append(row)
                    self._count(s, slim)
                    added += 1
                    if row.get("result") != "success":
                        self._queue_evidence(s, rid)
                    self._track_setup(s, row)
        except OSError:
            return added
        ing["offset"] = offset
        return added

    @staticmethod
    def _head(path):
        """Fingerprint of the file's first complete line ("" if none yet)."""
        try:
            with path.open("rb") as fh:
                first = fh.readline(4096)
        except OSError:
            return ""
        if not first.endswith(b"\n"):
            return ""
        return hashlib.sha1(first).hexdigest()[:16]

    def _count(self, s, slim):
        c = s.state.setdefault("counters", {"runs": 0, "ok": 0, "fail": 0, "lost": 0})
        rows = s.rows()
        c["runs"] = len(rows)
        c["ok"] = sum(1 for r in rows if r.get("result") == "success")
        c["lost"] = sum(1 for r in rows if r.get("result") == "lost")
        c["fail"] = c["runs"] - c["ok"]

    def _track_setup(self, s, row):
        if row.get("result") == "setup_failed":
            s.state["setupFailStreak"] = s.state.get("setupFailStreak", 0) + 1
            s.state["lastSetupFail"] = row.get("failReason") or ""
        else:
            s.state["setupFailStreak"] = 0

    def _breaker(self, s):
        """N consecutive setup failures: pause the plan rather than fill the
        ledger with hundreds of identical non-runs. One click resumes."""
        st = s.state
        limit = int(st["config"].get("breaker", BREAKER_DEFAULT) or 0)
        if limit and not st.get("breakerTripped") and st.get("setupFailStreak", 0) >= limit:
            st["breakerTripped"] = True
            st["breakerPending"] = True
            st["status"] = "paused"
            st["statusDetail"] = (f"setup keeps failing ({st['setupFailStreak']} in a row): "
                                  + (st.get("lastSetupFail") or "?"))

    # -- evidence -----------------------------------------------------------

    def tool_path(self):
        explicit = getattr(self.cfg, "soak_tool", "") or ""
        cands = [Path(explicit)] if explicit else []
        if self.cfg.app_dir:
            cands.append(Path(self.cfg.app_dir).parent / "tools" / "dc_test_run.py")
        for c in cands:
            if c.is_file():
                return c
        return None

    def _queue_evidence(self, s, run_id):
        if run_id and run_id not in self._evidence_q:
            self._evidence_q.append(run_id)
        self._kick_evidence(s)

    def _requeue_evidence(self, s):
        for r in s.rows():
            if r.get("result") != "success" and s.evidence_state(r.get("runId")) == "pending":
                self._evidence_q.append(r["runId"])

    def _kick_evidence(self, s):
        if self._evidence_task is None or self._evidence_task.done():
            try:
                self._evidence_task = asyncio.get_running_loop().create_task(
                    self._drain_evidence(s))
            except RuntimeError:
                self._evidence_task = None     # no loop (sync caller): the next tick kicks it

    async def wait_evidence(self):
        """Let a queued capture pass finish (tests; shutdown)."""
        while self._evidence_task is not None and not self._evidence_task.done():
            await asyncio.sleep(0.01)

    async def _drain_evidence(self, s):
        while self._evidence_q:
            rid = self._evidence_q.pop(0)
            if not RUN_ID_RE.fullmatch(rid or "") or s.evidence_state(rid) != "pending":
                continue
            await self.capture(s, rid)

    async def capture(self, s, run_id):
        """dc_test_run.py <runId> --dump evidence/<runId>/ — its report on
        stdout becomes report.txt. One at a time, niced: this runs next to a
        busy worldserver, and it has to beat the next restart's log wipe."""
        out_dir = s.evidence_dir / run_id
        out_dir.mkdir(parents=True, exist_ok=True)
        tool = self.tool_path()
        if tool is None:
            (out_dir / "error.txt").write_text(
                "dc_test_run.py not found — set [soak] dc_test_run in testdeck.toml\n",
                encoding="utf-8")
            return False
        argv = [sys.executable, str(tool), run_id,
                "--data-dir", str(self.cfg.server_root), "--dump", str(out_dir),
                "--notable", "200"]
        if self.cfg.log_dir:
            argv += ["--log-dir", str(self.cfg.log_dir)]
        if self.nice and os.name == "posix" and shutil.which("nice"):
            argv = ["nice", "-n", "10"] + argv
        spawn = self._spawn
        if spawn is None:
            from .util import spawn as spawn
        try:
            rc, out, err = await spawn(argv, timeout=EVIDENCE_TIMEOUT_S, merge_stderr=False)
        except Exception as e:                      # noqa: BLE001
            (out_dir / "error.txt").write_text(f"capture failed: {e}\n", encoding="utf-8")
            return False
        text = out.decode("utf-8", "replace")
        if rc != 0 and not text.strip():
            (out_dir / "error.txt").write_text(
                f"dc_test_run.py exited {rc}:\n{err.decode('utf-8', 'replace')[-4000:]}",
                encoding="utf-8")
            return False
        (out_dir / "report.txt").write_text(text, encoding="utf-8")
        return True

    def evidence(self, s, run_id):
        d = s.evidence_dir / run_id
        files = []
        if d.is_dir():
            for p in sorted(d.iterdir()):
                if p.is_file():
                    files.append({"name": p.name, "size": p.stat().st_size})
        report = ""
        rp = d / "report.txt"
        if rp.is_file():
            with rp.open("rb") as fh:
                report = fh.read(REPORT_CAP_BYTES).decode("utf-8", "replace")
        err = ""
        ep = d / "error.txt"
        if ep.is_file():
            err = ep.read_text(encoding="utf-8", errors="replace")[:4000]
        state = s.evidence_state(run_id)
        if state == "pending" and run_id in self._evidence_q:
            state = "queued"
        return {"state": state, "report": report, "error": err, "files": files}

    def evidence_file(self, s, run_id, name):
        """Resolve-then-contain: the path must stay inside this run's folder."""
        root = (s.evidence_dir / run_id).resolve()
        try:
            f = (root / name).resolve()
        except (OSError, ValueError):
            return None
        if f.is_relative_to(root) and f.is_file():
            return f
        return None


async def loop_soak(get_supervisor):
    while True:
        try:
            sup = get_supervisor()
            if sup is not None:
                await sup.tick()
        except Exception as e:                        # noqa: BLE001
            print(f"testdeck: soak loop error: {e}", flush=True)
        await asyncio.sleep(2)
