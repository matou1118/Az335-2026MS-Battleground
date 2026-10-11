"""Dungeon map pack: Blizzard map art plus the exact world->image transform.

Two halves that must not be confused:

  * READING (`load`, `pick_floor`) is pure stdlib and is what the server does
    on every request. It never needs Pillow or mpyq.
  * BUILDING (`build`) turns a WoW client — plus, for the Classic and TBC
    dungeons the 3.3.5a client has no maps for, the WDM-patch release MPQ —
    into that pack. It imports Pillow and mpyq lazily, so a deck that only
    serves an already-built pack does not need them installed at all.

The art is Blizzard's and is NOT redistributed with the Test Deck: a pack is
generated on the operator's own host, from their own client, into `data_dir`.

WHY THE TRANSFORM IS EXACT. DungeonMap.dbc states, per floor, the world-space
rectangle its art covers, so there is nothing to calibrate — an earlier attempt
at this used the Atlas addon's art, which is a hand-drawn schematic that no
affine can fit, and it never aligned. Two traps, both verified against every
creature spawn in acore_world:

  * DungeonMap's four floats are NOT minX/maxX/minY/maxY as wowdev names them.
    The order is (minY, maxY, minX, maxX) — the first pair is the world Y
    axis. WorldMapArea is the same: locLeft/locRight are Y max/min,
    locTop/locBottom are X max/min. The pack normalises this so no consumer
    has to know it.
  * The usable image is 1002x668 inside the assembled 1024x768 tile grid
    (12 tiles, 4 across, 3 down, row-major). Using the full 1024x768 puts
    every dot low and left and drops the bottom of the dungeon off the image.
"""

import io
import json
import os
import struct
import sys
import time
import urllib.request
from collections import defaultdict

PACK_FILE = "mappack.json"

TILE = 256
COLS, ROWS = 4, 3
USABLE_W, USABLE_H = 1002, 668

WDM_REPO = "Trimitor/WDM-patch"
WDM_TAG = "2.4.5-stable"

# Interface art lives in the LOCALE archives, not Data/*.MPQ. Highest patch
# wins. Searching the non-locale archives finds nothing at all.
LOCALE_MPQS = [
    "patch-{loc}-3.MPQ", "patch-{loc}-2.MPQ", "patch-{loc}.MPQ",
    "lichking-locale-{loc}.MPQ", "expansion-locale-{loc}.MPQ",
    "locale-{loc}.MPQ", "base-{loc}.MPQ",
]


# --------------------------------------------------------------------------- #
# Reading — stdlib only, this is the request path
# --------------------------------------------------------------------------- #

def load(mappack_dir):
    """The pack manifest, or None when this host has not built one."""
    try:
        with open(os.path.join(str(mappack_dir), PACK_FILE),
                  encoding="utf-8") as fh:
            data = json.load(fh)
    except (OSError, ValueError):
        return None
    return data if isinstance(data.get("maps"), dict) else None


def pick_floor(x, y, z, rec):
    """Which floor of `rec` a world position belongs on, or None for "no floor
    draws this spot" — a real answer, not a failure. Ulduar, Sunwell and Black
    Temple all have areas their map art simply does not cover.

    Rect containment decides it outright for most maps: Scarlet Monastery and
    Dire Maul, for instance, have no multi-storey WMO group at all, their wings
    being separated in XY. Where rects do overlap, `floor_rule.zsteps` breaks
    the tie — those are Blizzard's own DungeonMapChunk MinZ thresholds, not a
    heuristic of ours.
    """
    inside = [f["floor"] for f in rec.get("floors", ())
              if f["minX"] <= x <= f["maxX"] and f["minY"] <= y <= f["maxY"]]
    if not inside:
        return None
    if len(inside) == 1:
        return inside[0]
    rule = rec.get("floor_rule")
    if rule:
        best = None
        for step in rule.get("zsteps", ()):            # ascending by minZ
            if z >= step["minZ"] and step["floor"] in inside:
                best = step["floor"]
        if best is not None:
            return best
    return min(inside)


