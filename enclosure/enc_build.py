"""Build PCB1 plug-in enclosure for FDM on a 0.4 nozzle: body cup + bonded -X lid, no screw, no snap.
The parting plane is vertical at x = PX: the body wraps the PCB on five sides and the board slides
in along +X; the lid is the Type-C end cap carrying the port and the joint ring groove.
Board locating is by two edge slots, not ribs: a tail socket in the body +X wall (1.75 tall =
board 0.83 + antenna stack 0.92) and a front slot in the lid (0.83 tall = board). Both are line to
line on the measured board, so the board is clamped at both ends with no rattle; the printer kerf
(~0.03/side) opens them to a slide fit. Body prints +X face down. Lid prints parting-face UP (bed
= Type-C face, grows +X) with NO support: the end is a smooth lofted nose (tongue stadium -> full
section at the groove start) whose outward growth stays <=45 deg from the print axis; the flare
relief closes with a steep taper so no cavity ceiling is a 90 deg overhang; port bore vertical.
Coords in PCB space (mm)."""
import FreeCAD as App
import Part, Mesh, os, math

OUT = "/Users/lw/work/vst_lan_mic/enclosure"
OBJ = "/Users/lw/work/vst_lan_mic/enclosure/3D_PCB1_2026-11-09.obj"

# --- measured board (user caliper, mm) ---
BL, BW, BT = 28.48, 16.80, 0.83     # length / width / thickness
ANT_T = 0.92                        # antenna stack above board top at the tail
TAIL = BT + ANT_T                   # 1.75 tail stack height
BX0 = 0.015                         # board front edge (connector datum)
BX1 = BX0 + BL                      # board rear edge 28.495
BCY = -8.0355                       # mesh board centre in y; actual width centred on it
BY0, BY1 = BCY - BW / 2, BCY + BW / 2
BZ1 = 0.0                           # board top
BZ0 = BZ1 - BT                      # board bottom -0.83
GAPX = 0.00                         # board rear edge to tail-socket bottom: line to line (no play)
ENG = 2.00                          # tail engagement: how far the tail sits into the socket

# --- component envelope (mesh, mm) ---
# USB-C plug: metal tube y -12.173..-3.923 z -1.600..0.810 (2.41 tall), tip x -10.485; stands 7.2
#   proud of the end face. Rear flare z -2.00..1.20, y -12.10..-4.00, forward tip x -2.923,
#   envelope ends ~x 0.60.
# Button plunger tip x 3.750..5.750 y 0.449 z 0.410..1.310. Shield can z 2.420, x ..21.840
#   (stops well before the tail socket top lip). Nothing but the button passes the board +Y edge.
CLEAR = 0.45          # board edge to cavity wall air (no ribs; locating is by the two slots)
T = 2.44              # = LAND 0.84 + GRO_W 0.82 + inner 0.78; outer wall min 0.84 (was 0.4/0.8)
RV, RH = 2.00, 0.80   # outer corner / outer rim fillet radii (RH < T so the rim stays solid)

X0, X1 = BX0 - CLEAR, BX1 + GAPX     # cavity plan; +X wall inner face = tail-socket bottom
Y0, Y1 = BY0 - CLEAR, BY1 + CLEAR
ZFL = -2.000 - 0.80                  # cavity floor (lowest part -2.00)
ZTOP = 2.420 + 0.50                  # cavity ceiling (tallest part = shield can 2.420)
PLUG_TIP = -10.485
FLARE_X0 = -2.93                   # flare relief start, just ahead of the flare tip (-2.923)
PROUD = 7.20                       # plug must stand >= 7.2 proud of the end face (cased phone)
OX0 = PLUG_TIP + PROUD             # end face -3.285; port lip = FLARE_X0-OX0 = 0.355
OX1 = X1 + T
OY0, OY1 = Y0 - T, Y1 + T

# --- vertical parting plane ---
# The nose taper runs from the Type-C face to the bond-groove start (PX-GRO_D), so the groove sits
# on full section. PX stays ahead of the button pinhole (front edge 4.25): hole wholly in body.
# The lid is a ~7 mm end cap clearing the shield-can front + plunger (upper chamber below).
PX = 4.05

