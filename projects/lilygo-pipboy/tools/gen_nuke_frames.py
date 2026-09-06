#!/usr/bin/env python3
"""
Generate the mushroom-cloud animation frames as LVGL I1 (1-bit) images.

WHY FRAMES AND NOT SHAPES
Three earlier attempts assembled the explosion from LVGL primitives at runtime
-- an arc, then tweened circles, then resized lobes. All three read as UI
widgets, because a circle drawn by a widget toolkit has a mathematically
perfect edge and nothing in 1950s comic art does. Frame art fixes that at the
source: every outline here is perturbed by summed sine harmonics, so no two
lobes are the same shape and none of them is a circle.

WHY I1 AND NOT A1
A1 was the obvious choice and it is a trap. LV_COLOR_FORMAT_A1 is a valid enum
in every LVGL build, but in 9.5 the SOFTWARE renderer has no blitter for it --
grep the tree and A1 appears only under src/draw/nema_gfx/ and src/draw/vg_lite/,
both hardware accelerators this ESP32-S3 does not have. An A1 image compiles
clean, links clean, and draws nothing at all; the only trace is a runtime
LV_LOG_WARN("Not supported source color format").

I1 is the 1bpp format the software renderer does implement
(LV_DRAW_SW_SUPPORT_I1, default y). Same 16,080 bytes per frame, so the whole
sequence costs ~129KB of flash against a 1MB app partition (measured: the
build reports 0x9a220 used of 0x100000, 40% free WITH the frames in). At
runtime the app swaps an image source; it allocates nothing, which matters
because building LVGL objects from app_main is what overflowed the main task
stack earlier in this project.

I1 is INDEXED, not alpha -- but LVGL 9.5's i1_image_blend never reads the
palette. It does `chan_val = get_bit(...) * 255` and pushes that straight
through l8_to_rgb565. So a set bit is opaque white and a clear bit is opaque
black, which is exactly a black-and-white comic panel and needs no recolour.

The 8 palette bytes are still MANDATORY even though nothing reads them.
lv_draw_buf_goto_xy() unconditionally skips
LV_COLOR_INDEXED_PALETTE_SIZE(cf) * sizeof(lv_color32_t) bytes before the
pixels. Omit them and every frame renders shifted by 8 bytes -- one and a
fifth rows of garbage.

The cloud is the SET bits. The black gaps between overlapping lobes ARE the
heavy comic outlines: the ink is the background.

WHAT THE FIRST FRAME SET GOT WRONG
Rendering the frames to a contact sheet showed three things the drawing code
was doing that no comic artist does:
  * the stem was a straight-sided box -- two perfectly vertical edges, the most
    geometric thing that could possibly appear in the picture;
  * the condensation ring was a true ellipse with a true elliptical hole;
  * the cap was a shallow symmetric crescent rather than a cauliflower dome.
All three are fixed below: the stem's edges wander independently, the ring is a
garland of overlapping billows that merely sits on an ellipse, and the cap is
built as a packed dome. Nothing is mirror-symmetric on purpose -- the +x and -x
lobe positions are deliberately unequal.

Run:  python3 tools/gen_nuke_frames.py
Writes: main/nuke_frames.c
"""
import math
import random
from PIL import Image, ImageDraw

W, H = 536, 240
CX = W // 2
GROUND = 226          # y of the ground line (billows run off the panel)

# The stem is drawn down to here, not to GROUND. Its bottom edge is a straight
# horizontal line, and INK outlines it -- ending the column at GROUND drew a
# hard rectangle across the base of the picture. Running it past the panel edge
# puts that straight edge where no one can see it.
STEM_BOTTOM = H + 26
N_FRAMES = 8

# Ink width in pixels. This is the black gap between lobes, so it is the single
# strongest control over how "drawn" the result looks. Thin reads as vector art.
INK = 5


def blob(d, cx, cy, r, seed, wob=0.13, squash=0.92):
    """A lobe that is deliberately not a circle.

    Radius is modulated by two sine harmonics at incommensurate frequencies,
    so the outline wanders the way an inked contour does. Same seed gives the
    same lobe across frames, which keeps the cloud coherent as it grows rather
    than boiling randomly.

    The amplitude matters more than it looks. A first pass used wob=0.20 plus
    an 8th harmonic; summed, that swings the radius between 0.62r and 1.38r
    six to eight times around the circle, and every lobe came out as a spiky
    maple leaf. Two harmonics at 0.13 keep the deviation under 20% -- enough
    that no edge is ever a true arc, little enough that it still reads as a
    billow of smoke.
    """
    rnd = random.Random(seed)
    p1, p2 = (rnd.uniform(0, 6.283) for _ in range(2))
    pts = []
    for i in range(96):
        th = i * 2 * math.pi / 96
        rr = r * (1.0
                  + wob * math.sin(3 * th + p1)
                  + wob * 0.50 * math.sin(5 * th + p2))
        pts.append((cx + rr * math.cos(th), cy + rr * math.sin(th) * squash))
    d.polygon(pts, fill=1)
    d.line(pts + [pts[0]], fill=0, width=INK, joint="curve")


