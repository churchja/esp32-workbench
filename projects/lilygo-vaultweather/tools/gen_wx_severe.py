#!/usr/bin/env python3
"""
Generate the full-screen severe-weather animation as LVGL I1 (1bpp) frames.

Run:	python3 tools/gen_wx_severe.py [--sheet /path/to/contact_sheet.png]
Writes: main/wx_severe_frames.c

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
8 bytes -- one and a fifth rows of garbage at this width. data_size includes
them.

I1 is INDEXED, not alpha, but lv_draw_sw_blend_to_rgb565.c does
`chan_val = get_bit(src_buf_i1, src_x) * 255` and pushes that straight through
l8_to_rgb565. A set bit is therefore OPAQUE WHITE and a clear bit OPAQUE BLACK,
which is exactly a black-and-white comic panel and needs no recolour.

Bit order is MSB-first, because that is what the blitter's get_bit does:
	(buf[i / 8] >> (7 - (i % 8))) & 1

FOOTPRINT -- THE ARITHMETIC, NOT AN ESTIMATE
	stride  = (536 + 7) // 8            =     67 bytes/row
	bitmap  = 67 * 240                  = 16,080 bytes/frame
	frame   = 16,080 + 8 palette        = 16,088 bytes
	total   = 16,088 * 10               = 160,880 bytes (157.1 KiB)
The factory app partition is 0x300000 = 3,145,728 bytes (partitions.csv).
The build measured before these frames existed was 1,403,008 bytes -- 44.6%
used, 55.4% free. Adding them takes it to 1,563,888, i.e. 49.7% used. Fits with
1.5MB to spare.

THE INK IS THE BACKGROUND
Set bits are the drawing. The black gaps where one contour cuts into the mass
behind it ARE the heavy comic outlines. Nothing is stroked white.

SHAPE RULES CARRIED OVER FROM gen_nuke_frames.py AND gen_wx_icons.py
  * No rectangles, no true circles, no ellipses. A mathematically perfect edge
	reads as a UI widget, not as comic art. Every closed contour here is
	perturbed by summed sine harmonics.
  * Two harmonics at wob=0.13. An earlier pass on the nuke used 0.20 plus an
	8th harmonic; summed, that swings the radius between 0.62r and 1.38r six to
	eight times around, and every lobe came out a spiky maple leaf.
  * Nothing is mirror-symmetric on purpose. The +x and -x lobe offsets are
	deliberately unequal, and the core sits left of the panel centre so the
	anvil has room to stream downwind to the right.

CHOREOGRAPHY -- WHY THE BOLT IS NOT ON EVERY FRAME
	f0-f2  cell builds and drifts, rain shifts down
	f3     strike: thick forked bolt, and the cell's interior scallops are
	       dropped so the mass goes solid -- that IS the illumination, and it
	       is far clearer in 1bpp than any attempt at a glow
	f4     FULL WHITE FLASH, every bit set, one frame only
	f5     afterimage: same channel, 0.34 width, no forks
	f6-f9  cell settles, rain continues, no bolt
A bolt on every frame reads as a static squiggle stuck to the cloud. The
one-frame full-panel inversion is what sells it as lightning, and it is why
nothing may strike again on f6+ -- a second bolt right after the flash reads as
a stutter, not as a second stroke.

WHY THE RAIN LOOPS AND THE CELL BREATHES
Every continuous quantity is a function of phase = i / 10 through sin/cos or a
mod-1 wrap, so frame 10 is frame 0 by construction: the streaks advance exactly
two whole streak periods over the ten frames, and the cell's growth and drift
are cos/sin of the phase (peak at f5, back to the f0 value at f10). main()
prints the mean per-frame pixel delta against the 9->0 seam so the claim is
measured, not asserted. The strike and the flash are supposed to be
discontinuities and are excluded from that mean.

WHAT THE EARLIER PASSES GOT WRONG
Recorded rather than quietly fixed. None of it was visible until the frames were
rendered to a contact sheet and looked at; several of the later items were not
visible even then and had to be found by scanning the pixels.

PASS 1 filled every lobe and stroked its own outline black, the way
gen_nuke_frames.py does at 536x240. Thirty lobes each carrying a full closed
outline came out as a dry stone wall: a pile of cobblestones, not a cloud. Same
failure gen_wx_icons.py hit at 96x96, same fix -- the SILHOUETTE is one unbroken
union and the billows are suggested by interior scallops that stop short of the
edge. See cell() and _interior_arc().

PASS 1 made the anvil 438px wide and the tower 256px, within a factor of 1.7 of
each other, and the whole thing read as a flat cloud bank. A 536x240 panel
cannot hold a tall narrow tower plus an anvil plus rain plus a horizon, so
"towering" has to be carried by the WIDTH RATIO instead of by height.

PASS 1 spaced the rain streaks 14px apart with a 5.5px black halo on each side.
The halos merged and the shaft rendered as one solid hatched trapezoid with two
straight walls.

PASS 1 ran the bolt vertically down through the middle of the cell, where it was
white-on-white and invisible, with only 44px of clear air below the cloud base.

PASS 2 built _ribbon()'s joint patches from the full segment half-width, which
at the channel's 12px head filled the inside of every turn and squared it off;
the bolt read as folded planks.

PASS 3 put the tower's scallops on three lobes per course over four level
courses. Every arc landed at its neighbours' height and the tower rendered as a
regular grid of arches -- a stack of croissants.

PASS 3 set the canopy's dy on a smooth curve, which made it lens-shaped. An
anvil is flat on top because the updraft could rise no further, so the canopy is
laid out on constant lobe-TOP instead.

PASS 4 tapered the tower the wrong way and left a round 172x110 dome under a
wide flat canopy -- which is the silhouette of the Pip-Boy project's mushroom
cloud. Two animations on one device must not share a shape. The column widens
upward now, which is both the real profile and unmistakably not that one.

PASS 5 left the outermost top-course lobe 14px below the canopy's underside, and
the sine wobble opened a wedge-shaped black notch in the cell's shoulder.

PASS 6 shipped two black holes inside the white mass, 6x11 and 10x8, where a
course-C lobe and a base lobe overlapped by 3px before their wobble and by
nothing after it. On the solid strike frame they read as bit errors. They were
found by a connected-component scan, not by eye, and that scan is now
blob_report() and runs on every build.

PASS 8's first blob_report used fill >= 0.35 of the bounding box and flagged
seven interior scallops and zero holes. Thickness, not fill, is what separates a
5px pen stroke from a hole.

PASS 11 flooded the background with 4-connectivity, the same connectivity as the
components. A black pixel joined to the outside world only through a diagonal
then counts as enclosed, and one corner nick on a scallop tip was reported as a
defect for three passes. The background flood is 8-connected now.
"""
import argparse
import math
import random