# --- USB-C port (male plug) in the lid -X wall ---
AXZ = -0.395           # tube measured centre (z -1.600..0.810)
PH = 2.50              # bore height: tube is 2.41 tall, so 2.50 leaves ~0.045 air per side
TUBE_Y = (-12.173, -3.923)
GAP = 0.05             # y air per side; z air ~0.045/side from the 2.50 bore on the 2.41 tube
# Cased-phone clearance: no added boss (that would shorten plug reach); the end shoulders taper
# back around a central stadium tongue (same silhouette as the port, uniform ring width).
# Parting-UP print (bed = Type-C face, grows +X): every outer wall may grow outward at most 45 deg
# from the print axis. A slim tongue would need >45 deg to reach the full section in the available
# length, so the ring is thickened to 2.85 and the nose runs all the way to the bond-groove start.
TONGUE_WALL = 2.85
TONGUE_W = (TUBE_Y[1] - TUBE_Y[0]) + 2 * GAP + 2 * TONGUE_WALL
TONGUE_H = PH + 2 * TONGUE_WALL
TCY = (TUBE_Y[0] + TUBE_Y[1]) / 2
# end faces sit T beyond the cavity floor/ceiling (thin wall); the port is no longer centred on
# the end face in z, which is cosmetic only
OZ0, OZ1 = ZFL - T, ZTOP + T
# Recess receiving the connector rear flare, from just ahead of its forward tip to the parting
# plane; keeps the outer face a clean stadium lip. The flare never crosses PX. The recess floor
# sits at the ACTUAL flare bottom (-2.00) + 0.05 air, not the cavity floor: a deeper recess would
# dip below the stadium tongue and open a hole through the shaved bottom shoulder.
FLARE = dict(x=(FLARE_X0, PX + 0.06), y=(TUBE_Y[0] - 0.05, TUBE_Y[1] + 0.05), z=(-2.05, 1.25))

# Button in the +Y wall (body): dia 1.00 pinhole on the plunger tip; the tip stays inside the
# cavity face, so no pocket is needed.
WIN = dict(c=(4.750, 0.860), r=0.50)
# No antenna hold-down pad: it would block the +X slide-in; the tail socket top lip (z 0.92)
# rides line to line on the antenna stack instead.


def box(x0, x1, y0, y1, z0, z1):
    return Part.makeBox(x1 - x0, y1 - y0, z1 - z0, App.Vector(x0, y0, z0))


def fuse(shps):
    return shps[0].fuse(shps[1:])


def prism_x(x0, x1, yy, zz, r):
    """Rounded-rect prism along X (fillet the 4 X-parallel corner edges); r=0 -> plain box."""
    b = box(x0, x1, yy[0], yy[1], zz[0], zz[1])
    if not r:
        return b
    es = [e for e in b.Edges
          if abs(e.tangentAt(e.FirstParameter).x) > 0.99 and e.Length > (x1 - x0) * 0.5]
    return b.makeFillet(r, es)


def port(g):
    """(yy, zz, r) of the connector silhouette: y grown by g, z = 2.40 bore on the tube axis."""
    # 0.02 flat: OCCT rejects a fillet radius exactly equal to half the box height
    return [((TUBE_Y[0] - g, TUBE_Y[1] + g),
             (AXZ - PH / 2, AXZ + PH / 2), PH / 2 - 0.02)]


def port_solid(x0, x1, g):
    return [prism_x(x0, x1, yy, zz, r) for yy, zz, r in port(g)]


# --- bonded joint on the vertical parting face, sized for a 0.4 nozzle ---
# flat land | ring groove (lid) receiving the rib (body) | inner wall. The bond ring is 0.82 wide
# (two+ lines) so it does not snap; land 0.84 + ring 0.82 + inner 0.78 = T. FIT 0: rib line to line
# on the groove in the model; the measured printer opens the groove ~0.06 = clearance + glue room.
LAND = 0.84           # outer lip of the bond ring = min outer wall 0.84
GRO_W, GRO_D = 0.82, 1.60
FIT = 0.00
RIB_W, RIB_H = GRO_W - 2 * FIT, GRO_D - 0.40


