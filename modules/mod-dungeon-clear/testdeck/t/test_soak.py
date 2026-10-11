"""Continuous mode: the soak supervisor's reconcile tick against fake sidecar
files and a recording bridge, and the /api/soak routes on top of it."""

import asyncio
import json
import os
import sys
import time
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from conftest import fake_auth_db, make_client, TEST_USER, TEST_PASSWORD   # noqa: E402
from test_routes import CATALOGUE, RecordingBridge, use_bridge          # noqa: E402
from testdeck import soak as S                                          # noqa: E402
from testdeck.context import ctx                                        # noqa: E402
from testdeck.routes.runs import TimelineStore                          # noqa: E402

POOL = [{"token": "blackfathom", "heroic": False},
        {"token": "mechanar", "heroic": True}]
CONFIG = {"pool": POOL, "concurrent": 2, "pick": "bag", "level": 0, "seed": 0,
          "ilvl": 0, "quality": 0, "autoResume": True, "breaker": 3}


def catalogue(pool_support=True):
    cat = json.loads(json.dumps(CATALOGUE))
    if pool_support:
        cat["limits"]["planPool"] = True
    return cat


def write_catalogue(cfg, pool_support=True, mtime=None):
    p = cfg.testdungeons_file
    p.write_text(json.dumps(catalogue(pool_support)))
    if mtime is not None:
        os.utime(p, (mtime, mtime))


def heartbeat(cfg, plans=(), runs=(), ts=None, active=True):
    cfg.testrun_live_file.write_text(json.dumps({
        "active": active, "ts": time.time() if ts is None else ts,
        "plans": list(plans), "runs": list(runs)}))


def plan_obj(pid, active=1, state="running", pool=POOL):
    return {"planId": pid, "dungeon": "pool", "endless": True, "active": active,
            "state": state, "pool": [dict(e) for e in pool], "concurrent": 2}


def run_row(rid, pid, result="success", **kw):
    row = {"runId": rid, "planId": pid, "dungeon": "blackfathom", "heroic": False,
           "result": result, "failReason": "" if result == "success" else "wiped on 'Gelihast' at 12,4",
           "durationS": 600, "bossesKilled": 3, "bossesTotal": 7,
           "startedAtMs": 1, "endedAtMs": 2}
    row.update(kw)
    return row


def append_runs(cfg, *rows, newline=True):
    with cfg.testruns_file.open("a", encoding="utf-8") as fh:
        for i, r in enumerate(rows):
            text = r if isinstance(r, str) else json.dumps(r)
            fh.write(text + ("\n" if newline or i < len(rows) - 1 else ""))


class FakeSpawn:
    """Stands in for util.spawn: records argv, tracks overlap."""

    def __init__(self):
        self.calls = []
        self.active = 0
        self.max_active = 0

    async def __call__(self, argv, timeout=None, merge_stderr=True):
        self.calls.append(argv)
        self.active += 1
        self.max_active = max(self.max_active, self.active)
        await asyncio.sleep(0.01)
        self.active -= 1
        return 0, f"REPORT for {argv[argv.index('--dump') - 3 + 1]}\n".encode(), b""


def make_sup(cfg, bridge=None, timelines=None):
    bridge = bridge or RecordingBridge()
    spawn = FakeSpawn()
    sup = S.SoakSupervisor(cfg, lambda: bridge, lambda: timelines, spawn=spawn)
    sup.nice = False
    tool = cfg.data_dir / "dc_test_run.py"
    tool.parent.mkdir(parents=True, exist_ok=True)
    tool.write_text("# stand-in\n")
    cfg.soak_tool = str(tool)
    return sup, bridge, spawn


def run(coro):
    return asyncio.run(coro)


async def started(cfg, sup, pid="tp-1"):
    """Start a soak and let it adopt plan `pid` from the heartbeat."""
    s = await sup.start("Tester", json.loads(json.dumps(CONFIG)))
    heartbeat(cfg, plans=[plan_obj(pid)])
    await sup.tick()
    assert s.state["currentPlanId"] == pid
    return s


# ---------------------------------------------------------------------------
# start + adoption
# ---------------------------------------------------------------------------


def test_start_issues_one_endless_pool_plan_and_adopts_it(cfg):
    write_catalogue(cfg)
    sup, bridge, _ = make_sup(cfg)

    async def go():
        s = await sup.start("Tester", dict(CONFIG))
        assert bridge.cmds == [".dc test plan start pool=blackfathom,mechanar:heroic "
                               "total=0 concurrent=2"]
        # A plan that is not ours (different pool) is never adopted.
        heartbeat(cfg, plans=[plan_obj("tp-other", pool=[{"token": "mc", "heroic": False}])])
        await sup.tick()
        assert s.state["currentPlanId"] == ""
        heartbeat(cfg, plans=[plan_obj("tp-other", pool=[{"token": "mc", "heroic": False}]),
                              plan_obj("tp-1")])
        await sup.tick()
        assert s.state["currentPlanId"] == "tp-1"
        assert s.state["status"] == "running"
        assert len(bridge.cmds) == 1
        return s

    s = run(go())
    assert json.loads(s.state_file.read_text())["planIds"] == ["tp-1"]