from PIL import Image, ImageDraw, ImageFilter

W, H = 536, 240
N_FRAMES = 10
SS = 2						# supersample factor; thin streaks stair-step at 1x
TAU = 2.0 * math.pi

# Black gap between overlapping shapes, in final pixels. The single strongest
# control over how "drawn" the result looks -- thin reads as vector art. 5px
# against ~30px lobes is the ratio gen_nuke_frames.py settled on at this size.
INK = 5.0

# Core centre. The panel centre is 268; the updraft sits left of it so the
# anvil can stream downwind to the right without running out of panel.
CXT = 232
ANVIL_Y = 36

RIDGE_Y = 217				# mean horizon; the ridge wanders about it

# Rain travels from inside the cloud to below the horizon, so streaks are never
# seen to appear or vanish -- the cloud and the ground clip them. SPAN must be
# an exact multiple of SPACING or the wrap is not seamless.
RAIN_TOP = 126.0
RAIN_SPACING = 30.0
RAIN_SPAN = 90.0			# 3 * SPACING
RAIN_ADVANCE = 2.0			# whole streak periods per 10-frame loop


# ---------------------------------------------------------------------------
# primitives -- all coordinates are in panel space and scaled on the way out
# ---------------------------------------------------------------------------

def _px(pts):
	return [(x * SS, y * SS) for (x, y) in pts]


def _ink_w(ink):
	return max(1, int(round(ink * SS)))


def ink_then_fill(d, pts, ink=INK):
	"""Lay a black halo down first, then fill white at full size.

	For anything that sits IN FRONT of an existing white mass at its true size
	-- the bolt over the cloud, a rain streak over the ridge. Filling first and
	stroking after would eat 2.5px off a 4px-wide streak and leave nothing.
	"""
	p = _px(pts)
	if ink > 0:
		d.line(p + [p[0]], fill=0, width=_ink_w(ink) * 2, joint="curve")
	d.polygon(p, fill=255)


def lobe_pts(cx, cy, r, seed, wob=0.13, squash=0.92, n=96):
	"""A billow that is deliberately not a circle.

	Radius modulated by two incommensurate sine harmonics, so the contour
	wanders the way an inked edge does. Same seed gives the same lobe on every
	frame, which keeps the cell coherent as it drifts instead of boiling.

	wob=0.13 is not arbitrary: two harmonics at 0.20 plus an 8th swing the
	radius between 0.62r and 1.38r six to eight times around, and every lobe
	comes out a spiky maple leaf.
	"""
	rnd = random.Random(seed)
	p1, p2 = (rnd.uniform(0, TAU) for _ in range(2))
	pts = []
	for i in range(n):
		th = i * TAU / n
		rr = r * (1.0
				  + wob * math.sin(3 * th + p1)
				  + wob * 0.50 * math.sin(5 * th + p2))
		pts.append((cx + rr * math.cos(th), cy + rr * math.sin(th) * squash))
	return pts


