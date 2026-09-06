#!/usr/bin/env python3
"""
Generate the eight moon-phase icons plus the moon-behind-cloud icon as LVGL I1
(1bpp) images.

Run:    python3 tools/gen_wx_moon.py [--sheet /path/to/contact_sheet.png]
Writes: main/wx_moon_frames.c

WHY I1 AND NOT A1
A1 is the obvious 1bpp choice and it is a trap. LV_COLOR_FORMAT_A1 is a valid
enum in every LVGL build, but the 9.5 SOFTWARE renderer has no blitter for it --
grep the tree and A1 appears only under src/draw/nema_gfx/ and src/draw/vg_lite/,
hardware accelerators this ESP32-S3 does not have. An A1 image compiles clean,
links clean, and draws NOTHING; the only trace is a runtime
LV_LOG_WARN("Not supported source color format").

I1 is the 1bpp format the software renderer does implement
(LV_DRAW_SW_SUPPORT_I1, default y).

THE 8 PALETTE BYTES ARE MANDATORY EVEN THOUGH NOTHING READS THEM
lv_draw_buf_goto_xy() unconditionally skips
LV_COLOR_INDEXED_PALETTE_SIZE(cf) * sizeof(lv_color32_t) bytes before the
pixels. For I1 that is 2 * 4 = 8. Omit them and every frame renders shifted by
8 bytes -- two thirds of a row of garbage at this width. data_size includes
them.

I1 is INDEXED, not alpha, but lv_draw_sw_blend_to_rgb565.c does
`chan_val = get_bit(src_buf_i1, src_x) * 255` and pushes that straight through
l8_to_rgb565. A set bit is therefore OPAQUE WHITE and a clear bit OPAQUE BLACK,
which is exactly a black-and-white comic panel and needs no recolour.

Bit order is MSB-first, because that is what the blitter's get_bit does:
	(buf[i / 8] >> (7 - (i % 8))) & 1

FOOTPRINT
	stride = (96 + 7) // 8      =    12 bytes/row
	bitmap = 12 * 96            = 1,152 bytes/image
	image  = 1,152 + 8 palette  = 1,160 bytes
	total  = 1,160 * 9          = 10,440 bytes (10.2 KiB)

THE GEOMETRY THAT MAKES OR BREAKS THIS: THE TERMINATOR IS AN ELLIPSE ARC
The lit/unlit boundary is the projection of the day/night great circle onto the
disc, which is half an ellipse -- NOT a straight chord and NOT a circular arc.
A crescent cut by a straight chord is a pac-man; a crescent cut by a circle of
the same radius is a fingernail with two hard points and no belly.

The construction here is exact and needs no special cases. Take the LIT half of
the disc contour and scale it horizontally about the disc centre by

	k = cos(2*pi*f)          f = phase fraction, 0 = new, 0.5 = full

That scaled curve IS the terminator:
	f = 0     k = +1   terminator lands on the lit limb        -> zero lit area
	f = 0.125 k = +0.71 terminator inside the lit half         -> crescent
	f = 0.25  k =  0   terminator collapses to the centre line -> half lit
	f = 0.375 k = -0.71 terminator crosses onto the dark half  -> gibbous
	f = 0.5   k = -1   terminator lands on the dark limb       -> fully lit
and because the scale is about x = cx, the two pole points have x - cx = 0 and
are therefore FIXED. The terminator meets the limb exactly at the cusps with no
seam to clean up, whatever the wobble did to the outline.

NORTHERN HEMISPHERE ORIENTATION
Waxing (f < 0.5) is lit on the RIGHT limb, waning (f > 0.5) on the LEFT. That
is the one thing on this panel any user can check by walking outside, so it is
a single flag -- `side` -- applied once, not a per-phase table that can rot.
vaultweather.h records that this is wrong south of the equator and that the
device is configured for Evansville, Indiana.

THE INK IS THE BACKGROUND
Set bits are the drawing. The black between the unlit-limb arc and the lit mass
IS the heavy comic outline. Nothing is stroked white except that arc, which is
the dark limb and has nothing behind it to cut into.

SHAPE RULES CARRIED OVER FROM gen_wx_icons.py
  * No true circles. Every closed contour is perturbed by two summed sine
	harmonics -- 1.0px + 0.6px on the disc, 20% + 7% on the maria. The
	amplitudes are per-shape and the reason is in disc_pts() and _blob(): a
	fraction that is right at r=11 is a deformity at r=34 and a countable
	number of lobes at r=7.
  * Everything is rasterised into an 8-bit 384x384 buffer and box-downsampled to
	96x96 before thresholding, for the same reason gen_wx_icons.py does it: a
	2px ink gap drawn natively into a 1-bit canvas stair-steps badly enough to
	break the terminator into a staircase.

WHY THE CLOUD IS IMPORTED RATHER THAN REDRAWN
wx_moon_cloud_frame has to sit in a rotation next to PARTLY and CLOUDY. A cloud
redrawn from the same description but not the same code drifts, so cloud()
comes from gen_wx_icons directly, with PARTLY's own seed0, scale and base_y,
and the import is asserted against its W/H/SS so a divergence is a loud failure
rather than a subtly wrong icon.

WHAT THE EARLIER PASSES GOT WRONG
Recorded rather than quietly fixed. None of it was visible before the frames
were rendered to a contact sheet and looked at, and the arithmetic error in
pass 2 was not visible on the sheet either -- it had to be derived.

PASS 1 reused gen_wx_icons' lobe_pts settings for the disc unchanged
(wob=0.055, harmonics 3 and 5). Those are tuned for r=11 cloud lobes; at r=34
the 3-fold alone is 1.9px and every phase rendered as a rounded triangle. Eight
eggs. See disc_pts().

PASS 1 outlined the whole disc with a closed white ring on every phase. On the
quarters that is exactly right. On the crescents it is fatal -- the ring's
lit-side half lies under the lit crescent, the two fuse into one closed outline
with a fat right edge, and both crescent tiles were unrecognisable as moons.
The ring is now an arc over the dark limb only. See unlit_arc().

PASS 2's arc trim was 7% of the half-limb on every phase, from an arithmetic
error: the gap between the dark limb and the terminator was taken as d*|1-k|
when it is d*|1+k|. For a crescent (k=+0.71) that happens to give the right
answer to within a factor; for a gibbous (k=-0.71) it overstates the gap by
6x. The real gap 7% in from the cusp is 2.2px, so on both gibbous tiles the
1.5px arc welded onto the lit mass at both ends, closing a dark lens and
leaving a stray white speck. The trim is now solved from the gap equation.

PASS 2 drew the maria with lobe_pts at wob=0.09. Its leading harmonic is
3-fold, and a 3-fold deformation of a 7px blob is a countable number of lobes:
every mare on the sheet was a little heart or clover. _blob() now leads with a
2-fold at a random angle, which is elongation rather than lobes.

PASS 3's two left-hand maria sat within 3px of the disc centre line, so the
fit test rejected both at LAST QUARTER and that tile rendered as a featureless
white half-disc -- the exact failure FULL MOON's craters exist to prevent.
They were moved outboard; nothing about the fit test changed.
"""
import argparse
import math
import os
import random
import sys