def test_history_that_predates_the_soak_is_not_ingested(cfg):
    write_catalogue(cfg)
    append_runs(cfg, run_row("tr-old", "tp-1"))
    sup, _, _ = make_sup(cfg)

    async def go():
        s = await started(cfg, sup)
        await sup.tick()
        return s

    assert run(go()).rows() == []


# ---------------------------------------------------------------------------
# ingest by byte offset
# ---------------------------------------------------------------------------


def test_ingest_by_offset_filters_to_our_plans(cfg):
    write_catalogue(cfg)
    sup, _, _ = make_sup(cfg)

    async def go():
        s = await started(cfg, sup)
        append_runs(cfg, run_row("tr-1", "tp-1"), run_row("tr-x", "tp-9"),
                    run_row("tr-2", "tp-1", result="wipe"))
        await sup.tick()
        return s

    s = run(go())
    assert [r["runId"] for r in s.rows()] == ["tr-1", "tr-2"]
    assert s.state["counters"] == {"runs": 2, "ok": 1, "fail": 1, "lost": 0}
    # Verbatim copy: the full record comes back by offset.
    assert s.record("tr-2")["failReason"] == "wiped on 'Gelihast' at 12,4"


def test_partial_last_line_waits_for_the_next_tick(cfg):
    write_catalogue(cfg)
    sup, _, _ = make_sup(cfg)

    async def go():
        s = await started(cfg, sup)
        line = json.dumps(run_row("tr-1", "tp-1"))
        with cfg.testruns_file.open("a") as fh:
            fh.write(line[:40])
        await sup.tick()
        assert s.rows() == []
        with cfg.testruns_file.open("a") as fh:
            fh.write(line[40:] + "\n")
        await sup.tick()
        return s

    assert [r["runId"] for r in run(go()).rows()] == ["tr-1"]


def test_truncated_history_rescans_without_duplicates(cfg):
    write_catalogue(cfg)
    sup, _, _ = make_sup(cfg)

    async def go():
        s = await started(cfg, sup)
        append_runs(cfg, run_row("tr-1", "tp-1"), run_row("tr-2", "tp-1"))
        await sup.tick()
        # "Clear run history" truncates in place: the file shrinks under us.
        with cfg.testruns_file.open("r+") as fh:
            fh.truncate(0)
        append_runs(cfg, run_row("tr-2", "tp-1"), run_row("tr-3", "tp-1"))
        await sup.tick()
        return s

    assert [r["runId"] for r in run(go()).rows()] == ["tr-1", "tr-2", "tr-3"]


# ---------------------------------------------------------------------------
# worldserver goes away: lost rows, then exactly one resume
# ---------------------------------------------------------------------------


def test_plan_vanishing_synthesizes_lost_rows_and_resumes_once(cfg):
    write_catalogue(cfg, mtime=time.time() - 3600)
    timelines = TimelineStore()
    sup, bridge, spawn = make_sup(cfg, timelines=timelines)
    live = {"runId": "tr-live", "planId": "tp-1", "dungeon": "mechanar", "heroic": True,
            "stage": "Clearing", "state": "combat", "elapsedS": 300,
            "bossesKilled": 1, "bossesTotal": 3,
            "recent": [{"t": 10, "state": "combat", "detail": "pulling pack 4"}]}

    async def go():
        s = await started(cfg, sup)
        heartbeat(cfg, plans=[plan_obj("tp-1")], runs=[live])
        timelines.accrue([live])
        await sup.tick()
        assert [r["runId"] for r in s.state["liveRuns"]] == ["tr-live"]

        # Heartbeat stops. Within LOST_AFTER_S nothing is declared lost...
        heartbeat(cfg, plans=[plan_obj("tp-1")], runs=[live], ts=time.time() - 30)
        await sup.tick()
        assert s.rows() == []
        # ...after it, the live run gets a synthesized record.
        s.state["planLastSeenMs"] -= (S.LOST_AFTER_S + 5) * 1000
        await sup.tick()
        assert s.state["status"] == "server down"
        rec = s.record("tr-live")
        assert rec["result"] == "lost" and rec["lost"] is True
        assert rec["failReason"] == "worldserver went away"
        assert rec["statusTimeline"][0]["detail"] == "pulling pack 4"
        assert rec["dungeon"] == "mechanar" and rec["heroic"] is True
        await sup.wait_evidence()
        assert s.evidence_state("tr-live") == "done"

        # Still down (catalogue not rewritten): no start.
        n = len(bridge.cmds)
        await sup.tick()
        assert len(bridge.cmds) == n

        # The worldserver boots and rewrites its catalogue: exactly one start,
        # however many ticks run before the new plan shows up.
        write_catalogue(cfg, mtime=time.time() + 5)
        await sup.tick()
        await sup.tick()
        await sup.tick()
        starts = [c for c in bridge.cmds if c.startswith(".dc test plan start")]
        assert len(starts) == 2       # the original + one resume
        assert s.state["status"] == "resuming" and s.state["restarts"] == 1

        heartbeat(cfg, plans=[plan_obj("tp-2")])
        await sup.tick()
        assert s.state["currentPlanId"] == "tp-2"
        assert s.state["planIds"] == ["tp-1", "tp-2"]
        assert s.state["status"] == "running"
        return s

    run(go())


