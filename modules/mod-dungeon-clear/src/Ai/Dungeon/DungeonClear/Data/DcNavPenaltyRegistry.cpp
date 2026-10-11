/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DcNavPenaltyRegistry.h"

#include <array>

namespace
{
    // ---- the table ------------------------------------------------------
    // One row per navmesh shortcut a real player can't follow. Each box spans
    // only the MIDDLE Z band of its climb — the legitimate floor below and ledge
    // platform above sit just outside it, so a route that genuinely belongs down
    // there or up there is untaxed; only an edge climbing the face pays. A stiff
    // multiplier makes the A* corridor take the real way around whenever one
    // exists; it stays a cost, so the spot is never made unreachable.
    //
    // Lower Blackrock Spire (map 229), #1 — the big chasm climb. The navmesh
    // stitches a walkable poly up the wall between the lower walkway (~z30) and an
    // upper ledge (~z58), so the tank climbs a near-vertical face the party can't
    // follow. Observed bot traversal:
    //     [-127.33, -402.11, 30.32]  ->  [-124.88, -378.42, 58.40]
    // (≈28yd rise over ≈24yd ground = ~50°). Box mid-band z 33..56.
    //
    // Lower Blackrock Spire (map 229), #2 — a small ledge-hop further along the
    // (now-corrected) route, where some bots wedged on the step. Same class, much
    // smaller. Observed traversal:
    //     [-61.70, -382.77, 48.88]  <->  [-64.34, -378.49, 54.70]
    // (≈5.8yd rise over ≈5yd ground = ~49°). Tight box hugging the two endpoints,
    // mid-band z 49.4..54.2 so the lower walkway (≤~49) and the upper platform
    // (≥~54.7, which the proper route reaches from another direction) stay untaxed.
    //
    // Sethekk Halls (map 556) — the Talon King's back-door ramp. The instance is a
    // loop: Talon King Ikiss (44.7, 287.0, z25) sits on an upper ring you reach the
    // long way (west ramp near (-250, 210) up to z27, then east across the upper
    // room). But a narrow ramp climbs straight to his platform from a closed,
    // script-controlled door (GO 183398 at (44.8, 150.7, z0)) on the LOWER level
    // directly south of him. The mmap stitches that ramp into the navmesh ignoring
    // the shut door, so Detour's A* picks the short south climb, routes the party
    // back toward the entrance, and wedges at the door (it can't open it) — the
    // "can't reach Ikiss after Syth" symptom. The ramp is a single ~x45 column
    // (x≈40..50, void either side) rising y153->245, z0->27, opening onto the
    // shared platform only at y>=250. Box the whole climb up to (not into) the
    // platform — x 25..68, y 150..248, z -5..30 — so every edge on the shortcut is
    // taxed while the upper room / platform (y>=250) and the lower lobby (y<150)
    // stay untaxed and the legitimate west-ramp route is left at base cost.
    // The Arcatraz (map 552) — the five Arcatraz Sentinel (20869) spawns. NOT a
    // navmesh shortcut: this is the route half of DcHazardRegistry. Each rooted
    // Sentinel pulses 563-937 damage in 15yd every second, forever, so the
    // router should prefer a line that hugs the far wall. Boxes are the emitter
    // position +-22 in XY and +-12 in Z (the registry's radius / zBand), matching
    // DcHazardRegistry's rows so the route half and the live half agree.
    //
    // COST MULTIPLIER IS 8, NOT 40, AND HAZARDS ARE DELIBERATELY NOT WIRED INTO
    // THE StridedPathfinder HARD REJECT. Sentinels 138931 (255.5,158.9) and
    // 138932 (253.9,131.9) sit 27yd apart in what is the only corridor through
    // that stretch of the Containment Core: their boxes overlap and span it.
    // A cost is survivable there (the route still goes through, just last) —
    // a rejection would strand the party. 8 is enough to bend a route around an
    // emitter when floor space exists, without making an unavoidable corridor
    // rank worse than a genuinely broken navmesh shortcut at 40.
    //
    // If test runs show bots still walking the pulse, NARROW THE BOXES rather
    // than raising the multiplier — a wider tax on a mandatory corridor buys
    // nothing and starts competing with the shortcut rows above.
    //
    // Underbog (map 546) — a navmesh shortcut up normally unwalkable geometry:
    // the mmap stitches a walkable face the party can't follow, so Detour's A*
    // climbs it instead of taking the intended route. The shortcut runs between
    // (35.17, -364.37, 27.57) and (66.6, -357.99, 33.77). This sits in a very
    // wide-open area, so the box is drawn generously around the whole run (with
    // margin) rather than hugging a narrow band — an over-sized box here only
    // makes the router prefer the open floor around it, never strands anyone.
    // costMult 40 (a spot a real player can't be, same class as the LBRS/Sethekk
    // shortcut rows above).
    //
    // Pit of Saron (map 658) — the north bridge between Krick's arena and the
    // ambush ramp, the shortcut that makes the tank climb out of the arena's
    // south-east corner instead of walking round to the ramp's foot.
    //
    // MEASURED AGAINST THE MAP'S OWN MMTILES, not eyeballed. The arena floor
    // (z~510-522) and the ramp that climbs east to Tyrannus (z~522-565) are
    // separated by a chasm for the whole stretch y 64..94, and EXACTLY THREE
    // walkable links cross it:
    //     y 84.5   (862.20, 84.53, 517.34) -> (869.07, 84.27, 520.91)   +3.6yd
    //     y 76.8   (865.87, 76.75, 524.31) -> (869.69, 75.11, 527.40)   +3.1yd
    //     y 52.9   (861.93, 52.93, 517.07) -> (871.13, 58.27, 522.07)   +5.0yd
    // The last one is the ramp's real foot, the ground the party walks after
    // gate 1 (anchors 6 -> 7 of Leg A). The y 84.5 link opens onto a dead-end
    // tongue. The y 76.8 link is the bad one: A* takes it, and the corridor it
    // produces — arena -> (862.2, 84.5) -> (861.5, 79.3) -> (865.9, 76.8) ->
    // (872.8, 74.1, 528.87) -> east up the ramp — is 145yd against 189yd for
    // the way round, so the search prefers it by 44yd every time.
    //
    // IT IS ALREADY DOCUMENTED, from the other end: the Leg A header in
    // PitOfSaronEvents notes that "the corridor A* actually prefers between
    // Krick and the ledge climbs the ramp at y ~ 76". The anchors were authored
    // to walk round it; this row is what stops everything that does NOT follow
    // the anchors — stragglers, recovery paths, the StridedPathfinder fallback —
    // from taking it and clipping the party through the arena wall.
    //
    // THE BOX IS SMALL BECAUSE IT ONLY HAS TO CUT ONE LINK. x 862..873,
    // y 71..81, z 520..531 covers the y 76.8 crossing and nothing else:
    //   - four navmesh polys fall inside it, all of them the bridge itself;
    //   - hard-removing every one of them leaves ZERO walkable polys cut off
    //     from the arena (marooned = 0), and that still holds under the stricter
    //     "ban a poly if ANY corner is inside" reading of the StridedPathfinder
    //     point screen, which bans 10 polys and still maroons nothing;
    //   - no point of the intended route is taxed. The nearest legitimate
    //     corridor point is (860.00, 69.40, 518.47), outside on all three axes,
    //     and anchors 3..8 (846.6/84.7 through 882.6/57.5) are all clear.
    // Z FLOOR 520 IS LOAD-BEARING: the descent from the arena to gate 1 runs
    // z 517..519 directly under the west half of the box, so a ceiling above it
    // is what keeps the way round untaxed.
    //
    // Verified reroute: Ick -> the top of the ambush ramp goes 145yd -> 189yd
    // and now crosses at (861.93, 52.93) -> (871.13, 58.27), i.e. the ramp foot
    // by gate 1. Ick -> gate 1 (88yd) and gate 1 -> ramp top (100yd) are
    // unchanged. costMult 40, the shortcut class.
    //
    // Blackrock Spire (map 229) — the LBRS / UBRS WING REGIONS, the only
    // wing-tagged rows (see the header). A "ubrs" row keeps an LBRS run's routes
    // out of Upper Blackrock Spire, an "lbrs" row keeps a UBRS run out of Lower;
    // the same boxes tell DcRunWing's fallback which half the party stands in.
    //
    // MEASURED OFF THE MAP'S OWN MMTILES AND SPAWNS (2026-09-28): all six 229
    // tiles (5,294 polys) parsed into a poly graph, cross-tile links matched on
    // edge position AND height (without the height check a z111 edge joined a
    // z-35 one). With the polys within 8yd of the Dragonspine Door (GO 164725,
    // 126.9, -319.1, 70.9) blocked, everything still reachable from Emberseer is
    // UBRS (1,019 polys, walled off from LBRS); the rest reachable from the
    // portal is LBRS or the shared entry hall (1,944). Every UBRS poly centre is
    // inside a ubrs box and no UBRS vertex inside an lbrs one; every LBRS centre
    // outside the hall is inside an lbrs box and no hall poly is.
    //
    // THE SHARED ENTRY HALL IS UNFENCED FOR BOTH: portal nook, main hall, stairs
    // and the door approach, roughly x 56..130, y -392..-215, z 44..76. A UBRS
    // run (and its rez recovery) walks portal -> door through it; the LBRS run
    // leaves it by the west stairs (x 52..55, y -307..-326), its only other exit.
    // It holds Scarshield packs (two flank the door approach), fightable by either.
    //
    // The halves are STACKED, so the Z bands carry the split where XY overlaps:
    // UBRS west of x15 starts at z111 while LBRS peaks at z95 (Wyrmthalak) -> the
    // z104 floor of the first ubrs box and the z100 cap of the west lbrs box; the
    // rookery above the hall starts at z89.9 over a hall at <= z73.6 -> z82 / z80.
    // The door approach and the Dragonspine Hall share a height (z65-74 vs 71), so
    // there the cut is the plane x = 128.5 plus two side boxes for rune rooms 1
    // and 3, which clear the approach lane (y -312.3..-325.6) by ~1.9yd.
    //
    // Route checks over the poly graph: portal -> Omokk -> ... -> Wyrmthalak (521
    // points) touches no ubrs box; portal -> door -> Emberseer -> Father Flame ->
    // Rend -> Beast -> Drakkisath (258) touches no lbrs box; portal -> door touches
    // neither. costMult 40, the shortcut class; a party that somehow stands inside
    // the other wing still routes out (a fence never cages — see the header).
    constexpr std::array<DcNavPenaltyVolume, 18> kVolumes = {{
        { 229, -134.0f, -406.0f, 33.0f, -118.0f, -374.0f, 56.0f, 40.0f },
        { 229,  -65.5f, -384.0f, 49.4f,  -60.5f, -377.0f, 54.2f, 40.0f },
        { 556,   25.0f,  150.0f, -5.0f,   68.0f,  248.0f, 30.0f, 40.0f },
        { 546,   25.0f, -375.0f, 22.0f,  77.0f, -347.0f, 40.0f, 40.0f },
        { 552,  233.5f,  136.9f, 10.4f,  277.5f,  180.9f, 34.4f,  8.0f },
        { 552,  231.9f,  109.9f, 10.4f,  275.9f,  153.9f, 34.4f,  8.0f },
        { 552,  242.3f,  -83.3f, 10.5f,  286.3f,  -39.3f, 34.5f,  8.0f },
        { 552,  314.5f,    5.4f, 36.4f,  358.5f,   49.4f, 60.4f,  8.0f },
        { 552,  373.4f,   -3.8f, 36.3f,  417.4f,   40.2f, 60.3f,  8.0f },
        { 658,  862.0f,   71.0f, 520.0f,  873.0f,   81.0f, 531.0f, 40.0f },
        // 229 UBRS: upper floor z>=104 — Drakkisath hall, Rend/Gyth stadium, The Beast
        { 229,  -35.0f, -595.0f, 104.0f,  245.0f, -225.0f, 140.0f, 40.0f, "ubrs" },
        // 229 UBRS: rookery / Father Flame above the hall, Emberseer's room, ramps
        { 229,   15.0f, -372.0f,  82.0f,  245.0f, -225.0f, 104.0f, 40.0f, "ubrs" },
        // 229 UBRS: Dragonspine Hall past the door (x>128.5), rune rooms 2, 4-7, Emberseer In
        { 229,  128.5f, -372.0f,  66.0f,  245.0f, -225.0f, 104.0f, 40.0f, "ubrs" },
        // 229 UBRS: rune room 1, south of the door approach
        { 229,  110.0f, -372.0f,  66.0f,  128.5f, -327.5f,  82.0f, 40.0f, "ubrs" },
        // 229 UBRS: rune room 3, north of the door approach
        { 229,  110.0f, -310.5f,  66.0f,  128.5f, -225.0f,  82.0f, 40.0f, "ubrs" },
        // 229 LBRS: the west — Omokk, Vosh, Voone, spiders, Urok, Zigris, Halycon, Wyrmthalak
        { 229, -235.0f, -600.0f, -45.0f,   10.0f, -215.0f, 100.0f, 40.0f, "lbrs" },
        // 229 LBRS: middle strip, capped under the rookery (UBRS there starts at z89.9)
        { 229,   10.0f, -600.0f, -45.0f,   55.0f, -215.0f,  80.0f, 40.0f, "lbrs" },
        // 229 LBRS: the east, south of the entry hall
        { 229,   55.0f, -600.0f, -45.0f,  140.0f, -393.0f,  80.0f, 40.0f, "lbrs" },
    }};