def yz_frame(s0, s1, x0, x1):
    """Ring between two insets from the outer y/z faces, x0..x1 long; rounded like the shell."""
    def ring(s):
        b = box(x0, x1, OY0 + s, OY1 - s, OZ0 + s, OZ1 - s)
        r = RV - s
        if r < 1e-6:
            return b
        es = [e for e in b.Edges if abs(e.tangentAt(e.FirstParameter).x) > 0.99]
        return b.makeFillet(r, es)
    return ring(s0).cut(ring(s1))


def x_face(shp, x):
    """Planar face lying in the outer end plane x = const."""
    for f in shp.Faces:
        bb = f.BoundBox
        if abs(bb.XMin - x) < 1e-6 and abs(bb.XMax - x) < 1e-6 and bb.XLength < 1e-6:
            return f
    raise RuntimeError("no face at x=%.3f" % x)


def shell_fillets(shp, x_end, rim_r=RH):
    """R2 on the 4 outer X-parallel corner edges; rim_r > 0 also rounds the free end face rim.
    The lid passes rim_r=0: OCCT refuses both fillet and chamfer on the nose-root ring, so that
    crease is kept as one clean sharp line (the lofted nose already removes the crater)."""
    # pin the 4 outer corner edges by position: the tongue/recess adds inner X-parallel edges
    es = [e for e in shp.Edges
          if abs(e.tangentAt(e.FirstParameter).x) > 0.99 and e.Length > 1.0
          and (abs(e.BoundBox.YMin - OY0) < 1e-6 or abs(e.BoundBox.YMax - OY1) < 1e-6)
          and (abs(e.BoundBox.ZMin - OZ0) < 1e-6 or abs(e.BoundBox.ZMax - OZ1) < 1e-6)]
    s = shp.makeFillet(RV, es) if es else shp
    # rim: only the outer perimeter of the end face. Internal rims (tongue root, recess step)
    # keep their own smaller blends so the fillets do not overlap. rim_r=0 skips it (lofted nose
    # root ring refuses OCCT fillet/chamfer; it stays a single clean crease).
    if not rim_r:
        return s
    rim = [e for e in s.Edges if e.BoundBox.XLength < 1e-6
           and abs(e.BoundBox.XMin - x_end) < 1e-6
           and (e.BoundBox.YLength > 1.0 or e.BoundBox.ZLength > 1.0)
           and (abs(e.BoundBox.YMin - OY0) < 1e-6 or abs(e.BoundBox.YMax - OY1) < 1e-6
                or abs(e.BoundBox.ZMin - OZ0) < 1e-6 or abs(e.BoundBox.ZMax - OZ1) < 1e-6)]
    assert rim, "no rim edges at x=%s" % x_end
    return s.makeFillet(rim_r, rim)


def joint_cuts():
    """(lid groove, body rib)."""
    g0, g1 = LAND, LAND + GRO_W
    groove = yz_frame(g0, g1, PX - GRO_D, PX + 0.06)
    rib = yz_frame(g0 + FIT, g1 - FIT, PX - RIB_H, PX + 0.06)
    return groove, rib


doc_name = "PCB1_enclosure"
if doc_name in App.listDocuments():
    App.closeDocument(doc_name)
doc = App.newDocument(doc_name)

