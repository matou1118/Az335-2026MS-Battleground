/* The Live page's dungeon map: bot positions drawn on the real Blizzard map
 * art. The transform is exact rather than fitted — DungeonMap.dbc states the
 * world rectangle each floor's image covers, and the pack has already turned
 * that into an affine. Nothing here calibrates anything.
 *
 * The art is generated per host (`python3 -m testdeck mappack`) from the
 * operator's own WoW client, so "no pack" is an ordinary state that has to
 * read as a setup hint, not as an error. */

import { useEffect, useMemo, useState } from "react";
import { api, ApiError } from "../api/client";
import type { BotPos, MapPackIndex, MapPackMap } from "../api/types";
import { CLASS_COLOR, CLASS_NAME } from "../data/wow";

/* One fetch per map id for the life of the page. The pack is static between
 * rebuilds, and the Live view re-renders every 3s — refetching there would be
 * 20 requests a minute for a payload that never changes. Failures are cached
 * too, deliberately: a dungeon with no art must not retry forever. */
const mapCache = new Map<number, Promise<MapPackMap | null>>();
let indexCache: Promise<MapPackIndex> | null = null;

function fetchIndex(): Promise<MapPackIndex> {
  if (!indexCache)
    indexCache = api.get<MapPackIndex>("/api/mappack").catch(() => ({
      available: false,
      reason: "cannot reach the server",
      maps: [],
    }));
  return indexCache;
}

function fetchMap(mapId: number): Promise<MapPackMap | null> {
  let p = mapCache.get(mapId);
  if (!p) {
    p = api.get<MapPackMap>(`/api/mappack/${mapId}`).catch((e) => {
      if (e instanceof ApiError && e.status === 404) return null;
      throw e;
    });
    mapCache.set(mapId, p);
  }
  return p;
}

/* Which floor a world position belongs on, or null for "no floor draws this
 * spot" — a real answer, not a failure: Ulduar, Sunwell and Black Temple all
 * have areas their map art does not cover.
 *
 * Rect containment settles it outright for most dungeons; Scarlet Monastery
 * and Dire Maul have no multi-storey WMO group at all, their wings being
 * separated in XY. Where rects overlap, the zstep ladder breaks the tie. */
export function pickFloor(
  x: number,
  y: number,
  z: number,
  m: MapPackMap,
): number | null {
  const inside = m.floors
    .filter((f) => x >= f.minX && x <= f.maxX && y >= f.minY && y <= f.maxY)
    .map((f) => f.floor);
  if (!inside.length) return null;
  if (inside.length === 1) return inside[0];
  let best: number | null = null;
  for (const s of m.floorRule?.zsteps ?? [])
    if (z >= s.minZ && inside.includes(s.floor)) best = s.floor;
  return best ?? Math.min(...inside);
}

type Assigned = { bot: BotPos; floor: number | null };