    // ---- polygonal no-go regions ----------------------------------------
    // Same contract as kVolumes (a route cost, and a hard reject in the
    // StridedPathfinder corridor screen), but a polygon footprint for a spot a
    // box can't hug.
    //
    // Sethekk Halls (map 556) — a room corner where the navmesh stitches a sliver
    // of floor out over a drop, so a bot that clips the corner falls under the
    // world. The five vertices are the measured arc that rounds the corner off,
    // all on the z≈26.7 floor; the polygon they enclose is the pocket to keep
    // routes out of. The corner is fenced with the arc itself rather than a box
    // because the arc's bounding box would spill well past it into open floor,
    // and the StridedPathfinder screen HARD-rejects (doesn't just tax) any
    // corridor entering the region — an over-sized footprint there could wall off
    // a legitimate lane and strand the party. costMult 40 matches the other 556
    // shortcut row (a spot a real player can't be, not a survivable hazard).
    //
    // Hellfire Ramparts (map 543) — the drop-off edge along the south-west side of
    // the zone-in room. The measured line runs the diagonal
    // (-1367.45, 1645.24, 68.46) -> (-1335.65, 1668.71, 68.47) (≈39.5yd), so an
    // axis-aligned box hugging it would be a ~32x23yd blob spilling across the
    // whole corner and swallowing the walkable floor. A polygon lets the footprint
    // be a strip laid along the edge instead, keeping routes off it while the room
    // centre stays clear. Z band 62..76 straddles the ~z68 floor. costMult 40.
    //
    // THE STRIP IS ASYMMETRIC ABOUT THE MEASURED LINE, AND THAT IS THE WHOLE POINT.
    // It was first authored as that line inflated ±2yd (a ~4yd-thick strip centred
    // on it) and that stranded people. The measured line is a straight chord, but
    // the navmesh edge it is supposed to trace BOWS: sampled perpendicular to the
    // chord, walkable floor runs out anywhere from 0 to 5.5yd on the far side of
    // it, bulging furthest around the middle. Only the two ENDS of the chord are
    // over the void; its middle is 2-3yd inboard of the real edge. So the ±2yd
    // strip sliced a ribbon out of the room and left ≈29 sq yd of ordinary floor
    // marooned between the strip and the drop — reachable only by crossing the
    // strip, which is a hard reject in the StridedPathfinder screen. A party that
    // zoned in and drifted onto it (the zone-in point is ~11yd away) could not be
    // routed out, and the tank set off round the far side of the room instead:
    // the reported "starts on the wrong side of the invisible wall" symptom.
    //
    // The fix is to stop centring the strip on the chord and push its far side out
    // into the void: the footprint now runs from 10yd BEYOND the chord (past the
    // furthest the floor ever reaches, so that boundary is over the drop along the
    // entire length) to 2yd inboard of it — where the room floor is untouched, as
    // before. Nothing can be marooned behind a boundary that is over a cliff.
    // Verified against the map's mmtiles: 0.0 sq yd of walkable floor now lies on
    // the far side, down from ≈29. The inboard edge has NOT moved, so routes are
    // steered exactly as they were; only the far side changed.
    // Ragefire Chasm (map 389) — a funnel wall, authored from four measured
    // points rather than a single straight run. The route producer cuts across a
    // stretch of cave floor the party should not take; fencing the crossing sends
    // it round the intended way. The measured polyline is
    //     (-283.38, -37.05, -58.46) -> (-277.23, -22.82, -58.18)
    //  -> (-265.03, -17.26, -56.65) -> (-238.73, -22.41, -58.18)
    // (three legs, ≈15.5 / 13.4 / 26.8yd, bending ~40° at each joint). No single
    // quad follows a bent line, so it is authored as one thin quad PER LEG — each
    // leg inflated ±2yd on its perpendicular (≈4yd thick), same recipe as the
    // Hellfire Ramparts wall above. Every leg is extended 2yd past each of its
    // ends: at the two interior joints that makes consecutive quads overlap so
    // the bend leaves no gap to squeeze through, and at the two outer ends it
    // puts the measured endpoints strictly inside the footprint rather than on
    // its boundary edge (where the even-odd test is ill-defined) and seals the
    // wall against whatever geometry it abuts. Z band -64..-50 straddles the
    // ≈-58 floor. costMult 40 (a line the party must not cross, same class as
    // the shortcut rows above).
    // Wailing Caverns (map 43) — the WALL between the lower east floor (z~-63)
    // and the upper west plateau (z~-56/-54) that carries the Pythas -> Skum leg.
    // Recast stitches walkable faces up that wall, so Detour's A* climbs it
    // instead of taking the tunnel loop a player has to walk. Reported traversal:
    // the tank leaves the lower floor at (-76.78, -259.83, -64.64) and climbs
    //     (-78.1, -259.7, -62.6)  ->  (-85.3, -263.5, -53.3)
    // (9.3yd rise over 8.1yd ground = ~49 degrees).
    //
    // THE FENCE COVERS THE WHOLE WALL, NOT JUST THE REPORTED RAMP. There is a
    // SECOND stitched face 12yd north — (-89.1, -248.0, -60.5) -> (-92.3, -250.7,
    // -54.4), 6.1yd over 4.2yd = ~55 degrees, steeper still — onto the same
    // plateau. Fencing only the reported ramp moves the climb there and nothing
    // else changes: measured against the real mmtiles, the corridor goes from
    // 30yd (up the ramp) to 50yd (up the north face) instead of to the 240yd way
    // round. Both faces have to be priced for the party to walk the tunnels.
    //
    // Authored as a 6yd-wide strip laid ALONG the wall — three quads, one per leg
    // of the (-92.5,-246.5) -> (-88.5,-252.5) -> (-80.5,-261.5) -> (-73.5,-266.5)
    // polyline, each inflated +-3yd on its perpendicular and extended 2yd past
    // both ends (consecutive quads overlap at the bends; the outer ends seal
    // against the geometry either side). Same recipe as the Ragefire funnel wall
    // above. A box cannot do this job: the wall is diagonal, and any axis-aligned
    // box hugging it also swallows either the plateau or the lower floor.
    //
    // Z band -63.5..-52.0 straddles the climbs (which run -63.2 up to -53.3). The
    // plateau's own floor is at -56.4/-54.6, i.e. INSIDE that band — Z alone
    // cannot separate the plateau from the shelves on the wall, so the XY strip is
    // what keeps it clear, and the strip is drawn to stop short of it.
    //
    // Validated against the map's mmtiles (tools/probe_navmesh.py + a poly-
    // adjacency walk over 043*.mmtile), per the no-cage rule above:
    //   - six polys fall inside the strip, all of them wall faces or the shelves
    //     between them; the plateau, the lower floor and the -60.5 rock spine the
    //     legitimate route crosses are all outside it;
    //   - hard-removing every one of those six leaves ZERO walkable polys cut off
    //     from the instance entrance (marooned = 0), so nothing is caged even
    //     under the StridedPathfinder's hard reject;
    //   - the shortcut is priced out: lower floor -> plateau goes 30yd -> 240yd,
    //     and the leg that actually matters, Pythas -> Skum, goes 414yd -> 459yd
    //     (+45yd, round the north tunnels). Entrance -> Pythas and Entrance ->
    //     Skum are unchanged.
    // costMult 40, the shortcut class (a line a real player cannot walk).
    // Halls of Lightning (map 602) — the two NAV_SLIME moats that flank the Slag
    // Furnace walkway, and the ONLY rows on this list that are not about a
    // navmesh shortcut.
    //
    // The moats are navigable mesh: the mmap generator meshes the liquid SURFACE
    // and stamps those polys NAV_SLIME (flags 0x04), so a route can sit perfectly
    // on the navmesh and still have the party wading the length of the pit. That
    // is what these rows price — exactly as Azjol-Nerub's lake had to be priced
    // ([[dc-an-lower-kingdom-is-flooded]]), except there the answer was to move
    // the authored anchors and here it is to tell the ROUTER, because the pit is
    // also where the transit's stragglers and its recovery paths get re-routed.
    //
    // MEASURED, NOT EYEBALLED. t/TestHallsOfLightningRouteProbe scans the pit on a
    // 4yd grid and asks each column whether the surface a bot would stand on is a
    // NAV_SLIME poly — asked of the POLY, because both indirect tests tried first
    // were wrong: "is there a NAV_GROUND poly nearby" calls a shoreline column dry
    // (it carries both the slime and the walkway shelf 3.7yd above it), and the
    // Azjol-Nerub ground-drop test disagrees with itself at the channel mouths,
    // where the floor really is walkable at the slime's own height.
    //
    // The answer is an HOURGLASS rather than a pair of rectangles. Each channel
    // runs y -200 .. -124 (the two mouths, y -204 and y -120, are dry across the
    // whole pit), reaches x 1310 (west) / x 1350 (east) through most of its
    // length, and pinches back to x 1294 / x 1370 at the waist, y -168 .. -160.
    // A single box around either one would have to swallow 16yd of walkable
    // walkway at the waist to cover the bulges. Hence polygons, which is what the
    // DcNavPenaltyPolygon form is for.
    //
    // THE WALKWAY IS UNTAXED, and that is the property the probe asserts rather
    // than "no dry column is ever inside a row". The dry span is at its narrowest
    // x 1314..1346 (y -124, -140, -144, -188), the authored route hugs x
    // 1330..1341 — dead centre — and NEITHER polygon's inner edge ever crosses
    // x 1312 (west) or x 1348 (east), so the band the party actually walks can
    // never be taxed. A handful of columns at the ragged channel edges fall on
    // the wrong side of a straight polygon edge in both directions; on a 4yd grid
    // against an 8-vertex ring that is unavoidable, and a costMult is survivable
    // there in a way a rejection would not be.
    //
    // Nothing is caged: both channels are dead ends against the pit walls, so no
    // pocket of floor is cut off on the far side of a row, and both consumers
    // exempt a route that BEGINS inside one — which is what a bot knocked into
    // the slime needs.
    //
    // Z BAND 16..22.5, DELIBERATELY BELOW THE FLOOR. The slime surface probes at
    // z 20.18 and the walkway at z 23.88, so a ceiling under the floor means the
    // rows can only ever tax an edge whose midpoint is ON the liquid. Widening
    // this to cover the floor would tax the walkway's own edges at the pit walls
    // for nothing.
    //
    // costMult 6 rather than the shortcut rows' 40: wading is slow and it hurts,
    // but it is not a place a real player cannot be, and the pit is a corridor the
    // party must cross either way. Same reasoning as the Arcatraz hazard rows.
    constexpr std::array<DcNavPenaltyPolygon, 10> kPolygons = {{
        { 556, 15.0f, 38.0f, 40.0f, 5,
          { -233.29f, -230.34f, -209.82f, -192.94f, -192.04f },
          {  275.04f,  309.39f,  326.92f,  305.38f,  271.93f } },
        { 543, 62.0f, 76.0f, 40.0f, 4,   // far side over the drop | inboard side unchanged
          { -1373.39f, -1341.59f, -1334.46f, -1366.26f },
          {  1653.29f,  1676.76f,  1667.10f,  1643.63f } },
        { 389, -64.0f, -50.0f, 40.0f, 4,   // leg 1
          { -282.34f, -274.60f, -278.27f, -286.01f },
          {  -39.68f,  -21.78f,  -20.19f,  -38.09f } },
        { 389, -64.0f, -50.0f, 40.0f, 4,   // leg 2
          { -278.22f, -262.38f, -264.04f, -279.88f },
          {  -25.47f,  -18.25f,  -14.61f,  -21.83f } },
        { 389, -64.0f, -50.0f, 40.0f, 4,   // leg 3
          { -267.38f, -237.15f, -236.38f, -266.61f },
          {  -18.84f,  -24.76f,  -20.83f,  -14.91f } },
        { 43, -63.5f, -52.0f, 40.0f, 4,   // wall leg 1 (north face)
          { -91.11f, -84.89f, -89.89f, -96.11f },
          { -243.17f, -252.50f, -255.83f, -246.50f } },
        { 43, -63.5f, -52.0f, 40.0f, 4,   // wall leg 2
          { -87.59f, -76.93f, -81.41f, -92.07f },
          { -249.01f, -261.00f, -264.99f, -253.00f } },
        { 43, -63.5f, -52.0f, 40.0f, 4,   // wall leg 3 (the reported ramp)
          { -80.38f, -70.13f, -73.62f, -83.87f },
          { -257.90f, -265.22f, -270.10f, -262.78f } },
        { 602, 16.0f, 22.5f, 6.0f, 8,   // Slag Furnace, WEST moat
          { 1274.0f, 1274.0f, 1308.0f, 1312.0f, 1296.0f, 1296.0f, 1312.0f, 1312.0f },
          { -122.0f, -202.0f, -202.0f, -174.0f, -170.0f, -158.0f, -154.0f, -122.0f } },
        { 602, 16.0f, 22.5f, 6.0f, 8,   // Slag Furnace, EAST moat
          { 1386.0f, 1386.0f, 1358.0f, 1348.0f, 1368.0f, 1368.0f, 1348.0f, 1348.0f },
          { -122.0f, -202.0f, -202.0f, -174.0f, -170.0f, -158.0f, -154.0f, -122.0f } },
    }};