def test_a_reboot_seen_via_the_catalogue_is_immediate(cfg):
    write_catalogue(cfg, mtime=time.time() - 3600)
    sup, bridge, _ = make_sup(cfg)

    async def go():
        s = await started(cfg, sup)
        # The heartbeat is gone and the catalogue is newer than the last time
        # the plan was seen: the server already restarted — no 60 s wait.
        cfg.testrun_live_file.unlink()
        write_catalogue(cfg, mtime=time.time() + 5)
        await sup.tick()
        assert s.state["status"] == "server down"
        await sup.tick()
        assert s.state["status"] == "resuming"
        return s

    run(go())
    assert sum(c.startswith(".dc test plan start") for c in bridge.cmds) == 2


def test_real_record_supersedes_a_lost_row(cfg):
    write_catalogue(cfg, mtime=time.time() - 3600)
    sup, _, _ = make_sup(cfg)

    async def go():
        s = await started(cfg, sup)
        heartbeat(cfg, plans=[plan_obj("tp-1")], runs=[{"runId": "tr-a", "planId": "tp-1",
                                                         "dungeon": "blackfathom"}])
        await sup.tick()
        s.state["planLastSeenMs"] -= (S.LOST_AFTER_S + 5) * 1000
        heartbeat(cfg, ts=time.time() - 100)
        await sup.tick()
        assert s.rows()[0]["result"] == "lost"
        # It was a hitch: the plan is back and the run's real record lands.
        heartbeat(cfg, plans=[plan_obj("tp-1")])
        s.state["status"] = "running"
        append_runs(cfg, run_row("tr-a", "tp-1"))
        await sup.tick()
        return s

    s = run(go())
    assert [(r["runId"], r["result"]) for r in s.rows()] == [("tr-a", "success")]
    assert s.state["counters"]["lost"] == 0


def test_auto_resume_off_stops_the_soak(cfg):
    write_catalogue(cfg, mtime=time.time() - 3600)
    sup, bridge, _ = make_sup(cfg)

    async def go():
        cfgd = dict(CONFIG, autoResume=False)
        s = await sup.start("Tester", cfgd)
        heartbeat(cfg, plans=[plan_obj("tp-1")])
        await sup.tick()
        cfg.testrun_live_file.unlink()
        write_catalogue(cfg, mtime=time.time() + 5)
        await sup.tick()
        await sup.tick()
        return s

    s = run(go())
    assert s.state["status"] == "stopped"
    assert "auto-resume off" in s.state["statusDetail"]
    assert sum(c.startswith(".dc test plan start") for c in bridge.cmds) == 1


# ---------------------------------------------------------------------------
# circuit breaker
# ---------------------------------------------------------------------------


def test_breaker_pauses_after_consecutive_setup_failures_and_resumes(cfg):
    write_catalogue(cfg)
    sup, bridge, _ = make_sup(cfg)

    async def go():
        s = await started(cfg, sup)
        append_runs(cfg, run_row("tr-1", "tp-1", result="setup_failed", failReason="no pool char"),
                    run_row("tr-2", "tp-1", result="setup_failed", failReason="no pool char"))
        await sup.tick()
        assert ".dc test plan pause tp-1" not in bridge.cmds
        append_runs(cfg, run_row("tr-3", "tp-1", result="setup_failed", failReason="no pool char"))
        await sup.tick()
        assert bridge.cmds.count(".dc test plan pause tp-1") == 1
        assert s.state["status"] == "paused"
        assert "setup keeps failing" in s.state["statusDetail"]
        # The plan reports itself paused; the breaker does not fire again.
        heartbeat(cfg, plans=[plan_obj("tp-1", state="paused")])
        await sup.tick()
        assert bridge.cmds.count(".dc test plan pause tp-1") == 1
        await sup.resume(s)
        assert ".dc test plan resume tp-1" in bridge.cmds
        assert s.state["setupFailStreak"] == 0 and not s.state["breakerTripped"]
        return s

    run(go())


