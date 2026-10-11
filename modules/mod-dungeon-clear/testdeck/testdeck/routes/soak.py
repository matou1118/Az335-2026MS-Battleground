"""Continuous mode ("soak") — see testdeck/soak.py for the model.

Anyone who can log in may start a session; one runs at a time (it owns the
bot budget). Its owner or an admin ([auth] admin_gmlevel, "affects other
people") may pause, edit, stop and delete it — the same rule as rosters.
"""

import json
import time
from typing import List, Optional

from fastapi import APIRouter, HTTPException, Request
from fastapi.responses import FileResponse
from pydantic import BaseModel

from ..auth import request_session
from ..context import ctx
from ..mysql import addclass_pool_size
from ..soak import (PLAN_ID_RE, RUN_ID_RE, TOKEN_RE, cluster_reason, entry_key,
                    row_key)
from .plans import api_testdungeons, catalogue_rows, check_dungeon, resolve_alias
from .runs import audit

router = APIRouter()

MAX_POOL = 200
MAX_CONCURRENT = 100
PAGE_MAX = 200


def sup():
    if ctx.soak is None:
        raise HTTPException(503, "continuous mode is not initialised")
    return ctx.soak


def me(request):
    s = request_session(request)
    return s.username if s else ""


def is_admin(request):
    s = request_session(request)
    return s is not None and s.gmlevel >= ctx.cfg.admin_gmlevel


def require_owner(request, soak, what):
    owner = soak.state.get("owner") or ""
    if owner and owner == me(request):
        return
    if is_admin(request):
        return
    raise HTTPException(403, f"{what}: this session belongs to {owner or 'nobody'} — "
                             f"only its owner or a GM level {ctx.cfg.admin_gmlevel} can")


def get_soak(soak_id):
    s = sup().get(soak_id)
    if s is None:
        raise HTTPException(404, f"no continuous session '{soak_id}'")
    return s


class PoolEntry(BaseModel):
    token: str
    heroic: bool = False


class SoakStartRequest(BaseModel):
    pool: List[PoolEntry]
    concurrent: int = 2
    pick: str = "bag"
    level: int = 0
    seed: int = 0
    # Gear ceiling for every run: 0 = the server's AutoGear* values, -1 = no
    # limit, 1..400 = that item level. No per-dungeon ladder applies to a
    # mixed pool, so any ceiling the module accepts is accepted here.
    ilvl: int = 0
    quality: int = 0
    autoResume: bool = True
    breaker: int = 5


class SoakEditRequest(BaseModel):
    pool: Optional[List[PoolEntry]] = None
    concurrent: Optional[int] = None


class SoakStopRequest(BaseModel):
    mode: str = "drain"


def validate_pool(rows, pool, cat=None):
    if not pool:
        raise HTTPException(400, "pick at least one dungeon")
    if len(pool) > MAX_POOL:
        raise HTTPException(400, f"at most {MAX_POOL} pool entries")
    out, seen = [], set()
    for e in pool:
        if not TOKEN_RE.fullmatch(e.token or ""):
            raise HTTPException(400, f"bad dungeon token '{e.token}'")
        # A retired token (brs) is stored as its replacement (lbrs), so the
        # pool's keys match the plan's and the run records'.
        token = resolve_alias(cat or {}, e.token)
        check_dungeon(rows, token, e.heroic)
        key = entry_key(token, e.heroic)
        if key in seen:
            raise HTTPException(400, f"'{key}' is in the pool twice")
        seen.add(key)
        out.append({"token": token, "heroic": bool(e.heroic)})
    return out


def validate_concurrent(n):
    if not 1 <= n <= MAX_CONCURRENT:
        raise HTTPException(400, f"concurrent must be 1..{MAX_CONCURRENT}")


async def require_pool_support():
    cat = await api_testdungeons()
    if not (cat.get("limits") or {}).get("planPool"):
        raise HTTPException(409, "this worldserver predates continuous mode (its catalogue "
                                 "has no limits.planPool) — rebuild mod-dungeon-clear")
    return cat


