/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "SealedEncounterRegistry.h"

#include <cmath>

namespace
{
    // --- Magisters' Terrace (585) — Selin Fireheart -----------------------------
    //
    // The volume is Selin's room, and it is deliberately the SAME box as this room's
    // FightInPlaceRegistry zone: [216,260] x [-45,45]. Both are derived from the same
    // two facts — Selin's own CanAIAttack plane (`who->GetPositionX() > 216.0f`) and
    // the Assembly Chamber Door hanging at X=215.1 — so agreement is not a
    // coincidence to be maintained by hand. It is asserted in
    // t/TestSealedEncounter.cpp rather than shared as a literal, because the two
    // registries answer different questions ("may the pull drag out of here" vs
    // "will the door lock me out") and a future room could easily need one and not
    // the other.
    //
    // approachRadius 45yd from the boss. Selin spawns at (242.07, 0.3), so 45yd
    // reaches back to X~197 — the staging chamber in front of the doorway, which is
    // where the party needs to start closing up. It does NOT reach the scripted-pull
    // camp at (170.46, 0.57), 71.6yd out, so the guard-pack stages run under the
    // ordinary gates exactly as before and only the final walk-in is affected.
    //
    // musterSpread 10yd. Follow-tank trails at min(followDistance, 6yd), so the party
    // sits inside this by construction while moving and the clump costs nothing in the
    // healthy case; it only bites on a genuine straggler. Tighter would fight
    // follow-tank's own spacing and turn every approach into a stutter.
    // --- Azjol-Nerub (601) — Anub'arak -----------------------------------------
    //
    // instance_azjol_nerub registers THREE DOOR_TYPE_ROOM doors on DATA_ANUBARAK
    // (192396 / 192397 / 192398, all at x 550-552, y 252-256), and
    // boss_anub_arak::JustEngagedWith schedules EVENT_CLOSE_DOORS at 5s, whose
    // only body is `BossAI::_JustEngagedWith()` — the SetBossState(IN_PROGRESS)
    // that shuts them. Five seconds after the pull the arena is sealed, and a
    // straggler still in the north corridor is out for the whole fight.
    //
    // The volume is the arena itself, read off the live 601 navmesh: one flat
    // floor at z 224.07-224.29 filling a rough circle x 528-572, y 236-276, fed
    // by a single corridor from the north (x 544-560, climbing to z ~230 by
    // y 320) and leaving by a second to the south. [526,574] x [234,278] is that
    // floor with 2yd of slack and nothing else — it agrees with the encounter's
    // own BossBoundaryData, a CircleBoundary at (550.6, 253.6) r 32.
    //
    // approachRadius 45yd from Anub'arak's spawn (551.0, 248.3, 224.0) reaches
    // back to y ~293, i.e. up into the mouth of the north corridor, which is
    // where the party needs to be closing up. It does NOT reach the Anub'ar
    // Prime Guards at y 341, so that pull runs under the ordinary gates.
    //
    // musterSpread 10yd, the same number and the same reasoning as Selin's.
    // --- Gundrak (604) — Gal'darah ---------------------------------------------
    //
    // instance_gundrak registers GO_GAL_DARAH_DOORS0 (192568, hanging at
    // (1848.03, 743.82, 135.95)) as DOOR_TYPE_ROOM against DATA_GAL_DARAH, so
    // InstanceScript::UpdateDoorState holds it `open &= (state != IN_PROGRESS)` —
    // shut for exactly as long as the fight lasts. It is the only way into the
    // arena: the wing is one causeway east from the bridge crossing, and its two
    // other doors (193208 / 193209) are PASSAGE doors on post-kill exit corridors
    // that dead-end inside the same component.
    //
    // The volume. comp#1 splits cleanly at the door plane, with a 7.4yd DEAD BAND
    // straddling the door that makes the cut unambiguous:
    //
    //   west of x 1849 — the approach causeway: 10 polys, X 1796.53-1841.60,
    //                    Y 735.47-751.73, Z 119.20-135.74
    //   east of x 1849 — the arena and its exits: 278 polys, X 1850.13-1981.07,
    //                    Y 640.00-846.93, Z 135.20-137.60
    //
    // The geometrically ideal box is X 1849-1982, Y 648-848, Z 133-142, enclosing
    // 275 comp#1 polys and zero foreign walkable geometry — but SealedEncounterRow
    // has NO Z BAND (InSealedRoom is 2D), so this takes the fallback. minX 1855
    // rather than 1849 keeps the box clear of the door corridor itself; minY 648
    // excludes two comp#0 polys sitting in the arena's own Z band at the far
    // south-west corner (X 1849.6-1854.9, Y 640.0-645.1), at the cost of a few
    // yards of the south-east exit tail near (1915-1920, 641-647) — which is behind
    // PASSAGE door 193209 and irrelevant to the "inside when the door shuts" test.
    //
    // KNOWN LIMITATION, ACCEPTED. Without a Z band the 2D footprint also contains
    // 52 comp#0 polys spanning Z 110.67-178.14 — the entrance walkway passing
    // OVERHEAD and the moat below — so a party member standing on that walkway
    // would read as "in the sealed room". On a real run this cannot happen: by the
    // time Gal'darah is pulled the party has been teleported to (1802, 743.5) and
    // walked east, and there is no path from there back up to the entrance walkway.
    // Recorded so a future reader does not mistake the loose box for an oversight.
    // If a Z band is ever added to SealedEncounterRow, tighten this row to
    // 1849 / 1982 / 648 / 848 / 133 / 142.
    //
    // approachRadius 75, and NOT the 45 the two rows above take. That number is not
    // a house style — it is "far enough back that the gates arm before the party
    // threads the door", and this room is much deeper than Selin's or Anub'arak's:
    // its door is 66.76yd from its boss, where Selin's is 27yd. Measured along the
    // causeway centreline, 3D from Gal'darah's spawn:
    //
    //     x 1835  z 130.26   80.00yd      x 1850  z 135.97   64.75yd
    //     x 1840  z 132.84   74.84yd      x 1855  z 136.21   59.75yd
    //     x 1848  z 135.34   66.76yd  <-- the door
    //
    // so 45 would arm the gates only at x ~1870, twenty yards INSIDE the room and
    // past the muster point — the clump would never get a chance to close the party
    // up, and the muster would be asking about a threshold everyone had already
    // crossed or been left behind at. 75 arms at x ~1840, about 8yd west of the
    // door on the ramp, which is where the party needs to be tightening. It does
    // not reach back to the teleport landing at 114yd, so the crossing itself runs
    // under the ordinary gates.
    //
    // The natural muster spot inside is (1858.00, 743.60, 136.23) — 10yd past the
    // door plane, on 136.23 ground in comp#1 with an 11.5yd continuous walkable
    // disc (x 1852 gives 5.25, x 1854 gives 7.25), 7 polys from the teleport
    // landing and 3yd inside minX. There is no field for it here (the volume IS the
    // muster), so it is recorded rather than authored.
    //
    // musterSpread 10yd, the same number and the same reasoning as Selin's:
    // follow-tank trails at min(followDistance, 6yd), so the party sits inside it by
    // construction while moving and it only bites on a genuine straggler.
    // --- Halls of Stone (599) — Sjonnir the Ironshaper -------------------------
    //
    // boss_sjonnirAI::JustEngagedWith puts GO_SJONNIR_DOOR (191296) into
    // GO_STATE_READY — it SHUTS behind the party the moment he is pulled — and it
    // is the only way into his room. That is the Selin shape exactly, with one
    // difference in the party's favour: Reset() reopens it (`if BRANN_DOOR is DONE
    // -> SetGoState(GO_STATE_ACTIVE)`), so a wipe here is cheap and does not cost
    // the door. What a wipe cannot undo is a follower left OUTSIDE for the whole
    // fight while four people solo a boss with an infinite add stream.
    //
    // Without this row that lockout is also invisible to the clear in a second,
    // nastier way: a closed 191296 is precisely the signal five runs of
    // tp-20260831-205458-3 ended on ("paused for over 60s: a closed door is
    // blocking the path"). A straggler outside a legitimately-shut encounter door
    // would report the same thing mid-fight and read as the very nav failure this
    // dungeon's automation was written to remove.
    //
    // The volume is the boss's own evade box, which instance_halls_of_stone states
    // outright: BossBoundaryData { BOSS_SJONNIR, new RectangleBoundary(1206.56f,
    // 1341.4185f, 579.9434f, 753.9599f) }. minX is nudged from 1206.56 to 1210 to
    // keep the box clear of the door corridor itself, so a bot standing IN the
    // doorway is not counted as already inside.
    //
    // KNOWN LIMITATION, ACCEPTED — the same one the Gundrak row carries, and worth
    // restating because map 599 is in the flat-grid-height family
    // (ac-map601-flat-gridheight-zero). InSealedRoom is 2D and SealedEncounterRow
    // has no Z band, and column probes inside this footprint return three surfaces:
    // the real floor at z 189.76 plus phantoms at 162.29 and 0.16. Those two are
    // artifacts of the map's flat grid height, not walkable floors with a path to
    // them, so nothing can stand on them and the 2D test is exact in practice.
    //
    // approachRadius 100, and NOT the 45 the first two rows take, for the reason
    // the Gundrak row spells out: the number has to arm the gates BEFORE the party
    // threads the door, and this room is deep. Sjonnir sits at (1295.21, 667.16,
    // 189.69), 88.7yd from his door — where Selin's is 27yd — so 45 would arm at
    // x ~1250, forty yards INSIDE the room and long past the muster point. 100 arms
    // at x ~1196, which is 1.5yd from objective 3's door-stage anchor: the party is
    // tightening up exactly where the event already parks it to talk to Brann. It
    // does not reach back to the Tribunal arena 400yd west, so the whole approach
    // from the arena runs under the ordinary gates.
    //
    // musterSpread 10, the same number and the same reasoning as the rows above:
    // follow-tank trails at min(followDistance, 6yd), so the party sits inside it
    // by construction while moving and it only bites on a genuine straggler.
    // --- Karazhan (532) — Shade of Aran --------------------------------------
    //
    // The Private Library Door (184517, at (-11189.5, -1880.9)) is the only way
    // into the Guardian's Library. Aran's Reset() opens it; 15s after he is
    // engaged it shuts and goes NOT_SELECTABLE for the fight. So the real budget
    // is fifteen seconds, not the whole fight, and anyone still in the corridor
    // then is out.
    //
    // The volume is the library floor, flood-filled on the live 532 navmesh with
    // the door cut: 42 polys, x -11194.1..-11136.0, y -1941.1..-1882.9,
    // z 232.2..234.6, none shared with the corridor (x -11217.9..-11185.9,
    // y -1878.4..-1851.7, north-west of the door). maxY -1886 keeps the door and
    // its sill (-11187.7, -1883.3) outside; it drops 2 of the 42 polys.
    //
    // Z BAND 225-245. Curator's floor (~166) and the terrace (~92) are stacked
    // under this footprint, and two polys at z 287 above it; the band holds
    // only the library.
    //
    // approachRadius 50. The door is 39.1yd from Aran and the points 8/10yd out
    // along the corridor are 46.8/48.8yd, so 50 arms the clump just before the
    // party reaches the door.
    //
    // musterSpread 10, the same number and reasoning as the rows below.
    // --- Karazhan (532) — Netherspite ------------------------------------------
    //
    // The Massive Door (185521, at (-11186.2, -1665.1)) stands open, shuts when
    // Netherspite is engaged and reopens when he dies, and he does
    // DoZoneInCombat on engage: a member left in the corridor is in combat and
    // locked out for the whole fight.
    //
    // The Celestial Watch lies north-east of the door: 181 polys, x
    // -11217.7..-11062.8, y -1704.8..-1555.2, z 278.3..281.8. No single box both
    // fits the room and excludes the approach corridor (9 corridor polys fall
    // inside the room's full extent, x -11219.9..-11193.2), so minX -11182 drops
    // the room's far west strip (x < -11182, y -1662..-1590) and keeps the
    // doorway out. About 131 of the 181 room polys are in; no corridor poly is.
    //
    // Z BAND 276-284: levels below (z 115-187) and above (287-364).
    //
    // approachRadius 107. The door is 97.5yd from Netherspite; 8/10yd out along
    // the corridor are 105.1/107.0yd.
    // --- Karazhan (532) — Prince Malchezaar -------------------------------------
    //
    // The Netherspace Door (185134, at (-11018.5, -1967.9)) is a DOOR_TYPE_ROOM
    // door on DATA_MALCHEZAAR: it shuts the moment he is engaged and stays shut
    // for the whole fight, and Netherspace is the only way to him — anyone still
    // on the stair landing outside is locked out, infernals and all.
    //
    // Netherspace lies east of the door: 32 polys flood-filled with the door cut
    // (TestKarazhanChessProbe.PrintsNetherspace), x -11020.5..-10906.1,
    // y -2033.1..-1938.7, z 275.1..275.9. minX -11015 keeps the door and its sill
    // out. The stair landing outside is at the same height (z 274.7, x -11056),
    // so the X bound is what keeps it out; no corridor poly lies east of the door
    // line at platform height.
    //
    // Z BAND 272-280: the tower's lower floors below (the Gamesman's Hall is
    // z 221-238) and the stair itself climbing to 274.
    //
    // approachRadius 86. The door is 75.8yd from Prince; 8/10yd out along the
    // landing are ~84/86yd.
    SealedEncounterRow const kRows[] =
    {
        // mapId  boss   minX    maxX    minY    maxY   approach  muster
        {   585, 24723, 216.0f, 260.0f, -45.0f, 45.0f,    45.0f,  10.0f },
        {   601, 29120, 526.0f, 574.0f, 234.0f, 278.0f,   45.0f,  10.0f },
        {   604, 29306, 1855.0f, 1982.0f, 648.0f, 848.0f,  75.0f,  10.0f },
        {   599, 27978, 1210.0f, 1342.0f, 580.0f, 754.0f, 100.0f,  10.0f },
        //                                                           minZ    maxZ
        {   532, 16524, -11195.0f, -11135.0f, -1942.0f, -1886.0f, 50.0f, 10.0f, 225.0f, 245.0f },
        {   532, 15689, -11182.0f, -11058.0f, -1706.0f, -1550.0f, 107.0f, 10.0f, 276.0f, 284.0f },
        {   532, 15690, -11015.0f, -10900.0f, -2040.0f, -1935.0f,  86.0f, 10.0f, 272.0f, 280.0f },
    };
}