from PIL import Image, ImageDraw

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_wx_icons as wxi          # cloud() -- see docstring

W = H = 96
SS = 4                              # supersample factor; see docstring
TAU = 2.0 * math.pi
N_PHASES = 8                        # MUST equal WX_MOON_PHASES in vaultweather.h

# cloud() draws through gen_wx_icons' own module-level geometry constants, not
# through this file's. If that file ever moves off 96x96 or off 4x
# supersampling, the cloud would be drawn at a different scale into this canvas
# and the only symptom would be a cloud that no longer matches PARTLY.
if (wxi.W, wxi.H, wxi.SS) != (W, H, SS):
	raise SystemExit("gen_wx_icons geometry changed (%dx%d @%dx); "
	                 "gen_wx_moon.py shares its canvas and must be updated"
	                 % (wxi.W, wxi.H, wxi.SS))

INK = 2.4                           # gen_wx_icons.INK, same pen for the same set

R_MOON = 34.0                       # 34 + 1.6px wobble + 1.3px arc = 37 < 48
CX = CY = 48.0

PHASE_NAMES = ["NEW MOON", "WAX CRESCENT", "FIRST QTR", "WAX GIBBOUS",
               "FULL MOON", "WAN GIBBOUS", "LAST QTR", "WAN CRESCENT"]