def image_path(mappack_dir, map_id, floor, pack=None):
    """The on-disk image for one floor, or None if the pack does not have it.

    Resolve-then-contain, never a scan for "..": the map id and floor arrive
    from the URL, and joining an absolute path silently discards everything to
    its left. Same lesson as the SPA route.
    """
    pack = pack if pack is not None else load(mappack_dir)
    if not pack:
        return None
    rec = pack["maps"].get(str(map_id))
    if not rec:
        return None
    rel = next((f.get("image") for f in rec.get("floors", ())
                if f.get("floor") == floor), None)
    if not rel:
        return None
    root = os.path.realpath(str(mappack_dir))
    full = os.path.realpath(os.path.join(root, rel))
    if os.path.commonpath([root, full]) != root or not os.path.isfile(full):
        return None
    return full


# --------------------------------------------------------------------------- #
# Building — DBC
# --------------------------------------------------------------------------- #

def read_wdbc(blob):
    if blob[:4] != b"WDBC":
        raise ValueError("not a WDBC file")
    rc, _fc, rs, _ss = struct.unpack_from("<4I", blob, 4)
    body = blob[20:20 + rc * rs]
    return [body[i * rs:(i + 1) * rs] for i in range(rc)], blob[20 + rc * rs:]


def _cstr(strs, ofs):
    if ofs <= 0 or ofs >= len(strs):
        return ""
    return strs[ofs:strs.find(b"\0", ofs)].decode("utf-8", "replace")


def load_dungeon_map(blobs):
    """-> ({mapId: {floor: (minX, maxX, minY, maxY)}}, {rowId: (mapId, floor)}).

    The second dict is what DungeonMapChunk's DungeonMapID points at.
    """
    out = defaultdict(dict)
    ids = {}
    for blob in blobs:
        if not blob:
            continue
        rows, _strs = read_wdbc(blob)
        for r in rows:
            row_id, map_id, floor = struct.unpack_from("<3i", r, 0)
            min_y, max_y, min_x, max_x = struct.unpack_from("<4f", r, 12)
            out[map_id][floor] = (min_x, max_x, min_y, max_y)
            ids[row_id] = (map_id, floor)
    return out, ids


def load_world_map_area(blobs):
    """-> {mapId: {name, minX, maxX, minY, maxY, bounded}}, later blobs winning.

    An instance has exactly one row; a continent has one PER ZONE, so `bounded`
    — how many of its rows carry real bounds — is what tells the two apart, and
    a continent is not a dungeon map. A DungeonMap-driven instance's row is
    all-zero by design and is kept only for the art directory name.
    """
    out = {}
    for blob in blobs:
        if not blob:
            continue
        rows, strs = read_wdbc(blob)
        for r in rows:
            _id, map_id, _area = struct.unpack_from("<3i", r, 0)
            name = _cstr(strs, struct.unpack_from("<i", r, 12)[0])
            left, right, top, bottom = struct.unpack_from("<4f", r, 16)
            rec = {"name": name, "minX": bottom, "maxX": top,
                   "minY": right, "maxY": left, "bounded": 0}
            have = out.get(map_id)
            n = (have["bounded"] if have else 0) + (1 if bottom != top else 0)
            if have is None or (have["minX"] == have["maxX"] and bottom != top):
                out[map_id] = rec
            out[map_id]["bounded"] = n
    return out


def load_dungeon_map_chunk(blobs, ids):
    """-> {mapId: [(wmoGroupId, floor, minZ)]}.

    How the client itself picks a floor: the WMO group you stand in names the
    floor, and where one group spans storeys a MinZ threshold splits it.
    MinZ -10000 means "no height condition".
    """
    out = defaultdict(list)
    for blob in blobs:
        if not blob:
            continue
        rows, _strs = read_wdbc(blob)
        for r in rows:
            _id, map_id, wmo_group, dm_id = struct.unpack_from("<4i", r, 0)
            min_z = struct.unpack_from("<f", r, 16)[0]
            known = ids.get(dm_id)
            if known and known[0] == map_id:
                out[map_id].append((wmo_group, known[1], min_z))
    return out


