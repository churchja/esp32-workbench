#!/usr/bin/env python3
"""
Generate the animated weather-condition icons as LVGL I1 (1-bit) images.

Run:    python3 tools/gen_wx_icons.py [--sheet /path/to/contact_sheet.png]
Writes: main/wx_icons.c

WHY I1 AND NOT A1
A1 is the obvious 1bpp choice and it is a trap. LV_COLOR_FORMAT_A1 is a valid
enum in every LVGL build, but the 9.5 SOFTWARE renderer has no blitter for it --
grep the tree and A1 appears only under src/draw/nema_gfx/ and src/draw/vg_lite/,
hardware accelerators this ESP32-S3 does not have. An A1 image compiles clean,
links clean, and draws nothing at all; the only trace is a runtime
LV_LOG_WARN("Not supported source color format").

I1 is the 1bpp format the software renderer does implement
(LV_DRAW_SW_SUPPORT_I1, default y).

THE PALETTE BYTES ARE MANDATORY EVEN THOUGH NOTHING READS THEM
lv_draw_buf.c:389 and :439 both skip
LV_COLOR_INDEXED_PALETTE_SIZE(cf) * sizeof(lv_color32_t) bytes before the
pixels, unconditionally. For I1 that is 2 * 4 = 8 bytes. Omit them and every
frame renders shifted by 8 bytes -- two thirds of a row of garbage at this
width. data_size must include them.

I1 is INDEXED, not alpha, but lv_draw_sw_blend_to_rgb565.c:481 does
`chan_val = get_bit(src_buf_i1, src_x) * 255` and pushes that straight through
l8_to_rgb565. A set bit is therefore OPAQUE WHITE and a clear bit OPAQUE BLACK,
which is exactly a black-and-white comic panel and needs no recolour.

Bit order is MSB-first, because that is what the blitter's get_bit does:
	(buf[i / 8] >> (7 - (i % 8))) & 1

FOOTPRINT
96x96 at 1bpp is (96 + 7) / 8 = 12 bytes per row x 96 rows = 1152 bytes of
bitmap, + 8 palette bytes = 1160 bytes per frame.
8 icons x 8 frames x 1160 = 74,240 bytes (72.5 KiB) of .rodata.

THE INK IS THE BACKGROUND
Set bits are the drawing. The black gaps where one lobe's outline cuts into
the lobe behind it ARE the heavy comic outlines. Nothing is stroked white.

SHAPE RULES CARRIED OVER FROM tools/gen_nuke_frames.py IN THE PIP-BOY PROJECT
  * No circles, no ellipses, no rectangles. A mathematically perfect edge reads
	as a UI widget, not as comic art. Every closed contour here is perturbed by
	summed sine harmonics.
  * Two harmonics at wob=0.13. An earlier pass on the nuke used 0.20 plus an
	8th harmonic; summed, that swings the radius between 0.62r and 1.38r six to
	eight times around, and every lobe came out as a spiky maple leaf.
  * Nothing is mirror-symmetric on purpose. The +x and -x lobe offsets are
	deliberately unequal.

WHY EVERYTHING IS DRAWN 4x AND DOWNSAMPLED
The nuke frames are 536x240 and drew fine at native resolution. At 96x96 a
2px ink gap drawn natively into a 1-bit canvas stair-steps so badly the cloud
contours break up. Everything is rasterised into an 8-bit 384x384 buffer and
box-downsampled to 96x96, then thresholded. The result is still hard 1bpp --
the panel gets no greys -- but the edge lands where the geometry actually is.

SEAMLESS LOOPS
Frame 7 must lead back into frame 0; a visible jump is worse than no animation.
Every animated quantity here is a function of (i / 8) through sin/cos or a
mod-1 wrap, so frame 8 is frame 0 by construction. main() prints the mean
per-frame pixel delta alongside the 7->0 delta so the claim is measured, not
asserted. The exception is STORM, where the lightning is SUPPOSED to be a
discontinuity -- but it fires on frames 2 and 3, never across the 7->0 seam.
"""
import argparse
import math
import random

from PIL import Image, ImageDraw, ImageFilter

W = H = 96
N_FRAMES = 8
SS = 4                      # supersample factor; see docstring
TAU = 2.0 * math.pi