# ---------------------------------------------------------------------------
# the storm cell
# ---------------------------------------------------------------------------

# The ice canopy. (dx, dy, r, squash, scallop), dy relative to ANVIL_Y.
#
# The slab crests left of centre and tapers to a wisp at the downwind (right)
# end. An anvil is not a symmetric bar: it is the plume that spread out when the
# updraft hit the tropopause and then got sheared, so the upwind end is blunt
# and short and the downwind end runs on and thins.
# dy is chosen so that (dy - r*squash) -- the TOP of each lobe -- is close to
# constant across the main span. That is not a stylistic choice: an anvil is
# flat on top because the updraft hit the tropopause and could not rise
# further, so every lobe of the canopy tops out at the same height. Pass 3 set
# dy on a smooth curve instead and the canopy came out lens-shaped, which reads
# as a lens-shaped cloud. The last three lobes break the rule deliberately: the
# trailing edge drops away and thins to a wisp downwind.
ANVIL = [
	(-126, -8, 17, 0.44, False),
	(-94, -6, 23, 0.46, True),
	(-56, -3, 29, 0.50, True),
	(-14, -1, 33, 0.54, True),
	(30, -2, 31, 0.50, True),
	(74, -5, 27, 0.46, True),
	(118, -8, 23, 0.40, True),
	(160, -8, 19, 0.36, True),
	(200, -4, 15, 0.32, False),
	(234, 2, 11, 0.30, False),
	(260, 8, 7, 0.28, False),
]

# The overshooting top: the updraft's last surge punching a dome through the
# canopy, directly over the column. Two lumps, different sizes, different
# heights. It is the single detail that separates "thunderhead" from "big
# cloud" and it costs two lobes -- but pass 3 made them r=15 at squash 0.60 and
# the dome grew taller than the canopy was thick, so the whole cell read as a
# cauliflower with two thin fins stuck on. It has to be a BUMP.
OVERSHOOT = [
	(-10, -20, 13, 0.62, False),
	(10, -17, 9, 0.60, False),
]

# The updraft column. Absolute y, dx from CXT. Deliberately narrow: ~180px
# against the anvil's ~410 is what makes it read as a thunderhead on a panel too
# short to hold a tall one.
#
# The column WIDENS UPWARD -- 180px where it meets the canopy, 119px at the
# rain base. That is the real cumulonimbus profile, and here it is also load
# bearing: pass 4 tapered the other way, which put a round 172x110 dome under a
# wide flat canopy, and a wide cap over a dome is the silhouette of the Pip-Boy
# project's mushroom cloud. Two animations on one device must not share a
# shape.
#
# The lobes are staggered in BOTH axes and the courses hold different numbers of
# them. Pass 2 used three lobes per course on four level courses; every scallop
# then landed at the same height as its neighbours and the tower rendered as a
# regular grid of arches -- a stack of croissants. Staggering the centres
# staggers the arcs with them, which is how overlapping billows actually pile
# up. Roughly a quarter of the lobes carry no scallop at all, so the line work
# never falls into a pattern.
# Every lobe of the TOP course has to top out ABOVE the canopy's underside at
# its own dx, or the two masses meet at a tangent and the wobble opens a black
# notch between them. Pass 5 left the outermost one 14px low and the sheet
# showed a wedge bitten out of the cell's shoulder. The canopy underside runs
# roughly 40 -> 53 -> 43 across the column, so the top course sits at 34..41.
TOWER = [
	(-70, 58, 27, 0.88, True),
	(-24, 60, 29, 0.90, True),
	(24, 58, 28, 0.88, True),
	(66, 62, 24, 0.86, True),
	(-62, 88, 25, 0.90, True),
	(-14, 92, 28, 0.92, False),
	(34, 86, 25, 0.88, True),
	(64, 96, 18, 0.86, True),
	(-52, 114, 23, 0.90, True),
	(-6, 118, 26, 0.90, True),
	(38, 112, 22, 0.88, False),
	(-42, 136, 19, 0.80, True),
	(2, 142, 21, 0.76, False),
	(40, 136, 18, 0.80, True),
]

# Interior fillers, carrying no scallop and never reaching the silhouette.
# Measured, not guessed: a connected-component scan of the flash frame found two
# black holes inside the white mass, 6x11px at (203,124) and 10x8px at (253,131),
# where a course-C lobe and a base lobe overlapped by only 3px before their sine
# wobble and by nothing after it. Ten final pixels is far past what the
# morphological close in cell() can fill, and widening that kernel enough would
# start rounding off the billows on the outside.
FILL = [
	(-28, 124, 20, 0.88, False),
	(22, 130, 20, 0.86, False),
]

# Ragged base. Flat lobes hanging under the tower -- this is where the rain
# comes out, and a clean straight underside would read as a shelf.
SCUD = [
	(-52, 140, 10, 0.50, False), (-14, 148, 14, 0.48, False),
	(26, 150, 15, 0.46, False), (58, 144, 11, 0.50, False),
]


