# Test Deck API

Everything is JSON under `/api/`. Every route except `POST /api/login` and
`GET /api/session` requires a session cookie; every non-GET additionally
requires the CSRF header `X-TestDeck: 1`. Unauthenticated API calls get
`401 {"detail": "not logged in"}`.

Command-issuing routes return a shared **command reply** shape:

```json
{
  "ok": true,          // transport-level success (SOAP fault => false)
  "exact": true,       // true: reply is exactly this command's output (SOAP)
  "cmd": ".dc test start blackfathom",
  "pending": false,    // true: the test driver is still logging in — the
                       // start did NOT happen; retry, or use a plan
  "reply": ["Test run started", "..."]
}
```

With the fuzzy transports (screen/tmux) `reply` is the console tail after the
last echo of `cmd`; treat it as advisory and the live heartbeat as truth.

## Auth

| Route | Body / params | Notes |
|---|---|---|
| `POST /api/login` | `{username, password}` | SRP6 check against `auth.account`, then `MAX(gmlevel)` from `account_access` (RealmID −1 or `[auth] realm_id`) and a check for an active `account_banned` row. 401 generic on unknown/wrong (both paths cost the same modexp, so they cannot be told apart by timing); **403 with instructions** when the password was right but gmlevel is too low, or the account is banned; 429 on throttle; 503 when the auth DB is unreachable. Sets the `tdeck` cookie. |
| `POST /api/logout` | — | Clears the cookie. |
| `GET /api/session` | — | `{authenticated, username?, gmlevel?, accountId?, admin?}` — what the SPA reads on boot. `gmlevel` is the LIVE value, re-read from the auth DB. |

Authorization is re-checked per request, not taken from the cookie: every
`/api/*` call re-reads the account's gmlevel and ban state from the auth
database (cached ~60s per account), so a demotion or ban takes hold within the
minute. A database outage falls back to the cookie's own DB-verified level
rather than logging everyone out.

## Status

`GET /api/status` → `{realm, since, statusCheck, catalogue, liveRuns,
livePlans, bridge, health, version}`. `realm` is `ONLINE/OFFLINE/FAILED/
UNKNOWN` from the configured `[realm] status_check`; a fresh run heartbeat
forces `ONLINE`. `health.problems` is the config-validation banner. Cached 5s.

## Catalogue

`GET /api/testdungeons` → the module's `dc_test_dungeons.json` verbatim:
`{limits, gearDefaults, qualities, dungeons:[{token, name, mapId, expansion,
level, heroicLevel, wing, gear:[{ilvl,label}], gearHeroic?}]}`. Empty shape
until the worldserver has written it. `expansion` is Map.dbc's own id (0
classic, 1 TBC, 2 WotLK), so the UI shelves rows by the server's client build
instead of its own map-id table.

## Runs

| Route | Body | Notes |
|---|---|---|
| `POST /api/testruns/start` | `{dungeon, heroic?, level?, seed?, ilvl?, quality?}` | One pool run. `ilvl` must be on the catalogue ladder for that difficulty (0 = server default, −1 = none). May answer `pending: true` (driver logging in) — the UI retries twice then falls back to a 1-run plan. |
| `POST /api/testruns/start-roster` | `{dungeon, members[5], heroic?}` | Members positional: tank, heal, dps×3. Refuses before issuing anything: unknown/duplicate names, **characters on another account unless admin (403)**, online characters (409), cross-faction, exhausted `AccountInstancesPerHour` (429). Adds `roles` to the reply. |
| `GET /api/testruns/live` | — | `{runs, plans}` from the ~2s heartbeat; each run carries `timeline` — the **server-accumulated** full status history (the raw heartbeat only keeps the last 8 entries). Stale (>15s) heartbeat reads as idle. |
| `GET /api/testruns?limit=100` | — | Finished-run records, newest first. **Additive schema** — render a key only when present. |
| `POST /api/testruns/stop` | `{runId}` or `"all"` | `"all"` also stops plans first. Stopping one run of a live plan lets the plan launch a replacement. |
| `POST /api/testruns/clear` | — | Admin only. Truncates the JSONL (keeps `.bak`); 409 while anything is live. |

## Plans

| Route | Body |
|---|---|
| `POST /api/testplans/start` | `{dungeon, total, concurrent?, level?, seed?, heroic?, ilvl?, quality?}` — total capped by the catalogue's `planMaxTotal` when positive |
| `GET /api/testplans?limit=50` | Finished campaign summaries, newest first |
| `POST /api/testplans/stop` | `{planId}` or `"all"` |
| `POST /api/testplans/clear` | Admin only |

## Roster data