def test_a_success_resets_the_setup_streak(cfg):
    write_catalogue(cfg)
    sup, bridge, _ = make_sup(cfg)

    async def go():
        s = await started(cfg, sup)
        append_runs(cfg, *[run_row(f"tr-{i}", "tp-1", result="setup_failed") for i in range(2)],
                    run_row("tr-ok", "tp-1"),
                    *[run_row(f"tr-b{i}", "tp-1", result="setup_failed") for i in range(2)])
        await sup.tick()
        return s

    s = run(go())
    assert not s.state["breakerTripped"]
    assert ".dc test plan pause tp-1" not in bridge.cmds


# ---------------------------------------------------------------------------
# stopping
# ---------------------------------------------------------------------------


def test_drain_pauses_then_stops_when_idle_then_finishes(cfg):
    write_catalogue(cfg)
    sup, bridge, _ = make_sup(cfg)

    async def go():
        s = await started(cfg, sup)
        await sup.stop(s, "drain")
        assert bridge.cmds[-1] == ".dc test plan pause tp-1"
        assert s.state["status"] == "draining"
        heartbeat(cfg, plans=[plan_obj("tp-1", active=1, state="paused")])
        await sup.tick()
        assert ".dc test plan stop tp-1" not in bridge.cmds
        heartbeat(cfg, plans=[plan_obj("tp-1", active=0, state="paused")])
        await sup.tick()
        await sup.tick()
        assert bridge.cmds.count(".dc test plan stop tp-1") == 1
        # The plan finalizes: summary line, then gone from the heartbeat.
        cfg.testplans_file.write_text(
            json.dumps({"planId": "tp-1", "checkpoint": True, "result": "running"}) + "\n"
            + json.dumps({"planId": "tp-1", "checkpoint": False, "result": "stopped"}) + "\n")
        heartbeat(cfg, plans=[])
        await sup.tick()
        return s

    s = run(go())
    assert s.state["status"] == "stopped" and s.state["stoppedAtMs"]
    assert sup.active() is None


def test_a_checkpoint_line_alone_is_not_a_finished_plan(cfg):
    cfg.testplans_file.write_text(
        json.dumps({"planId": "tp-1", "checkpoint": True}) + "\n")
    assert S.final_plan_summary(cfg.testplans_file, "tp-1") is None


# ---------------------------------------------------------------------------
# evidence
# ---------------------------------------------------------------------------


def test_capture_queue_runs_one_at_a_time(cfg):
    write_catalogue(cfg)
    sup, _, spawn = make_sup(cfg)

    async def go():
        s = await started(cfg, sup)
        append_runs(cfg, *[run_row(f"tr-{i}", "tp-1", result="wipe") for i in range(4)],
                    run_row("tr-ok", "tp-1"))
        await sup.tick()
        await sup.wait_evidence()
        return s

    s = run(go())
    assert spawn.max_active == 1
    assert len(spawn.calls) == 4                  # failures only
    for i in range(4):
        assert s.evidence_state(f"tr-{i}") == "done"
        assert (s.evidence_dir / f"tr-{i}" / "report.txt").read_text().startswith("REPORT")
    argv = spawn.calls[0]
    assert "--dump" in argv and "--data-dir" in argv and argv[2] == "tr-0"


def test_missing_tool_records_an_error_not_a_crash(cfg):
    write_catalogue(cfg)
    sup, _, _ = make_sup(cfg)
    cfg.soak_tool = str(cfg.data_dir / "nope.py")
    cfg.app_dir = cfg.data_dir / "nowhere"

    async def go():
        s = await started(cfg, sup)
        append_runs(cfg, run_row("tr-1", "tp-1", result="wipe"))
        await sup.tick()
        await sup.wait_evidence()
        return s

    s = run(go())
    assert s.evidence_state("tr-1") == "failed"


def test_evidence_file_is_contained_to_the_run_folder(cfg):
    sup, _, _ = make_sup(cfg)
    s = S.Soak(sup.root, "sk-20260928-120000")
    (s.evidence_dir / "tr-1").mkdir(parents=True)
    (s.evidence_dir / "tr-1" / "report.txt").write_text("ok")
    (s.evidence_dir / "tr-2").mkdir(parents=True)
    (s.evidence_dir / "tr-2" / "report.txt").write_text("other run")
    s.dir.mkdir(parents=True, exist_ok=True)
    (s.dir / "state.json").write_text("{}")
    assert sup.evidence_file(s, "tr-1", "report.txt") is not None
    for bad in ("../../state.json", "../tr-2/report.txt", "/etc/passwd", "..", ""):
        assert sup.evidence_file(s, "tr-1", bad) is None, bad