# Which limb the sheet expects to be lit, so the contact sheet states the claim
# rather than leaving it to be eyeballed.
PHASE_LIT = ["--", "R", "R", "R", "both", "L", "L", "L"]


# ---------------------------------------------------------------------------
# primitives -- all coordinates are in 96-space and scaled on the way out
# ---------------------------------------------------------------------------

def _px(pts):
	return [(x * SS, y * SS) for (x, y) in pts]


def _ink_w(ink):
	return max(1, int(round(ink * SS)))


def disc_pts(cx, cy, r, seed, wob=0.030, squash=0.978, n=192):
	"""The moon's limb: a disc that is deliberately not a circle.

	n must be divisible by 4 and theta starts at -pi/2, so pts[0] is the top
	pole, pts[n/2] is the bottom pole, pts[0:n/2+1] is exactly the right half
	and pts[n/2:] + pts[0] is exactly the left half. lit_polygon() and the
	unlit-limb arc both rely on that split being exact -- a half taken by
	filtering on cos(theta) > 0 would miss the pole point by half a step and
	leave a notch at each cusp.

	HARMONICS 3 AND 7, NOT 3 AND 5, AND AT A THIRD OF THE CLOUD'S AMPLITUDE.
	PASS 1 used gen_wx_icons' lobe_pts settings unchanged (wob=0.055, 3rd and
	5th). Those are tuned for r=11 cloud lobes and an r=18 sun disc, where 5.5%
	is one pixel. At r=34 the same fraction is 1.9px on the 3rd harmonic alone
	and every phase rendered as a rounded triangle -- an egg, not a moon. The
	moon is the one shape in this icon set the eye knows is round, so the
	wobble has to be visible as an inked line and invisible as a distortion:
	1.0px on a 3-fold plus 0.6px on a 7-fold. The 7-fold carries most of the
	"hand-drawn" read at a fraction of the shape distortion the 3-fold costs.
	"""
	rnd = random.Random(seed)
	p1, p2 = (rnd.uniform(0, TAU) for _ in range(2))
	pts = []
	for i in range(n):
		th = -math.pi / 2.0 + i * TAU / n
		rr = r * (1.0
		          + wob * math.sin(3 * th + p1)
		          + wob * 0.60 * math.sin(7 * th + p2))
		pts.append((cx + rr * math.cos(th), cy + rr * math.sin(th) * squash))
	return pts


def _halves(contour):
	"""(lit-side-right half, lit-side-left half), each running top pole ->
	bottom pole. Both include both poles, so any curve built from one of them
	starts and ends exactly on the other's endpoints."""
	h = len(contour) // 2
	right = contour[0:h + 1]
	left = (contour[h:] + [contour[0]])[::-1]
	return right, left


