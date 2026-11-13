"""Build PCB1 plug-in enclosure for FDM on a 0.4 nozzle: base + bonded lid, no screw, no snap.
Every printed feature is >= 0.40 (one line); board locators and the bonded stop are modelled at 0.05
per side, which the measured 0.4-nozzle kerf (~0.03/side) opens to about 0.08 assembled. Coords in PCB space (mm)."""
import FreeCAD as App
import Part, Mesh, os

OUT = "/Users/lw/work/vst_lan_mic/enclosure"
OBJ = "/Users/lw/work/vst_lan_mic/enclosure/3D_PCB1_2026-11-09.obj"

# --- measured envelope (mm) ---
# PCB   x 0.015..28.414  y -16.061..-0.010  z -0.800..0.000 (board was 1.0 thick, now 0.8)
# USB-C plug (part_02): metal tube y -12.173..-3.923 z -1.515..0.775 (axis z -0.43), tip at
#   x -10.485. The user wants 7.40 mm of the plug proud of the end face, so the shell ends at
#   x -3.085 (WRAP = 1.20 over the tube). Behind the tube its shell body flares in z to
#   -2.00..1.13 while staying inside the bore in y (-12.10..-4.00), and reaches forward to
#   x -2.923. It still fits the stadium bore up to x -2.85, so the bore stays a pure stadium for
#   the outer 0.24 of the wall and opens to a slot behind it (see "relief").
# Button (part_00) plunger tip past board edge: x 3.768..5.714 y 0.449 z 0.444..1.282
# Tallest part = shield can (part_01) z 2.400. Nothing but the button passes y = -0.010.
CLEAR = 0.45          # board edge to cavity wall, per side: a 0.40 locating rib + 0.05 model gap
LOC = 0.40            # locating rib width in plan (one line), welded to the cavity wall below PART
T = 2.00              # wall thickness, five 0.4 passes
PART = 0.05           # parting plane, just above board top
RV, RH = 2.00, 1.50   # outer corner / outer rim fillet radii

X0, X1 = 0.015 - CLEAR, 28.414 + CLEAR       # cavity plan  -0.285 .. 28.714
Y0, Y1 = -16.061 - CLEAR, -0.010 + CLEAR     #                -16.361 .. 0.290
ZFL, ZBB = -2.000 - 0.80, -0.800             # cavity floor (lowest part -2.00) / board bottom
ZTOP = 2.400 + 0.50                          # cavity ceiling
# Derived, not a free knob: the end face sits so the plug stays PROUD mm out of the shell (the user's
# 7.40), whatever CLEAR is. With CLEAR 0.45 this lands on the same x -3.085 as before.
PLUG_TIP = -10.485     # front end of the USB-C metal tube
PROUD = 7.40
WRAP = X0 - T - PLUG_TIP - PROUD             # extra shell length past the cavity on -X
OX0, OX1 = X0 - T - WRAP, X1 + T
OY0, OY1 = Y0 - T, Y1 + T

# --- USB-C port (male plug) in the -X wall ---
# Shell is symmetric about the connector axis, so the port is centred on the end face AND on the
# metal shell. Per the user the plug is exactly 2.40 thick: the opening is cut line to line on that
# height in z -- the printer opened cuts ~0.03 per side in the stop tests, which is the whole z
# clearance and leaves no visible gap above or below the shell. Only y keeps GAP air so the plug can
# slide in. The tube still shoulders on the end face in x.
# HZ keeps both slabs > RH so the bottom/top rim fillets stay fillets (bottom 5.38, top 4.42).
# The wall is T long; the connector flare no longer fits that bore behind x -2.85, so RELIEF opens
# the back of the wall to the flare envelope and the outer 0.23 stays a pure stadium lip.
AXZ = -0.43            # connector shell axis: tube measures z -1.515..0.775 (2.29..2.42 tall)
PH = 2.40              # plug shell thickness per the user: the port matches it line to line
TUBE_Y = (-12.173, -3.923)
GAP = 0.05             # y air per side on the 8.28-wide shell; z is line to line on PH
HZ = 4.90
OZ0, OZ1 = AXZ - HZ, AXZ + HZ
# rear flare of the connector: z -2.000..1.130 (the stadium bore stops at -1.720..0.860) while
# its y -12.10..-4.00 stays inside the bore; opened behind x -2.85 with 0.05 air around the body,
# which keeps the relief 0.05 inside the narrowed bore mouth on each side
FLARE = dict(x=(-2.85, X0 + 1.01), y=(TUBE_Y[0] - 0.05, TUBE_Y[1] + 0.05), z=(ZFL - 0.01, 1.19))

