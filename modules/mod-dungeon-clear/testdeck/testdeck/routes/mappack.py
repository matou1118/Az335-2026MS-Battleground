"""The dungeon map pack: what the Live page draws bot positions on.

The pack is generated on this host (`python3 -m testdeck mappack`) from the
operator's own WoW client — no Blizzard art ships with the Test Deck — so every
endpoint here has to answer honestly when there is no pack rather than 404 and
leave the UI guessing.

The manifest is served per map, not whole. A full pack's manifest is ~230 KB,
most of it `floor_rule.chunks` (raw WMOGroupID rows kept for a consumer that
knows which WMO group a unit is in). The browser needs one map at a time and
picks floors from rects and zsteps, so `chunks` is dropped on the way out.
"""

import os

from fastapi import APIRouter, HTTPException
from fastapi.responses import FileResponse

from .. import mappack as mp
from ..context import ctx

router = APIRouter()

_cache = {"key": None, "pack": None}


def _pack():
    """The manifest, reloaded when the file changes underneath us — rebuilding
    a pack must not need a server restart."""
    path = os.path.join(str(ctx.cfg.mappack_dir), mp.PACK_FILE)
    try:
        key = os.stat(path).st_mtime_ns
    except OSError:
        _cache.update(key=None, pack=None)
        return None
    if _cache["key"] != key:
        _cache.update(key=key, pack=mp.load(ctx.cfg.mappack_dir))
    return _cache["pack"]


def _reason(cfg):
    """Why there is no pack, in the operator's terms."""
    if not cfg.dbc_dir or not os.path.isdir(str(cfg.dbc_dir)):
        return ("no dbc/ directory found — set [paths] dbc_dir to the "
                "server's extracted DBC folder, then run: "
                "python3 -m testdeck mappack")
    return ("no map pack built on this host — run: "
            "python3 -m testdeck mappack")


@router.get("/api/mappack")
async def api_mappack():
    """Which maps this host can draw. Cheap enough for the Live page to ask
    once per session."""
    pack = _pack()
    if not pack:
        return {"available": False, "reason": _reason(ctx.cfg), "maps": []}
    return {"available": True,
            "generated": pack.get("generated"),
            "usable": pack.get("usable"),
            "maps": sorted(int(m) for m in pack["maps"])}


@router.get("/api/mappack/{map_id}")
async def api_mappack_map(map_id: int):
    pack = _pack()
    rec = (pack or {}).get("maps", {}).get(str(map_id))
    if not rec:
        raise HTTPException(status_code=404, detail="no map art for this map")
    rule = rec.get("floor_rule")
    out = {"mapId": map_id, "name": rec.get("name"), "kind": rec.get("kind"),
           "floors": [dict(f, url="/api/mappack/img/%d/%d" % (map_id, f["floor"]))
                      for f in rec.get("floors", ())]}
    if rule:
        # `chunks` is the bulk of the manifest and useless without a WMO group.
        out["floorRule"] = {"default": rule.get("default"),
                            "zsteps": rule.get("zsteps", [])}
    return out


@router.get("/api/mappack/img/{map_id}/{floor}")
async def api_mappack_img(map_id: int, floor: int):
    path = mp.image_path(ctx.cfg.mappack_dir, map_id, floor, pack=_pack())
    if not path:
        raise HTTPException(status_code=404, detail="no such floor image")
    # Content-hashed by nothing, but a pack is regenerated rarely and the URL
    # carries no version — revalidate rather than pin a stale floor for a day.
    return FileResponse(path, headers={"Cache-Control": "no-cache"})