def lit_polygon(cx, contour, k, side, seed=0):
	"""Lit limb out, terminator back. See the docstring for why k = cos(2*pi*f)
	scaled about cx is the terminator and not an approximation of one.

	THE BOW. At first and last quarter k is exactly 0 and the terminator is a
	dead-straight vertical line -- geometrically right, and the only
	ruler-drawn edge in the whole icon set. PASS 1 shipped it and the two
	quarter tiles read as a UI progress bar sitting inside a hand-inked disc.
	`bow` adds up to ~0.9px of sideways wander, enveloped by sin(pi*u) so it
	vanishes at both cusps and the terminator still meets the limb exactly.
	0.9px against a 34px radius is 2.6% -- an inked line at 96px, and far below
	the 3.7-day width of a phase bucket, so it cannot misread the phase.
	"""
	right, left = _halves(contour)
	limb = right if side > 0 else left
	rnd = random.Random(seed + 91)
	ph = rnd.uniform(0, TAU)
	n = len(limb)
	term = []
	for j in range(n - 1, -1, -1):
		x, y = limb[j]
		u = j / (n - 1.0)
		env = math.sin(math.pi * u)
		bow = env * (0.90 * math.sin(math.pi * u + ph)
		             + 0.35 * math.sin(2 * math.pi * u + ph * 1.7))
		term.append((cx + k * (x - cx) + bow, y))
	return limb + term


# The unlit-limb arc has to fit in the black between the dark limb and the
# terminator, and that gap closes to nothing at both cusps. How fast it closes
# depends entirely on the phase:
#
#	dark limb at   cx - side*d          d = r*|cos(theta)|, 0 at the cusps
#	terminator at  cx + k*side*d
#	gap            = d * |1 + k|
#
# k = -0.707 (gibbous) gives 0.29*d -- the gap is a THIRD of what the same
# geometry gives a crescent at k = +0.707 (1.71*d). PASS 2 used one flat 7%
# trim for every phase, which is right for a crescent and nowhere near enough
# for a gibbous: at 7% in, d is 7.5px and the gap is 2.2px, so the 1.5px arc
# welded itself onto the lit mass at both ends and the two gibbous tiles
# rendered with a closed dark lens plus a stray white speck below it.
ARC_CLEAR = 4.6      # arc width + a black margin on each side, in 96-space


def unlit_arc(contour, r, k, side, width):
	"""The part of the limb that bounds the UNLIT region, stopped short of both
	cusps at the point where the dark sliver gets too narrow to hold the line.

	Returns [] when there is nowhere for the arc to go.

	WHY THIS IS AN ARC AND NOT A RING. PASS 1 drew the whole contour as a
	closed white ring on every phase. On the quarters that reads beautifully --
	the dark half is outlined, exactly the comic device. On the crescents it is
	fatal: the ring's lit-side half lies underneath the lit crescent, so ring
	and crescent fuse into one closed outline with a fat right edge and the
	tile reads as an egg, not a moon. Both crescents on the pass 1 sheet were
	unrecognisable. Drawing only the dark half leaves the crescent open, and
	the trim keeps the arc away from the cusps, where it would weld back on and
	bring the egg with it.
	"""
	right, left = _halves(contour)
	arc = left if side > 0 else right
	n = len(arc)

	# theta runs -pi/2 .. +pi/2 across the half, so d = r*sin(pi*u).
	need = (width + ARC_CLEAR) / max(1e-6, r * abs(1.0 + k))
	if need >= 1.0:
		return []
	u = math.asin(min(1.0, need)) / math.pi
	if u > 0.40:                       # nothing but cusp left; drop the arc
		return []
	lo = int(n * u)
	return arc[lo:n - lo]


def _fits(mask, x, y, rad, n=24):
	"""True if a circle of radius `rad` at (x, y) lies wholly inside `mask`.

	The lit region is simply connected (a disc, a half-disc or a lune), so a
	boundary circle that is entirely inside implies the whole disc is -- no
	interior sampling needed.
	"""
	m = mask.load()
	lim = W * SS - 1
	for i in range(n):
		a = i * TAU / n
		px = int(round((x + rad * math.cos(a)) * SS))
		py = int(round((y + rad * math.sin(a)) * SS))
		if not (0 <= px <= lim and 0 <= py <= lim) or m[px, py] < 128:
			return False
	return True