_pool_cache = {"t": 0.0, "n": None}


async def cached_pool_size():
    """The addclass pool size, re-counted at most once a minute."""
    now = time.time()
    if now - _pool_cache["t"] > 60:
        _pool_cache["t"] = now
        _pool_cache["n"] = await addclass_pool_size()
    return _pool_cache["n"]


def view(s, with_stats=False):
    out = s.summary()
    if with_stats:
        out["stats"] = s.stats()
    return out


@router.get("/api/soak")
async def api_soak(request: Request):
    """The active session (with stats) and the recent ones (summaries)."""
    sv = sup()
    cat = await api_testdungeons()
    active = sv.active()
    recent = []
    for sid in sv.all_ids()[:40]:
        s = sv.get(sid)
        if s is not None and (active is None or s.id != active.id):
            recent.append(view(s))
    return {
        "active": view(active, with_stats=True) if active else None,
        "recent": recent,
        "supported": bool((cat.get("limits") or {}).get("planPool")),
        "evidenceTool": sv.tool_path() is not None,
        "me": me(request), "admin": is_admin(request),
        "addclassPool": await cached_pool_size(),
    }


@router.get("/api/soak/{soak_id}")
async def api_soak_one(soak_id: str):
    return view(get_soak(soak_id), with_stats=True)


@router.post("/api/soak/start")
async def api_soak_start(req: SoakStartRequest, request: Request):
    sv = sup()
    await require_pool_support()
    cat, rows = await catalogue_rows()
    pool = validate_pool(rows, req.pool, cat)
    validate_concurrent(req.concurrent)
    if req.pick not in ("bag", "random"):
        raise HTTPException(400, "pick must be bag or random")
    if not 0 <= req.level <= 80:
        raise HTTPException(400, "level must be 0..80")
    if req.seed < 0:
        raise HTTPException(400, "seed must be >= 0")
    if req.ilvl != -1 and not 0 <= req.ilvl <= 400:
        raise HTTPException(400, "ilvl must be 0 (server default), -1 (no limit) or 1..400")
    if req.quality not in range(0, 6):
        raise HTTPException(400, "quality must be 0 (server default) or 1..5")
    if not 0 <= req.breaker <= 1000:
        raise HTTPException(400, "breaker must be 0 (off) or a count")

    async with sv.lock:
        running = sv.active()
        if running is not None:
            raise HTTPException(409, f"a continuous session is already running "
                                     f"({running.id}, started by {running.state.get('owner')})")
        config = {"pool": pool, "concurrent": req.concurrent, "pick": req.pick,
                  "level": req.level, "seed": req.seed, "ilvl": req.ilvl,
                  "quality": req.quality, "autoResume": req.autoResume,
                  "breaker": req.breaker}
        audit(request, f"soak start pool={','.join(entry_key(e['token'], e['heroic']) for e in pool)} "
                       f"concurrent={req.concurrent}")
        s = await sv.start(me(request), config)
    return view(s, with_stats=True)


@router.post("/api/soak/{soak_id}/edit")
async def api_soak_edit(soak_id: str, req: SoakEditRequest, request: Request):
    sv = sup()
    s = get_soak(soak_id)
    require_owner(request, s, "editing")
    if not s.active:
        raise HTTPException(409, "this session is not running")
    pool = None
    if req.pool is not None:
        cat, rows = await catalogue_rows()
        pool = validate_pool(rows, req.pool, cat)
    if req.concurrent is not None:
        validate_concurrent(req.concurrent)
    if pool is None and req.concurrent is None:
        raise HTTPException(400, "nothing to change")
    if not s.state.get("currentPlanId"):
        raise HTTPException(409, "the worldserver has not registered this session's plan "
                                 "yet — try again in a few seconds")
    audit(request, f"soak edit {soak_id}"
                   + (f" pool={','.join(entry_key(e['token'], e['heroic']) for e in pool)}" if pool else "")
                   + (f" concurrent={req.concurrent}" if req.concurrent is not None else ""))
    async with sv.lock:
        await sv.edit(s, pool=pool, concurrent=req.concurrent)
    return view(s)