# ---------------------------------------------------------------------------
# routes
# ---------------------------------------------------------------------------


@pytest.fixture
def soak_client(cfg, monkeypatch):
    """A logged-in client whose app's supervisor has a fake capture."""
    def build(gmlevel=3):
        fake_auth_db(monkeypatch, gmlevel=gmlevel)
        c = make_client(cfg)
        c.__enter__()
        c.headers["X-TestDeck"] = "1"
        r = c.post("/api/login", json={"username": TEST_USER, "password": TEST_PASSWORD})
        assert r.status_code == 200, r.text
        ctx.soak._spawn = FakeSpawn()
        ctx.soak.nice = False
        return c
    return build


def test_route_start_validates_and_refuses_a_second_session(cfg, soak_client):
    write_catalogue(cfg)
    c = soak_client()
    br = use_bridge()
    body = {"pool": POOL, "concurrent": 3}
    assert c.post("/api/soak/start", json={"pool": []}).status_code == 400
    assert c.post("/api/soak/start", json={"pool": [{"token": "blackfathom", "heroic": True}]}
                  ).status_code == 400          # classic dungeon, no heroic
    assert c.post("/api/soak/start", json={"pool": [{"token": "nope"}]}).status_code == 400
    assert c.post("/api/soak/start", json={"pool": POOL + POOL[:1]}).status_code == 400
    assert c.post("/api/soak/start", json=dict(body, concurrent=0)).status_code == 400
    assert c.post("/api/soak/start", json=dict(body, pick="weighted")).status_code == 400
    r = c.post("/api/soak/start", json=body)
    assert r.status_code == 200, r.text
    sid = r.json()["soakId"]
    assert br.cmds == [".dc test plan start pool=blackfathom,mechanar:heroic total=0 concurrent=3"]
    r2 = c.post("/api/soak/start", json=body)
    assert r2.status_code == 409 and sid in r2.json()["detail"]
    got = c.get("/api/soak").json()
    assert got["active"]["soakId"] == sid and got["supported"] is True
    assert got["active"]["stats"]["runs"] == 0


def test_route_pool_stores_a_retired_token_as_its_replacement(cfg, soak_client):
    write_catalogue(cfg)
    c = soak_client()
    use_bridge()
    # brs and lbrs are the same dungeon now.
    r = c.post("/api/soak/start", json={"pool": [{"token": "brs"}, {"token": "lbrs"}]})
    assert r.status_code == 400 and "twice" in r.json()["detail"]
    r = c.post("/api/soak/start", json={"pool": [{"token": "brs"}, {"token": "ubrs"}]})
    assert r.status_code == 200, r.text
    s = ctx.soak.get(r.json()["soakId"])
    assert [e["token"] for e in s.state["config"]["pool"]] == ["lbrs", "ubrs"]


def test_normalize_pool_rewrites_and_dedupes():
    aliases = {"brs": "lbrs"}
    pool = [{"token": "brs", "heroic": False}, {"token": "lbrs", "heroic": False},
            {"token": "ubrs", "heroic": False}]
    out, changed = S.normalize_pool(pool, aliases)
    assert changed
    assert out == [{"token": "lbrs", "heroic": False}, {"token": "ubrs", "heroic": False}]
    same, changed = S.normalize_pool(out, aliases)
    assert same == out and not changed


def test_a_saved_session_with_a_retired_token_reads_it_as_the_replacement(cfg):
    write_catalogue(cfg)
    s = S.Soak(cfg.data_dir / "soaks", "sk-20260101-000000")
    s.state = {"config": dict(CONFIG, pool=[{"token": "brs", "heroic": False}]),
               "status": "stopped"}
    s.save()
    got = S.SoakSupervisor(cfg, lambda: None).get("sk-20260101-000000")
    assert got is not None
    assert got.state["config"]["pool"] == [{"token": "lbrs", "heroic": False}]
    assert S.catalogue_aliases(cfg.testdungeons_file) == {"brs": "lbrs"}
    assert S.catalogue_aliases(cfg.testdungeons_file.parent / "missing.json") == {}


def test_route_start_refuses_a_server_without_pool_plans(cfg, soak_client):
    write_catalogue(cfg, pool_support=False)
    c = soak_client()
    use_bridge()
    r = c.post("/api/soak/start", json={"pool": POOL})
    assert r.status_code == 409 and "predates" in r.json()["detail"]
    assert c.get("/api/soak").json()["supported"] is False