    // Even-odd ray cast — true iff (x,y) is inside the polygon's XY footprint.
    // Handles convex or concave simple polygons and is winding-agnostic.
    bool PointInPolygonXY(DcNavPenaltyPolygon const& p, float x, float y)
    {
        bool inside = false;
        for (uint32 i = 0, j = p.vertCount - 1; i < p.vertCount; j = i++)
        {
            float const xi = p.vx[i], yi = p.vy[i];
            float const xj = p.vx[j], yj = p.vy[j];
            bool const straddles = (yi > y) != (yj > y);
            if (straddles && x < (xj - xi) * (y - yi) / (yj - yi) + xi)
                inside = !inside;
        }
        return inside;
    }
}

bool DcNavPenaltyRegistry::RowActive(char const* rowWing, std::string_view runWing)
{
    if (!rowWing || !*rowWing)
        return true;
    return !runWing.empty() && runWing != rowWing;
}

bool DcNavPenaltyRegistry::HasVolumes(uint32 mapId, std::string_view runWing)
{
    for (auto const& v : kVolumes)
        if (v.mapId == mapId && RowActive(v.wing, runWing))
            return true;
    for (auto const& p : kPolygons)
        if (p.mapId == mapId && RowActive(p.wing, runWing))
            return true;
    return false;
}