@router.post("/api/soak/{soak_id}/pause")
async def api_soak_pause(soak_id: str, request: Request):
    sv = sup()
    s = get_soak(soak_id)
    require_owner(request, s, "pausing")
    if not s.active:
        raise HTTPException(409, "this session is not running")
    audit(request, f"soak pause {soak_id}")
    async with sv.lock:
        await sv.pause(s)
    return view(s)


@router.post("/api/soak/{soak_id}/resume")
async def api_soak_resume(soak_id: str, request: Request):
    sv = sup()
    s = get_soak(soak_id)
    require_owner(request, s, "resuming")
    if not s.active:
        raise HTTPException(409, "this session is not running")
    audit(request, f"soak resume {soak_id}")
    async with sv.lock:
        await sv.resume(s)
    return view(s)


@router.post("/api/soak/{soak_id}/stop")
async def api_soak_stop(soak_id: str, req: SoakStopRequest, request: Request):
    """drain: stop launching, let live runs finish, then end the plan.
    now: end the plan at once (live runs are aborted and recorded)."""
    sv = sup()
    s = get_soak(soak_id)
    require_owner(request, s, "stopping")
    if req.mode not in ("drain", "now"):
        raise HTTPException(400, "mode must be drain or now")
    if not s.active:
        raise HTTPException(409, "this session is not running")
    audit(request, f"soak stop {soak_id} mode={req.mode}")
    async with sv.lock:
        await sv.stop(s, req.mode)
    return view(s)


@router.delete("/api/soak/{soak_id}")
async def api_soak_delete(soak_id: str, request: Request):
    sv = sup()
    s = get_soak(soak_id)
    require_owner(request, s, "deleting")
    if s.active:
        raise HTTPException(409, "stop the session before deleting it")
    audit(request, f"soak delete {soak_id}")
    sv.delete(s)
    return {"deleted": soak_id}


def _matches(row, result, dungeon, cluster):
    r = row.get("result")
    if result == "fail" and r == "success":
        return False
    if result == "ok" and r != "success":
        return False
    if result == "lost" and r != "lost":
        return False
    if dungeon and row_key(row) != dungeon:
        return False
    if cluster and (r or "?") + ": " + cluster_reason(row.get("failReason")) != cluster:
        return False
    return True


@router.get("/api/soak/{soak_id}/runs")
async def api_soak_runs(soak_id: str, result: str = "all", dungeon: str = "",
                        cluster: str = "", cursor: int = 0, limit: int = 50):
    """The ledger, newest first, filtered server-side and paged by cursor.
    Never truncated: every run the session ever recorded is reachable."""
    s = get_soak(soak_id)
    if result not in ("all", "fail", "ok", "lost"):
        raise HTTPException(400, "result must be all|fail|ok|lost")
    limit = max(1, min(limit, PAGE_MAX))
    cursor = max(0, cursor)
    rows = [r for r in reversed(s.rows()) if _matches(r, result, dungeon, cluster)]
    page = rows[cursor:cursor + limit]
    out = []
    for r in page:
        item = {k: v for k, v in r.items() if not k.startswith("_")}
        if r.get("result") != "success":
            item["evidence"] = s.evidence_state(r.get("runId"))
        out.append(item)
    nxt = cursor + limit if cursor + limit < len(rows) else None
    return {"runs": out, "total": len(rows), "nextCursor": nxt}


@router.get("/api/soak/{soak_id}/runs/{run_id}")
async def api_soak_run(soak_id: str, run_id: str):
    s = get_soak(soak_id)
    if not RUN_ID_RE.fullmatch(run_id):
        raise HTTPException(400, "bad runId")
    rec = s.record(run_id)
    if rec is None:
        raise HTTPException(404, f"{run_id} is not in this session")
    return rec


@router.get("/api/soak/{soak_id}/stats")
async def api_soak_stats(soak_id: str):
    return get_soak(soak_id).stats()