# Black gap between overlapping shapes, in final (96-space) pixels. This is the
# single strongest control over how "drawn" the result looks -- thin reads as
# vector art. 2.4 is ~15% of a typical cloud lobe radius here, matching the
# 5px/35px ratio the nuke frames settled on.
INK = 2.4

ICON_NAMES = ["CLEAR", "PARTLY", "CLOUDY", "FOG",
			  "DRIZZLE", "RAIN", "SNOW", "STORM"]


# ---------------------------------------------------------------------------
# primitives -- all coordinates are in 96-space and scaled on the way out
# ---------------------------------------------------------------------------

def _px(pts):
	return [(x * SS, y * SS) for (x, y) in pts]


def _ink_w(ink):
	return max(1, int(round(ink * SS)))


def fill_then_ink(d, pts, ink=INK):
	"""Fill white, then cut the outline black. Shrinks the shape by ink/2.

	Used for the billows, where every lobe eating into its neighbour is the
	whole point.
	"""
	p = _px(pts)
	d.polygon(p, fill=255)
	if ink > 0:
		d.line(p + [p[0]], fill=0, width=_ink_w(ink), joint="curve")


def ink_then_fill(d, pts, ink=INK):
	"""Lay a black halo down first, then fill white at full size.

	Used for anything that has to sit IN FRONT of an existing white mass at its
	true size -- the lightning bolt over the storm cloud, fog banks over each
	other. fill_then_ink would eat a 2.4px sliver off a 5px-thick fog band and
	leave nothing.
	"""
	p = _px(pts)
	if ink > 0:
		d.line(p + [p[0]], fill=0, width=_ink_w(ink) * 2, joint="curve")
	d.polygon(p, fill=255)