SealedEncounterRow const* SealedEncounterRegistry::Find(uint32 mapId, uint32 bossEntry)
{
    for (SealedEncounterRow const& r : kRows)
        if (r.mapId == mapId && r.bossEntry == bossEntry)
            return &r;
    return nullptr;
}

bool SealedEncounterRegistry::InSealedRoom(SealedEncounterRow const& row, float x, float y)
{
    return x >= row.minX && x <= row.maxX && y >= row.minY && y <= row.maxY;
}

bool SealedEncounterRegistry::InSealedRoom(SealedEncounterRow const& row,
                                           float x, float y, float z)
{
    if (!InSealedRoom(row, x, y))
        return false;
    if (row.minZ == 0.0f && row.maxZ == 0.0f)
        return true;
    return z >= row.minZ && z <= row.maxZ;
}

bool SealedEncounterRegistry::InApproachRange(SealedEncounterRow const& row,
                                             float x, float y, float z,
                                             float bx, float by, float bz)
{
    // 3D, so a party passing on another floor of a multi-level instance cannot arm
    // the gates from below or above the boss.
    float const dx = x - bx;
    float const dy = y - by;
    float const dz = z - bz;
    return (dx * dx + dy * dy + dz * dz) <= (row.approachRadius * row.approachRadius);
}
