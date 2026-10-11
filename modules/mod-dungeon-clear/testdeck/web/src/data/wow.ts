/* Static 3.3.5a game data: names and colours the server sends only as ids.
 * Fixed by the client build, so it lives here rather than being fetched —
 * except the ilvl ladder, which is per dungeon and comes from the module. */

export const CLASS_NAME: Record<number, string> = {
  1: "Warrior",
  2: "Paladin",
  3: "Hunter",
  4: "Rogue",
  5: "Priest",
  6: "Death Knight",
  7: "Shaman",
  8: "Mage",
  9: "Warlock",
  11: "Druid",
};

/* In-game class colours; Shaman kept at the true #0070DE (dots and borders),
 * with a lightened variant for text on dark backgrounds. */
export const CLASS_COLOR: Record<number, string> = {
  1: "#C79C6E",
  2: "#F58CBA",
  3: "#ABD473",
  4: "#FFF569",
  5: "#FFFFFF",
  6: "#C41F3B",
  7: "#0070DE",
  8: "#69CCF0",
  9: "#9482C9",
  11: "#FF7D0A",
};

export const CLASS_TEXT_COLOR: Record<number, string> = {
  ...CLASS_COLOR,
  7: "#2996FF",
};

export const RACE_NAME: Record<number, string> = {
  1: "Human",
  2: "Orc",
  3: "Dwarf",
  4: "Night Elf",
  5: "Undead",
  6: "Tauren",
  7: "Gnome",
  8: "Troll",
  10: "Blood Elf",
  11: "Draenei",
};

export const QUALITY_CHOICES = [
  { v: 1, label: "normal" },
  { v: 2, label: "uncommon" },
  { v: 3, label: "rare" },
  { v: 4, label: "epic" },
  { v: 5, label: "legendary" },
];

export const QUALITY_COLOR: Record<number, string> = {
  1: "#ffffff",
  2: "#1eff00",
  3: "#0070dd",
  4: "#a335ee",
  5: "#ff8000",
};

/* Run records carry the class as a lowercase name ("warrior"); map it back
 * to the id space the color tables use. */
export const CLASS_ID_BY_NAME: Record<string, number> = {
  warrior: 1,
  paladin: 2,
  hunter: 3,
  rogue: 4,
  priest: 5,
  "death knight": 6,
  deathknight: 6,
  dk: 6,
  shaman: 7,
  mage: 8,
  warlock: 9,
  druid: 11,
};

export function classIdByName(name?: string): number {
  return CLASS_ID_BY_NAME[(name ?? "").toLowerCase()] ?? 0;
}

/* Class-color a bare player name using the run's comp as the lookup table —
 * deaths, diag members and roster rows only carry the name. */
export function classColorFor(
  name: string | undefined,
  comp: { name?: string; class?: string }[] | undefined,
): string | undefined {
  if (!name || !comp) return undefined;
  const m = comp.find(
    (c) => (c.name ?? "").toLowerCase() === name.toLowerCase(),
  );
  const id = m ? classIdByName(m.class) : 0;
  return id ? CLASS_TEXT_COLOR[id] : undefined;
}

/* The heartbeat writes "heal"; accept "healer" too so a later module-side
 * rename can't silently drop healers to the bottom of the sort. */
export const ROLE_ORDER: Record<string, number> = {
  tank: 0,
  heal: 1,
  healer: 1,
  dps: 2,
};

/* Roles are POSITIONAL — the order is the contract with the worldserver. */
export const ROSTER_SLOTS = ["tank", "heal", "dps", "dps", "dps"] as const;

/* Instance map → expansion. A FALLBACK only: the module reads each row's
 * expansion out of Map.dbc and emits it in the catalogue, so expansionOfRow()
 * below prefers that and this table is what answers for a sidecar written by a
 * worldserver older than the field. Naxxramas (533) is the level-80 WotLK raid
 * on this build. Anything unlisted is Classic, so a new module row degrades to
 * a sane shelf rather than to "unknown". */
export const EXPANSION_NAME = [
  "Classic",
  "The Burning Crusade",
  "Wrath of the Lich King",
] as const;

const TBC_MAPS = new Set([
  269, 532, 534, 540, 542, 543, 544, 545, 546, 547, 548, 550, 552, 553, 554,
  555, 556, 557, 558, 560, 564, 565, 568, 580, 585,
]);
const WOTLK_MAPS = new Set([
  533, 574, 575, 576, 578, 595, 599, 600, 601, 602, 603, 604, 608, 615, 616,
  619, 624, 631, 632, 649, 650, 658, 668, 724,
]);

export function expansionOf(mapId: number): 0 | 1 | 2 {
  if (TBC_MAPS.has(mapId)) return 1;
  if (WOTLK_MAPS.has(mapId)) return 2;
  return 0;
}

/* The expansion of a catalogue row: the module's own value when it sent one,
 * the map-id table otherwise. Use this rather than expansionOf() anywhere a
 * Dungeon row is in hand — it cannot drift from the server's Map.dbc. */
export function expansionOfRow(d: {
  mapId: number;
  expansion?: number;
}): 0 | 1 | 2 {
  const e = d.expansion;
  return e === 0 || e === 1 || e === 2 ? e : expansionOf(d.mapId);
}