def lobe_pts(cx, cy, r, seed, wob=0.13, squash=0.92, n=80):
	"""A billow that is deliberately not a circle.

	Radius modulated by two incommensurate sine harmonics, so the contour
	wanders the way an inked edge does. Same seed gives the same lobe on every
	frame, which keeps a cloud coherent as it drifts instead of boiling.
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


def blob(d, cx, cy, r, seed, wob=0.13, squash=0.92, ink=INK):
	fill_then_ink(d, lobe_pts(cx, cy, r, seed, wob, squash), ink)


# ---------------------------------------------------------------------------
# the cloud -- shared by six of the eight icons
# ---------------------------------------------------------------------------

# (dx, dy-above-base, r, squash, draw-interior-contour).
#
# WHAT THE FIRST PASS GOT WRONG
# The nuke frames build every mass by filling a lobe and then stroking its own
# outline black, so each new lobe bites into the one behind it. At 536x240 with
# r=35 lobes and a 5px ink that reads as billowing smoke. Rendered at 96x96
# with r=11 lobes it does not: a 2.4px stroke removes 1.2px from the lobe and
# the neighbour removes another 2.4, so any pair overlapping by less than ~4px
# comes apart. The contact sheet showed six clouds that had each fallen into a
# heap of six separate pebbles.
#
# So the cloud is inked the way a comic actually inks a cloud: the SILHOUETTE
# is one unbroken mass, and the billows are suggested by a few interior
# scallop lines that stop before they reach the edge. cloud() fills the union
# first, then draws black arcs only along the parts of a lobe's contour that
# lie safely inside the rest of the mass. The silhouette can no longer be cut,
# whatever the lobe layout does.
#
# WHICH LOBES GET A CONTOUR, AND WHY IT IS ONLY THE UPPER ONES
# The pass that first drew interior arcs put them on the two flat underside
# lobes. The part of a squashed lobe that lies inside the mass is its TOP arc,
# which is convex UPWARD -- and two long shallow upward curves side by side in
# the middle of a white mass read, unmistakably, as a pair of closed eyes.
# Every cloud on the sheet had a face. The arcs have to be the BOTTOM edges of
# the upper lobes instead: convex downward, which is what the underside of a
# billow overlapping the billow behind it actually looks like.
#
# WHY THE LOBES CASCADE INSTEAD OF SITTING ON A CROWN AND TWO SHOULDERS
# A symmetric crown-plus-shoulders layout bottoms every lobe out at nearly the
# same height, so the three scallops came out as a level row of shallow curves
# -- two of them side by side still read as eyelids and the long one under the
# storm cloud read as a mouth. Staggering the lobe centres in BOTH axes
# staggers the arcs with them: they now step down and outward from the crown,
# which is how overlapping billows actually stack.
CLOUD_SPEC = [
	(-16.0, -14.5, 10.5, 0.92, True),   # upper-left billow
	(1.0, -20.5, 13.0, 0.94, True),     # crown, tallest and off-centre
	(18.0, -12.5, 11.0, 0.90, True),    # right billow, lower than the left
	(27.0, -6.0, 8.0, 0.92, False),     # tail; no mirror of it on the left
	(-15.0, -5.5, 15.5, 0.50, False),   # underside billows: silhouette only
	(13.5, -5.0, 16.0, 0.48, False),
]


def _interior_runs(pts, others_mask, ink):
	"""The single longest stretch of a lobe's BOTTOM arc that lies safely
	inside the rest of the mass.

	Two restrictions, both learned from the contact sheet:

	Bottom arc only. Taking every part of the contour that happened to be
	inside the mass let the line wrap up around the lobe's flanks, and the
	resulting hooks curled back on themselves -- three clouds in a row looked
	like they had a cursive letter inked into them. lobe_pts walks theta from
	0, and screen y grows downward, so theta in (0, pi) is the lower half; the
	window is trimmed further at both ends to keep the curve shallow.

	Longest run only. Several short fragments of the same arc read as dashes.

	`others_mask` has already been eroded by half the stroke width, so a run
	that survives can be stroked without any ink reaching the outside of the
	cloud. The silhouette therefore cannot be cut, whatever the lobe layout
	does.
	"""
	n = len(pts)
	lo, hi = int(n * 0.12), int(n * 0.38)      # theta ~ 0.24pi .. 0.76pi
	m = others_mask.load()
	lim = W * SS - 1
	best, cur = [], []
	for i in range(lo, hi + 1):
		x, y = pts[i]
		px, py = int(x * SS), int(y * SS)
		ok = 0 <= px <= lim and 0 <= py <= lim and m[px, py] > 127
		if ok:
			cur.append(pts[i])
		else:
			if len(cur) > len(best):
				best = cur
			cur = []
	if len(cur) > len(best):
		best = cur
	return [best] if len(best) >= 6 else []


def cloud(d, cx, base_y, s=1.0, seed0=200, ink=INK, flash=False, halo=False):
	"""Cumulus mass. `base_y` is where the underside sits, not the centre.

	`halo` lays a black ring outside the whole silhouette first, for the icons
	where the cloud has to sit in front of something else -- PARTLY's sun, the
	back cloud in CLOUDY. Without it the two white masses merge into one blob.

	`flash` drops the interior contours so the cloud goes solid. That is the
	lightning frame: losing the scallops reads as the whole cloud lighting up,
	which is cheaper and far clearer than any 1bpp attempt at a glow.
	"""
	lobes = [(lobe_pts(cx + dx * s, base_y + dy * s, r * s, seed0 + k,
	                   wob=0.14, squash=sq), con)
	         for k, (dx, dy, r, sq, con) in enumerate(CLOUD_SPEC)]

	if halo:
		w = _ink_w(ink) * 2
		for (pts, _) in lobes:
			p = _px(pts)
			d.line(p + [p[0]], fill=0, width=w, joint="curve")

	for (pts, _) in lobes:
		d.polygon(_px(pts), fill=255)

	if flash:
		return

	ink = ink * max(0.72, s)                   # a small cloud needs a small pen
	erode = _ink_w(ink) | 1                    # MinFilter needs an odd kernel
	for k, (pts, con) in enumerate(lobes):
		if not con:
			continue
		others = Image.new("L", (W * SS, H * SS), 0)
		od = ImageDraw.Draw(others)
		for j, (p2, _) in enumerate(lobes):
			if j != k:
				od.polygon(_px(p2), fill=255)
		others = others.filter(ImageFilter.MinFilter(erode))
		for run in _interior_runs(pts, others, ink):
			d.line(_px(run), fill=0, width=_ink_w(ink), joint="curve")


# ---------------------------------------------------------------------------
# the sun
# ---------------------------------------------------------------------------

def sun(d, cx, cy, r, phase, n_rays=9, ray_len=13.0, gap=3.2, seed=300,
		ink=INK):
	"""Disc plus detached tapered rays.

	The rays start at r + gap, never touching the disc. The black ring that
	leaves is what makes it read as inked 1950s art rather than a clip-art sun.

	`phase` is i / N_FRAMES. Ray length carries a wave that travels once around
	the disc per loop: cos(TAU * (k / n_rays - phase)). 9 rays against an
	8-frame loop means the crest advances ~1.1 rays per frame, so it shimmers
	round rather than strobing. Being a function of phase alone, frame 8 is
	frame 0 exactly.

	Ray sides are bowed outward by 8% at the midpoint. Straight-sided triangles
	around a disc look like a compass rose.
	"""
	d_ink = 0.0            # the disc sits on black; an outline would be invisible
	blob(d, cx, cy, r, seed, wob=0.055, squash=0.97, ink=d_ink)

	tilt = 0.17            # nothing lines up with the pixel grid
	half = TAU / n_rays * 0.30
	for k in range(n_rays):
		th = tilt + k * TAU / n_rays
		L = ray_len * (1.0 + 0.30 * math.cos(TAU * (k / n_rays - phase)))
		r0 = r + gap
		r1 = r0 + L
		skew = 0.35 * half         # tips lean, so no ray is its own mirror

		def pol(rr, aa):
			return (cx + rr * math.cos(aa), cy + rr * math.sin(aa))

		a = pol(r0, th - half)
		b = pol(r0, th + half)
		tip = pol(r1, th + skew)
		# bowed side midpoints
		m1 = pol((r0 + r1) * 0.5 * 1.03, th + half * 0.62)
		m2 = pol((r0 + r1) * 0.5 * 1.03, th - half * 0.62)
		ink_then_fill(d, [a, b, m1, tip, m2], ink=ink * 0.5)


# ---------------------------------------------------------------------------
# precipitation
# ---------------------------------------------------------------------------

def streak(d, x, y, length, dx, halfw=1.5, ink=INK):
	"""A rain sliver: pointed at both ends, bulged off-centre so it is not a
	parallelogram."""
	pts = [(x, y),
		   (x + dx * 0.30 + halfw, y + length * 0.30),
		   (x + dx, y + length),
		   (x + dx * 0.70 - halfw, y + length * 0.72)]
	ink_then_fill(d, pts, ink * 0.6)


def flake(d, cx, cy, r, rot, ink=INK):
	"""Six-point star.

	Six-fold symmetry is not decoration, it is what makes the tumble loop: the
	flake turns 180 degrees over the eight frames, and 180 is a multiple of 60,
	so frame 8 is pixel-identical to frame 0. Any per-arm jitter would break
	that, so the arms are identical here and the variation lives in per-flake
	size and starting angle instead.
	"""
	pts = []
	for k in range(12):
		rr = r if k % 2 == 0 else r * 0.34
		a = rot + k * TAU / 12
		pts.append((cx + rr * math.cos(a), cy + rr * math.sin(a)))
	ink_then_fill(d, pts, ink * 0.55)


def bolt(d, x, y, s, ink=INK, fork=False):
	"""Lightning. Hand-set zigzag: the two limbs are different lengths and the
	tip is off the vertical, because a symmetric bolt reads as a logo."""
	p = [(9.5, 0.0), (-4.0, 20.0), (4.5, 19.0),
		 (-9.0, 44.0), (-2.0, 22.5), (-12.0, 23.5)]
	ink_then_fill(d, [(x + px * s, y + py * s) for (px, py) in p], ink)
	if fork:
		q = [(6.0, 12.0), (16.0, 26.0), (10.5, 26.5), (5.0, 17.0)]
		ink_then_fill(d, [(x + px * s, y + py * s) for (px, py) in q], ink)


def fog_band(d, yc, x0, x1, t, wavelen, off, ink=INK):
	"""A drifting bank.

	Thickness is enveloped to zero at both ends so the band has no cut-off, and
	modulated by a sine in x. Sliding `off` by exactly one wavelength over the
	eight frames returns the identical shape, which is what makes the drift
	loop; different wavelengths per band give different apparent speeds off the
	same 8-frame clock.
	"""
	top, bot = [], []
	n = 64
	for i in range(n + 1):
		u = i / n
		x = x0 + (x1 - x0) * u
		env = (1.0 - (2.0 * u - 1.0) ** 2) ** 0.24
		a = TAU * (x + off) / wavelen
		top.append((x, yc - t * env * (0.74 + 0.26 * math.sin(a))))
		bot.append((x, yc + t * env * (0.74 + 0.26 * math.sin(a + 2.30))))
	ink_then_fill(d, top + bot[::-1], ink)


# ---------------------------------------------------------------------------
# the eight icons
# ---------------------------------------------------------------------------

def draw_clear(d, i):
	sun(d, 48, 48, 18.0, i / N_FRAMES, n_rays=9, ray_len=14.0, gap=3.4)


def draw_partly(d, i):
	# cos, not sin: see draw_cloudy
	ph = TAU * i / N_FRAMES
	sun(d, 36, 29, 13.5, i / N_FRAMES, n_rays=7, ray_len=9.0, gap=2.8,
	    seed=310)
	# Cloud last and haloed, so it reads as being IN FRONT of the sun. Without
	# the halo the two white masses merge and it reads as one lumpy object.
	cloud(d, 52 + 4.0 * math.cos(ph), 72, 0.92, seed0=210, halo=True)


def draw_cloudy(d, i):
	ph = TAU * i / N_FRAMES
	cloud(d, 36 + 4.0 * math.cos(ph), 45, 0.62, seed0=220)
	cloud(d, 51 - 4.0 * math.cos(ph + 0.9), 84, 1.00, seed0=230, halo=True)


def draw_fog(d, i):
	ph = i / N_FRAMES
	# Six thin high-frequency ribbons read as an audio waveform, not as fog.
	# Five thicker banks with a shallower wave read as banks of it, and the
	# ragged run of lengths stops the stack looking like a printed pattern.
	#   yc,  x0,  x1,   t,  wavelen, direction
	spec = [(19, 19, 78, 3.2, 38.0, +1),
	        (35, 5, 92, 4.2, 52.0, -1),
	        (51, 11, 88, 3.8, 33.0, +1),
	        (67, 7, 85, 4.0, 44.0, -1),
	        (82, 24, 76, 3.0, 29.0, +1)]
	for (yc, x0, x1, t, wl, dr) in spec:
		fog_band(d, yc, x0, x1, t, wl, dr * ph * wl)


def draw_drizzle(d, i):
	ph = i / N_FRAMES
	# (x, phase). Uneven columns; an evenly spaced row is a pattern, not rain.
	cols = [(29, 0.00), (40, 0.55), (48, 0.22), (58, 0.78), (67, 0.38),
			(34, 0.68), (62, 0.10)]
	for k, (x, p0) in enumerate(cols):
		t = (p0 + ph) % 1.0
		# Radius goes to zero at both ends of the fall, so a drop fades in and
		# out instead of popping. That taper IS the intermittency -- a hard
		# on/off mask blinks.
		r = 3.6 * math.sin(math.pi * t)
		if r < 0.7:
			continue
		y = 50 + t * 44
		blob(d, x + 1.5 * math.sin(TAU * t + k), y, r, 400 + k,
			 wob=0.22, squash=1.05, ink=0.0)
	cloud(d, 48, 46, 1.00, seed0=240)


def draw_rain(d, i):
	ph = i / N_FRAMES
	cols = [(27, 0.00), (37, 0.50), (46, 0.15), (56, 0.65), (66, 0.32),
			(32, 0.80), (61, 0.42)]
	for (x, p0) in cols:
		for sub in (0.0, 0.5):
			t = (p0 + sub + ph) % 1.0
			y = 34 + t * 62          # starts behind the cloud, exits off-tile
			streak(d, x, y, 15.0, -3.4)
	# Drawn last: the streaks emerge from behind it rather than popping in.
	cloud(d, 48, 46, 1.00, seed0=250)


def draw_snow(d, i):
	ph = i / N_FRAMES
	# (x, phase, radius, base rotation)
	spec = [(29, 0.00, 6.4, 0.0), (44, 0.42, 5.6, 0.5), (58, 0.18, 6.8, 1.1),
			(68, 0.72, 5.2, 0.3), (36, 0.60, 5.0, 0.8)]
	for (x, p0, r, r0) in spec:
		t = (p0 + ph) % 1.0
		y = 36 + t * 62
		sway = 4.5 * math.sin(TAU * t + r0 * 3.0)
		flake(d, x + sway, y, r, r0 + math.pi * ph)
	cloud(d, 48, 44, 1.00, seed0=260)


def draw_storm(d, i):
	ph = i / N_FRAMES
	# Light rain on every frame, so the icon is never dead between strikes and
	# never gets confused with CLOUDY.
	for (x, p0) in [(26, 0.10), (70, 0.55), (33, 0.70)]:
		t = (p0 + ph) % 1.0
		streak(d, x, 40 + t * 56, 13.0, -3.0)

	strike = (i == 3)
	leader = (i == 2)
	cloud(d, 48, 47, 1.10, seed0=270, flash=strike)
	if leader:
		# Leader stroke: unforked, on the other side of the cloud, and only
		# slightly shorter. At 0.62 scale it read as a stray tick mark rather
		# than as lightning; two strikes in different places over two frames
		# is what makes it flicker instead of blink.
		bolt(d, 38, 45, 0.86)
	elif strike:
		bolt(d, 54, 42, 1.05, fork=True)


DRAW = [draw_clear, draw_partly, draw_cloudy, draw_fog,
		draw_drizzle, draw_rain, draw_snow, draw_storm]


def render(icon, i):
	"""Rasterise one frame to a 1-bit-valued PIL 'L' image at 96x96."""
	big = Image.new("L", (W * SS, H * SS), 0)
	d = ImageDraw.Draw(big)
	DRAW[icon](d, i)
	small = big.resize((W, H), Image.BOX)
	return small.point(lambda v: 255 if v >= 118 else 0)


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
		f.write("/* GENERATED by tools/gen_wx_icons.py -- do not edit.\n"
				" * %d icons x %d frames, %dx%d, LVGL I1 (1bpp indexed),\n"
				" * %d bytes each (8 palette bytes + %d bitmap) = %d bytes total.\n"
				" *\n"
				" * The 8 palette bytes are mandatory. lv_draw_buf_goto_xy()\n"
				" * unconditionally skips LV_COLOR_INDEXED_PALETTE_SIZE(cf) * 4\n"
				" * bytes before the pixels; omit them and every frame renders\n"
				" * shifted. data_size includes them.\n"
				" *\n"
				" * I1 and not A1: LVGL 9.5's software renderer has no A1 blitter,\n"
				" * so an A1 image links clean and draws nothing.\n"
				" *\n"
				" * Set bits blit as opaque white and clear bits as opaque black,\n"
				" * so the gaps between lobes ARE the comic outline. */\n"
				% (len(ICON_NAMES), N_FRAMES, W, H, per, STRIDE * H,
				   per * len(ICON_NAMES) * N_FRAMES))
		f.write('#include "lvgl.h"\n')
		f.write('#include "vaultweather.h"\n\n')
		f.write("/* The table is indexed by wx_icon_t. If the enum grows, this\n"
				" * generator has to grow with it -- fail the build rather than\n"
				" * hand the UI a half-filled array. */\n")
		f.write("_Static_assert(WX_ICON_COUNT == %d,\n"
				"               \"wx_icon_t changed; re-run tools/gen_wx_icons.py\");\n\n"
				% len(ICON_NAMES))

		for ic, name in enumerate(ICON_NAMES):
			for fr in range(N_FRAMES):
				b = packed[ic][fr]
				f.write("static const uint8_t wxi_%s_%d[] = {\n" %
						(name.lower(), fr))
				for off in range(0, len(b), 24):
					f.write("\t" + ",".join(str(v) for v in b[off:off + 24]) + ",\n")
				f.write("};\n")
			f.write("\n")

		f.write("const lv_image_dsc_t wx_icon_frames[WX_ICON_COUNT][%d] = {\n"
				% N_FRAMES)
		for ic, name in enumerate(ICON_NAMES):
			f.write("\t{ /* %s */\n" % name)
			for fr in range(N_FRAMES):
				f.write("\t\t{ .header = { .magic = LV_IMAGE_HEADER_MAGIC,"
						" .cf = LV_COLOR_FORMAT_I1, .w = %d, .h = %d,"
						" .stride = %d },\n"
						"\t\t  .data_size = sizeof(wxi_%s_%d),"
						" .data = wxi_%s_%d },\n"
						% (W, H, STRIDE, name.lower(), fr, name.lower(), fr))
			f.write("\t},\n")
		f.write("};\n\n")

		f.write("const int wx_icon_frame_count[WX_ICON_COUNT] = {\n")
		for name in ICON_NAMES:
			f.write("\t%d,  /* %s */\n" % (N_FRAMES, name))
		f.write("};\n")


def _font(size):
	for p in ("/System/Library/Fonts/Supplemental/Arial Bold.ttf",
			  "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"):
		try:
			from PIL import ImageFont
			return ImageFont.truetype(p, size)
		except Exception:
			continue
	from PIL import ImageFont
	return ImageFont.load_default()


def write_sheet(path, packed):
	"""White on black, exactly as the panel shows them."""
	lab_w, pad, cell = 84, 5, W
	sw = lab_w + N_FRAMES * (cell + pad) + pad
	sh = 24 + len(ICON_NAMES) * (cell + pad) + pad
	sheet = Image.new("L", (sw, sh), 0)
	d = ImageDraw.Draw(sheet)
	fnt = _font(15)
	small = _font(11)
	for fr in range(N_FRAMES):
		d.text((lab_w + fr * (cell + pad) + cell // 2 - 8, 6),
			   "f%d" % fr, fill=140, font=small)
	for ic, name in enumerate(ICON_NAMES):
		y = 24 + ic * (cell + pad)
		d.text((6, y + cell // 2 - 8), name, fill=255, font=fnt)
		for fr in range(N_FRAMES):
			sheet.paste(unpack_i1(packed[ic][fr]),
						(lab_w + fr * (cell + pad), y))
	sheet.save(path)


def loop_report(frames):
	"""Mean absolute pixel delta between adjacent frames vs. the 7->0 seam.

	A seam much larger than the mean is a visible jump. Printing it keeps the
	'seamless' claim measurable instead of asserted.
	"""
	out = []
	for ic, name in enumerate(ICON_NAMES):
		px = [f.load() for f in frames[ic]]
		def diff(a, b):
			n = 0
			for y in range(H):
				for x in range(W):
					if px[a][x, y] != px[b][x, y]:
						n += 1
			return n
		steps = [diff(k, k + 1) for k in range(N_FRAMES - 1)]
		seam = diff(N_FRAMES - 1, 0)
		mean = sum(steps) / len(steps)
		out.append((name, mean, seam, max(steps)))
	return out


def main():
	ap = argparse.ArgumentParser()
	ap.add_argument("--out", default="main/wx_icons.c")
	ap.add_argument("--sheet")
	a = ap.parse_args()

	frames = [[render(ic, fr) for fr in range(N_FRAMES)]
			  for ic in range(len(ICON_NAMES))]
	packed = [[pack_i1(f) for f in row] for row in frames]

	write_c(a.out, packed)
	per = 8 + STRIDE * H
	total = per * len(ICON_NAMES) * N_FRAMES
	print("wrote %s  %d icons x %d frames, %d bytes each, %d bytes total (%.1f KiB)"
		  % (a.out, len(ICON_NAMES), N_FRAMES, per, total, total / 1024.0))

	if a.sheet:
		write_sheet(a.sheet, packed)
		print("wrote %s" % a.sheet)

	print("%-9s %8s %8s %8s" % ("icon", "mean d", "7->0", "max d"))
	for (name, mean, seam, mx) in loop_report(frames):
		flag = "  <-- SEAM JUMP" if seam > mean * 1.9 and seam > 140 else ""
		print("%-9s %8.0f %8d %8d%s" % (name, mean, seam, mx, flag))


if __name__ == "__main__":
	main()