def test_route_owner_or_admin_may_control(cfg, soak_client):
    write_catalogue(cfg)
    c = soak_client(gmlevel=1)                   # a tester, not an admin
    br = use_bridge()
    sid = c.post("/api/soak/start", json={"pool": POOL}).json()["soakId"]
    s = ctx.soak.get(sid)
    assert c.post(f"/api/soak/{sid}/edit", json={"concurrent": 4}).status_code == 409  # no plan yet
    s.state["currentPlanId"] = "tp-1"
    assert c.post(f"/api/soak/{sid}/pause").status_code == 200   # own session
    s.state["owner"] = "Somebody"
    s.save()
    for path in ("pause", "resume"):
        assert c.post(f"/api/soak/{sid}/{path}").status_code == 403
    assert c.post(f"/api/soak/{sid}/stop", json={"mode": "now"}).status_code == 403
    assert c.post(f"/api/soak/{sid}/edit", json={"concurrent": 4}).status_code == 403
    c.__exit__(None, None, None)

    admin = soak_client(gmlevel=3)
    br = use_bridge()
    assert admin.post(f"/api/soak/{sid}/edit", json={"concurrent": 4}).status_code == 200
    assert br.cmds[-1] == ".dc test plan edit tp-1 concurrent=4"
    assert admin.post(f"/api/soak/{sid}/stop", json={"mode": "sideways"}).status_code == 400
    assert admin.post(f"/api/soak/{sid}/stop", json={"mode": "now"}).status_code == 200
    assert br.cmds[-1] == ".dc test plan stop tp-1"
    assert admin.delete(f"/api/soak/{sid}").status_code == 409   # still stopping


def test_route_runs_are_paged_and_filtered_never_capped(cfg, soak_client):
    write_catalogue(cfg)
    c = soak_client()
    use_bridge()
    sid = c.post("/api/soak/start", json={"pool": POOL}).json()["soakId"]
    s = ctx.soak.get(sid)
    for i in range(650):
        s.append(run_row(f"tr-{i:04d}", "tp-1", result="wipe" if i % 5 == 0 else "success"))
    r = c.get(f"/api/soak/{sid}/runs", params={"result": "fail", "limit": 100}).json()
    assert r["total"] == 130 and len(r["runs"]) == 100
    assert r["runs"][0]["runId"] == "tr-0645"             # newest first
    assert r["runs"][0]["evidence"] in ("pending", "queued", "done")
    r2 = c.get(f"/api/soak/{sid}/runs",
               params={"result": "fail", "limit": 100, "cursor": r["nextCursor"]}).json()
    assert len(r2["runs"]) == 30 and r2["nextCursor"] is None
    assert r2["runs"][-1]["runId"] == "tr-0000"
    cl = "wipe: wiped on '…' at #,#"
    assert c.get(f"/api/soak/{sid}/runs", params={"cluster": cl}).json()["total"] == 130
    stats = c.get(f"/api/soak/{sid}/stats").json()
    assert stats["runs"] == 650 and stats["clusters"][0]["count"] == 130
    row = next(p for p in stats["perDungeon"] if p["key"] == "blackfathom")
    assert row["runs"] == 650 and len(row["last"]) == 20
    assert c.get(f"/api/soak/{sid}/runs/tr-0003").json()["runId"] == "tr-0003"
    assert c.get(f"/api/soak/{sid}/runs/tr-9999").status_code == 404


def test_route_evidence_download_is_contained(cfg, soak_client):
    write_catalogue(cfg)
    c = soak_client()
    use_bridge()
    sid = c.post("/api/soak/start", json={"pool": POOL}).json()["soakId"]
    s = ctx.soak.get(sid)
    s.append(run_row("tr-1", "tp-1", result="wipe"))
    d = s.evidence_dir / "tr-1"
    d.mkdir(parents=True)
    (d / "report.txt").write_text("the report")
    (d / "tr-1.DungeonClear.log").write_text("log slice")
    ev = c.get(f"/api/soak/{sid}/evidence/tr-1").json()
    assert ev["state"] == "done" and ev["report"] == "the report"
    assert {f["name"] for f in ev["files"]} == {"report.txt", "tr-1.DungeonClear.log"}
    assert c.get(f"/api/soak/{sid}/evidence/tr-1/tr-1.DungeonClear.log").text == "log slice"
    # Dot segments never reach the file route with a traversal in `name`:
    # either the route 404s or the path normalises to one the SPA answers
    # with index.html. Either way the soak's own files are not served.
    for bad in ("..%2F..%2Fstate.json", "%2E%2E%2Fstate.json", "..%2F..%2F..%2Fsession.secret"):
        r = c.get(f"/api/soak/{sid}/evidence/tr-1/{bad}")
        assert r.status_code == 404 or "DC Test Deck" in r.text, bad
        assert "soakId" not in r.text and "the report" not in r.text, bad
    assert c.get(f"/api/soak/{sid}/evidence/tr-9/report.txt").status_code == 404
    assert c.get("/api/soak/sk-bogus").status_code == 404