| Route | Notes |
|---|---|
| `GET /api/accounts` | Player accounts (bot prefix excluded) with char counts, remaining instance budget and `mine`; plus `myAccountId` and `admin` |
| `GET /api/characters?search&cls&faction&accountId&minlevel&maxlevel&limit` | Real characters with class/level/faction/guild/account, `online`, `instancesLeft`, talent `spec` (read offline from Talent.dbc; empty when DBCs are absent), and `owned` — whether this caller may draft it |
| `GET /api/rosters` / `POST /api/rosters` | Saved rosters (`{name, members[5], owner, mine, writable}`), stored in `data_dir/rosters.json`. Creating one is open to any tester; overwriting one owned by somebody else needs admin (403). |
| `DELETE /api/rosters/{name}` | Your own; anyone else's needs admin |

## Logs

| Route | Notes |
|---|---|
| `GET /api/logs` | `*.log` files in the server's `log_dir` |
| `GET /api/logs/stream?file=X&lines=200` | SSE: one `lines` batch, then `line` events; `--- rotated ---` marker on truncation; 15s keepalives |

## Dungeon map pack

Generated per host by `python3 -m testdeck mappack`; the Live page draws bot
positions on it. Absent is an ordinary state — the routes say so rather than
leaving the UI to guess.

| Route | Notes |
|---|---|
| `GET /api/mappack` | `{available, generated, usable, maps[]}`, or `{available: false, reason}` naming the command that builds one |
| `GET /api/mappack/{mapId}` | One map: `{mapId, name, kind, floors[], floorRule?}`; 404 when the pack has no art for it. `floorRule.chunks` is **stripped** — it is most of a full manifest's ~230 KB and is meaningless without a WMO group |
| `GET /api/mappack/img/{mapId}/{floor}` | The floor image (WebP by default). Path is resolved-then-contained against the pack directory |

Each floor carries the world rect (`minX/maxX` are world **X**, north, and drive
the *vertical* image axis; `minY/maxY` are world **Y**, west, horizontal) and a
ready-made affine:

```
px = ax*worldX + bx*worldY + cx
py = ay*worldX + by*worldY + cy      # in a w x h image
```

Pick a floor by rect containment first, using `floorRule.zsteps` (Blizzard's own
`DungeonMapChunk` MinZ thresholds, ascending) only to break a tie between floors
whose rects both contain the position. `pick_floor()` in `testdeck/mappack.py`
is the reference implementation. A position on no floor is a real answer, not a
failure.

## Continuous mode (soak)

A session spans one endless pool plan per worldserver lifetime; its state lives
in `<data_dir>/soaks/<sk-id>/`. Mutations need the session's owner or
`admin_gmlevel`, and each is written to the audit log.

| Route | Notes |
|---|---|
| `GET /api/soak` | `{active (with stats), recent[], supported, evidenceTool, me, admin, addclassPool}` — `addclassPool` = characters in the playerbots addclass pool (null if uncountable); `supported` mirrors the catalogue's `limits.planPool` |
| `POST /api/soak/start` | `{pool:[{token,heroic}], concurrent, pick: bag\|random, level, seed, ilvl, quality, autoResume, breaker}`; 409 while another session runs or on a server without pool plans |
| `GET /api/soak/{id}` | One session with stats (past sessions too) |
| `POST /api/soak/{id}/edit` | `{pool?, concurrent?}` → `.dc test plan edit`; 409 until the plan is registered |
| `POST /api/soak/{id}/pause` · `/resume` | Stop / restart launching; live runs play out |
| `POST /api/soak/{id}/stop` | `{mode: drain\|now}` — drain pauses, then ends the plan once idle |
| `GET /api/soak/{id}/runs` | Ledger newest-first: `result=all\|fail\|ok\|lost`, `dungeon=<token[:heroic]>`, `cluster=<label>`, `cursor`, `limit≤200` → `{runs, total, nextCursor}`. No cap |
| `GET /api/soak/{id}/runs/{runId}` | The full record, read by offset |
| `GET /api/soak/{id}/stats` | Per-dungeon rates / median / last-20, failure clusters, runs per hour, disk use |
| `GET /api/soak/{id}/evidence/{runId}` | `{state, report, error, files[]}`; `…/{file}` downloads one (resolved-then-contained); `POST …/capture` retries a failed capture |
| `DELETE /api/soak/{id}` | Only when stopped; removes the folder |
| `GET/POST /api/soak-presets`, `DELETE /api/soak-presets/{name}` | Named pools, owned like rosters |

`GET /api/testruns` also takes `planId=` and `result=` (a verdict, or `fail`),
and `GET /api/testplans` hides endless plans' checkpoint lines unless
`checkpoints=true`.