def _cell_lobes(grow, drift):
	"""Every lobe of the cell, with its scallop flag and pen weight.

	`drift` is applied 1.6x to the anvil and 0.6x to the tower: the canopy is in
	faster air than the column, and that differential is what makes the cell
	look sheared rather than sliding sideways as one piece.

	The anvil's pen is 0.70 of the tower's. Its lobes are squashed to a vertical
	half-height near 12px and a full 5px scallop through one of those closes it
	off into a bead.
	"""
	out = []
	for k, (dx, dy, r, sq, sc) in enumerate(OVERSHOOT):
		out.append((lobe_pts(CXT + drift * 1.1 + dx * grow,
							 ANVIL_Y + dy * grow, r * grow, 580 + k,
							 squash=sq), sc, INK * 0.70))
	for k, (dx, dy, r, sq, sc) in enumerate(ANVIL):
		out.append((lobe_pts(CXT + drift * 1.6 + dx * grow,
							 ANVIL_Y + dy * grow, r * grow, 600 + k,
							 squash=sq), sc, INK * 0.70))
	for k, (dx, y, r, sq, sc) in enumerate(TOWER):
		out.append((lobe_pts(CXT + drift * 0.6 + dx * grow,
							 ANVIL_Y + (y - ANVIL_Y) * grow, r * grow, 640 + k,
							 squash=sq), sc, INK))
	for k, (dx, y, r, sq, sc) in enumerate(FILL):
		out.append((lobe_pts(CXT + drift * 0.6 + dx * grow,
							 ANVIL_Y + (y - ANVIL_Y) * grow, r * grow, 660 + k,
							 squash=sq), sc, INK))
	for k, (dx, y, r, sq, sc) in enumerate(SCUD):
		out.append((lobe_pts(CXT + drift * 0.4 + dx * grow,
							 ANVIL_Y + (y - ANVIL_Y) * grow, r * grow, 680 + k,
							 wob=0.17, squash=sq), sc, INK))
	return out


def _interior_arc(pts, mask, ink):
	"""The longest run of this lobe's BOTTOM arc that a stroke cannot leak out
	of, or None.

	Three restrictions, all of them learned the hard way in gen_wx_icons.py and
	re-learned here:

	Bottom arc only. Taking every part of the contour that happened to lie
	inside the mass let the line wrap up around the lobe's flanks into hooks
	that curled back on themselves. lobe_pts walks theta from 0 and screen y
	grows downward, so indices 0..n/2 are the lower half; the window is trimmed
	at both ends to keep the curve shallow. A scallop is the underside of a
	billow overlapping the billow behind it -- convex downward, always.

	Longest run only, and it has to be worth drawing. Several short fragments of
	one arc read as dashes, and a short run rendered as an isolated tick mark
	floating in the white mass. The floor is a 22px CHORD, not a sample count: a
	fixed count is 52 degrees of arc whatever the lobe, which is 30px across the
	tower's r=30 billows and 20px across the canopy's r=23 ones, so a count that
	looks right on the tower leaves dashes on the canopy.

	Margin. Every sample is tested at the contour AND at half the pen width
	either side of it along the lobe's own radius. If all three are inside the
	union, the stroke cannot reach the silhouette, so the outline of the cell
	can never be cut no matter what the lobe layout does. That is the whole
	difference between this and pass 1's pile of cobblestones.
	"""
	n = len(pts)
	lo, hi = int(n * 0.10), int(n * 0.40)
	m = mask.load()
	lim_x, lim_y = W * SS - 1, H * SS - 1
	off = ink * 0.5 + 1.5
	cx = sum(p[0] for p in pts) / n
	cy = sum(p[1] for p in pts) / n

	best, cur = [], []
	for i in range(lo, hi + 1):
		x, y = pts[i]
		dx, dy = x - cx, y - cy
		L = math.hypot(dx, dy) or 1.0
		ok = True
		for s in (0.0, off, -off):
			qx = int((x + dx / L * s) * SS)
			qy = int((y + dy / L * s) * SS)
			if not (0 <= qx <= lim_x and 0 <= qy <= lim_y and m[qx, qy] > 127):
				ok = False
				break
		if ok:
			cur.append(pts[i])
		else:
			if len(cur) > len(best):
				best = cur
			cur = []
	if len(cur) > len(best):
		best = cur
	if len(best) < 6:
		return None
	chord = math.hypot(best[-1][0] - best[0][0], best[-1][1] - best[0][1])
	return best if chord >= 22.0 else None