HX0, HX1 = OX0 - 0.06, X0 + 0.05
hole = port_solid(HX0, HX1, GAP)
# shoulder relief: end slab minus the smooth nose loft -> no flat crater shoulder, no hard lips
def rrect(x, y0, y1, z0, z1, r):
    # rounded-rect wire in plane x (same 8-edge topology as the shell section, so lofts match)
    k = 0.70710678 * r
    V = lambda y, z: App.Vector(x, y, z)
    e = [Part.makeLine(V(y0 + r, z0), V(y1 - r, z0)),
         Part.Arc(V(y1 - r, z0), V(y1 - r + k, z0 + r - k), V(y1, z0 + r)).toShape(),
         Part.makeLine(V(y1, z0 + r), V(y1, z1 - r)),
         Part.Arc(V(y1, z1 - r), V(y1 - r + k, z1 - r + k), V(y1 - r, z1)).toShape(),
         Part.makeLine(V(y1 - r, z1), V(y0 + r, z1)),
         Part.Arc(V(y0 + r, z1), V(y0 + r - k, z1 - r + k), V(y0, z1 - r)).toShape(),
         Part.makeLine(V(y0, z1 - r), V(y0, z0 + r)),
         Part.Arc(V(y0, z0 + r), V(y0 + r - k, z0 + r - k), V(y0 + r, z0)).toShape()]
    return Part.Wire(e)


# Smooth nose: loft the tongue stadium up to the full shell section at the bond-groove start, so
# the groove sits on full section and the outward growth stays <=45 deg from the +X print axis
# (parting-UP, support-free). No flat recessed shoulder, no crater lips.
NOSE_END = PX - GRO_D
# loft end is outset 0.1 past the shell so it cuts the corner cleanly: an exactly-coincident end
# section leaves zero-area degenerate sliver faces at the nose root (the right-view spike)
nose = Part.makeLoft([rrect(OX0, TCY - TONGUE_W / 2, TCY + TONGUE_W / 2,
                            AXZ - TONGUE_H / 2, AXZ + TONGUE_H / 2, TONGUE_H / 2 - 0.02),
                      rrect(NOSE_END, OY0 - 0.1, OY1 + 0.1, OZ0 - 0.1, OZ1 + 0.1, RV + 0.1)], True)
shoulder = box(OX0 - 0.1, NOSE_END, OY0 - 1, OY1 + 1, OZ0 - 1, OZ1 + 1).cut(nose)
win = Part.makeCylinder(WIN["r"], OY1 + 0.11 - (Y1 - 0.01),
                        App.Vector(WIN["c"][0], Y1 - 0.01, WIN["c"][1]), App.Vector(0, 1, 0))
# rounded-rect section (r=0.3): no 90 deg vertical corners (FDM corner stress). Parting-UP print:
# a flat front ceiling/floor would be a 90 deg overhang, so the front closes with a steep z-taper
# (y stays full width: the flare front face is full width). Mouth left just tall enough for it.
_fx0, _fx1 = FLARE["x"]
_fy0, _fy1 = FLARE["y"]
_fz0, _fz1 = FLARE["z"]
_mz0, _mz1 = -1.00, 0.20          # mouth clears the flare front face (-0.74..-0.06)
_run = (_fz1 - _mz1) / 4.0        # 76 deg taper: flare z grows faster than 45 deg near the front
relief = Part.makeLoft([rrect(_fx0, _fy0, _fy1, _mz0, _mz1, 0.3),
                        rrect(_fx0 + _run, _fy0, _fy1, _fz0, _fz1, 0.3)], True)
relief = relief.fuse(prism_x(_fx0 + _run, _fx1, FLARE["y"], FLARE["z"], 0.3))
# lid front slot: captures the board front edge in y/z (line to line on 16.80 x 0.83) and bottoms
# on the board front face with no x play (1 micron relief only absorbs mesh/caliper datum noise).
# Open at the parting plane so the edge slides in as the lid closes.
lidslot = box(BX0 - 0.001, PX + 0.01, BY0, BY1, BZ0, BZ1)
# body main chamber, then the tail socket: a rectangular tube at +X whose bore is the tail
# cross-section (16.80 x 1.75), ENG long, bottomed GAPX behind the board rear edge. The material
# above/below/beside it forms the socket lips that clamp the tail.
main = box(PX - 0.01, BX1 - ENG, Y0, Y1, ZFL - 0.01, ZTOP)
socket = box(BX1 - ENG - 0.01, X1, BY0, BY1, BZ0, BZ0 + TAIL)
# lid upper chamber: with the parting moved back, the shield-can front (x 2.625..) and the button
# plunger fall inside the lid, so it needs full-height clearance above the board from there on
lid_up = box(2.625 - CLEAR, PX + 0.01, Y0, Y1, BZ1 - 0.01, ZTOP)
groove, rib = joint_cuts()