@router.get("/api/soak/{soak_id}/evidence/{run_id}")
async def api_soak_evidence(soak_id: str, run_id: str):
    s = get_soak(soak_id)
    if not RUN_ID_RE.fullmatch(run_id) or not s.has_run(run_id):
        raise HTTPException(404, f"{run_id} is not in this session")
    return sup().evidence(s, run_id)


@router.post("/api/soak/{soak_id}/evidence/{run_id}/capture")
async def api_soak_capture(soak_id: str, run_id: str, request: Request):
    """Re-run a capture that failed (or never ran)."""
    s = get_soak(soak_id)
    require_owner(request, s, "capturing evidence")
    if not RUN_ID_RE.fullmatch(run_id) or not s.has_run(run_id):
        raise HTTPException(404, f"{run_id} is not in this session")
    d = s.evidence_dir / run_id
    for name in ("error.txt",):
        try:
            (d / name).unlink()
        except OSError:
            pass
    audit(request, f"soak capture {soak_id} {run_id}")
    ok = await sup().capture(s, run_id)
    return {"ok": ok, **sup().evidence(s, run_id)}


@router.get("/api/soak/{soak_id}/evidence/{run_id}/{name}")
async def api_soak_evidence_file(soak_id: str, run_id: str, name: str):
    s = get_soak(soak_id)
    if not RUN_ID_RE.fullmatch(run_id):
        raise HTTPException(404, "not found")
    f = sup().evidence_file(s, run_id, name)
    if f is None:
        raise HTTPException(404, "not found")
    return FileResponse(f, media_type="text/plain; charset=utf-8",
                        filename=f.name)


# ---------------------------------------------------------------------------
# Presets: named pools, owned like rosters.
# ---------------------------------------------------------------------------


def presets_file():
    return ctx.cfg.data_dir / "soak_presets.json"


def load_presets():
    try:
        data = json.loads(presets_file().read_text(encoding="utf-8", errors="replace"))
    except (FileNotFoundError, json.JSONDecodeError, OSError):
        return {}
    return data if isinstance(data, dict) else {}


def save_presets(data):
    path = presets_file()
    tmp = path.with_suffix(".json.tmp")
    tmp.write_text(json.dumps(data, indent=1), encoding="utf-8")
    tmp.replace(path)


class PresetSaveRequest(BaseModel):
    name: str
    pool: List[PoolEntry]


@router.get("/api/soak-presets")
async def api_presets(request: Request):
    who, admin = me(request), is_admin(request)
    return {"presets": [
        {"name": k, "pool": v.get("pool") or [], "owner": v.get("owner") or "",
         "writable": admin or not v.get("owner") or v.get("owner") == who}
        for k, v in sorted(load_presets().items())]}


@router.post("/api/soak-presets")
async def api_preset_save(req: PresetSaveRequest, request: Request):
    name = (req.name or "").strip()
    if not 1 <= len(name) <= 60 or any(c in name for c in "\r\n\t"):
        raise HTTPException(400, "a preset name is 1-60 characters")
    cat, rows = await catalogue_rows()
    pool = validate_pool(rows, req.pool, cat)
    presets = load_presets()
    existing = presets.get(name)
    who = me(request)
    if existing and existing.get("owner") and existing.get("owner") != who \
            and not is_admin(request):
        raise HTTPException(403, f"'{name}' belongs to {existing['owner']} — save it "
                                 "under another name")
    presets[name] = {"pool": pool, "owner": who}
    save_presets(presets)
    audit(request, f"soak preset save '{name}' ({len(pool)} entries)")
    return {"saved": name}


@router.delete("/api/soak-presets/{name}")
async def api_preset_delete(name: str, request: Request):
    presets = load_presets()
    if name not in presets:
        raise HTTPException(404, f"no preset '{name}'")
    owner = presets[name].get("owner") or ""
    if owner != me(request) and not is_admin(request):
        raise HTTPException(403, f"'{name}' belongs to {owner or 'nobody'}")
    presets.pop(name)
    save_presets(presets)
    audit(request, f"soak preset delete '{name}'")
    return {"deleted": name}


__all__ = ["router", "PLAN_ID_RE"]