# ---------------------------------------------------------------------------
# the moon
# ---------------------------------------------------------------------------

# (dx, dy, r, seed) at R_MOON = 34, scaled with the disc.
#
# Fixed for every phase, because the moon is tidally locked and shows the same
# face: a crater that moved between FIRST QTR and FULL would be a different
# moon. Positions are loosely the near-side maria -- Imbrium upper left,
# Serenitatis/Tranquillitatis across the middle, Nubium low -- which is why
# nothing here is mirror-symmetric.
#
# The two left-hand entries are held out past x = -11. PASS 3 had them at -11.5
# and -6.5, which put their fit margins across the centre line, so BOTH were
# rejected at LAST QUARTER and that tile came out a featureless white half-disc.
# Anything meant to survive a quarter phase has to clear the terminator by its
# own radius plus the margin.
MARIA = [
	(-13.5, -9.5, 8.0, 501),
	(12.5, -3.5, 6.4, 502),
	(8.0, -18.0, 3.8, 503),
	(-11.5, 13.5, 5.8, 504),
	(7.0, 16.5, 4.2, 505),
]

# Four specks, deliberately much smaller than the smallest crater and set on a
# loose diagonal so they do not land where a face's features would.
EARTHSHINE = [
	(-9.0, -7.0, 2.8, 521),
	(-0.5, 1.5, 2.3, 522),
	(8.0, 10.5, 2.0, 523),
	(11.5, -13.0, 1.7, 524),
]


def _blob(d, cx, cy, r, seed, fill, n=64):
	"""A mare: an off-round dark patch.

	NOT gen_wx_icons.lobe_pts(). Its first harmonic is 3-fold, which is right
	for a cloud billow and wrong for anything the size of a crater: at wob=0.16
	(PASS 1) and again at wob=0.09 (PASS 2) every mare came out with a notch at
	one end and the sheet was covered in little hearts and clovers. A 3-fold
	deformation on a blob only 6-9px across is a countable number of lobes, and
	the eye counts them.

	The leading harmonic here is 2-fold instead, which is an ellipse at a random
	angle -- elongation, not lobes, which is also what the actual maria look
	like. The 5-fold on top at a third of the amplitude keeps the edge from
	being a drafted ellipse.
	"""
	rnd = random.Random(seed)
	p1, p2 = (rnd.uniform(0, TAU) for _ in range(2))
	pts = []
	for i in range(n):
		th = i * TAU / n
		rr = r * (1.0
		          + 0.20 * math.sin(2 * th + p1)
		          + 0.07 * math.sin(5 * th + p2))
		pts.append((cx + rr * math.cos(th), cy + rr * math.sin(th)))
	d.polygon(_px(pts), fill=fill)