def cell(img, grow, drift, flash=False):
	"""Anvil, tower and base scud as ONE white mass with interior scallops.

	The mass is rasterised into a mask and morphologically CLOSED before it is
	pasted. Three lobes meeting at a shallow angle leave a pinhole a few pixels
	across that no amount of lobe nudging reliably removes, and on the solid
	strike frame those pinholes rendered as a scatter of black specks inside the
	white cell -- they looked like bit errors, which is the worst thing a 1bpp
	image can look like. A 9px dilate-then-erode at SS=2 fills anything under 4
	final pixels; a 5px kernel left three 2x2 specks, which at this panel's pitch
	still read as dust. It costs the silhouette nothing but a ~2px softening of
	concave corners, and the notches between lobes are ten times that.

	`flash` drops the scallops so the cell goes solid. That is the strike frame:
	losing the interior line work reads as the whole cell lighting up from the
	inside, which is cheaper and far clearer than any 1bpp attempt at a glow.
	"""
	lobes = _cell_lobes(grow, drift)

	mask = Image.new("L", (W * SS, H * SS), 0)
	md = ImageDraw.Draw(mask)
	for (pts, _, _) in lobes:
		md.polygon(_px(pts), fill=255)
	mask = mask.filter(ImageFilter.MaxFilter(9)).filter(ImageFilter.MinFilter(9))
	img.paste(255, mask=mask)

	if flash:
		return

	d = ImageDraw.Draw(img)
	for (pts, sc, ink) in lobes:
		if not sc:
			continue
		run = _interior_arc(pts, mask, ink)
		if run:
			d.line(_px(run), fill=0, width=_ink_w(ink), joint="curve")


def ridge(d):
	"""The horizon.

	Three incommensurate sines, so no part of it repeats and none of it is
	level. The polygon closes off the bottom of the panel: that edge is a dead
	straight line, which is why it is placed 12px BELOW the panel where nobody
	can see it -- the same trick gen_nuke_frames.py uses for the stem base.
	"""
	pts = []
	n = 160
	for i in range(n + 1):
		x = -12.0 + (W + 24.0) * i / n
		y = (RIDGE_Y
			 - 5.0 * math.sin(TAU * x / 389.0 + 0.7)
			 - 2.6 * math.sin(TAU * x / 151.0 + 2.1)
			 - 1.4 * math.sin(TAU * x / 67.0 + 4.3))
		pts.append((x, y))
	pts += [(W + 12.0, H + 12.0), (-12.0, H + 12.0)]
	d.polygon(_px(pts), fill=255)


def streak(d, x, y, length, dx, halfw, ink):
	"""A rain sliver: pointed at both ends, bulged off-centre so it is not a
	parallelogram. A parallelogram at this size reads as a tally mark."""
	pts = [(x, y),
		   (x + dx * 0.30 + halfw, y + length * 0.30),
		   (x + dx, y + length),
		   (x + dx * 0.70 - halfw, y + length * 0.72)]
	ink_then_fill(d, pts, ink)


# (x, phase, weight). The shaft sits under the TOWER, not under the anvil --
# rain falls out of the core, and a curtain across the whole panel both reads as
# static and swallows the cell's shape. Columns are unevenly spaced and unevenly
# phased; an even grid reads as hatching. The 0.6-weight columns at each end
# fray the edges so the shaft does not end in two straight walls.
# (x, phase, weight, slant). Every streak sharing one slant is what turned
# pass 3's shaft into a block of hatching: identical angle plus even spacing IS
# hatching, whatever the individual sliver looks like. Slant varies +-30% and
# weight (length and width together) varies 0.5..1.0, so no two columns fall at
# the same rate or reach the same depth.
RAIN_COLS = [
	(160, 0.55, 0.55, -4.4), (178, 0.11, 1.00, -6.8), (195, 0.68, 0.80, -5.6),
	(211, 0.30, 0.96, -7.4), (227, 0.88, 0.88, -6.0), (244, 0.43, 1.00, -6.6),
	(260, 0.06, 0.74, -5.0), (276, 0.74, 0.94, -7.2), (292, 0.35, 0.86, -5.8),
	(307, 0.95, 0.66, -6.4), (322, 0.21, 0.50, -4.8),
]


def rain(d, phase):
	"""The shaft.

	Each column carries RAIN_SPAN / RAIN_SPACING streaks at fixed spacing and
	slides down by RAIN_ADVANCE whole spacings over the loop. Because the
	streaks in a column are identical and evenly spaced, sliding by a whole
	spacing lands each one exactly where its neighbour was: frame 10 is frame 0,
	with no jump at the 9->0 seam and nothing appearing or vanishing. Travel
	runs from inside the cloud to below the horizon, so the clipping is done by
	the cloud and the ground rather than by an on/off mask, which blinks.

	The halo is 0.22 of the pen, not the 0.55 pass 1 used. Against a black sky a
	white sliver needs no outline at all; the sliver of one is there only so two
	streaks that cross still read as two.
	"""
	n = int(RAIN_SPAN / RAIN_SPACING)
	slide = RAIN_ADVANCE * phase * RAIN_SPACING
	for (x, p0, wgt, slant) in RAIN_COLS:
		for k in range(n):
			y = RAIN_TOP + ((p0 * RAIN_SPACING + k * RAIN_SPACING + slide)
							% RAIN_SPAN)
			streak(d, x, y, 23.0 * wgt, slant, 1.9 * wgt, INK * 0.22)