# Button window in the +Y wall (lid side): 2 x 1 mm over the plunger only (measured
# x 3.768..5.714 z 0.444..1.282), centred on it; the switch body stays covered by the wall.
WIN = dict(x=(4.741 - 1.0, 4.741 + 1.0), z=(0.863 - 0.5, 0.863 + 0.5),
           y=(Y1 - 1.0, OY1 + 0.1))


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
    """(yy, zz, r) of the connector silhouette: y grown by g, z line to line on the 2.40 shell."""
    # 0.02 flat: OCCT rejects a fillet radius exactly equal to half the box height
    return [((TUBE_Y[0] - g, TUBE_Y[1] + g),
             (AXZ - PH / 2, AXZ + PH / 2), PH / 2 - 0.02)]


def port_solid(x0, x1, g):
    return [prism_x(x0, x1, yy, zz, r) for yy, zz, r in port(g)]


# --- bonded joint on the parting face, sized for a 0.4 nozzle ---
# s is measured inwards from each outer face, so the wall under the parting plane reads, outside in:
# flat land | locating stop | inner wall. The stop is one ring groove in the base with the lid rib down
# the middle of it: the rib locates the two halves, the land and the groove mouth keep glue off the
# outside, and the rib runs FIT clear of the groove in the model: the real clearance is that FIT plus
# what the printer adds -- it opened the 1.20 groove to a measured 1.26 while the rib prints true, so
# FIT 0.05 assembles at ~0.08 per side: a shade looser than line to line, still no rattle, and the glue
# film gets room. No dam, no bleed holes: the halves close on the land, not on glue.
# LAND + GRO_W + INNER = T, and each of them is a whole number of 0.4 passes. Both halves print face up
# on the parting plane. Corner arcs of every
# frame follow the outer R2 (their radius shrinks by s), so nothing breaks through at the corners and
# the parting plane keeps a flat outside margin with no witness mark.
LAND = 0.40                  # smooth margin from the outer face to the groove mouth (one line)
GRO_W, GRO_D = 1.20, 1.60    # groove in the base: width in plan, depth below the parting plane
FIT = 0.05                   # model gap per side; ~0.08 assembled after the measured groove opening
RIB_W, RIB_H = GRO_W - 2 * FIT, GRO_D - 0.40    # rib 1.10 x 1.20; 0.40 left under it in the groove


def plan_frame(s0, s1, z0, z1):
    """Ring between two insets from the outer faces, z0..z1 tall; rounded like the shell."""
    def ring(s):
        b = box(OX0 + s, OX1 - s, OY0 + s, OY1 - s, z0, z1)
        r = RV - s
        if r < 1e-6:
            return b                     # inset has reached the cavity, whose corners are square
        es = [e for e in b.Edges if abs(e.tangentAt(e.FirstParameter).z) > 0.99]
        return b.makeFillet(r, es)
    return ring(s0).cut(ring(s1))


def outer_edges(shp):
    """The 4 outer vertical corner edges (lines lying on two outer box faces);
    pocket and hole mouths also give short vertical lines, so pin them by position."""
    out = []
    for e in shp.Edges:
        bb = e.BoundBox
        if bb.XLength > 1e-6 or bb.YLength > 1e-6 or abs(bb.ZMin - bb.ZMax) < 1e-6:
            continue
        if min(abs(bb.XMin - OX0), abs(bb.XMin - OX1)) < 1e-6 \
                and min(abs(bb.YMin - OY0), abs(bb.YMin - OY1)) < 1e-6:
            out.append(e)
    assert len(out) == 4, "corner edges found: %d" % len(out)
    return out


def plane_face(shp, z):
    """Planar face lying in the outer top/bottom plane."""
    for f in shp.Faces:
        bb = f.BoundBox
        if abs(bb.ZMin - z) < 1e-6 and abs(bb.ZMax - z) < 1e-6 and bb.ZLength < 1e-6:
            return f
    raise RuntimeError("no face at z=%.3f" % z)


def shell_fillets(shp, *rims):
    """R2 on the 4 vertical corners, R1.5 on the outer rim of each given plane (the free
    face only), so the parting plane keeps a sharp landing and no V groove shows at the
    seam."""
    shp = shp.makeFillet(RV, outer_edges(shp))
    for z in rims:
        shp = shp.makeFillet(RH, plane_face(shp, z).Edges)
    return shp


def joint_cuts(flare):
    """(base groove, lid rib)."""
    g0, g1 = LAND, LAND + GRO_W                 # locating stop: groove in base, rib on the lid
    rib = plan_frame(g0 + FIT, g1 - FIT, PART - RIB_H, PART + 0.06).cut(flare)
    return plan_frame(g0, g1, PART - GRO_D, PART + 0.06), rib


doc_name = "PCB1_enclosure"
if doc_name in App.listDocuments():
    App.closeDocument(doc_name)
doc = App.newDocument(doc_name)