def build_floor_rule(floors, chunks):
    """The chunk rows, reduced to something an (x, y, z)-only consumer can use.

    `chunks` rides along verbatim so a consumer that DOES know the unit's WMO
    group can do the exact client lookup instead of the approximation.
    """
    steps = sorted({(round(mz, 2), fl) for _g, fl, mz in chunks
                    if mz > -9999.0 and fl in floors})
    return {"default": min(floors),
            "zsteps": [{"minZ": mz, "floor": fl} for mz, fl in steps],
            "chunks": [{"wmoGroup": g, "floor": fl, "minZ": round(mz, 2)}
                       for g, fl, mz in sorted(chunks)]}


def affine(bounds, w=USABLE_W, h=USABLE_H):
    """px = ax*worldX + bx*worldY + cx ; py = ay*worldX + by*worldY + cy."""
    min_x, max_x, min_y, max_y = bounds
    sx = w / (max_y - min_y)
    sy = h / (max_x - min_x)
    return {"w": w, "h": h,
            "ax": 0.0, "bx": -sx, "cx": max_y * sx,
            "ay": -sy, "by": 0.0, "cy": max_x * sy}


# --------------------------------------------------------------------------- #
# Building — art sources
# --------------------------------------------------------------------------- #

class MpqSource:
    """MPQ archives tried in order. MPQ hashing uppercases the name, so case
    never matters here."""

    def __init__(self, paths, name):
        import mpyq
        self.name = name
        self.archives = []
        for p in paths:
            if not os.path.exists(p):
                continue
            try:
                self.archives.append(mpyq.MPQArchive(p))
            except Exception:                     # truncated or odd archive
                pass

    def __bool__(self):
        return bool(self.archives)

    def get(self, rel):
        name = rel.replace("/", "\\")
        for arc in self.archives:
            try:
                blob = arc.read_file(name)
            except Exception:
                blob = None
            if blob:
                return blob
        return None


class DirSource:
    """A WDM-patch checkout, or any dir laid out as Interface/WorldMap/..."""

    def __init__(self, root, lang):
        self.name = "wdm"
        self.index = {}
        for base in (os.path.join(root, "Stable", lang),
                     os.path.join(root, lang), root):
            if not os.path.isdir(base):
                continue
            for dirpath, _dirs, files in os.walk(base):
                for f in files:
                    full = os.path.join(dirpath, f)
                    rel = os.path.relpath(full, base).replace(os.sep, "/")
                    self.index.setdefault(rel.lower(), full)
            if self.index:
                break

    def __bool__(self):
        return bool(self.index)

    def get(self, rel):
        p = self.index.get(rel.lower())
        if not p:
            return None
        with open(p, "rb") as fh:
            return fh.read()


def client_source(client_root, locale):
    data = os.path.join(str(client_root), "Data", locale)
    if not os.path.isdir(data):
        data = os.path.join(str(client_root), locale)
    return MpqSource([os.path.join(data, p.format(loc=locale))
                      for p in LOCALE_MPQS], "client")


def wdm_release(cache_dir, lang, tag=WDM_TAG, log=None):
    """Download the WDM release MPQ once (~47 MB) and open it.

    One request, not the ~1600 it takes to pull the loose blobs out of the
    project's git tree.
    """
    os.makedirs(str(cache_dir), exist_ok=True)
    path = os.path.join(str(cache_dir), "patch-%s-M-%s.MPQ" % (lang, tag))
    if not os.path.exists(path) or os.path.getsize(path) == 0:
        url = ("https://github.com/%s/releases/download/%s/patch-%s-M.MPQ"
               % (WDM_REPO, tag, lang))
        if log:
            log("downloading %s" % url)
        tmp = path + ".part"
        with urllib.request.urlopen(url, timeout=300) as fh, open(tmp, "wb") as out:
            while True:
                chunk = fh.read(1 << 20)
                if not chunk:
                    break
                out.write(chunk)
        os.replace(tmp, path)
    return MpqSource([path], "wdm")