def draw_moon(d, cx, cy, r, f, seed=600, craters=True):
	"""One moon at phase fraction f (0 = new, 0.5 = full, wraps at 1).

	Draw order is fill -> craters -> unlit-limb arc. The arc goes last so the
	dark limb is one clean unbroken line; a crater drawn over it would put a
	black nick in the silhouette, which reads as damage rather than as a
	crater.
	"""
	f = f % 1.0
	k = math.cos(TAU * f)
	side = 1.0 if f < 0.5 else -1.0        # NORTHERN HEMISPHERE. One flag.
	contour = disc_pts(cx, cy, r, seed)
	s = r / R_MOON                         # crater/mark scale factor
	pen = max(0.62, s)                     # gen_wx_icons' floor: a small icon
	                                       # still needs a visible pen

	lit = (1.0 - math.cos(TAU * f)) / 2.0  # illuminated fraction

	if f == 0.0:
		# A genuinely blank tile reads as a rendering failure, so new moon is
		# the unlit limb -- closed, and the heaviest line in the set because it
		# is carrying the whole icon alone -- plus earthshine specks.
		d.line(_px(contour + [contour[0]]), fill=255,
		       width=_ink_w(2.6 * pen), joint="curve")
		for (dx, dy, mr, ms) in EARTHSHINE:
			_blob(d, cx + dx * s, cy + dy * s, mr * s, ms, 255)
		return

	poly = lit_polygon(cx, contour, k, side, seed)
	mask = Image.new("L", (W * SS, H * SS), 0)
	ImageDraw.Draw(mask).polygon(_px(poly), fill=255)
	d.polygon(_px(poly), fill=255)

	# Craters only where there is enough lit disc to hold one. On a crescent the
	# belly is ~10px wide and a black bite out of it reads as a broken glyph,
	# not as a crater. 0.30 is comfortably above the 0.146 of the two crescent
	# phases and below the 0.5 of the quarters.
	if craters and lit > 0.30:
		for (dx, dy, cr, cs) in MARIA:
			x, y, rr = cx + dx * s, cy + dy * s, cr * s
			# Margin keeps an ink width of white between crater and edge, so a
			# crater can never open onto the limb or the terminator.
			if _fits(mask, x, y, rr + INK * 1.25 * s):
				_blob(d, x, y, rr, cs, 0)

	if lit < 0.985:
		# Floored at 1.3px rather than scaled all the way down with the disc:
		# on the moon-behind-cloud tile r is 19, and 1.5 * s came out at 0.93px,
		# which the 96px threshold rendered as a dashed hairline.
		aw = max(1.3, 1.5 * s)
		arc = unlit_arc(contour, r, k, side, aw)
		if arc:
			d.line(_px(arc), fill=255, width=_ink_w(aw), joint="curve")


def draw_moon_cloud(d):
	"""Moon behind a cloud, for PARTLY at night.

	f = 0.17 rather than a phase index: this icon is phase-independent, and a
	crescent is the only lunar shape that cannot be mistaken for PARTLY's sun
	at a glance. 0.17 gives a ~10px belly at r=19, where the 0.125 of a real
	waxing crescent would give 5.5px and half close up under the threshold.

	Still lit on the RIGHT. Being decorative is not a licence to draw a moon
	that is lit backwards.

	Craters off: at r=19 the maria scale to 2.5-4px, which after the downsample
	is a speckle rather than a mark, and speckle inside a 10px crescent belly
	just chews it up.
	"""
	draw_moon(d, 33.0, 33.0, 19.0, 0.17, seed=640, craters=False)
	# Same cloud as PARTLY -- same function, same seed0, same scale, same base.
	# halo=True lays black outside the silhouette first; without it the moon and
	# the cloud merge into one lumpy white mass.
	wxi.cloud(d, 52.0, 72.0, 0.92, seed0=210, halo=True)


def render_moon(p):
	big = Image.new("L", (W * SS, H * SS), 0)
	draw_moon(ImageDraw.Draw(big), CX, CY, R_MOON, p / N_PHASES)
	return big.resize((W, H), Image.BOX).point(lambda v: 255 if v >= 118 else 0)


def render_moon_cloud():
	big = Image.new("L", (W * SS, H * SS), 0)
	draw_moon_cloud(ImageDraw.Draw(big))
	return big.resize((W, H), Image.BOX).point(lambda v: 255 if v >= 118 else 0)


# ---------------------------------------------------------------------------
# I1 packing
# ---------------------------------------------------------------------------

# lv_color32_t is {blue, green, red, alpha}. Index 0 black, index 1 white.
# LVGL 9.5's RGB565 I1 blend ignores these, but lv_draw_buf_goto_xy() skips over
# them regardless, so the bytes have to be here and there have to be 8.
PALETTE = bytes([0, 0, 0, 255, 255, 255, 255, 255])
STRIDE = (W + 7) // 8


def pack_i1(img):
	px = img.load()
	out = bytearray(STRIDE * H)
	for y in range(H):
		base = y * STRIDE
		for x in range(W):
			if px[x, y]:
				out[base + (x >> 3)] |= 0x80 >> (x & 7)
	return PALETTE + bytes(out)