# ---------------------------------------------------------------------------
# lightning
# ---------------------------------------------------------------------------

# Centreline (x, y, half-width). The channel leaves the cell's lower right
# flank and crosses open sky diagonally to the ground on the right of the panel:
# 90px of black background to be seen against, where a vertical bolt under the
# core had 44px and half of that was white cloud.
#
# Hand-set: the lateral swings alternate but are never equal, the vertical steps
# are never equal, and the channel tapers from 9px to a point. A bolt built from
# a regular zigzag reads as a logo.
BOLT_MAIN = [(280, 134, 7.5), (324, 166, 5.9), (294, 178, 4.7),
			 (352, 204, 3.2), (330, 212, 2.2), (376, 232, 0.6)]
# Forks of deliberately different lengths, leaving at different heights and on
# opposite sides. Two equal forks read as a tuning fork. The right fork is
# angled much flatter than the main channel: pass 2's ran nearly parallel to it
# and the pair read as one double-ruled line, not as a branch. Neither fork
# reaches the ground -- only the leader does.
BOLT_FORKS = [
	[(324, 166, 3.4), (376, 174, 2.2), (352, 184, 1.5), (398, 196, 0.5)],
	[(294, 178, 2.6), (258, 190, 1.5), (272, 199, 0.5)],
]


def _ribbon(path, wscale):
	"""A tapering ribbon along a centreline, as one quad per segment.

	Emitted as separate quads rather than one offset polygon on purpose: a
	single polygon walking down one side and back up the other self-intersects
	at a sharp zigzag corner, and PIL fills with an even-odd rule, so the
	corners punch holes. Separate quads just union.

	The joint patch closes the notch that opens on the outside of a turn. It is
	scaled to 0.55 of the half-width: at full width it also filled the INSIDE of
	every turn, squaring the corner off, and the bolt read as folded planks.
	"""
	quads = []
	for i in range(len(path) - 1):
		x0, y0, w0 = path[i]
		x1, y1, w1 = path[i + 1]
		dx, dy = x1 - x0, y1 - y0
		L = math.hypot(dx, dy) or 1.0
		nx, ny = -dy / L, dx / L
		a, b = w0 * wscale, w1 * wscale
		quads.append([(x0 + nx * a, y0 + ny * a),
					  (x1 + nx * b, y1 + ny * b),
					  (x1 - nx * b, y1 - ny * b),
					  (x0 - nx * a, y0 - ny * a)])
		if i + 2 < len(path):
			x2, y2, _ = path[i + 2]
			e2x, e2y = x2 - x1, y2 - y1
			L2 = math.hypot(e2x, e2y) or 1.0
			mx, my = -e2y / L2, e2x / L2
			j = b * 0.55
			for s in (1.0, -1.0):
				quads.append([(x1, y1),
							  (x1 + nx * j * s, y1 + ny * j * s),
							  (x1 + mx * j * s, y1 + my * j * s)])
	return quads


def bolt(d, wscale=1.0, ink=INK, forks=True):
	"""Draw the channel in front of everything, haloed.

	Two passes, not per-quad ink_then_fill: every quad's black halo is laid
	first and every white fill second, otherwise a later segment's halo cuts a
	black seam straight across the channel it is supposed to continue.
	"""
	quads = _ribbon(BOLT_MAIN, wscale)
	if forks:
		for f in BOLT_FORKS:
			quads += _ribbon(f, wscale)

	w = _ink_w(ink) * 2
	for q in quads:
		p = _px(q)
		d.line(p + [p[0]], fill=0, width=w, joint="curve")
	for q in quads:
		d.polygon(_px(q), fill=255)


# ---------------------------------------------------------------------------
# frames
# ---------------------------------------------------------------------------

FRAME_NOTE = ["build", "build", "build", "STRIKE", "FLASH",
			  "afterimage", "settle", "settle", "settle", "settle"]


def draw_frame(img, i):
	d = ImageDraw.Draw(img)
	phase = i / N_FRAMES

	if i == 4:
		# The whole panel inverted. One frame. This is the thing that makes the
		# strike read as lightning rather than as a squiggle switched on.
		d.rectangle([0, 0, W * SS, H * SS], fill=255)
		return

	# Both are exactly periodic in phase, so frame 10 is frame 0: the cell grows
	# through f0-f5 and settles back through f6-f9 with no seam.
	grow = 1.0 + 0.030 * (1.0 - math.cos(TAU * phase))
	drift = 7.0 * math.sin(TAU * phase)

	rain(d, phase)
	cell(img, grow, drift, flash=(i == 3))
	# cell() pasted through a mask, so `d` is stale -- ImageDraw caches the
	# image's pixel access object and paste() invalidates it.
	d = ImageDraw.Draw(img)
	if i == 3:
		bolt(d, 1.0)
	elif i == 5:
		bolt(d, 0.34, forks=False)
	ridge(d)