def tile_names(art_dir, floor, dungeon_style):
    for i in range(1, COLS * ROWS + 1):
        leaf = ("%s%d_%d.blp" % (art_dir, floor, i) if dungeon_style
                else "%s%d.blp" % (art_dir, i))
        yield "Interface/WorldMap/%s/%s" % (art_dir, leaf)


def assemble(sources, art_dir, floor, dungeon_style):
    """-> (PIL.Image cropped to 1002x668, source name) or (None, None)."""
    from PIL import Image
    for src in sources:
        blobs = []
        for rel in tile_names(art_dir, floor, dungeon_style):
            b = src.get(rel)
            if not b:
                break
            blobs.append(b)
        if len(blobs) != COLS * ROWS:
            continue
        canvas = Image.new("RGB", (COLS * TILE, ROWS * TILE))
        try:
            for i, b in enumerate(blobs):
                t = Image.open(io.BytesIO(b)).convert("RGB")
                canvas.paste(t, ((i % COLS) * TILE, (i // COLS) * TILE))
        except Exception:
            continue
        return canvas.crop((0, 0, USABLE_W, USABLE_H)), src.name
    return None, None


# --------------------------------------------------------------------------- #

def missing_build_deps():
    """Which of the build-only dependencies this interpreter lacks."""
    out = []
    for mod, pkg in (("PIL", "pillow"), ("mpyq", "mpyq")):
        try:
            __import__(mod)
        except ImportError:
            out.append(pkg)
    return out


def build(out_dir, dbc_dir, client_dir=None, wdm_dir=None, cache_dir=None,
          lang="enUS", maps=None, fmt="webp", quality=82, offline=False,
          log=None):
    """Generate the pack into `out_dir`. Returns (written, gaps).

    `maps` is an explicit list of map ids, or None for everything the DBCs
    describe. Art is looked for in WDM first (it also carries a corrected
    Wailing Caverns rect) and the client second.
    """
    log = log or (lambda _m: None)
    lack = missing_build_deps()
    if lack:
        raise RuntimeError(
            "building a map pack needs %s — install with:  pip install %s"
            % (" and ".join(lack), " ".join(lack)))

    def stock(name):
        p = os.path.join(str(dbc_dir), name) if dbc_dir else None
        if p and os.path.exists(p):
            with open(p, "rb") as fh:
                return fh.read()
        return None

    sources = []
    wdm = None
    if wdm_dir:
        d = DirSource(str(wdm_dir), lang)
        wdm = d if d else None
    if wdm is None and not offline:
        found = wdm_release(cache_dir or os.path.join(str(out_dir), ".cache"),
                            lang, log=log)
        wdm = found if found else None
    if wdm is not None:
        sources.append(wdm)
    if client_dir:
        client = client_source(client_dir, lang)
        if client:
            sources.append(client)
    if not sources:
        raise RuntimeError(
            "no art source: point --client at a WoW client, or allow the "
            "WDM-patch download (drop --offline)")

    dm, dm_ids = load_dungeon_map([stock("DungeonMap.dbc")])
    wma = load_world_map_area([stock("WorldMapArea.dbc")])
    chunk_blobs = [stock("DungeonMapChunk.dbc")]
    if wdm is not None:
        blob = wdm.get("DBFilesClient/DungeonMap.dbc")
        if blob:
            more, more_ids = load_dungeon_map([blob])
            for map_id, floors in more.items():
                dm[map_id].update(floors)
            dm_ids.update(more_ids)
        blob = wdm.get("DBFilesClient/WorldMapArea.dbc")
        if blob:
            wma.update(load_world_map_area([blob]))
        blob = wdm.get("DBFilesClient/DungeonMapChunk.dbc")
        if blob:
            chunk_blobs.append(blob)
    if not dm and not wma:
        raise RuntimeError("no DBC data — set [paths] dbc_dir to the server's "
                           "extracted dbc/ directory")
    chunks = load_dungeon_map_chunk(chunk_blobs, dm_ids)

    want = sorted(maps) if maps else sorted(
        set(dm) | {m for m, v in wma.items()
                   if v["minX"] != v["maxX"] and v["bounded"] == 1})

    maps_dir = os.path.join(str(out_dir), "maps")
    os.makedirs(maps_dir, exist_ok=True)
    save_kw = {} if fmt == "png" else {"quality": quality}

    pack = {"generated": time.strftime("%Y-%m-%dT%H:%M:%S"),
            "usable": [USABLE_W, USABLE_H],
            "projection": "px=(maxY-y)/(maxY-minY)*w ; py=(maxX-x)/(maxX-minX)*h",
            "maps": {}}
    gaps, made = [], 0

    for map_id in want:
        entry = wma.get(map_id)
        if not entry:
            gaps.append((map_id, "no WorldMapArea row — no art directory name"))
            continue
        art_dir = entry["name"]
        floors = dict(dm.get(map_id) or {})
        dungeon_style = bool(floors)
        if not dungeon_style:
            if entry["minX"] == entry["maxX"]:
                gaps.append((map_id, "%s: no DungeonMap rows and no bounds"
                             % art_dir))
                continue
            if entry["bounded"] > 1:
                gaps.append((map_id, "%s: a continent, not a dungeon map"
                             % art_dir))
                continue
            floors = {1: (entry["minX"], entry["maxX"],
                          entry["minY"], entry["maxY"])}

        rec = {"name": art_dir, "kind": "dungeon" if dungeon_style else "terrain",
               "floors": []}
        for floor in sorted(floors):
            b = floors[floor]
            if b[1] - b[0] <= 0 or b[3] - b[2] <= 0:
                # Map 0 carries a few placeholder rows with no extent.
                gaps.append((map_id, "%s floor %d: zero-extent bounds"
                             % (art_dir, floor)))
                continue
            f = {"floor": floor,
                 "image": "maps/%d_%d.%s" % (map_id, floor, fmt),
                 "minX": round(b[0], 3), "maxX": round(b[1], 3),
                 "minY": round(b[2], 3), "maxY": round(b[3], 3)}
            f.update(affine(b))
            rec["floors"].append(f)
        if len(floors) > 1 and chunks.get(map_id):
            rec["floor_rule"] = build_floor_rule(floors, chunks[map_id])

        keep = []
        for f in rec["floors"]:
            img, origin = assemble(sources, art_dir, f["floor"], dungeon_style)
            if img is None:
                gaps.append((map_id, "%s floor %d: art not found"
                             % (art_dir, f["floor"])))
                continue
            img.save(os.path.join(str(out_dir), f["image"]), **save_kw)
            made += 1
            f["source"] = origin
            keep.append(f)
        rec["floors"] = keep
        if not keep:
            continue
        pack["maps"][str(map_id)] = rec
        log("  %-5d %-24s %d floor(s)  %s"
            % (map_id, art_dir, len(keep), keep[0]["source"]))

    tmp = os.path.join(str(out_dir), PACK_FILE + ".tmp")
    with open(tmp, "w", encoding="utf-8") as fh:
        json.dump(pack, fh, indent=1, sort_keys=True)
    os.replace(tmp, os.path.join(str(out_dir), PACK_FILE))
    return made, gaps


if __name__ == "__main__":                        # pragma: no cover
    sys.exit("run this as:  python3 -m testdeck mappack")
