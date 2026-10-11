"""The dungeon map pack: DBC decoding, floor selection, and the routes.

Nothing here builds a real pack — that needs Pillow, mpyq and a WoW client,
none of which a test host has. What it does cover is every part the server
runs on a request, plus the two decoding traps that made the first attempt at
this feature fail.
"""

import json
import struct
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from conftest import make_client              # noqa: E402
from testdeck import mappack as mp            # noqa: E402


# --------------------------------------------------------------------------- #
# DBC decoding
# --------------------------------------------------------------------------- #

def wdbc(rows, record_size, string_block=b"\0"):
    head = struct.pack("<4s4I", b"WDBC", len(rows), record_size // 4,
                       record_size, len(string_block))
    return head + b"".join(rows) + string_block


def test_dungeon_map_float_order_is_y_then_x():
    """The four floats are (minY, maxY, minX, maxX), NOT minX/maxX/minY/maxY as
    wowdev names them — the first pair is the world Y axis.

    This is the trap that put every dot in the wrong hemisphere the first time.
    Reading them in the documented order and calling it a day is exactly the
    bug, so the ordering gets a test of its own.
    """
    row = struct.pack("<3i4f i", 136, 389, 1,
                      -285.99, 452.87,      # minY, maxY
                      -452.95, 39.62,       # minX, maxX
                      321)
    dm, ids = mp.load_dungeon_map([wdbc([row], 32)])
    # DBC floats are float32; compare at that precision, not exactly.
    assert dm[389][1] == pytest.approx((-452.95, 39.62, -285.99, 452.87),
                                       rel=1e-6)
    assert ids[136] == (389, 1)


def test_world_map_area_counts_bounded_rows():
    """A continent has one bounded row per ZONE; an instance has exactly one.
    That count is the only thing separating them, and treating a continent as
    a dungeon map produces a plausible-looking, completely wrong image."""
    def row(map_id, name_ofs, left, right, top, bottom):
        return struct.pack("<3i i 4f 3i", 1, map_id, 0, name_ofs,
                           left, right, top, bottom, -1, 0, 0)

    strings = b"\0Ragefire\0Elwynn\0Westfall\0"
    ragefire, elwynn, westfall = 1, 10, 17
    blob = wdbc([row(389, ragefire, 0, 0, 0, 0),
                 row(0, elwynn, 100.0, -100.0, 200.0, -200.0),
                 row(0, westfall, 300.0, 50.0, 400.0, 100.0)], 44, strings)
    wma = mp.load_world_map_area([blob])

    assert wma[389]["name"] == "Ragefire"
    assert wma[389]["bounded"] == 0        # all-zero by design; name only
    assert wma[0]["bounded"] == 2          # a continent
    # One record is kept per map — the FIRST bounded row, Elwynn here. What
    # matters is the axis mapping: left/right are Y max/min, top/bottom are
    # X max/min.
    assert wma[0]["minY"] == -100.0 and wma[0]["maxY"] == 100.0
    assert wma[0]["minX"] == -200.0 and wma[0]["maxX"] == 200.0


def test_dungeon_map_chunk_resolves_floors_and_ignores_strays():
    dm_rows = [struct.pack("<3i4f i", 1, 574, 1, 0.0, 1.0, 0.0, 1.0, 0),
               struct.pack("<3i4f i", 2, 574, 2, 0.0, 1.0, 0.0, 1.0, 0)]
    _dm, ids = mp.load_dungeon_map([wdbc(dm_rows, 32)])

    def chunk(map_id, group, dm_id, min_z):
        return struct.pack("<4i f", 1, map_id, group, dm_id, min_z)

    blob = wdbc([chunk(574, 22853, 1, -10000.0),
                 chunk(574, 22853, 2, 68.0),
                 chunk(574, 9999, 4242, 0.0),      # DungeonMapID we don't know
                 chunk(601, 25050, 1, 0.0)], 20)   # right row, wrong map
    chunks = mp.load_dungeon_map_chunk([blob], ids)

    assert sorted(chunks[574]) == [(22853, 1, -10000.0), (22853, 2, 68.0)]
    assert 601 not in chunks