# The nose top taper IS the top slant (no separate chamfer wedge): one continuous surface from the
# tongue front to full height at the groove start, <=45 deg from the +X print axis.
# R2 section corners match the tail (+X) so lid and tail read as one continuous rounded shell.
lid = shell_fillets(box(OX0, PX, OY0, OY1, OZ0, OZ1).cut(shoulder), NOSE_END, rim_r=0)


def blend(shape, edges, r):
    # remove 90 deg edge rings; fall back to a chamfer if OCCT refuses the fillet
    if not edges:
        return shape
    try:
        return shape.makeFillet(r, edges)
    except Exception:
        try:
            return shape.makeChamfer(r, edges)
        except Exception:
            return shape


# generous round on the tongue front rim (stadium arcs already full)
es = [e for e in lid.Edges
      if e.BoundBox.XLength < 1e-6 and abs(e.BoundBox.XMin - OX0) < 1e-6
      and e.BoundBox.YMin > TCY - TONGUE_W / 2 - 0.01
      and e.BoundBox.YMax < TCY + TONGUE_W / 2 + 0.01
      and e.BoundBox.ZMin > AXZ - TONGUE_H / 2 - 0.01
      and e.BoundBox.ZMax < AXZ + TONGUE_H / 2 + 0.01]
lid = blend(lid, es, 0.6)
lid = lid.cut(fuse(hole + [relief, lidslot, lid_up, groove]))
# cove the relief mouth ring so the small remaining front face has no 90 deg edge
mouth = [e for e in lid.Edges if e.BoundBox.XLength < 1e-6 and abs(e.BoundBox.XMin - _fx0) < 1e-6
         and e.BoundBox.ZMin > _mz0 - 0.01 and e.BoundBox.ZMax < _mz1 + 0.01]
lid = blend(lid, mouth, 0.1)   # < the 0.123 y gap so the cove cannot touch the flare front corners
body = shell_fillets(box(PX, OX1, OY0, OY1, OZ0, OZ1), OX1)
body = body.cut(main.fuse([socket, win])).fuse([rib])

for nm, shp in (("Body", body), ("Lid", lid)):
    o = doc.getObject(nm) or doc.addObject("Part::Feature", nm)
    o.Shape = shp

ref = doc.getObject("PCB_ref") or doc.addObject("Mesh::Feature", "PCB_ref")
ref.Mesh = Mesh.Mesh(OBJ)
doc.recompute()
for o in doc.Objects:
    try:
        o.ViewObject.Transparency = 70
    except Exception:
        pass

# static interference check: PCB mesh vertices inside either shell solid
bad = {"Body": [], "Lid": []}
pts = [(q.x, q.y, q.z) for q in ref.Mesh.Points]
for x, y, z in pts:
    if X0 < x < X1 and Y0 < y < Y1 and ZFL < z < ZTOP:
        continue          # inside the cavity void, cannot touch either wall
    v = App.Vector(x, y, z)
    for nm, shp in (("Body", body), ("Lid", lid)):
        if shp.isInside(v, 1e-6, True):
            bad[nm].append((x, y, z))
print("interference verts: Body=%d Lid=%d" % (len(bad["Body"]), len(bad["Lid"])))
for nm, bp in bad.items():
    if bp:
        print("  %s x %.2f..%.2f y %.2f..%.2f z %.2f..%.2f  e.g. %s" % (
            nm, min(q[0] for q in bp), max(q[0] for q in bp),
            min(q[1] for q in bp), max(q[1] for q in bp),
            min(q[2] for q in bp), max(q[2] for q in bp),
            ["%.2f/%.2f/%.2f" % q for q in bp[:6]]))