def _despeckle(img):
	"""Clear black pixels whose eight neighbours are all white.

	A scallop stroke ends on an odd sub-pixel boundary and the SS=2 box
	downsample occasionally sheds one pixel off the tip. Measured, with the
	scallops switched off and back on: the lobe union itself is clean, and two
	frames in ten carried exactly one shed pixel. At INK=5 no intended line is
	ever one pixel wide with eight white neighbours, so this can only remove
	that artefact -- it cannot touch a rain streak, a bolt edge or a scallop.
	Last stage before packing, so nothing downstream can reintroduce it.
	"""
	px = img.load()
	kill = [(x, y)
			for y in range(1, H - 1) for x in range(1, W - 1)
			if not px[x, y] and all(px[x + dx, y + dy]
									for dy in (-1, 0, 1) for dx in (-1, 0, 1)
									if dx or dy)]
	for (x, y) in kill:
		px[x, y] = 255
	return img


def render(i):
	big = Image.new("L", (W * SS, H * SS), 0)
	draw_frame(big, i)
	small = big.resize((W, H), Image.BOX).point(lambda v: 255 if v >= 118 else 0)
	return _despeckle(small)


# ---------------------------------------------------------------------------
# I1 packing
# ---------------------------------------------------------------------------

# lv_color32_t is {blue, green, red, alpha}. Index 0 black, index 1 white.
# LVGL 9.5's RGB565 I1 blend ignores these, but lv_draw_buf_goto_xy() skips
# over them regardless, so the bytes have to be here and there have to be 8.
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

def write_c(path, packed):
	per = 8 + STRIDE * H
	with open(path, "w") as f:
		f.write("/* GENERATED by tools/gen_wx_severe.py -- do not edit.\n"
				" * %d frames, %dx%d, LVGL I1 (1bpp indexed), %d bytes each\n"
				" * (8 palette bytes + %d bitmap) = %d bytes total (%.1f KiB).\n"
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
				" * black, so the gaps between contours ARE the comic outline.\n"
				" *\n"
				" * Frame 4 is every bit set -- the lightning flash. It is not\n"
				" * a corrupt frame. */\n"
				% (N_FRAMES, W, H, per, STRIDE * H, per * N_FRAMES,
				   per * N_FRAMES / 1024.0))
		f.write('#include "lvgl.h"\n\n')
		for i, b in enumerate(packed):
			f.write("static const uint8_t wxs_%d[] = {  /* %s */\n"
					% (i, FRAME_NOTE[i]))
			for off in range(0, len(b), 24):
				f.write("\t" + ",".join(str(v) for v in b[off:off + 24]) + ",\n")
			f.write("};\n")
		f.write("\nconst lv_image_dsc_t wx_severe_frames[%d] = {\n" % N_FRAMES)
		for i in range(N_FRAMES):
			f.write("\t{ .header = { .magic = LV_IMAGE_HEADER_MAGIC,"
					" .cf = LV_COLOR_FORMAT_I1, .w = %d, .h = %d,"
					" .stride = %d },\n"
					"\t  .data_size = sizeof(wxs_%d), .data = wxs_%d },\n"
					% (W, H, STRIDE, i, i))
		f.write("};\n\n")
		f.write("const int wx_severe_frame_count = %d;\n" % N_FRAMES)


def _font(size):
	from PIL import ImageFont
	for p in ("/System/Library/Fonts/Supplemental/Arial Bold.ttf",
			  "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"):
		try:
			return ImageFont.truetype(p, size)
		except Exception:
			continue
	return ImageFont.load_default()