HX0, HX1 = OX0 - 0.06, X0 + 0.05            # bore prism: through the -X wall (+ into the chamber)
hole = port_solid(HX0, HX1, GAP)
win = box(WIN["x"][0], WIN["x"][1], WIN["y"][0], WIN["y"][1], WIN["z"][0], WIN["z"][1])
# board step: floor is 1.0mm narrower in plan than the cavity, so the ledge top is the
# board bottom plane and the board drops onto it
cav_low = box(X0 + 1.0, X1 - 1.0, Y0 + 1.0, Y1 - 1.0, ZFL - 0.01, ZBB)
# slot for the connector rear flare, and with it the relief under the -X ledge. It starts at
# x -2.85, inside the wall, so the outer face keeps the clean stadium and nothing squares it off.
relief = box(*FLARE["x"], *FLARE["y"], *FLARE["z"])
cav_up = box(X0 - WRAP, X1, Y0, Y1, ZBB - 0.01, PART + 0.06)
cav_top = box(X0 - WRAP, X1, Y0, Y1, PART, ZTOP)
# board edge locators: a rib LOC wide against the cavity wall on all four sides, standing on the
# ledge from the board bottom to the board top, so the PCB cannot drift in x-y; its inner face is
# CLEAR - LOC off the board, which a shrunk print still clears. The -X run sits in the port relief
# and is cut away there.
band = box(X0, X1, Y0, Y1, ZBB, 0.0).cut(
    box(X0 + LOC, X1 - LOC, Y0 + LOC, Y1 - LOC, ZBB - 0.01, 0.01)).cut(relief)
# The port relief reaches the parting plane, so the stop is broken where it crosses: on the -X wall
# the ring stops short of the connector (y -12.37..-3.72) and the two ends
# of the bead have to be pinched off there. Glue bead: run it along the rib, the 0.30 gaps on both
# sides hold it, the land and the groove mouth keep it off the outside; the joint is permanent.
groove, rib = joint_cuts(relief)

base = shell_fillets(box(OX0, OX1, OY0, OY1, OZ0, PART)
                     .cut(cav_low.fuse([cav_up, relief, groove] + hole)), OZ0).fuse(band)
lid = shell_fillets(box(OX0, OX1, OY0, OY1, PART, OZ1)
                    .cut(cav_up.fuse([cav_top, win, relief] + hole)), OZ1).fuse(rib)

for nm, shp in (("Base", base), ("Lid", lid)):
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
bad = {"Base": [], "Lid": []}
pts = [(q.x, q.y, q.z) for q in ref.Mesh.Points]
for x, y, z in pts:
    if X0 < x < X1 and Y0 < y < Y1 and ZFL < z < ZTOP:
        continue          # inside the cavity void, cannot touch either wall
    v = App.Vector(x, y, z)
    for nm, shp in (("Base", base), ("Lid", lid)):
        if shp.isInside(v, 1e-6, True):
            bad[nm].append((x, y, z))
print("interference verts: Base=%d Lid=%d" % (len(bad["Base"]), len(bad["Lid"])))
for nm, bp in bad.items():
    if bp:
        print("  %s x %.2f..%.2f y %.2f..%.2f z %.2f..%.2f  e.g. %s" % (
            nm, min(q[0] for q in bp), max(q[0] for q in bp),
            min(q[1] for q in bp), max(q[1] for q in bp),
            min(q[2] for q in bp), max(q[2] for q in bp),
            ["%.2f/%.2f/%.2f" % q for q in bp[:6]]))
# the locators live inside the cavity, so the skip above never sees them: test them directly
inb = [q for q in pts if band.isInside(App.Vector(*q), 1e-6, True)]
print("board verts inside locator band: %d" % len(inb))
print("joint: land %.2f | groove %.2f x %.2f, rib %.2f x %.2f, model gap %.2f per side (print adds"
      " ~0.03) | inner wall %.2f"
      % (LAND, GRO_W, GRO_D, RIB_W, RIB_H, FIT, T - (LAND + GRO_W)))
print("outer size %.2f x %.2f x %.2f mm" % (OX1 - OX0, OY1 - OY0, OZ1 - OZ0))
for (yy, zz, r) in port(GAP):
    print("port y %7.3f..%7.3f z %7.3f..%7.3f r %.2f" % (yy[0], yy[1], zz[0], zz[1], r))
print("port z centre %.3f, axis %.3f, end-face centre %.3f" % (AXZ, AXZ, (OZ0 + OZ1) / 2))
print("button window x %.3f..%.3f z %.3f..%.3f = %.2f x %.2f mm"
      % (WIN["x"][0], WIN["x"][1], WIN["z"][0], WIN["z"][1],
         WIN["x"][1] - WIN["x"][0], WIN["z"][1] - WIN["z"][0]))
print("port wall x %.3f..%.3f (%.2f thick), plug proud %.3f, flare relief from x %.3f"
      % (OX0, X0 - WRAP, T, OX0 - PLUG_TIP, FLARE["x"][0]))
print("volumes mm3: base %.0f lid %.0f" % (base.Volume, lid.Volume))
print("solids: base %d lid %d" % (len(base.Solids), len(lid.Solids)))

os.makedirs(OUT, exist_ok=True)
doc.saveAs(os.path.join(OUT, "PCB1_enclosure.FCStd"))
print("saved to", OUT)