def test_build_floor_rule_keeps_only_real_thresholds():
    rule = mp.build_floor_rule(
        {1: None, 2: None, 3: None},
        [(1, 1, -10000.0), (1, 2, 68.0), (2, 3, 156.0), (3, 9, 500.0)])
    assert rule["default"] == 1
    assert rule["zsteps"] == [{"minZ": 68.0, "floor": 2},
                              {"minZ": 156.0, "floor": 3}]
    # floor 9 is not a floor of this map and must not become a step,
    # but the raw row still rides along for a WMO-group-aware consumer.
    assert len(rule["chunks"]) == 4


def test_affine_round_trips_the_rect_corners():
    """The corners of the world rect must land on the corners of the image —
    that IS the calibration, so a sign slip here is the whole feature."""
    bounds = (-452.95, 39.62, -285.99, 452.87)     # minX, maxX, minY, maxY
    a = mp.affine(bounds)
    px = lambda x, y: a["ax"] * x + a["bx"] * y + a["cx"]   # noqa: E731
    py = lambda x, y: a["ay"] * x + a["by"] * y + a["cy"]   # noqa: E731

    # maxY (furthest west) is the LEFT edge; maxX (furthest north) is the TOP.
    assert px(0, bounds[3]) == 0
    assert round(px(0, bounds[2]), 6) == mp.USABLE_W
    assert py(bounds[1], 0) == 0
    assert round(py(bounds[0], 0), 6) == mp.USABLE_H


# --------------------------------------------------------------------------- #
# Floor selection
# --------------------------------------------------------------------------- #

def floors(*rects):
    return [{"floor": i + 1, "minX": r[0], "maxX": r[1],
             "minY": r[2], "maxY": r[3]} for i, r in enumerate(rects)]


def test_pick_floor_single_rect():
    rec = {"floors": floors((0, 100, 0, 100))}
    assert mp.pick_floor(50, 50, 999, rec) == 1


def test_pick_floor_off_map_is_none_not_a_guess():
    """No floor drawing a spot is a real answer. Ulduar, Sunwell and Black
    Temple all have areas the art does not cover, and silently snapping those
    to floor 1 would draw bots in rooms they are nowhere near."""
    rec = {"floors": floors((0, 100, 0, 100))}
    assert mp.pick_floor(500, 500, 0, rec) is None


def test_pick_floor_disjoint_wings_need_no_z():
    """Scarlet Monastery and Dire Maul have NO multi-storey WMO group at all;
    rect containment alone has to pick the wing."""
    rec = {"floors": floors((0, 100, 0, 100), (200, 300, 200, 300))}
    assert mp.pick_floor(250, 250, 0, rec) == 2
    assert mp.pick_floor(50, 50, 0, rec) == 1


def test_pick_floor_uses_blizzards_z_ladder_on_overlap():
    rec = {"floors": floors((0, 100, 0, 100), (0, 100, 0, 100),
                            (0, 100, 0, 100)),
           "floor_rule": {"default": 1,
                          "zsteps": [{"minZ": 68.0, "floor": 2},
                                     {"minZ": 156.0, "floor": 3}]}}
    assert mp.pick_floor(50, 50, 10, rec) == 1        # below every step
    assert mp.pick_floor(50, 50, 100, rec) == 2
    assert mp.pick_floor(50, 50, 900, rec) == 3
    assert mp.pick_floor(50, 50, 68.0, rec) == 2      # the step is inclusive


def test_pick_floor_ignores_a_step_whose_floor_is_elsewhere():
    """A ladder entry only counts when that floor's rect also contains the
    point — otherwise a tall bot in the west wing gets the east wing's map."""
    rec = {"floors": floors((0, 100, 0, 100), (500, 600, 500, 600)),
           "floor_rule": {"default": 1,
                          "zsteps": [{"minZ": 10.0, "floor": 2}]}}
    assert mp.pick_floor(50, 50, 900, rec) == 1


# --------------------------------------------------------------------------- #
# image_path containment
# --------------------------------------------------------------------------- #

def fake_pack(root, fmt="webp"):
    (root / "maps").mkdir(parents=True, exist_ok=True)
    (root / "maps" / "389_1.webp").write_bytes(b"RIFF0000WEBPfake")
    pack = {"generated": "2026-08-30T00:00:00", "usable": [1002, 668],
            "maps": {"389": {"name": "Ragefire", "kind": "dungeon",
                             "floors": [{"floor": 1,
                                         "image": "maps/389_1." + fmt,
                                         "minX": -452.95, "maxX": 39.62,
                                         "minY": -285.99, "maxY": 452.87,
                                         "ax": 0.0, "bx": -1.0, "cx": 0.0,
                                         "ay": -1.0, "by": 0.0, "cy": 0.0,
                                         "w": 1002, "h": 668,
                                         "source": "wdm"}],
                             "floor_rule": {"default": 1, "zsteps": [],
                                            "chunks": [{"wmoGroup": 1,
                                                        "floor": 1,
                                                        "minZ": -10000.0}]}}}}
    (root / "mappack.json").write_text(json.dumps(pack))
    return pack