def stem(d, top_y, half_w, seed=90):
    """The column. Both edges wander INDEPENDENTLY and the base flares.

    The first version of this drew a constant-taper polygon, which came out as
    a rectangle once the ground billows covered its ends -- two dead-straight
    vertical lines through the middle of the picture. Two different phase
    offsets mean the left and right edges never mirror each other, so the
    column bulges and pinches like smoke instead of like a column.

    Frequency is as important as independence. At six cycles over the length
    the two edges crossed each other and the stem drew as a lightning bolt;
    about one and a half slow cycles gives a bulge instead of a zigzag.
    """
    rnd = random.Random(seed)
    pl, pr = rnd.uniform(0, 6.283), rnd.uniform(0, 6.283)
    left, right = [], []
    n = 34
    for i in range(n + 1):
        t = i / n
        y = top_y + (STEM_BOTTOM - top_y) * t
        flare = 0.62 + 0.55 * t * t          # narrow at the neck, wide at the base
        wl = half_w * flare * (1 + 0.20 * math.sin(1.6 * t * 6.283 + pl)
                                 + 0.08 * math.sin(3.1 * t * 6.283 + pl))
        wr = half_w * flare * (1 + 0.20 * math.sin(1.3 * t * 6.283 + pr)
                                 + 0.08 * math.sin(2.7 * t * 6.283 + pr))
        left.append((CX - 4 - wl, y))
        right.append((CX + 4 + wr, y))
    pts = left + right[::-1]
    d.polygon(pts, fill=1)
    d.line(pts + [pts[0]], fill=0, width=INK, joint="curve")


def collar(d, cy, rx, ry, scale, seed0=70):
    """The condensation skirt as a GARLAND OF BILLOWS, not a torus.

    An outlined ellipse with an elliptical hole is a geometric shape and looked
    like one. A row of lobes that merely happens to droop along an ellipse
    reads as cloud, and the black gaps where they overlap give it the ink.
    """
    n = 7
    for i in range(n):
        u = -1.0 + 2.0 * i / (n - 1)                 # -1 .. 1 across the skirt
        x = CX + rx * u * (1.0 + 0.05 * math.sin(3.0 * u))   # uneven spacing
        y = cy + ry * (1.0 - u * u) * 0.85 - ry * 0.25       # droops at the ends
        r = (17.0 + 11.0 * (1.0 - abs(u))) * scale
        blob(d, x, y, r, seed0 + i, wob=0.15, squash=0.85)


def cap(d, cy, scale, seed0=10):
    """Cauliflower dome: lobes packed into a dome envelope in three courses,
    drawn back to front so later outlines cut into earlier fills.

    The +x and -x entries are deliberately NOT mirrored. A symmetric cloud
    reads as a diagram of a cloud.
    """
    spec = [
        # lower course -- the wide underside of the dome
        (-140, 18, 31), (-76, 25, 36), (-6, 28, 34), (62, 24, 36), (128, 15, 29),
        # middle course
        (-102, -6, 35), (-32, -11, 41), (38, -8, 39), (104, -1, 33),
        # crown
        (-50, -35, 33), (20, -40, 36), (80, -26, 29),
    ]
    for i, (dx, dy, r) in enumerate(spec):
        blob(d, CX + dx * scale, cy + dy * scale, r * scale, seed0 + i)


def ground(d, scale, seed0=50):
    """Base surge. Irregular sizes and spacing -- an evenly spaced row of
    same-size blobs is a pattern, not debris."""
    spec = [(-231, 6, 19), (-172, 10, 24), (-108, 2, 28), (-40, 8, 25),
            (26, 4, 28), (94, 9, 24), (156, 3, 22), (218, 10, 18)]
    for i, (dx, dy, r) in enumerate(spec):
        blob(d, CX + dx * scale, GROUND - 6 + dy, r * scale, seed0 + i, wob=0.15)