def write_sheet(path, packed):
	"""White on black, exactly as the panel shows them. Two columns."""
	pad, hdr = 8, 20
	cols = 2
	rows = (N_FRAMES + cols - 1) // cols
	sw = pad + cols * (W + pad)
	sh = pad + rows * (H + hdr + pad)
	sheet = Image.new("L", (sw, sh), 40)
	d = ImageDraw.Draw(sheet)
	fnt = _font(15)
	for i in range(N_FRAMES):
		cx = pad + (i % cols) * (W + pad)
		cy = pad + (i // cols) * (H + hdr + pad)
		d.text((cx, cy), "f%d  %s" % (i, FRAME_NOTE[i]), fill=255, font=fnt)
		sheet.paste(unpack_i1(packed[i]), (cx, cy + hdr))
	sheet.save(path)


def loop_report(frames):
	"""Mean per-frame pixel delta vs. the 9->0 seam.

	f3-f4-f5 are deliberate discontinuities (strike, flash, afterimage) and
	would swamp the mean, so it is taken over the continuous steps only. A seam
	much larger than that mean is a visible jump.
	"""
	px = [f.load() for f in frames]

	def diff(a, b):
		n = 0
		for y in range(H):
			for x in range(W):
				if px[a][x, y] != px[b][x, y]:
					n += 1
		return n

	steps = [diff(a, a + 1) for a in (0, 1, 5, 6, 7, 8)]
	return sum(steps) / len(steps), max(steps), diff(N_FRAMES - 1, 0)


def blob_report(frames):
	"""Count enclosed black BLOBS -- black regions with no path to the panel
	edge that are compact rather than line-shaped.

	A hole inside a white mass reads as a bit error, which is the worst thing a
	1bpp image can look like, and the eye reads it as dust on the panel rather
	than as a drawing mistake. Pass 6 shipped two and they were only found by
	scanning, so the scan stays.

	The interior scallops are also enclosed black regions, deliberately, so
	shape is what separates them. A first cut used fill >= 0.35 of the bounding
	box and flagged seven scallops and no holes -- a bowed 30x13 stroke fills
	0.41 of its box, and a short straight one fills 0.62. Thickness is the
	discriminator that actually works: min(bbox) >= 6px excludes anything only a
	5px pen wide, and fill >= 0.55 excludes the bowed ones. Measured against the
	two real holes pass 6 shipped, 6x11 at 0.89 and 10x8 at 0.65, both are caught
	with margin.

	The second clause catches the other failure mode: a speck too small to trip
	the first. A scallop's chord is at least 22px, so nothing legitimate ever
	fits in a 5x5 box; three 2x2 specks that survived a narrower morphological
	close did, and they were invisible to the first clause.
	"""
	out = []
	for f in frames:
		px = f.load()
		seen = bytearray(W * H)
		stack = [(x, y) for x in range(W) for y in (0, H - 1) if px[x, y] == 0]
		stack += [(x, y) for y in range(H) for x in (0, W - 1) if px[x, y] == 0]
		for (x, y) in stack:
			seen[y * W + x] = 1
		# The background flood is 8-connected while the components below are
		# 4-connected. That pairing is not arbitrary: with both at 4, a black
		# pixel joined to the outside world only through a diagonal counts as
		# enclosed, and one such corner nick on the tip of a scallop was
		# reported as a defect for three passes running.
		while stack:
			cx, cy = stack.pop()
			for dy in (-1, 0, 1):
				for dx in (-1, 0, 1):
					nx, ny = cx + dx, cy + dy
					if 0 <= nx < W and 0 <= ny < H and not seen[ny * W + nx] \
							and px[nx, ny] == 0:
						seen[ny * W + nx] = 1
						stack.append((nx, ny))

		blobs = 0
		for y in range(H):
			for x in range(W):
				if px[x, y] or seen[y * W + x]:
					continue
				comp = []
				st = [(x, y)]
				seen[y * W + x] = 1
				while st:
					ax, ay = st.pop()
					comp.append((ax, ay))
					for nx, ny in ((ax + 1, ay), (ax - 1, ay),
								   (ax, ay + 1), (ax, ay - 1)):
						if 0 <= nx < W and 0 <= ny < H \
								and not seen[ny * W + nx] and px[nx, ny] == 0:
							seen[ny * W + nx] = 1
							st.append((nx, ny))
				xs = [p[0] for p in comp]
				ys = [p[1] for p in comp]
				bw, bh = max(xs) - min(xs) + 1, max(ys) - min(ys) + 1
				hole = min(bw, bh) >= 6 and len(comp) >= 0.55 * bw * bh
				speck = max(bw, bh) <= 5 and len(comp) >= 0.75 * bw * bh
				if hole or speck:
					blobs += 1
		out.append(blobs)
	return out


def main():
	ap = argparse.ArgumentParser()
	ap.add_argument("--out", default="main/wx_severe_frames.c")
	ap.add_argument("--sheet")
	a = ap.parse_args()

	frames = [render(i) for i in range(N_FRAMES)]
	packed = [pack_i1(f) for f in frames]

	write_c(a.out, packed)
	per = 8 + STRIDE * H
	total = per * N_FRAMES
	print("wrote %s  %d frames, %dx%d, %d bytes each, %d bytes total (%.1f KiB)"
		  % (a.out, N_FRAMES, W, H, per, total, total / 1024.0))

	if a.sheet:
		write_sheet(a.sheet, packed)
		print("wrote %s" % a.sheet)

	mean, mx, seam = loop_report(frames)
	print("continuous step mean %.0f px, max %d px, 9->0 seam %d px%s"
		  % (mean, mx, seam, "   <-- SEAM JUMP" if seam > mean * 1.9 else ""))

	blobs = blob_report(frames)
	print("enclosed black blobs per frame: %s%s"
		  % (blobs, "   <-- HOLES IN THE ART" if any(blobs) else ""))


if __name__ == "__main__":
	main()