def test_route_presets_follow_the_roster_owner_model(cfg, soak_client):
    write_catalogue(cfg)
    c = soak_client(gmlevel=1)
    assert c.post("/api/soak-presets", json={"name": "Classic", "pool": POOL}).status_code == 200
    got = c.get("/api/soak-presets").json()["presets"]
    assert got[0]["name"] == "Classic" and got[0]["writable"] and got[0]["owner"] == TEST_USER
    presets = json.loads((cfg.data_dir / "soak_presets.json").read_text())
    presets["Theirs"] = {"pool": POOL, "owner": "Somebody"}
    (cfg.data_dir / "soak_presets.json").write_text(json.dumps(presets))
    assert c.post("/api/soak-presets", json={"name": "Theirs", "pool": POOL}).status_code == 403
    assert c.delete("/api/soak-presets/Theirs").status_code == 403
    assert c.delete("/api/soak-presets/Classic").status_code == 200
    assert c.post("/api/soak-presets", json={"name": "", "pool": POOL}).status_code == 400


# ---------------------------------------------------------------------------
# the general history path
# ---------------------------------------------------------------------------


def test_testruns_filters_and_tails_backwards(cfg, client):
    rows = [run_row(f"tr-{i}", "tp-1" if i % 2 else "tp-2",
                    result="success" if i % 3 else "wipe") for i in range(2000)]
    append_runs(cfg, *rows)
    got = client.get("/api/testruns", params={"limit": 5}).json()["runs"]
    assert [r["runId"] for r in got] == ["tr-1999", "tr-1998", "tr-1997", "tr-1996", "tr-1995"]
    got = client.get("/api/testruns", params={"limit": 3, "planId": "tp-2",
                                              "result": "fail"}).json()["runs"]
    assert [r["runId"] for r in got] == ["tr-1998", "tr-1992", "tr-1986"]
    # The old 500-row ceiling is gone for a filtered view.
    assert len(client.get("/api/testruns", params={"limit": 1000}).json()["runs"]) == 1000


def test_testplans_hides_checkpoint_lines(cfg, client):
    cfg.testplans_file.write_text(
        json.dumps({"planId": "tp-1", "checkpoint": True}) + "\n"
        + json.dumps({"planId": "tp-2"}) + "\n"
        + json.dumps({"planId": "tp-1", "checkpoint": True}) + "\n")
    assert [p["planId"] for p in client.get("/api/testplans").json()["plans"]] == ["tp-2"]
    assert len(client.get("/api/testplans", params={"checkpoints": True}).json()["plans"]) == 3


# ---------------------------------------------------------------------------
# addclass pool size (the bound on harness bots now that MaxAddedBots is not)
# ---------------------------------------------------------------------------


def test_addclass_pool_counts_type_2_accounts_on_the_characters_server(cfg, monkeypatch):
    from testdeck import mysql as M
    cfg.playerbots_conf.write_text(
        'PlayerbotsDatabaseInfo = "127.0.0.1;3306;acore;pw;acore_playerbots"\n')
    seen = []

    async def fake_query(which, sql, cfg=None):
        seen.append((which, sql))
        return [["37"]]

    monkeypatch.setattr(M, "mysql_query", fake_query)
    assert run(M.addclass_pool_size(cfg)) == 37
    which, sql = seen[0]
    assert which == "characters"
    assert "`acore_playerbots`.playerbots_account_type" in sql and "account_type = 2" in sql


def test_addclass_pool_is_unknown_when_the_playerbots_db_is_elsewhere(cfg, monkeypatch):
    from testdeck import mysql as M
    cfg.playerbots_conf.write_text(
        'PlayerbotsDatabaseInfo = "10.0.0.9;3306;acore;pw;acore_playerbots"\n')

    async def boom(*a, **k):
        raise AssertionError("must not query")

    monkeypatch.setattr(M, "mysql_query", boom)
    assert run(M.addclass_pool_size(cfg)) is None


# ---------------------------------------------------------------------------
# long pools: a console line is capped (bridge MAX_CMD_LEN; screen drops lines
# past ~750 chars), so a big pool starts paused and is loaded in chunks
# ---------------------------------------------------------------------------