def frame(i):
    """Eight keyframes: bomb, flash, burst, column, cap forming, full, drift."""
    img = Image.new("1", (W, H), 0)
    d = ImageDraw.Draw(img)

    if i == 0:                       # bomb falling in
        d.ellipse([CX - 9, 40, CX + 9, 74], fill=1)
        d.polygon([(CX - 9, 44), (CX - 22, 26), (CX - 13, 48)], fill=1)
        d.polygon([(CX + 9, 44), (CX + 22, 26), (CX + 13, 48)], fill=1)
        d.polygon([(CX - 6, 74), (CX + 6, 74), (CX, 88)], fill=1)
        return img

    if i == 1:                       # detonation: the panel goes white
        d.rectangle([0, 0, W, H], fill=1)
        return img

    if i == 2:                       # first burst at the ground
        ground(d, 0.55)
        blob(d, CX - 6, GROUND - 52, 46, 7)
        return img

    if i == 3:                       # column climbing
        ground(d, 0.80)
        stem(d, 134, 22)
        blob(d, CX - 4, 112, 54, 7)
        blob(d, CX + 36, 132, 30, 8)
        return img

    if i == 4:                       # cap forming, skirt appears
        ground(d, 0.95)
        stem(d, 128, 24)
        cap(d, 92, 0.58)
        collar(d, 182, 60, 13, 0.64)
        return img

    if i == 5:                       # billowing out
        ground(d, 1.0)
        stem(d, 118, 27)
        cap(d, 80, 0.80)
        collar(d, 176, 90, 16, 0.82)
        return img

    if i == 6:                       # full cloud
        ground(d, 1.0)
        stem(d, 112, 29)
        cap(d, 70, 0.90)
        collar(d, 174, 112, 18, 0.95)
        return img

    # i == 7: drifting -- cap spreads and rises, skirt widens and thins
    ground(d, 0.96)
    stem(d, 106, 28)
    cap(d, 66, 0.94)
    collar(d, 172, 126, 15, 0.88)
    blob(d, CX + 196, 74, 22, 33, wob=0.16)   # a puff torn off downwind
    return img


# lv_color32_t is {blue, green, red, alpha}. Index 0 black, index 1 white.
# LVGL 9.5's RGB565 I1 blend ignores these, but lv_draw_buf_goto_xy() skips
# over them regardless, so the bytes have to be here and have to be 8 of them.
PALETTE = bytes([0, 0, 0, 255,  255, 255, 255, 255])


def pack_i1(img):
    """LVGL I1: 8 palette bytes, then rows padded to whole bytes, MSB leftmost.

    MSB-first is not a style choice -- it is what the blitter does:
        get_bit(buf, i) -> (buf[i / 8] >> (7 - (i % 8))) & 1
    """
    px = img.load()
    stride = (W + 7) // 8
    out = bytearray(stride * H)
    for y in range(H):
        base = y * stride
        for x in range(W):
            if px[x, y]:
                out[base + (x >> 3)] |= 0x80 >> (x & 7)
    return PALETTE + bytes(out)


def main():
    blobs = [pack_i1(frame(i)) for i in range(N_FRAMES)]
    stride = (W + 7) // 8
    with open("main/nuke_frames.c", "w") as f:
        f.write("/* GENERATED by tools/gen_nuke_frames.py -- do not edit.\n"
                " * %d frames, %dx%d, LVGL I1 (1bpp indexed), %d bytes each\n"
                " * (8 palette bytes + %d bitmap).\n"
                " * Set bits blit as opaque white, clear bits as opaque black, so\n"
                " * the gaps between lobes ARE the comic outline. */\n"
                % (N_FRAMES, W, H, 8 + stride * H, stride * H))
        f.write('#include "lvgl.h"\n\n')
        for i, b in enumerate(blobs):
            f.write("static const uint8_t nf%d[] = {" % i)
            f.write(",".join(str(x) for x in b))
            f.write("};\n")
        f.write("\nconst lv_image_dsc_t nuke_frame[%d] = {\n" % N_FRAMES)
        for i in range(N_FRAMES):
            f.write("  { .header = { .magic = LV_IMAGE_HEADER_MAGIC,"
                    " .cf = LV_COLOR_FORMAT_I1, .w = %d, .h = %d, .stride = %d },"
                    " .data_size = sizeof(nf%d), .data = nf%d },\n"
                    % (W, H, stride, i, i))
        f.write("};\nconst int nuke_frame_count = %d;\n" % N_FRAMES)
    print("wrote main/nuke_frames.c  %d frames, %d bytes total"
          % (N_FRAMES, (8 + stride * H) * N_FRAMES))


if __name__ == "__main__":
    main()