bool DcNavPenaltyRegistry::IsInsideRegion(uint32 mapId, float x, float y, float z, std::string_view runWing)
{
    return PenaltyAt(mapId, x, y, z, runWing) > 1.0f;
}

namespace
{
    bool InVolume(DcNavPenaltyVolume const& v, float x, float y, float z)
    {
        return x >= v.minX && x <= v.maxX && y >= v.minY && y <= v.maxY && z >= v.minZ &&
               z <= v.maxZ;
    }

    bool InPolygon(DcNavPenaltyPolygon const& p, float x, float y, float z)
    {
        return z >= p.minZ && z <= p.maxZ && PointInPolygonXY(p, x, y);
    }
}

float DcNavPenaltyRegistry::PenaltyAt(uint32 mapId, float x, float y, float z, std::string_view runWing)
{
    float worst = 1.0f;
    for (auto const& v : kVolumes)
    {
        if (v.mapId != mapId || !RowActive(v.wing, runWing) || !InVolume(v, x, y, z))
            continue;
        if (v.costMult > worst)
            worst = v.costMult;
    }
    for (auto const& p : kPolygons)
    {
        if (p.mapId != mapId || !RowActive(p.wing, runWing) || !InPolygon(p, x, y, z))
            continue;
        if (p.costMult > worst)
            worst = p.costMult;
    }
    return worst;
}

char const* DcNavPenaltyRegistry::WingRegionAt(uint32 mapId, float x, float y, float z)
{
    for (auto const& v : kVolumes)
        if (v.mapId == mapId && v.wing && *v.wing && InVolume(v, x, y, z))
            return v.wing;
    for (auto const& p : kPolygons)
        if (p.mapId == mapId && p.wing && *p.wing && InPolygon(p, x, y, z))
            return p.wing;
    return nullptr;
}