export default function DungeonMap({
  mapId,
  bots,
}: {
  mapId: number;
  bots: BotPos[];
}) {
  const [map, setMap] = useState<MapPackMap | null | undefined>(undefined);
  const [index, setIndex] = useState<MapPackIndex | null>(null);
  const [manualFloor, setManualFloor] = useState<number | null>(null);

  useEffect(() => {
    let live = true;
    setMap(undefined);
    setManualFloor(null);
    void fetchMap(mapId).then((m) => live && setMap(m));
    void fetchIndex().then((i) => live && setIndex(i));
    return () => {
      live = false;
    };
  }, [mapId]);

  /* Which floor each bot is on. Recomputed when the heartbeat moves someone,
   * not on every unrelated re-render of the card. Projection is deliberately
   * NOT done here: a dot is drawn through the affine of the floor being
   * *shown*, so an off-floor bot appears at its true world position in this
   * image's frame rather than at a coordinate from another floor's rect. */
  const assigned: Assigned[] = useMemo(() => {
    if (!map) return [];
    return bots
      .filter((b) => b.x !== undefined && b.y !== undefined)
      .map((b) => ({ bot: b, floor: pickFloor(b.x!, b.y!, b.z ?? 0, map) }));
  }, [map, bots]);

  if (map === undefined) return null; // first fetch in flight; no flicker
  if (map === null)
    return (
      <p className="mt-2 text-xs text-ink-600">
        {index && !index.available
          ? index.reason
          : "no map art for this dungeon in the pack on this host"}
      </p>
    );

  /* Follow the tank: the party is wherever they are, and a floor that flips
   * because two DPS lagged behind on the stairs is worse than useless. Fall
   * back to whichever floor holds the most of the party. */
  const tank = assigned.find((p) => p.bot.role === "tank" && p.floor !== null);
  const tally = new Map<number, number>();
  for (const p of assigned)
    if (p.floor !== null) tally.set(p.floor, (tally.get(p.floor) ?? 0) + 1);
  const busiest = [...tally.entries()].sort((a, b) => b[1] - a[1])[0]?.[0];
  const shown =
    manualFloor ?? tank?.floor ?? busiest ?? map.floors[0]?.floor ?? 1;
  const floor = map.floors.find((f) => f.floor === shown) ?? map.floors[0];
  if (!floor) return null;

  const offMap = assigned.filter((p) => p.floor === null).length;
  const placed = assigned.map((a) => ({
    ...a,
    px: ((floor.ax * a.bot.x! + floor.bx * a.bot.y! + floor.cx) / floor.w) * 100,
    py: ((floor.ay * a.bot.x! + floor.by * a.bot.y! + floor.cy) / floor.h) * 100,
  }));

  return (
    /* Bounded, not fluid. The art is 1002x668 and a run card is as wide as the
     * window, so `w-full` alone let the map grow without limit on a wide
     * monitor and pushed everything else off screen. It shrinks below the cap
     * on narrow screens; it never grows past it. */
    <div className="mt-2 w-full max-w-[34rem]">
      <div className="relative overflow-hidden rounded-lg border border-ink-800 bg-ink-950">
        <img
          src={floor.url}
          alt={`${map.name ?? "dungeon"} floor ${floor.floor}`}
          className="block w-full"
          style={{ aspectRatio: `${floor.w} / ${floor.h}` }}
          loading="lazy"
        />
        {placed.map((p, i) => {
          const onThisFloor = p.floor === floor.floor;
          /* Everyone is drawn. A bot on another floor stays visible but
           * recedes — that is exactly the case worth seeing (the healer who
           * never came down the stairs), and hiding them makes a five-man
           * look like a three-man. */
          if (p.px < -2 || p.px > 102 || p.py < -2 || p.py > 102) return null;
          const color = p.bot.cls ? CLASS_COLOR[p.bot.cls] : "#a1a1aa";
          const dead = p.bot.alive === false;
          return (
            <span
              key={i}
              title={`${p.bot.name ?? CLASS_NAME[p.bot.cls ?? 0] ?? "?"}${
                p.bot.role ? ` (${p.bot.role})` : ""
              }${onThisFloor ? "" : ` — floor ${p.floor ?? "?"}`}${
                dead ? " — dead" : ""
              }`}
              className="pointer-events-auto absolute -translate-x-1/2 -translate-y-1/2 rounded-full ring-1 ring-black/70"
              style={{
                left: `${p.px}%`,
                top: `${p.py}%`,
                width: onThisFloor ? 11 : 7,
                height: onThisFloor ? 11 : 7,
                backgroundColor: dead ? "#3f3f46" : color,
                opacity: onThisFloor ? (dead ? 0.55 : 1) : 0.3,
                boxShadow:
                  onThisFloor && p.bot.role === "tank"
                    ? "0 0 0 2px rgba(255,255,255,.75)"
                    : undefined,
              }}
            />
          );
        })}
      </div>

      <div className="mt-1.5 flex flex-wrap items-center gap-2 text-[11px] text-ink-600">
        {map.floors.length > 1 && (
          <span className="flex items-center gap-1">
            <span className="text-ink-500">floor</span>
            {map.floors.map((f) => (
              <button
                key={f.floor}
                type="button"
                onClick={() =>
                  setManualFloor(manualFloor === f.floor ? null : f.floor)
                }
                className={`rounded px-1.5 py-0.5 transition ${
                  f.floor === floor.floor
                    ? "bg-iris-500/25 text-iris-200"
                    : "hover:text-ink-300"
                }`}
                title={
                  manualFloor === f.floor
                    ? "click again to follow the tank"
                    : undefined
                }
              >
                {f.floor}
              </button>
            ))}
            {manualFloor !== null && (
              <span className="text-ink-700">(pinned)</span>
            )}
          </span>
        )}
        {offMap > 0 && (
          <span title="These positions fall outside every floor's drawn area — normal for the parts of an instance Blizzard never mapped.">
            {offMap} off-map
          </span>
        )}
      </div>
    </div>
  );
}