BIG = [{"token": f"dungeon-{i:02d}", "heroic": bool(i % 3 == 0)} for i in range(60)]
BIG_KEYS = [S.entry_key(e["token"], e["heroic"]) for e in BIG]


def big_plan(pid, keys, state="paused"):
    pool = [{"token": k.split(":")[0], "heroic": k.endswith(":heroic")} for k in keys]
    return plan_obj(pid, state=state, pool=pool)


def test_long_pool_starts_paused_loads_in_chunks_then_resumes_once(cfg):
    from testdeck.bridge import MAX_CMD_LEN
    write_catalogue(cfg)
    sup, bridge, _ = make_sup(cfg)

    async def go():
        s = await sup.start("Tester", dict(CONFIG, pool=BIG, concurrent=15))
        start = bridge.cmds[0]
        assert start.endswith(" paused") and len(start) <= MAX_CMD_LEN
        first = start[len(".dc test plan start pool="):].split(" ")[0].split(",")
        assert 0 < len(first) < len(BIG_KEYS) and first == BIG_KEYS[:len(first)]

        # The plan registers with the first chunk: adopted, and the rest added.
        heartbeat(cfg, plans=[big_plan("tp-1", first)])
        await sup.tick()
        assert s.state["currentPlanId"] == "tp-1"
        adds = [c for c in bridge.cmds if c.startswith(".dc test plan edit tp-1 add=")]
        assert adds and all(len(c) <= MAX_CMD_LEN for c in adds)
        added = [k for c in adds for k in c.split("add=")[1].split(",")]
        assert added == BIG_KEYS[len(first):]
        assert s.state["status"] == "starting"
        assert "loading the pool" in s.state["statusDetail"]
        assert ".dc test plan resume tp-1" not in bridge.cmds

        # The heartbeat has not caught up yet: nothing is re-sent.
        n = len(bridge.cmds)
        await sup.tick()
        assert len(bridge.cmds) == n

        # Pool complete: resume exactly once.
        heartbeat(cfg, plans=[big_plan("tp-1", BIG_KEYS)])
        await sup.tick()
        await sup.tick()
        assert bridge.cmds.count(".dc test plan resume tp-1") == 1
        assert not s.state["awaitingResume"]
        return s

    run(go())


def test_a_lost_add_chunk_is_resent(cfg):
    write_catalogue(cfg)
    sup, bridge, _ = make_sup(cfg)

    async def go():
        s = await sup.start("Tester", dict(CONFIG, pool=BIG))
        first = bridge.cmds[0][len(".dc test plan start pool="):].split(" ")[0].split(",")
        heartbeat(cfg, plans=[big_plan("tp-1", first)])
        await sup.tick()
        n_adds = sum(c.startswith(".dc test plan edit tp-1 add=") for c in bridge.cmds)
        # A chunk never landed (screen ate it): after the resend window the
        # missing entries go again, and only those.
        s.state["addSentAtMs"] -= S.ADD_RESEND_MS + 1
        partial = BIG_KEYS[:-5]
        heartbeat(cfg, plans=[big_plan("tp-1", partial)])
        await sup.tick()
        again = [c for c in bridge.cmds if c.startswith(".dc test plan edit tp-1 add=")][n_adds:]
        assert [k for c in again for k in c.split("add=")[1].split(",")] == BIG_KEYS[-5:]
        return s

    run(go())


def test_a_long_pool_edit_replaces_with_the_first_chunk_then_adds(cfg):
    from testdeck.bridge import MAX_CMD_LEN
    write_catalogue(cfg)
    sup, bridge, _ = make_sup(cfg)

    async def go():
        s = await started(cfg, sup)
        await sup.edit(s, pool=BIG, concurrent=8)
        edit = bridge.cmds[-1]
        assert edit.startswith(".dc test plan edit tp-1 pool=") and "concurrent=8" in edit
        assert len(edit) <= MAX_CMD_LEN
        first = edit.split("pool=")[1].split(" ")[0].split(",")
        # No add until the heartbeat can show the replaced pool.
        heartbeat(cfg, plans=[plan_obj("tp-1")])
        await sup.tick()
        assert not any("add=" in c for c in bridge.cmds)
        s.state["addSentAtMs"] -= S.ADD_RESEND_MS + 1
        heartbeat(cfg, plans=[big_plan("tp-1", first, state="running")])
        await sup.tick()
        added = [k for c in bridge.cmds if "add=" in c for k in c.split("add=")[1].split(",")]
        assert added == BIG_KEYS[len(first):]
        # A running session is never paused or resumed by a pool edit.
        assert not any(c.endswith(("resume tp-1", "pause tp-1")) for c in bridge.cmds)
        return s

    run(go())
