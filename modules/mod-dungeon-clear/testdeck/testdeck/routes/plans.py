"""`.dc test plan` campaigns, and the dungeon catalogue that drives the forms.

Every input is validated against the module-written catalogue
(dc_test_dungeons.json) before being spliced into a worldserver command. The
UI treats the returned reply as advisory and the live heartbeat as truth.
"""

import json
import re

from fastapi import APIRouter, HTTPException, Request
from pydantic import BaseModel

from .. import bridge as bridge_mod
from ..auth import require_admin
from ..context import ctx
from ..util import clear_jsonl, tail_jsonl
from .runs import audit, refuse_if_live

router = APIRouter()


@router.get("/api/testdungeons")
async def api_testdungeons():
    """The module's dungeon catalogue + test-run caps (dc_test_dungeons.json,
    written once at worldserver startup) — feeds the launch forms. Empty shape
    until the server has written it."""
    try:
        return json.loads(
            ctx.cfg.testdungeons_file.read_text(encoding="utf-8", errors="replace"))
    except (FileNotFoundError, json.JSONDecodeError, OSError):
        return {"limits": {}, "dungeons": []}


@router.get("/api/testplans")
async def api_testplans(limit: int = 50, checkpoints: bool = False):
    """Completed test-plan summaries: tail dc_testplans.jsonl, newest first.
    An endless (continuous-mode) plan also appends periodic checkpoint lines
    while it runs; those are left out unless asked for."""
    keep = None if checkpoints else (lambda row: not row.get("checkpoint"))
    return {"plans": tail_jsonl(ctx.cfg.testplans_file, limit, keep)}


@router.post("/api/testplans/clear")
async def api_testplans_clear(request: Request):
    """Wipe dc_testplans.jsonl (backed up to dc_testplans.jsonl.bak)."""
    require_admin(request, "clearing plan history")
    await refuse_if_live("plan history")
    audit(request, "clear plan history")
    return await clear_jsonl(ctx.cfg.testplans_file, "test-plan history",
                             use_sudo=ctx.cfg.use_sudo)


class PlanStartRequest(BaseModel):
    dungeon: str
    total: int
    concurrent: int = 0
    level: int = 0
    seed: int = 0
    heroic: bool = False
    # Party size for every run in the campaign: 0 = classic 5-man, 2..40 =
    # raid comp. Raid rows only (catalogue "raid": true).
    size: int = 0
    # Gear ceiling for every run in the campaign. 0 = inherit the server's
    # AiPlayerbot.AutoGear* values, -1 = no limit, >0 = that item level.
    # quality is 0 (inherit) or 1..5.
    ilvl: int = 0
    quality: int = 0


class PlanStopRequest(BaseModel):
    planId: str


def resolve_alias(cat, token):
    """A retired dungeon token (the catalogue's "aliases": brs -> lbrs after the
    Blackrock Spire split) as the token that replaced it; any other unchanged."""
    aliases = cat.get("aliases") or {}
    target = aliases.get(token)
    return target if isinstance(target, str) and target else token


async def catalogue_rows():
    """{token: row} from the catalogue, refusing early if it is not there yet."""
    cat = await api_testdungeons()
    rows = {d.get("token"): d for d in cat.get("dungeons", [])}
    if not rows:
        raise HTTPException(503, "dungeon catalogue not available yet "
                                 "(dc_test_dungeons.json missing — worldserver up?)")
    return cat, rows


def check_dungeon(rows, token, heroic):
    """Shared by plan, single-run and roster starts."""
    if token not in rows:
        raise HTTPException(400, f"unknown dungeon '{token}'")
    # heroicLevel 0/absent = no heroic mode: classic dungeons, which have no
    # heroic difficulty at all (mirrors the module-side gate).
    row = rows[token]
    # Scenario rows (module T1): a slice of a parent dungeon, launched by its
    # own token. The module refuses heroic on them (heroicLevel is always 0);
    # the parent must be in the same catalogue, or the row is a stale sidecar.
    if row.get("scenario"):
        parent = row.get("scenarioOf") or ""
        if parent not in rows or rows[parent].get("scenario"):
            raise HTTPException(400, f"scenario '{token}' has no parent dungeon "
                                     f"'{parent}' in the catalogue")
        if heroic:
            raise HTTPException(400, f"scenario '{token}' has no heroic mode")
    if heroic and not row.get("heroicLevel"):
        raise HTTPException(400, f"'{token}' has no heroic mode "
                                 "(classic dungeons have none)")