def unpack_i1(data):
	"""Decode with the blitter's own bit maths, so the contact sheet proves the
	packing and not just the drawing code."""
	img = Image.new("L", (W, H), 0)
	px = img.load()
	body = data[8:]
	for y in range(H):
		base = y * STRIDE
		for x in range(W):
			if (body[base + (x >> 3)] >> (7 - (x & 7))) & 1:
				px[x, y] = 255
	return img


# ---------------------------------------------------------------------------
# outputs
# ---------------------------------------------------------------------------

def write_c(path, packed, packed_cloud):
	per = 8 + STRIDE * H
	total = per * (N_PHASES + 1)
	with open(path, "w") as f:
		f.write("/* GENERATED by tools/gen_wx_moon.py -- do not edit.\n"
		        " * %d moon phases + 1 moon-behind-cloud, %dx%d, LVGL I1\n"
		        " * (1bpp indexed), %d bytes each (8 palette bytes + %d\n"
		        " * bitmap) = %d bytes total (%.1f KiB).\n"
		        " *\n"
		        " * The 8 palette bytes are mandatory. lv_draw_buf_goto_xy()\n"
		        " * unconditionally skips LV_COLOR_INDEXED_PALETTE_SIZE(cf) * 4\n"
		        " * bytes before the pixels; omit them and every frame renders\n"
		        " * shifted. data_size includes them.\n"
		        " *\n"
		        " * I1 and not A1: LVGL 9.5's software renderer has no A1\n"
		        " * blitter, so an A1 image links clean and draws nothing.\n"
		        " *\n"
		        " * Set bits blit as opaque white and clear bits as opaque\n"
		        " * black, so the black between the unlit limb and the lit mass\n"
		        " * IS the comic outline.\n"
		        " *\n"
		        " * Index order is the same as wx_moon_phase(): 0 NEW,\n"
		        " * 2 FIRST QUARTER, 4 FULL, 6 LAST QUARTER. Waxing phases are\n"
		        " * lit on the RIGHT limb (northern hemisphere).\n"
		        " *\n"
		        " * vaultweather.h is deliberately NOT included -- it pulls in\n"
		        " * esp_err.h and the whole app contract, none of which a table\n"
		        " * of bytes needs. The 8 below MUST track WX_MOON_PHASES. */\n"
		        % (N_PHASES, W, H, per, STRIDE * H, total, total / 1024.0))
		f.write('#include "lvgl.h"\n\n')

		for p in range(N_PHASES):
			f.write("static const uint8_t wxm_%d[] = {  /* %s */\n"
			        % (p, PHASE_NAMES[p]))
			b = packed[p]
			for off in range(0, len(b), 24):
				f.write("\t" + ",".join(str(v) for v in b[off:off + 24]) + ",\n")
			f.write("};\n")

		f.write("\nstatic const uint8_t wxm_cloud[] = {  /* MOON + CLOUD */\n")
		for off in range(0, len(packed_cloud), 24):
			f.write("\t" + ",".join(str(v) for v in packed_cloud[off:off + 24])
			        + ",\n")
		f.write("};\n\n")

		f.write("/* 8 == WX_MOON_PHASES (vaultweather.h). Not included here on\n"
		        " * purpose; keep the two in step by hand. */\n")
		f.write("const lv_image_dsc_t wx_moon_frames[8] = {\n")
		for p in range(N_PHASES):
			f.write("\t{ .header = { .magic = LV_IMAGE_HEADER_MAGIC,"
			        " .cf = LV_COLOR_FORMAT_I1, .w = %d, .h = %d,"
			        " .stride = %d },\n"
			        "\t  .data_size = sizeof(wxm_%d), .data = wxm_%d },"
			        "  /* %s */\n"
			        % (W, H, STRIDE, p, p, PHASE_NAMES[p]))
		f.write("};\n\n")

		f.write("const lv_image_dsc_t wx_moon_cloud_frame =\n"
		        "\t{ .header = { .magic = LV_IMAGE_HEADER_MAGIC,"
		        " .cf = LV_COLOR_FORMAT_I1, .w = %d, .h = %d,"
		        " .stride = %d },\n"
		        "\t  .data_size = sizeof(wxm_cloud), .data = wxm_cloud };\n"
		        % (W, H, STRIDE))