# the envelope skip hides the slot walls: test the front region against the lid and the tail
# region against the body directly (mesh board is a hair under the caliper board, so both clear)
front = [q for q in pts if q[0] < PX and lid.isInside(App.Vector(*q), 1e-6, False)]
tail = [q for q in pts if q[0] > BX1 - ENG and body.isInside(App.Vector(*q), 1e-6, False)]
print("front verts inside lid: %d | tail verts inside body: %d" % (len(front), len(tail)))
# assembly: the halves must not overlap (rib lives in the groove void), rib must fit the groove
ov, rg = lid.common(body).Volume, rib.cut(groove).Volume
print("assembly overlap mm3: %.6f | rib outside groove mm3: %.6f" % (ov, rg))
assert ov < 1e-3 and rg < 1e-3, "halves interfere or rib misses the groove"
# nose root must stay solid behind the taper end (no through-hole into the flare seat)
assert lid.isInside(App.Vector(NOSE_END + 0.1, TCY, -2.6), 1e-6, True), "nose root broken"
# no degenerate zero-area sliver faces (the right-view spike bug)
assert all(f.Area > 1e-6 for f in lid.Faces), "degenerate sliver face on lid"
# parting-UP support-free: nose outward growth <=45 deg from the +X print axis, and the nose must
# already be >=0.8 wall thick at the board front edge so the board corners are not exposed
nose_slope = ((OY1 - OY0) / 2 - TONGUE_W / 2) / (NOSE_END - OX0)
assert nose_slope <= 1.0, "nose overhang >45 deg (needs support)"
assert TONGUE_W / 2 + nose_slope * (BX0 - OX0) >= BY1 + 0.84, "nose outer wall <0.84 at board front"
# parting must stay ahead of the button pinhole so the hole is wholly in the body
assert PX < WIN["c"][0] - WIN["r"], "parting crosses the button pinhole"
# slide-in: board + flare sections pass the rim opening; shield can stops before the socket lip
assert Y0 < BY0 and BY1 < Y1 and ZFL - 0.01 < -2.00 and 1.20 < ZTOP
assert 21.840 < BX1 - ENG and 0.92 < ZTOP
print("joint: land %.2f | groove %.2f x %.2f (lid), rib %.2f x %.2f (body), gap %.2f/side"
      % (LAND, GRO_W, GRO_D, RIB_W, RIB_H, FIT))
print("parting x %.3f | lid %.2f long, body %.2f long" % (PX, PX - OX0, OX1 - PX))
print("board %.2f x %.2f x %.2f | tail socket %.2f tall x %.2f eng | lid slot %.2f tall"
      % (BL, BW, BT, TAIL, ENG, BT))
print("outer size %.2f x %.2f x %.2f mm" % (OX1 - OX0, OY1 - OY0, OZ1 - OZ0))
for (yy, zz, r) in port(GAP):
    print("port y %7.3f..%7.3f z %7.3f..%7.3f r %.2f" % (yy[0], yy[1], zz[0], zz[1], r))
print("button pinhole dia %.2f at x %.3f z %.3f" % (2 * WIN["r"], WIN["c"][0], WIN["c"][1]))
# printability, lid PARTING-UP (bed = Type-C face, grows +X), NO support: outer walls may grow
# outward at most 45 deg from the print axis; cavity ceilings likewise. Port bore stays vertical.
relief_from_vert = math.degrees(math.atan(_run / (_fz1 - _mz1)))
assert nose_slope <= 1.0 and relief_from_vert <= 45, "overhang >45 deg (needs support)"
print("lid parting-UP no-support: nose %.1f deg, relief ceiling %.1f deg from print axis (<=45)"
      % (math.degrees(math.atan(nose_slope)), relief_from_vert))
print("volumes mm3: body %.0f lid %.0f" % (body.Volume, lid.Volume))
print("solids: body %d lid %d" % (len(body.Solids), len(lid.Solids)))

os.makedirs(OUT, exist_ok=True)
doc.saveAs(os.path.join(OUT, "PCB1_enclosure.FCStd"))
Mesh.export([doc.getObject("Body"), doc.getObject("Lid")],
            os.path.join(OUT, "PCB1_enclosure.3mf"))
print("saved to", OUT)