def check_size(rows, token, size):
    """size is a raid-row knob: the module fields any 2..40 comp there, and a
    5-man dungeon can't be entered by a raid group at all — so the deck only
    forwards a size the catalogue's own bounds for that row allow."""
    if size == 0:
        return
    row = rows[token]
    if not row.get("raid"):
        raise HTTPException(400, f"'{token}' is not a raid — size only applies "
                                 "to raid rows")
    # A scenario runs at its parent's default size: its focus, grace and
    # watchdog budget were authored for that raid, so the form locks it.
    if row.get("scenario"):
        locked = int(row.get("defaultSize") or 0)
        if locked and size != locked:
            raise HTTPException(400, f"scenario '{token}' runs at size {locked} only")
    lo = int(row.get("sizeMin") or 2)
    hi = int(row.get("sizeMax") or 40)
    if not lo <= size <= hi:
        raise HTTPException(400, f"size must be {lo}..{hi} for '{token}'")


def check_gear(rows, token, heroic, ilvl, quality):
    """An item level is only accepted if the module's own ladder for this
    dungeon+difficulty offers it. The worldserver takes any number from the
    console, but the form is meant to be the curated list — an ilvl that
    isn't on it is a stale page, not a considered choice."""
    if quality not in range(0, 6):
        raise HTTPException(400, "quality must be 0 (server default) or 1..5")
    if ilvl in (0, -1):
        return
    row = rows[token]
    ladder = row.get("gearHeroic" if heroic else "gear") or row.get("gear") or []
    allowed = {c.get("ilvl") for c in ladder}
    if ilvl not in allowed:
        raise HTTPException(400, f"ilvl {ilvl} is not one of the choices for "
                                 f"'{token}' ({sorted(allowed)}) — reload the page")


@router.post("/api/testplans/start")
async def api_testplans_start(req: PlanStartRequest, request: Request):
    cat, rows = await catalogue_rows()
    req.dungeon = resolve_alias(cat, req.dungeon)
    check_dungeon(rows, req.dungeon, req.heroic)
    # planMaxTotal 0/absent = unlimited (the module's default). Only mirror a
    # positive cap — inventing a local one here just refused plans the
    # worldserver would have accepted.
    max_total = int(cat.get("limits", {}).get("planMaxTotal") or 0)
    if req.total < 1 or (max_total and req.total > max_total):
        raise HTTPException(400, "total must be >= 1" +
                                 (f" and <= {max_total}" if max_total else ""))
    if req.concurrent < 0:
        raise HTTPException(400, "concurrent must be >= 0 (0 = module default)")
    if not 0 <= req.level <= 80:
        raise HTTPException(400, "level must be 0..80")
    if req.seed < 0:
        raise HTTPException(400, "seed must be >= 0")
    check_size(rows, req.dungeon, req.size)
    check_gear(rows, req.dungeon, req.heroic, req.ilvl, req.quality)

    cmd = f".dc test plan start {req.dungeon} total={req.total}"
    if req.heroic:
        cmd += " heroic"
    if req.size:
        cmd += f" size={req.size}"
    if req.concurrent:
        cmd += f" concurrent={req.concurrent}"
    if req.level:
        cmd += f" level={req.level}"
    if req.seed:
        cmd += f" seed={req.seed}"
    if req.ilvl:
        cmd += " ilvl=none" if req.ilvl == -1 else f" ilvl={req.ilvl}"
    if req.quality:
        cmd += f" quality={req.quality}"
    audit(request, cmd)
    reply = await ctx.bridge.exec(cmd)
    return bridge_mod.public_reply(reply, cmd)


@router.post("/api/testplans/stop")
async def api_testplans_stop(req: PlanStopRequest, request: Request):
    """Stop one campaign (exact planId, e.g. tp-20260719-113000-1) or 'all'."""
    if req.planId != "all" and not re.fullmatch(r"tp-[A-Za-z0-9-]{1,40}", req.planId):
        raise HTTPException(400, "bad planId")
    cmd = f".dc test plan stop {req.planId}"
    audit(request, cmd)
    reply = await ctx.bridge.exec(cmd)
    return bridge_mod.public_reply(reply, cmd)