def test_image_path_resolves_a_real_floor(tmp_path):
    fake_pack(tmp_path)
    got = mp.image_path(tmp_path, 389, 1)
    assert got and Path(got).name == "389_1.webp"


def test_image_path_refuses_to_escape_the_pack(tmp_path):
    """Containment is resolve-then-compare, never a scan for "..". Joining an
    ABSOLUTE path discards everything to its left, which is the case a "no
    dots" blocklist misses — the same hole this app already had once in the
    SPA route."""
    secret = tmp_path.parent / "session.secret"
    secret.write_bytes(b"top secret")
    root = tmp_path / "pack"
    pack = fake_pack(root)
    for evil in ("../session.secret", str(secret), "maps/../../session.secret"):
        pack["maps"]["389"]["floors"][0]["image"] = evil
        (root / "mappack.json").write_text(json.dumps(pack))
        assert mp.image_path(root, 389, 1) is None


def test_load_rejects_a_pack_that_is_not_one(tmp_path):
    (tmp_path / "mappack.json").write_text('{"maps": "not a dict"}')
    assert mp.load(tmp_path) is None
    (tmp_path / "mappack.json").write_text("{ this is not json")
    assert mp.load(tmp_path) is None
    assert mp.load(tmp_path / "nowhere") is None


# --------------------------------------------------------------------------- #
# Routes
# --------------------------------------------------------------------------- #

def test_api_mappack_says_why_when_there_is_none(client, cfg):
    r = client.get("/api/mappack")
    assert r.status_code == 200
    body = r.json()
    assert body["available"] is False and body["maps"] == []
    # The reason has to name the command that fixes it — this is a setup
    # state, not an error, and the operator is the one who can resolve it.
    assert "testdeck mappack" in body["reason"]


def test_api_mappack_serves_a_built_pack(client, cfg):
    fake_pack(cfg.mappack_dir)

    r = client.get("/api/mappack")
    assert r.json() == {"available": True,
                        "generated": "2026-08-30T00:00:00",
                        "usable": [1002, 668],
                        "maps": [389]}

    r = client.get("/api/mappack/389")
    body = r.json()
    assert body["mapId"] == 389 and body["name"] == "Ragefire"
    assert body["floors"][0]["url"] == "/api/mappack/img/389/1"
    assert body["floors"][0]["maxY"] == 452.87
    # `chunks` is most of a real manifest's 230 KB and is useless without a
    # WMO group, so it must not be shipped to the browser.
    assert "chunks" not in body["floorRule"]
    assert body["floorRule"]["default"] == 1

    r = client.get("/api/mappack/img/389/1")
    assert r.status_code == 200 and r.content == b"RIFF0000WEBPfake"


def test_api_mappack_404s_for_maps_and_floors_it_lacks(client, cfg):
    fake_pack(cfg.mappack_dir)
    assert client.get("/api/mappack/601").status_code == 404
    assert client.get("/api/mappack/img/389/9").status_code == 404
    assert client.get("/api/mappack/img/601/1").status_code == 404


def test_api_mappack_picks_up_a_rebuild_without_a_restart(client, cfg):
    """A pack is regenerated by a CLI command while the server is running.
    Caching it by identity alone would make that look like a no-op until
    someone restarted the deck."""
    assert client.get("/api/mappack").json()["available"] is False
    fake_pack(cfg.mappack_dir)
    assert client.get("/api/mappack").json()["available"] is True


def test_mappack_requires_a_session(cfg):
    """Every /api/* route is behind the session gate; the map pack reveals the
    dungeon a run is in and where the party is standing."""
    anon = make_client(cfg)
    for path in ("/api/mappack", "/api/mappack/389", "/api/mappack/img/389/1"):
        assert anon.get(path).status_code == 401


def test_missing_build_deps_names_pip_packages():
    """Build-only imports must never be required to SERVE a pack, so this list
    is what the CLI turns into an install hint."""
    assert set(mp.missing_build_deps()) <= {"pillow", "mpyq"}