def _font(size):
	from PIL import ImageFont
	for p in ("/System/Library/Fonts/Supplemental/Arial Bold.ttf",
	          "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"):
		try:
			return ImageFont.truetype(p, size)
		except Exception:
			continue
	return ImageFont.load_default()


def write_sheet(path, packed, packed_cloud):
	"""White on black, exactly as the panel shows them.

	The label states which limb is SUPPOSED to be lit, so the sheet can be
	checked against the claim instead of against a memory of what a gibbous
	moon looks like.
	"""
	cols, pad, lab = 3, 22, 26   # 22px of gutter: at 10 the 13px labels collided
	imgs = [(PHASE_NAMES[p], PHASE_LIT[p], unpack_i1(packed[p]))
	        for p in range(N_PHASES)]
	imgs.append(("MOON+CLOUD", "R", unpack_i1(packed_cloud)))
	rows = (len(imgs) + cols - 1) // cols
	sw = pad + cols * (W + pad)
	sh = pad + rows * (H + lab + pad)
	sheet = Image.new("L", (sw, sh), 0)
	d = ImageDraw.Draw(sheet)
	fnt = _font(12)
	sml = _font(10)
	for i, (name, lit, im) in enumerate(imgs):
		x = pad + (i % cols) * (W + pad)
		y = pad + (i // cols) * (H + lab + pad)
		sheet.paste(im, (x, y))
		d.text((x, y + H + 3), "%d %s" % (i, name), fill=255, font=fnt)
		d.text((x, y + H + 15), "lit: %s" % lit, fill=150, font=sml)
	sheet.save(path)


def ink_report(frames, cloud_frame):
	"""Set-pixel counts.

	The illuminated FRACTION is arithmetic, so the rendered white area can be
	checked against it: a phase whose ink is far off cos(2*pi*f) has a
	terminator in the wrong place, which is exactly the failure that is hardest
	to see by eye on a 96px tile.
	"""
	out = []
	total = float(W * H)
	for p, img in enumerate(frames):
		n = sum(1 for v in img.getdata() if v)
		want = (1.0 - math.cos(TAU * p / N_PHASES)) / 2.0
		out.append((PHASE_NAMES[p], n, n / total, want))
	out.append(("MOON+CLOUD", sum(1 for v in cloud_frame.getdata() if v),
	            0.0, 0.0))
	return out


def main():
	ap = argparse.ArgumentParser()
	ap.add_argument("--out", default="main/wx_moon_frames.c")
	ap.add_argument("--sheet")
	a = ap.parse_args()

	frames = [render_moon(p) for p in range(N_PHASES)]
	cloud_frame = render_moon_cloud()
	packed = [pack_i1(f) for f in frames]
	packed_cloud = pack_i1(cloud_frame)

	write_c(a.out, packed, packed_cloud)
	per = 8 + STRIDE * H
	total = per * (N_PHASES + 1)
	print("wrote %s  %d phases + 1 cloud, %d bytes each, %d bytes total (%.1f KiB)"
	      % (a.out, N_PHASES, per, total, total / 1024.0))

	if a.sheet:
		write_sheet(a.sheet, packed, packed_cloud)
		print("wrote %s" % a.sheet)

	print("%-13s %7s %7s %7s" % ("phase", "px", "frac", "lit f"))
	for (name, n, frac, want) in ink_report(frames, cloud_frame):
		print("%-13s %7d %7.3f %7.3f" % (name, n, frac, want))


if __name__ == "__main__":
	main()
