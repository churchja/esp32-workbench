/*
 * wx_ui.c -- everything that reaches the glass.
 *
 * LilyGo T-Display S3 AMOLED Plus, RM67162, 536x240 landscape (main.c has
 * already rotated the panel; this file only ever sees 536 wide by 240 high).
 *
 * =========================================================================
 * LAYOUT ARITHMETIC -- every number below is checked against 536x240.
 * =========================================================================
 * The panel is extremely letterboxed (2.23:1). Nothing here is centred by
 * eye; each element's right and bottom edge is computed and compared to the
 * panel bound in the comment beside it.
 *
 * TEXT WIDTH RULE -- READ THIS BEFORE CHANGING ANY NUMBER BELOW.
 *
 *   lv_font_unscii_16:  N characters = 16*N px wide, 17 px per line
 *   lv_font_unscii_8 :  N characters =  8*N px wide,  9 px per line
 *
 * unscii_16 advances SIXTEEN pixels per character, not eight. Every glyph in
 * managed_components/lvgl__lvgl/src/font/lv_font_unscii_16.c carries
 * .adv_w = 256; LVGL stores adv_w in 1/16 px units, so 256/16 = 16 px. The
 * generator line in that same file reads "--size 16 --font unscii-8.ttf": it
 * is the 8x8 unscii face rendered at 16px, which doubles the advance. Its
 * .line_height is 17. lv_font_unscii_8 carries .adv_w = 128 = 8 px advance,
 * .line_height 9. Neither is scaled -- see the TRANSFORM note below.
 *
 * THIS BLOCK PREVIOUSLY CLAIMED 8 PX PER CHARACTER FOR BOTH FACES. That was
 * wrong, and every character budget derived from it was off by a factor of
 * two. It survived three reviews because the arithmetic was internally
 * consistent; it was the premise that was false, and a consistent derivation
 * from a false premise reads exactly like a correct one. It was finally
 * caught by decoding a screen capture of the running panel and measuring a
 * glyph: "SYNC" occupied about 60px for four characters, not 32.
 *
 * The visible defect it produced: lbl_sync held "SYNC 0M" (7 ch = 112px)
 * inside a 72px-wide label, LVGL word-wrapped it, and the second line "0M"
 * landed on top of lbl_src ("PWS") in the row below. Reported from the room
 * as "a graphic behind the SYNC PWS".
 *
 * Do not shorten or "tidy" a budget in this file. Recompute it.
 *
 * CLIP, DO NOT WRAP. Every fixed-width label here is set to
 * LV_LABEL_LONG_MODE_CLIP, in mk_lbl_w(). Correct arithmetic should make
 * wrapping impossible, but the two failure modes are not equally bad: a
 * clipped label loses characters off its own right edge and stays in its own
 * row, while a wrapped one silently overwrites the row beneath it -- which is
 * the bug above. lbl_toast is the one deliberate exception; it is a
 * multi-line box by design and says so at its definition.
 *
 *   PANEL            536 x 240
 *   ROOT             530 x 236 at (dx, dy), the anti-burn-in offset
 *                    dx max 6 -> 6 + 530 = 536 = panel width   (exact fit)
 *                    dy max 4 -> 4 + 236 = 240 = panel height  (exact fit)
 *
 *   TOP BAR          root-local y 0..56, always visible
 *     clock digits   w22 h40 t4 at x 6, 32, [colon 58..62], 70, 96
 *                    -> right edge 118, y 6..46          (bar is 56) OK
 *     date label     x130 y10 w160, cap 10 ch * 16 = 160 -> 290, y27  OK
 *     obs label      x130 y31 w144, cap  9 ch * 16 = 144 -> 274, y48  OK
 *     temp digits    w14 h27 t3 at x 300, 317, 334, 351
 *                    -> right edge 365, y 15..42                 OK
 *     temp unit      x369 y19, 1 ch = 16                 -> 385  OK
 *     sync label     x396 y10 w128 right-aligned, cap 8 ch = 128 -> 524 OK
 *     src label      x396 y31 w128 right-aligned, cap 3 ch = 48  -> 524 OK
 *     toast          x130 y10 160x34 UNSCII_8, 20 ch/line, <=3 lines
 *                    covers date+obs only              -> 290, y44 OK
 *                    (temp digits start at x300, divider at y54)
 *     divider        x0 y54 w530 h2                      -> y56  OK
 *
 *   The top-right corner is 128px, i.e. EIGHT characters, and that is a hard
 *   bound, not a preference: the bar temperature's "F" ends at x385 and the
 *   root's right margin matches the clock's left one at 6px, so the column
 *   runs x396..524. Nine characters would be 144px and does not fit at any
 *   indent. That is why the staleness word is "OLD" and not "STALE" --
 *   "STALE 47H" is nine characters. See sync_text().
 *
 *   PANEL AREA       root-local y 58..236 -> 178 tall
 *                    containers are 530x178 at (0,58); children below are in
 *                    panel-local coordinates.
 *
 *   PANEL 1 CURRENT
 *     big temp       w30 h60 t6 at x 8, 43, 78, 113
 *                    -> right edge 143, y 8..68                  OK
 *     unit "F"       x147 y32, 1 ch = 16                -> 163, y49   OK
 *     condition      x8 y96 w284, cap 17 ch = 272       -> 280, y113  OK
 *     cond value     x292 y96 w104, cap 6 ch = 96      -> 388, y113  OK
 *                    x292 is NOT arbitrary: the stats rows below use
 *                    "%-7s%s", so every value starts at 180 + 7*16.
 *                    The moon percentage shares that column instead of
 *                    trailing the phase name, which put it at a
 *                    different x for every phase.
 *                    (longest WMO text is 15 ch, longest moon name 12)
 *     feels/humid/   x180 y12 / y40 / y68, w208, cap 13 ch = 208
 *       dew                                             -> 388, y85   OK
 *                    (icon box starts at x396: 8px clear)
 *     icon box       x396 y16, 120x120                  -> 516, y136  OK
 *
 *   PANEL 2 CURRENT+
 *     col A          x8,   rows y 12/40/68/96, w256, cap 16 ch = 256 -> 264 OK
 *     col B          x284, rows y 12/40/68/96, w240, cap 15 ch = 240 -> 524 OK
 *                    gutter 264..284 = 20px; last row ends y113          OK
 *     source note    x8 y130 unscii_8, w272, cap 34 ch = 272 -> 280, y139 OK
 *
 *   Two 16px columns is all this panel can hold: 530 / 16 = 33 characters
 *   across the whole pane, so 16 + gutter + 15 is the packing. The format
 *   strings in render_pane_current_plus() were re-cut to those budgets; the
 *   worst case of each is listed there.
 *
 *   PANEL 3 FORECAST
 *     3 columns      w170 at x 8, 182, 356 -> right edges 178/352/526 OK
 *     column-local: every label is x0 w170 CENTRE, so the centring is a
 *     property of the width and not of a hand-computed x that goes stale
 *     when a font metric turns out to be wrong. Capacity 170/16 = 10 ch
 *     at unscii_16, 170/8 = 21 ch at unscii_8.
 *       day          x0 w170 y0,   3 ch = 48   -> centred, y17        OK
 *       icon box     x37  y18,  96x96          -> 133, y114           OK
 *       hi           x0 w170 y116, 7 ch = 112  -> centred, y133       OK
 *       lo           x0 w170 y134, 7 ch = 112  -> centred, y151       OK
 *       precip       x0 w170 y154 unscii_8, 8 ch = 64 -> centred, y163 OK
 *     attribution    x8 y166 unscii_8, 20 ch = 160 -> 168, y175       OK
 *
 *   SEVERE OVERLAY (screen-local, NOT root-local)
 *     box            x0 y0 536x240 = the full panel                OK
 *     image          centred, frames are exactly 536x240 -> no crop
 *
 *   PORTAL (root-local, covers the whole 530x236 root)
 *     title          full width, centred, y14, 20 ch = 320    -> y31  OK
 *     rule           x15 y38 w500 h2                          -> 515
 *     step 1         x40 y52,  21 ch = 336 -> 376, y69              OK
 *     ssid           x64 y74  w448, cap 28 ch = 448 -> 512, y91     OK
 *     step 2         x40 y104, 20 ch = 320 -> 360, y121             OK
 *     url            x64 y126 w448, cap 28 ch = 448 -> 512, y143    OK
 *     rule           x15 y156 w500 h2
 *     cursor         x20 y168, 16x17 = one unscii_16 cell -> 36     OK
 *     status         x40 y168 w480, cap 30 ch = 480 -> 520, y185    OK
 *     hint           x40 y200 unscii_8, 40 ch = 320 -> 360, y209    OK
 *
 *   28 characters is the widest the SSID and URL rows can be: they are
 *   indented to x64 under their step, and 530 - 64 - 6 = 460px = 28 ch. A
 *   32-character SSID (the 802.11 maximum) is 512px and cannot be shown at
 *   16px anywhere on this panel, so it clips at 28. Accepted: the default
 *   SSID is 15 characters and the default URL 18, and dropping this row to
 *   unscii_8 would shrink the single most important string on the setup
 *   screen to 8px tall.
 *
 *   SPLASH (root-local, covers the whole 530x236 root)
 *     title          full width, centred, y80, 17 ch = 272    -> y97  OK
 *     status         full width, centred, y120, cap 30 ch = 480 -> y137 OK
 *
 *   wx_ui_status() caps at 30 characters because that is the narrowest of
 *   its three destinations (the portal status row). Longest string any
 *   caller actually passes is "CONFIG ERASED -- RESTARTING", 27.
 *
 * =========================================================================
 * CONSTRAINTS THIS FILE OBEYS, AND WHY
 * =========================================================================
 * NO TRANSFORM PROPERTIES, ANYWHERE. Not transform_scale, not
 * transform_rotation, not transform_pivot. In the sibling project, pushing
 * bordered objects through LVGL's transform layer wedged the render loop:
 * the app kept running and the panel froze, and three rounds of hardware
 * testing were run against a frozen UI before anyone noticed. Everything
 * that moves here moves by lv_obj_set_pos / lv_obj_set_size.
 *
 * That ban costs us scaled text, and only unscii_8 and unscii_16 are
 * compiled in (CONFIG_LV_FONT_UNSCII_8/16 in sdkconfig.defaults; every
 * Montserrat face is =n). 16px is not "large" on a 240px-high panel, so the
 * clock and the two temperatures are drawn as real seven-segment digits made
 * of plain rectangles. They are also the right thing aesthetically, and they
 * animate for free by hiding and showing segments.
 *
 * ALL OBJECTS ARE BUILT ONCE, IN wx_ui_init. After that this file only ever
 * changes text, position, image source and hidden flags. Creating LVGL
 * objects at runtime from the main task overflowed that task's stack in the
 * sibling project.
 *
 * INVALID NEVER RENDERS AS ZERO. Every wx_val_t goes through fmt_*() below,
 * which emits "--" when valid == false. A barometer showing 0.00 inHg looks
 * like a measurement.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "lvgl.h"

#include "vaultweather.h"

#define TAG "wx_ui"

/* Weather icon frames are declared in vaultweather.h -- they cross a
 * translation-unit boundary, so the declaration belongs in the contract where
 * a generator change becomes a build error rather than a silent type
 * mismatch. Every read still goes through icon_frames_avail(), because an
 * icon that failed to generate has a frame count of 0. */

static int icon_frames_avail(wx_icon_t ic)
{
	if ((int)ic < 0 || (int)ic >= WX_ICON_COUNT)
		return 0;
	int n = wx_icon_frame_count[(int)ic];
	if (n < 0)
		n = 0;
	if (n > WX_ICON_MAX_FRAMES)
		n = WX_ICON_MAX_FRAMES;
	return n;
}

/* -------------------------------------------------------------------------
 * Geometry
 * ---------------------------------------------------------------------- */

#define PANEL_W        536
#define PANEL_H        240

#define ROOT_W         530
#define ROOT_H         236

#define BAR_H           56
#define AREA_Y          58
#define AREA_H         178      /* 236 - 58 */

#define ICON_ANIM_MS   150
#define PANEL_DWELL_S    8
#define N_PANELS         3
#define TOAST_TICKS      4      /* wx_ui_tick is 1Hz, so ~4s on screen */

/* Anti-burn-in. This is a real AMOLED and the top bar -- clock, date, temp
 * frame -- is on screen every second of every day for months. Organic
 * emitters age in proportion to accumulated drive, so a permanently-lit glyph
 * edge burns a permanent ghost. Moving the entire root a few pixels spreads
 * that wear over a larger set of subpixels.
 *
 * Six distinct offsets, no two consecutive entries equal (including the wrap
 * from the last back to the first) -- a sequence that repeated an offset back
 * to back would leave those pixels lit twice as long as the rest, which is
 * the thing being avoided. dx <= 6 and dy <= 4 keeps 530x236 inside 536x240
 * at every step; see the arithmetic block at the top.
 *
 * One step every 40s -> 4 minutes for the full cycle. Deliberately not a
 * divisor of 60, so the shift does not land on the same instant as the
 * once-a-minute clock redraw every time. */
static const uint8_t shift_x[] = { 0, 4, 2, 6, 1, 5 };
static const uint8_t shift_y[] = { 0, 2, 4, 1, 3, 4 };
#define N_SHIFT        (sizeof(shift_x) / sizeof(shift_x[0]))
#define SHIFT_PERIOD_S  40

/* -------------------------------------------------------------------------
 * Seven-segment digits
 *
 * Segment order is the standard A..G: A top, B upper-right, C lower-right,
 * D bottom, E lower-left, F upper-left, G middle.
 *
 *      AAAA
 *     F    B
 *     F    B
 *      GGGG
 *     E    C
 *     E    C
 *      DDDD
 *
 * A cell of width w, height h and stroke t has vertical segment length
 * vh = (h - 3t) / 2, so h must satisfy (h - 3t) % 2 == 0. All three sizes
 * below are chosen to make that exact rather than rounding.
 * ---------------------------------------------------------------------- */

typedef struct {
	int16_t w, h, t;
} seg_geom_t;

/* CLK: 22x40 t4 -> vh = (40-12)/2 = 14        exact */
static const seg_geom_t GEOM_CLK = { 22, 40, 4 };
/* BAR: 14x27 t3 -> vh = (27- 9)/2 =  9        exact */
static const seg_geom_t GEOM_BAR = { 14, 27, 3 };
/* BIG: 30x60 t6 -> vh = (60-18)/2 = 21        exact */
static const seg_geom_t GEOM_BIG = { 30, 60, 6 };

typedef struct {
	lv_obj_t *seg[7];
	uint8_t   mask;         /* what is currently lit, to skip no-op writes */
	bool      built;
} seg_digit_t;

static uint8_t seg_mask_for(char c)
{
	/* bit0 A, bit1 B, bit2 C, bit3 D, bit4 E, bit5 F, bit6 G */
	switch (c) {
	case '0': return 0x3F;
	case '1': return 0x06;
	case '2': return 0x5B;
	case '3': return 0x4F;
	case '4': return 0x66;
	case '5': return 0x6D;
	case '6': return 0x7D;
	case '7': return 0x07;
	case '8': return 0x7F;
	case '9': return 0x6F;
	case '-': return 0x40;          /* G alone; also our "no reading" glyph */
	default:  return 0x00;          /* space, and anything unexpected */
	}
}

static void seg_digit_build(seg_digit_t *d, lv_obj_t *parent,
			    int x, int y, const seg_geom_t *g,
			    const lv_style_t *style)
{
	const int w = g->w, h = g->h, t = g->t;
	const int vh = (h - 3 * t) / 2;
	const int hl = w - 2 * t;       /* horizontal segment length */

	const int16_t rect[7][4] = {
		{ t,        0,          hl, t  },   /* A */
		{ w - t,    t,          t,  vh },   /* B */
		{ w - t,    2 * t + vh, t,  vh },   /* C */
		{ t,        h - t,      hl, t  },   /* D */
		{ 0,        2 * t + vh, t,  vh },   /* E */
		{ 0,        t,          t,  vh },   /* F */
		{ t,        t + vh,     hl, t  },   /* G */
	};

	for (int i = 0; i < 7; i++) {
		lv_obj_t *s = lv_obj_create(parent);
		/* remove_style_all strips the theme's border, radius, padding
		 * and scroll behaviour in one call. A bare rectangle is what
		 * is wanted, and specifically NOT a bordered one. */
		lv_obj_remove_style_all(s);
		lv_obj_remove_flag(s, LV_OBJ_FLAG_SCROLLABLE);
		lv_obj_add_style(s, style, 0);
		lv_obj_set_pos(s, x + rect[i][0], y + rect[i][1]);
		lv_obj_set_size(s, rect[i][2], rect[i][3]);
		lv_obj_add_flag(s, LV_OBJ_FLAG_HIDDEN);
		d->seg[i] = s;
	}
	d->mask = 0;
	d->built = true;
}

static void seg_digit_set(seg_digit_t *d, char c)
{
	if (!d->built)
		return;
	uint8_t m = seg_mask_for(c);
	if (m == d->mask)
		return;                 /* nothing changed; do not invalidate */
	for (int i = 0; i < 7; i++) {
		if (m & (1u << i))
			lv_obj_remove_flag(d->seg[i], LV_OBJ_FLAG_HIDDEN);
		else
			lv_obj_add_flag(d->seg[i], LV_OBJ_FLAG_HIDDEN);
	}
	d->mask = m;
}

/* Write a right-aligned string across a run of digit cells. Extra cells on
 * the left go blank. */
static void seg_write(seg_digit_t *cells, int n, const char *s)
{
	int len = (int)strlen(s);
	for (int i = 0; i < n; i++) {
		int si = i - (n - len);
		seg_digit_set(&cells[i], (si >= 0 && si < len) ? s[si] : ' ');
	}
}

/* -------------------------------------------------------------------------
 * Styles
 *
 * Colour is carried by shared lv_style_t objects rather than set per object,
 * so the staleness tint is one lv_obj_report_style_change() instead of a walk
 * over ~250 segments and labels every second.
 *
 * The clock keeps its own style and stays green permanently: it is driven by
 * SNTP, not by the weather sync, and tinting it amber because a fetch failed
 * would be a lie about which subsystem is unhealthy.
 * ---------------------------------------------------------------------- */
static lv_style_t st_seg_accent;   /* seven-segment fill, staleness-tinted */
static lv_style_t st_seg_clock;    /* seven-segment fill, always green */
static lv_style_t st_txt_accent;   /* label text, staleness-tinted */
static lv_style_t st_txt_clock;    /* label text, always green */
static lv_style_t st_txt_dim;      /* captions, footnotes, rules */
static lv_style_t st_rule;         /* divider fill */

static lv_color_t accent_now;      /* what the accent styles currently hold */

/* -------------------------------------------------------------------------
 * Object handles -- all created once in wx_ui_init
 * ---------------------------------------------------------------------- */
static bool       ui_ready;

static lv_obj_t  *scr;
static lv_obj_t  *root;
static lv_obj_t  *dimmer;          /* brightness overlay, sibling of root */

static lv_obj_t  *severe_ov;       /* severe-weather takeover, under dimmer */
static lv_obj_t  *img_severe;

static lv_obj_t  *splash;
static lv_obj_t  *lbl_splash_status;

static lv_obj_t  *portal;
static lv_obj_t  *lbl_portal_ssid;
static lv_obj_t  *lbl_portal_url;
static lv_obj_t  *lbl_portal_status;
static lv_obj_t  *portal_cursor;

static lv_obj_t  *main_wrap;       /* top bar + panel area */

/* Top bar */
static seg_digit_t clk[4];
static lv_obj_t  *clk_colon[2];
static seg_digit_t bar_temp[4];    /* [sign][100s][10s][1s] */
static lv_obj_t  *lbl_bar_unit;
static lv_obj_t  *lbl_date;
static lv_obj_t  *lbl_obs;
static lv_obj_t  *lbl_toast;       /* transient status over the date block */
static lv_obj_t  *lbl_sync;
static lv_obj_t  *lbl_src;

/* Panels */
static lv_obj_t  *pane[N_PANELS];

/* Panel 1 */
static seg_digit_t big_temp[4];
static lv_obj_t  *lbl_big_unit;
static lv_obj_t  *lbl_cond;
static lv_obj_t  *lbl_cond_val;   /* right-hand value for the condition row */
static lv_obj_t  *lbl_feels;
static lv_obj_t  *lbl_humid;
static lv_obj_t  *lbl_dew;
static lv_obj_t  *img_cond;

/* Panel 2 */
static lv_obj_t  *lbl_p2a[4];
static lv_obj_t  *lbl_p2b[4];
static lv_obj_t  *lbl_note;

/* Panel 3 */
static lv_obj_t  *lbl_fc_day[WX_FORECAST_DAYS];
static lv_obj_t  *img_fc[WX_FORECAST_DAYS];
static lv_obj_t  *lbl_fc_hi[WX_FORECAST_DAYS];
static lv_obj_t  *lbl_fc_lo[WX_FORECAST_DAYS];
static lv_obj_t  *lbl_fc_pop[WX_FORECAST_DAYS];

/* -------------------------------------------------------------------------
 * Runtime state
 * ---------------------------------------------------------------------- */
typedef enum { MODE_SPLASH = 0, MODE_MAIN, MODE_PORTAL } ui_mode_t;

static ui_mode_t mode = MODE_SPLASH;

static wx_state_t g_st;            /* last pushed state, copied not aliased */
static char       g_note[40];      /* copy of cur_source_note; the fetcher may
                                    * reuse its buffer between calls */
static bool       g_have_state;

static uint32_t   tick_count;
static int        cur_pane;
static int        pane_dwell;
static int        shift_idx;
static int        last_min = -1;
static int        icon_frame;
static wx_icon_t  icon_sel = WX_ICON_UNKNOWN;
static uint8_t    bright = 255;
static int        toast_ticks;

/* 3-hour pressure tendency. Sampled on a fixed 20-minute grid into a 9-slot
 * ring so the comparison is against a reading from roughly three hours ago --
 * the standard meteorological window -- rather than "whatever the previous
 * screen update happened to hold", which would show a trend arrow that
 * flipped with every fetch. */
#define PRES_SLOTS      9
#define PRES_SAMPLE_S   (20 * 60)
/* ~1 hPa over 3h; below this, call the tendency flat */
#define PRES_SIGNIF_IN  0.03f
static float  pres_v[PRES_SLOTS];
static bool   pres_ok[PRES_SLOTS];
static int    pres_head;
static time_t pres_last;

/* -------------------------------------------------------------------------
 * Small helpers
 * ---------------------------------------------------------------------- */

/* Bounded, uppercased copy. Everything on this screen is uppercase: unscii is
 * a terminal face and mixed case reads as a different device. */
static void copy_upper(char *dst, size_t cap, const char *src, size_t max_ch)
{
	size_t lim = (max_ch + 1 < cap) ? max_ch + 1 : cap;
	size_t i = 0;
	if (lim == 0)
		return;
	if (src) {
		for (; src[i] && i + 1 < lim; i++) {
			char c = src[i];
			dst[i] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
		}
	}
	dst[i] = '\0';
}

/* The one place a wx_val_t turns into characters. valid == false yields "--",
 * never a number. */
static void fmt_val(char *buf, size_t cap, wx_val_t v, int dec, const char *suf)
{
	if (!suf)
		suf = "";
	if (!v.valid)
		snprintf(buf, cap, "--%s", suf);
	else
		snprintf(buf, cap, "%.*f%s", dec, (double)v.v, suf);
}

static void fmt_hhmm(char *buf, size_t cap, time_t t)
{
	if (t <= 0) {
		snprintf(buf, cap, "--:--");
		return;
	}
	struct tm tmv;
	localtime_r(&t, &tmv);
	snprintf(buf, cap, "%02d:%02d", tmv.tm_hour, tmv.tm_min);
}

/* 16-point compass. wind_deg is meteorological (the direction wind comes
 * FROM), which is what a station reports and what people expect to read. */
static const char *compass16(float deg)
{
	static const char *pts[16] = {
		"N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE",
		"S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW"
	};
	if (!isfinite(deg))
		return "--";
	float d = fmodf(deg, 360.0f);
	if (d < 0.0f)
		d += 360.0f;
	int i = (int)(d / 22.5f + 0.5f) & 15;
	return pts[i];
}

/* Round to a whole degree and clamp to what the layout has room for.
 * -99..999 covers anything this planet produces plus a stuck sensor; the
 * clamp exists so a garbage reading cannot print a fourth digit and walk off
 * the end of its column. */
static int clamp_temp(float f)
{
	if (!isfinite(f))
		return 0;
	long t = lroundf(f);
	if (t < -99)
		t = -99;
	if (t > 999)
		t = 999;
	return (int)t;
}

/* Drive a run of digit cells from a temperature. Cell 0 is the sign slot.
 * Invalid renders as "--" spanning the two rightmost cells, which is the same
 * convention the text labels use. */
static void seg_set_temp(seg_digit_t *cells, int n, wx_val_t v)
{
	char s[8];
	if (!v.valid) {
		seg_write(cells, n, "--");
		return;
	}
	snprintf(s, sizeof(s), "%d", clamp_temp(v.v));
	seg_write(cells, n, s);
}

/* -------------------------------------------------------------------------
 * Build helpers -- used only from wx_ui_init
 * ---------------------------------------------------------------------- */

/* A bare container: no theme border, no radius, no padding, no scrolling.
 * Padding matters because child coordinates are relative to the content area,
 * and the theme's default padding would silently offset every position in the
 * arithmetic block at the top of this file. */
static lv_obj_t *mk_box(lv_obj_t *parent, int x, int y, int w, int h)
{
	lv_obj_t *o = lv_obj_create(parent);
	lv_obj_remove_style_all(o);
	lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_scrollbar_mode(o, LV_SCROLLBAR_MODE_OFF);
	lv_obj_set_style_pad_all(o, 0, 0);
	lv_obj_set_style_border_width(o, 0, 0);
	lv_obj_set_pos(o, x, y);
	lv_obj_set_size(o, w, h);
	return o;
}

static lv_obj_t *mk_rule(lv_obj_t *parent, int x, int y, int w, int h)
{
	lv_obj_t *o = mk_box(parent, x, y, w, h);
	lv_obj_add_style(o, &st_rule, 0);
	return o;
}

static lv_obj_t *mk_lbl(lv_obj_t *parent, int x, int y, const lv_font_t *font,
			const lv_style_t *style, const char *init)
{
	lv_obj_t *o = lv_label_create(parent);
	lv_obj_remove_style_all(o);
	lv_obj_set_style_text_font(o, font, 0);
	lv_obj_add_style(o, style, 0);
	lv_obj_set_pos(o, x, y);
	lv_label_set_text(o, init ? init : "");
	return o;
}

/* Fixed-width label with an explicit alignment. Used for the right-hand sync
 * column so a shortening string ("SYNC 12M" -> "SYNC 3M") stays flush to the
 * same right edge instead of walking left, and for every label whose content
 * is bounded by a character budget rather than by a compile-time literal.
 *
 * LV_LABEL_LONG_MODE_CLIP is not optional here. A label with an explicit
 * width defaults to LV_LABEL_LONG_MODE_WRAP, and a wrapped label grows DOWN
 * into whatever is beneath it -- that is exactly how "SYNC 0M" ended up
 * printing "0M" on top of the source marker in the row below. Clipping keeps
 * a too-long string inside its own box; the arithmetic block at the top of
 * this file is what is supposed to make it never happen, and this is the
 * belt to that pair of braces. */
static lv_obj_t *mk_lbl_w(lv_obj_t *parent, int x, int y, int w,
			  const lv_font_t *font, const lv_style_t *style,
			  lv_text_align_t align, const char *init)
{
	lv_obj_t *o = mk_lbl(parent, x, y, font, style, init);
	lv_obj_set_width(o, w);
	lv_obj_set_style_text_align(o, align, 0);
	lv_label_set_long_mode(o, LV_LABEL_LONG_MODE_CLIP);
	return o;
}

static void build_styles(void)
{
	lv_style_init(&st_seg_accent);
	lv_style_set_bg_color(&st_seg_accent, WX_GREEN);
	lv_style_set_bg_opa(&st_seg_accent, LV_OPA_COVER);
	lv_style_set_radius(&st_seg_accent, 0);
	lv_style_set_border_width(&st_seg_accent, 0);

	lv_style_init(&st_seg_clock);
	lv_style_set_bg_color(&st_seg_clock, WX_GREEN);
	lv_style_set_bg_opa(&st_seg_clock, LV_OPA_COVER);
	lv_style_set_radius(&st_seg_clock, 0);
	lv_style_set_border_width(&st_seg_clock, 0);

	lv_style_init(&st_txt_accent);
	lv_style_set_text_color(&st_txt_accent, WX_GREEN);

	lv_style_init(&st_txt_clock);
	lv_style_set_text_color(&st_txt_clock, WX_GREEN);

	lv_style_init(&st_txt_dim);
	lv_style_set_text_color(&st_txt_dim, WX_DIM);

	lv_style_init(&st_rule);
	lv_style_set_bg_color(&st_rule, WX_DIM);
	lv_style_set_bg_opa(&st_rule, LV_OPA_COVER);
	lv_style_set_radius(&st_rule, 0);
	lv_style_set_border_width(&st_rule, 0);

	accent_now = WX_GREEN;
}

static void build_topbar(lv_obj_t *p)
{
	/* Clock: 24-hour HH:MM, no seconds. The user chose 24-hour and chose to
	 * drop seconds; dropping them means this -- the largest, most
	 * permanently-lit element on the panel -- redraws 60 times less often,
	 * which is an anti-burn-in decision as much as a legibility one. */
	static const int clk_x[4] = { 6, 32, 70, 96 };
	for (int i = 0; i < 4; i++)
		seg_digit_build(&clk[i], p, clk_x[i], 6, &GEOM_CLK,
				&st_seg_clock);

	/* Colon: two 4x4 blocks at the same stroke width as the digits, at the
	 * heights of the G segment boundaries so it reads as part of the face.
	 * x 58..62, well clear of digit 1 (ends 54) and digit 2 (starts 70). */
	for (int i = 0; i < 2; i++) {
		clk_colon[i] = mk_box(p, 58, 6 + (i ? 28 : 12), 4, 4);
		lv_obj_add_style(clk_colon[i], &st_seg_clock, 0);
	}

	/* "SAT 05 SEP" -- exactly 10 characters, 160px, x130..290, which stops
	 * 10px short of the temperature digits at x300. Fixed width so a
	 * strftime that returned something longer clips instead of wrapping
	 * down onto the OBS row. */
	lbl_date = mk_lbl_w(p, 130, 10, 160, &lv_font_unscii_16, &st_txt_clock,
			    LV_TEXT_ALIGN_LEFT, "--- -- ---");

	/* "OBS 14:32" -- 9 characters, 144px, x130..274.
	 *
	 * This is the timestamp the READING carries, not when we fetched it.
	 * A station that stopped uploading three hours ago still answers an
	 * HTTP request instantly, so fetch time would hide exactly the failure
	 * the user needs to see. */
	lbl_obs = mk_lbl_w(p, 130, 31, 144, &lv_font_unscii_16, &st_txt_accent,
			   LV_TEXT_ALIGN_LEFT, "OBS --:--");

	/* Current temperature: 4 cells, [sign][100s][10s][1s]. -99..999 F
	 * covers anything this planet produces plus sensor faults. */
	/* Pitch 19, not 17. Each cell is GEOM_BAR.w = 14 wide, so a 17 pitch
	 * left a 3px gap and the digits ran together -- a screen capture of
	 * "79F" was genuinely ambiguous at a glance. 19 gives a 5px gap and the
	 * last cell still ends at 353+14 = 367, clear of the "F" at x369. */
	static const int bar_x[4] = { 296, 315, 334, 353 };
	for (int i = 0; i < 4; i++)
		seg_digit_build(&bar_temp[i], p, bar_x[i], 15, &GEOM_BAR,
				&st_seg_accent);
	/* unscii covers ASCII 32..127 only (range_start 32, range_length 96 in
	 * the font source) -- there is no degree sign, so the unit is a bare
	 * "F". Same reason the pressure trend below uses ^ v = and not arrows. */
	lbl_bar_unit = mk_lbl(p, 369, 19, &lv_font_unscii_16, &st_txt_accent,
			      "F");

	/* Right column, right-aligned in x396..524 = 128px = EIGHT characters.
	 *
	 * The old box was x452..524, 72px, and was budgeted as nine characters
	 * on the belief that unscii_16 advanced 8px. It advances 16, so 72px is
	 * four and a half characters and "SYNC 0M" wrapped onto the source
	 * marker underneath. See the TEXT WIDTH RULE at the top of this file.
	 *
	 * x396 is the earliest this column can start: the bar temperature's
	 * unit "F" occupies x369..385. x524 is the latest it can end, leaving
	 * the same 6px right margin the clock has on the left. 128px is
	 * therefore the whole budget, and sync_text() is written to eight
	 * characters -- "NO SYNC" 7, "SYNC 59M" 8, "OLD 47H" 7, "OLD 99D" 7. */
	lbl_sync = mk_lbl_w(p, 396, 10, 128, &lv_font_unscii_16, &st_txt_accent,
			    LV_TEXT_ALIGN_RIGHT, "NO SYNC");
	/* Source marker: "PWS", "O-M" or "---". 3 characters = 48px, flush to
	 * the same right edge as the line above it. */
	lbl_src = mk_lbl_w(p, 396, 31, 128, &lv_font_unscii_16, &st_txt_accent,
			   LV_TEXT_ALIGN_RIGHT, "---");

	/* Transient status, shown by wx_ui_status() while the main view is up.
	 *
	 * main.c calls wx_ui_status("REFRESHING...") on a long button press,
	 * and the header scopes that function to the splash screen -- which is
	 * hidden by then, so the press would produce no feedback at all. This
	 * covers exactly the date and OBS block (x130..290, y10..44, clear of
	 * the temp digits at x300) with an opaque black patch for a few
	 * seconds. Those two are the least time-critical things in the bar; the
	 * clock, the temperature and the sync age are never occluded.
	 *
	 * UNSCII_8, not unscii_16, and that is forced by arithmetic rather than
	 * chosen for looks. The box can only be 160 wide (x130..290, because
	 * the temperature digits start at x300) and 34 tall (y10..44, because
	 * the divider is at y54). At 16px per character that is 10 characters
	 * over 2 lines = 20, and the longest message a caller passes is
	 * "CONFIG ERASED -- RESTARTING" at 27 -- it would have been cut in half.
	 * At 8px it is 20 characters over 3 lines = 60, and the 30-character
	 * cap in wx_ui_status() lands on two lines with room to spare.
	 *
	 * This is the ONE label that keeps LV_LABEL_LONG_MODE_WRAP. It is a
	 * multi-line box on purpose, it is opaque, and it is drawn over the two
	 * rows it is allowed to cover, so wrapping cannot damage anything
	 * underneath -- the 30-char cap bounds it to 2 of its 3 available
	 * lines. Stated explicitly rather than left to the default so that the
	 * next person auditing wrap modes in this file sees it was deliberate. */
	lbl_toast = mk_lbl(p, 130, 10, &lv_font_unscii_8, &st_txt_accent, "");
	lv_obj_set_size(lbl_toast, 160, 34);
	lv_label_set_long_mode(lbl_toast, LV_LABEL_LONG_MODE_WRAP);
	lv_obj_set_style_bg_color(lbl_toast, WX_BLACK, 0);
	lv_obj_set_style_bg_opa(lbl_toast, LV_OPA_COVER, 0);
	lv_obj_add_flag(lbl_toast, LV_OBJ_FLAG_HIDDEN);

	mk_rule(p, 0, 54, ROOT_W, 2);
}

static void build_pane_current(lv_obj_t *p)
{
	static const int big_x[4] = { 8, 43, 78, 113 };
	for (int i = 0; i < 4; i++)
		seg_digit_build(&big_temp[i], p, big_x[i], 8, &GEOM_BIG,
				&st_seg_accent);
	lbl_big_unit = mk_lbl(p, 147, 32, &lv_font_unscii_16, &st_txt_accent,
			      "F");

	/* Condition row. It is a FOURTH ROW of the stats block, not a caption
	 * floating under the temperature -- so its value shares the column the
	 * other three values sit in.
	 *
	 * The stats rows below use "%-7s%s": a 7-character caption then the
	 * value, so every value starts at 180 + 7*16 = x292. The moon
	 * percentage has to land on that same x or it reads as debris rather
	 * than as data. It was previously baked into one string
	 * ("WANING CRESCENT 31%"), which put the number wherever the name
	 * happened to end -- different for every phase.
	 *
	 * y96 continues the 28px row rhythm of the column above (12, 40, 68).
	 *
	 * Name: x8, width 284 -> x292, so it can never reach the value column.
	 * 284px is 17 characters; the longest WMO string is 15 ("FRZ DRIZZLE
	 * HVY", "VIOLENT SHOWERS", "STORM + LG HAIL", "HEAVY SNOW SHWR") and the
	 * longest moon name is 15 ("WAXING CRESCENT"), so 17 is slack, not a
	 * limit. The copy is still bounded: the table is free to grow. */
	lbl_cond = mk_lbl_w(p, 8, 96, 284, &lv_font_unscii_16, &st_txt_accent,
			    LV_TEXT_ALIGN_LEFT, "--");
	/* Value: x292, width 104 -> x396, stopping exactly at the icon box.
	 * 6 characters; the longest content is "100%". */
	lbl_cond_val = mk_lbl_w(p, 292, 96, 104, &lv_font_unscii_16,
				&st_txt_accent, LV_TEXT_ALIGN_LEFT, "");

	/* Stats column, cap 13 characters = 208px (x180..388).
	 * Format is "%-7s%s": 7-char caption then the value, so the numbers
	 * line up in a column without a second label per row.
	 *
	 * 13 is set by the icon box at x396, not by the content: 396 - 180 = 216
	 * and 13 characters is 208, leaving 8px clear. The budget the old
	 * comment claimed (14 ch) was computed at 8px per character and would
	 * have run to x404, i.e. straight into the icon. fmt_val() puts no
	 * upper bound on a value's digits -- a stuck sensor reading 12345.6
	 * prints "12346F" -- so the width is what actually enforces this. */
	lbl_feels = mk_lbl_w(p, 180, 12, 208, &lv_font_unscii_16, &st_txt_accent,
			     LV_TEXT_ALIGN_LEFT, "FEELS  --");
	lbl_humid = mk_lbl_w(p, 180, 40, 208, &lv_font_unscii_16, &st_txt_accent,
			     LV_TEXT_ALIGN_LEFT, "HUMID  --");
	lbl_dew   = mk_lbl_w(p, 180, 68, 208, &lv_font_unscii_16, &st_txt_accent,
			     LV_TEXT_ALIGN_LEFT, "DEW    --");

	/* Icon box, 120x120 at x396 y16 -> 516, 136. The generated frames are
	 * expected at 96x96 or smaller; the box is oversized so a larger asset
	 * still centres instead of being clipped by the parent, and LVGL clips
	 * children to the parent by default. */
	lv_obj_t *box = mk_box(p, 396, 16, 120, 120);
	img_cond = lv_image_create(box);
	lv_obj_remove_style_all(img_cond);
	lv_obj_center(img_cond);
	lv_obj_add_flag(img_cond, LV_OBJ_FLAG_HIDDEN);
}

static void build_pane_current_plus(lv_obj_t *p)
{
	/* Two columns. The pane is 530 wide and unscii_16 is 16px per character,
	 * so the entire row is 33 characters -- that is the budget, and the
	 * packing is 16 + gutter + 15:
	 *
	 *   col A  x8   w256  cap 16 ch -> x264
	 *   gutter                          20px
	 *   col B  x284 w240  cap 15 ch -> x524   (6px right margin, as the bar)
	 *
	 * The old comment budgeted 22 characters per column at 8px = 176px. At
	 * the true metric 22 characters is 352px: col A would have reached x362,
	 * printing straight through col B at x280, and col B would have reached
	 * x632 on a 530px pane. Neither column was ever going to be legible.
	 * render_pane_current_plus() lists the worst case of each row. */
	static const int row_y[4] = { 12, 40, 68, 96 };
	for (int i = 0; i < 4; i++) {
		lbl_p2a[i] = mk_lbl_w(p, 8, row_y[i], 256,
				      &lv_font_unscii_16, &st_txt_accent,
				      LV_TEXT_ALIGN_LEFT, "");
		lbl_p2b[i] = mk_lbl_w(p, 284, row_y[i], 240,
				      &lv_font_unscii_16, &st_txt_accent,
				      LV_TEXT_ALIGN_LEFT, "");
	}

	/* Which tier of the PWS fallback chain answered. unscii_8, cap 34
	 * characters = 272px (x8..280). Dim: it is diagnostic, not a reading,
	 * and should not compete with the numbers above it. */
	lbl_note = mk_lbl_w(p, 8, 130, 272, &lv_font_unscii_8, &st_txt_dim,
			    LV_TEXT_ALIGN_LEFT, "");
}

static void build_pane_forecast(lv_obj_t *p)
{
	static const int col_x[WX_FORECAST_DAYS] = { 8, 182, 356 };

	for (int i = 0; i < WX_FORECAST_DAYS; i++) {
		lv_obj_t *c = mk_box(p, col_x[i], 0, 170, 164);

		/* Every text row below is x0 w170 centre-aligned rather than
		 * placed at a hand-computed x. The previous code centred by
		 * hand -- day at x73, hi/lo at x57 -- and those numbers were
		 * (170 - 8*N)/2, i.e. correct only under the 8px-per-character
		 * belief. At 16px the day sat 12px right of centre and the
		 * hi/lo rows ran to x169, one pixel inside their own column.
		 * Centring on the width makes the placement a property of the
		 * container instead of a property of a font metric someone
		 * remembered, which is the bug this file just had. */

		/* Day abbreviation, 3 characters = 48px in a 170px column. */
		lbl_fc_day[i] = mk_lbl_w(c, 0, 0, 170, &lv_font_unscii_16,
					 &st_txt_accent,
					 LV_TEXT_ALIGN_CENTER, "---");

		lv_obj_t *box = mk_box(c, 37, 18, 96, 96);
		img_fc[i] = lv_image_create(box);
		lv_obj_remove_style_all(img_fc[i]);
		lv_obj_center(img_fc[i]);
		lv_obj_add_flag(img_fc[i], LV_OBJ_FLAG_HIDDEN);

		/* "HI  88F" / "LO -12F" -- 7 characters = 112px of 170. */
		lbl_fc_hi[i] = mk_lbl_w(c, 0, 116, 170, &lv_font_unscii_16,
					&st_txt_accent,
					LV_TEXT_ALIGN_CENTER, "HI   --");
		lbl_fc_lo[i] = mk_lbl_w(c, 0, 134, 170, &lv_font_unscii_16,
					&st_txt_accent,
					LV_TEXT_ALIGN_CENTER, "LO   --");
		/* "RAIN 40%" -- 8 characters at unscii_8 = 64px of 170. */
		lbl_fc_pop[i] = mk_lbl_w(c, 0, 154, 170, &lv_font_unscii_8,
					 &st_txt_dim,
					 LV_TEXT_ALIGN_CENTER, "RAIN  --");
	}

	/* Open-Meteo's data is CC-BY 4.0. Visible attribution is a licence
	 * condition, not decoration -- do not remove this because it is small
	 * or because the panel looks cleaner without it. 20 characters at
	 * unscii_8 = 160px (x8..168), y166..175, inside the 178px panel. */
	mk_lbl(p, 8, 166, &lv_font_unscii_8, &st_txt_dim,
	       "WX: OPEN-METEO CC-BY");
}

static void build_portal(lv_obj_t *parent)
{
	portal = mk_box(parent, 0, 0, ROOT_W, ROOT_H);
	lv_obj_set_style_bg_color(portal, WX_BLACK, 0);
	lv_obj_set_style_bg_opa(portal, LV_OPA_COVER, 0);
	lv_obj_add_flag(portal, LV_OBJ_FLAG_HIDDEN);

	/* Full-width centred title. 20 characters = 320px inside 530, so it
	 * fits at any shift offset. */
	mk_lbl_w(portal, 0, 14, ROOT_W, &lv_font_unscii_16, &st_txt_clock,
		 LV_TEXT_ALIGN_CENTER, "VAULT-TEC SETUP MODE");
	mk_rule(portal, 15, 38, 500, 2);

	/* Numbered, in the order the user does them, with the value they must
	 * read indented under each step. They are standing in front of this
	 * with a phone in hand; two short instructions beat one long one. */
	mk_lbl(portal, 40, 52, &lv_font_unscii_16, &st_txt_dim,
	       "1. JOIN THIS WIFI NET");            /* 21 ch = 336px -> x376 */
	/* SSID, cap 28 characters = 448px (x64..512).
	 *
	 * 28, not 32. The 802.11 limit is 32 and wx_cfg_t.ssid[33] carries it,
	 * but 32 characters is 512px and this row starts at x64 under its step
	 * number -- 530 - 64 - 6 = 460px = 28 characters. A 32-character SSID
	 * clips. The alternative was dropping the one string the user has to
	 * find on their phone to 8px tall, which is worse. */
	lbl_portal_ssid = mk_lbl_w(portal, 64, 74, 448, &lv_font_unscii_16,
				   &st_txt_accent, LV_TEXT_ALIGN_LEFT,
				   WX_PORTAL_SSID);

	mk_lbl(portal, 40, 104, &lv_font_unscii_16, &st_txt_dim,
	       "2. OPEN IN A BROWSER");             /* 20 ch = 320px -> x360 */
	/* URL, cap 28 characters = 448px (x64..512). Same bound as the SSID
	 * row above; the default "http://192.168.4.1" is 18. */
	lbl_portal_url = mk_lbl_w(portal, 64, 126, 448, &lv_font_unscii_16,
				  &st_txt_accent, LV_TEXT_ALIGN_LEFT,
				  "http://192.168.4.1");

	mk_rule(portal, 15, 156, 500, 2);

	/* A block cursor blinking at 1Hz off wx_ui_tick. It is the terminal
	 * idiom, and it is also the only proof on screen that the firmware is
	 * still running while the user waits on a page that has not loaded
	 * yet -- a frozen render loop and a working one look identical on a
	 * static screen.
	 *
	 * 16x17 is one unscii_16 cell, which is what a block cursor is meant to
	 * be. It was 8x17 -- half a cell -- for the same reason everything else
	 * in this file was half width. x20..36, and the status text starts at
	 * x40. */
	portal_cursor = mk_box(portal, 20, 168, 16, 17);
	lv_obj_add_style(portal_cursor, &st_seg_accent, 0);

	/* Status, cap 30 characters = 480px (x40..520). That 30 is where the
	 * cap in wx_ui_status() comes from -- this is the narrowest of its
	 * three destinations. */
	lbl_portal_status = mk_lbl_w(portal, 40, 168, 480, &lv_font_unscii_16,
				     &st_txt_accent, LV_TEXT_ALIGN_LEFT,
				     "AWAITING CONFIGURATION");

	mk_lbl(portal, 40, 200, &lv_font_unscii_8, &st_txt_dim,
	       "THE DISPLAY RETURNS WHEN SETUP COMPLETES");  /* 40 ch = 320px */
}

static void build_splash(lv_obj_t *parent)
{
	splash = mk_box(parent, 0, 0, ROOT_W, ROOT_H);

	mk_lbl_w(splash, 0, 80, ROOT_W, &lv_font_unscii_16, &st_txt_clock,
		 LV_TEXT_ALIGN_CENTER, "VAULT-TEC WEATHER");
	/* Full width, centred, so wx_ui_status() can hand us any length up to
	 * its 30-character cap without the caller doing layout. 530px is 33
	 * characters at unscii_16, so 30 always fits. */
	lbl_splash_status = mk_lbl_w(splash, 0, 120, ROOT_W, &lv_font_unscii_16,
				     &st_txt_accent, LV_TEXT_ALIGN_CENTER,
				     "BOOT");
}

/* -------------------------------------------------------------------------
 * Staleness
 * ---------------------------------------------------------------------- */

/* Accent colour as a function of age. Green at a fresh sync, walking to
 * WX_DIM across the first hour, then hard over to WX_AMBER.
 *
 * The amber step is deliberate rather than a continuation of the fade: a
 * slowly dimming green is still a green screen, and someone glancing at it
 * from across the room reads "fine". Past an hour the data is not fine, so
 * the whole weather half of the display changes hue. Stale data must never
 * look live. */
/* One hour, and ONE constant. The colour and the wording must flip at the
 * same instant: an earlier version faded to amber at 3600s while the text
 * still said "SYNC 60M" until 90 minutes, so for half an hour the screen was
 * shouting and the words were saying everything is fine. */
#define WX_STALE_S 3600

static lv_color_t accent_for_age(time_t age, bool have_sync)
{
	if (!have_sync)
		return WX_AMBER;
	if (age >= WX_STALE_S)
		return WX_AMBER;
	if (age < 0)
		age = 0;
	/* lv_color_mix(c1, c2, mix): mix=255 is all c1. Start all green, end
	 * all dim at the one-hour mark. */
	uint8_t mix = (uint8_t)(255 - (age * 255) / WX_STALE_S);
	return lv_color_mix(WX_GREEN, WX_DIM, mix);
}

/* EIGHT CHARACTERS, HARD. lbl_sync is 128px wide (x396..524) and unscii_16
 * advances 16px per character; see the corner arithmetic in build_topbar().
 * Every branch below is written to that bound and carries its length.
 *
 * The stale branches used to read "STALE %dH" / "STALE %dD". "STALE 47H" is
 * nine characters = 144px and does not fit in this corner at any indent, so
 * the word is "OLD". That is a deliberate wording change, not a typo: the
 * meaning is carried by the word AND by the accent going amber at the same
 * instant, and three characters is what there is room for. */
static void sync_text(char *buf, size_t cap, time_t age, bool have_sync)
{
	if (!have_sync) {
		snprintf(buf, cap, "NO SYNC");            /* 7 ch */
		return;
	}
	if (age < 0)
		age = 0;
	/* The word changes from SYNC to OLD at exactly WX_STALE_S, the same
	 * boundary accent_for_age() goes amber at. */
	if (age < WX_STALE_S) {
		/* age < 3600 so the minutes are 0..59: "SYNC 59M". */
		snprintf(buf, cap, "SYNC %dM", (int)(age / 60));   /* <= 8 ch */
	} else if (age < 48 * 3600) {
		/* hours 1..47: "OLD 47H". */
		snprintf(buf, cap, "OLD %dH", (int)(age / 3600));  /* <= 7 ch */
	} else {
		int d = (int)(age / 86400);
		if (d > 99)
			d = 99;
		snprintf(buf, cap, "OLD %dD", d);                  /* <= 7 ch */
	}
}

static void apply_accent(lv_color_t c)
{
	if (lv_color_eq(c, accent_now))
		return;                 /* nothing to repaint */
	lv_style_set_bg_color(&st_seg_accent, c);
	lv_style_set_text_color(&st_txt_accent, c);
	lv_obj_report_style_change(&st_seg_accent);
	lv_obj_report_style_change(&st_txt_accent);
	accent_now = c;
}

/* Recompute the sync line and the accent tint from the current clock. Split
 * out of wx_ui_tick so wx_ui_update can refresh staleness immediately without
 * also advancing the tick counter -- driving the panel rotation and the
 * pixel-shift phase off the fetch schedule would make an eight-second panel
 * last seven seconds on whichever second the data happened to land. */
static void refresh_staleness(time_t now)
{
	static char prev[16];
	char s[16];

	bool have_sync = g_have_state && g_st.last_ok > 0;
	time_t age = have_sync ? (now - g_st.last_ok) : 0;

	sync_text(s, sizeof(s), age, have_sync);
	if (strcmp(s, prev) != 0) {
		lv_label_set_text(lbl_sync, s);
		strncpy(prev, s, sizeof(prev) - 1);
		prev[sizeof(prev) - 1] = '\0';
	}
	apply_accent(accent_for_age(age, have_sync));
}

/* -------------------------------------------------------------------------
 * Panel rendering
 * ---------------------------------------------------------------------- */

static void render_topbar_temp(void)
{
	seg_set_temp(bar_temp, 4, g_st.cur.temp_f);
}

/* -------------------------------------------------------------------------
 * Night icons
 *
 * The generated condition art is a DAYTIME set: WX_ICON_CLEAR is a sun. At
 * 2am on a clear night that is simply a wrong picture, which is what the user
 * reported. CLEAR and PARTLY get a moon substitute after dark; CLOUDY, FOG,
 * DRIZZLE, RAIN, SNOW and STORM look the same at night and get no second
 * asset, because a night cloud and a day cloud are the same drawing.
 *
 * The forecast panel deliberately does NOT do this. A forecast row is a
 * whole-day summary and a moon on tomorrow's column would be meaningless.
 * ---------------------------------------------------------------------- */

/* True while img_cond is showing a moon rather than a wx_icon_frames entry.
 *
 * This exists for icon_anim_cb. The moon is ONE still image -- wx_moon_frames
 * is indexed by phase, not by animation frame -- but the icon timer fires
 * every ICON_ANIM_MS and blindly writes wx_icon_frames[icon_sel][icon_frame].
 * Without this flag the sun would be back on the glass within 150ms of every
 * update, which is the whole feature failing in a way that looks like a
 * flicker rather than a bug. */
static bool icon_is_moon;

/* wx_moon_frames[] and wx_moon_cloud_frame are generated. A frame that failed
 * to generate has a null data pointer, and handing LVGL a zero-size descriptor
 * is a crash inside the draw unit, not a blank image. Same rule as
 * icon_frames_avail(). */
static bool moon_frame_ok(const lv_image_dsc_t *d)
{
	return d && d->data && d->data_size > 0;
}

/* is_day is a plain bool: it has no "unknown" state, so anything that leaves
 * it clear reads as permanent night. wx_fetch.c initialises it to true and
 * only ever overwrites it from the Open-Meteo response, so a PWS-only cycle
 * already reports day -- but a state restored from the NVS cache carries
 * whatever was stored, and the failure mode is a moon at noon, which looks
 * broken to anyone standing in front of it.
 *
 * So sunrise/sunset get a veto. They come from wx_fetch_forecast(), a
 * different request from the one that sets is_day, so they are genuinely
 * independent evidence: between them it is day whatever the flag says. When
 * they are absent (never fetched) there is nothing to check against and the
 * flag stands on its own. */
static bool night_now(time_t now)
{
	if (g_st.cur.is_day)
		return false;
	if (g_st.sunrise > 0 && g_st.sunset > g_st.sunrise &&
	    now >= g_st.sunrise && now < g_st.sunset)
		return false;
	return true;
}

static void render_pane_current(void)
{
	char v[16], line[24];
	time_t now = time(NULL);

	seg_set_temp(big_temp, 4, g_st.cur.temp_f);

	/* Icon choice runs BEFORE the condition text, because at night it also
	 * decides that text: a moon over the word "CLEAR" says less than the
	 * phase name does, and the picture and the words must not disagree.
	 *
	 * WX_ICON_UNKNOWN == WX_ICON_COUNT, one past the end of the frame
	 * table, so it must never reach the array. */
	wx_icon_t ic = (g_st.cur.weather_code >= 0)
			? wx_code_to_icon(g_st.cur.weather_code)
			: WX_ICON_UNKNOWN;

	const lv_image_dsc_t *moon = NULL;
	const char *cond_text = NULL;
	const char *cond_val  = NULL;   /* right-hand column; empty by day */
	char moon_buf[16];

	if (night_now(now)) {
		if (ic == WX_ICON_CLEAR) {
			int st = wx_moon_step(now);
			if (st >= 0 && st < WX_MOON_STEPS &&
			    moon_frame_ok(&wx_moon_frames[st])) {
				moon = &wx_moon_frames[st];
				/* Name PLUS illuminated percentage. The name
				 * alone cannot separate a 5% crescent from a
				 * 45% one -- both are "waning crescent" and
				 * they look nothing alike -- and the percentage
				 * alone does not say which way it is heading.
				 * Longest: "WAXING CRESCENT 49%" = 19 chars,
				 * 304px, inside the 320px label. */
				cond_text = wx_moon_name(now);
				/* Into the VALUE column, not appended to the
				 * name -- see build_pane_current(). */
				snprintf(moon_buf, sizeof(moon_buf), "%d%%",
					 (int)(wx_moon_illum(now) * 100.0f + 0.5f));
				cond_val = moon_buf;
			}
		} else if (ic == WX_ICON_PARTLY &&
			   moon_frame_ok(&wx_moon_cloud_frame)) {
			/* Text stays the weather text. The cloud is the fact
			 * worth reporting and it covers most of the disc, which
			 * is also why there is one cloud frame and not eight. */
			moon = &wx_moon_cloud_frame;
		}
	}

	/* Cap 17 characters (272px), which cannot reach the value column at
	 * x292. weather_code -1 means "unknown", which is not the same as
	 * code 0 (clear sky). */
	if (!cond_text && g_st.cur.weather_code >= 0)
		cond_text = wx_code_to_text(g_st.cur.weather_code);
	if (cond_text) {
		char t[32];
		copy_upper(t, sizeof(t), cond_text, 17);
		lv_label_set_text(lbl_cond, t);
	} else {
		lv_label_set_text(lbl_cond, "--");
	}
	/* Empty by day: there is no second quantity to report about "OVERCAST",
	 * and a stale percentage left over from last night would be worse than
	 * a blank. */
	lv_label_set_text(lbl_cond_val, cond_val ? cond_val : "");

	/* "%-7s%s" -> 7-char caption + up to 6 of value = cap 13 ch (208px). */
	fmt_val(v, sizeof(v), g_st.cur.feels_f, 0, "F");
	snprintf(line, sizeof(line), "%-7s%s", "FEELS", v);
	lv_label_set_text(lbl_feels, line);

	fmt_val(v, sizeof(v), g_st.cur.humidity_pct, 0, "%");
	snprintf(line, sizeof(line), "%-7s%s", "HUMID", v);
	lv_label_set_text(lbl_humid, line);

	fmt_val(v, sizeof(v), g_st.cur.dew_f, 0, "F");
	snprintf(line, sizeof(line), "%-7s%s", "DEW", v);
	lv_label_set_text(lbl_dew, line);

	if (moon) {
		/* Two independent brakes on icon_anim_cb, because getting this
		 * wrong repaints the sun over the moon every ICON_ANIM_MS
		 * (150ms) and the symptom looks like a rendering glitch rather
		 * than a logic error. icon_is_moon stops the callback outright;
		 * pointing icon_sel at WX_ICON_UNKNOWN means that even if that
		 * check were removed, icon_frames_avail() returns 0 for it and
		 * the callback bails before indexing wx_icon_frames. */
		icon_is_moon = true;
		icon_sel = WX_ICON_UNKNOWN;
		icon_frame = 0;
		lv_image_set_src(img_cond, moon);
		lv_obj_remove_flag(img_cond, LV_OBJ_FLAG_HIDDEN);
		return;
	}
	icon_is_moon = false;

	int n = icon_frames_avail(ic);
	if (n > 0) {
		icon_sel = ic;
		if (icon_frame >= n)
			icon_frame = 0;
		lv_image_set_src(img_cond, &wx_icon_frames[(int)ic][icon_frame]);
		lv_obj_remove_flag(img_cond, LV_OBJ_FLAG_HIDDEN);
	} else {
		/* No art for this condition: show nothing rather than a
		 * zero-size descriptor. The condition text still tells the
		 * user what the weather is. */
		icon_sel = WX_ICON_UNKNOWN;
		lv_obj_add_flag(img_cond, LV_OBJ_FLAG_HIDDEN);
	}
}

/* Pressure tendency over the last ~3h. Returns '^', 'v' or '='.
 * unscii is ASCII-only, so this is not U+2191/U+2193. */
static char pressure_trend(void)
{
	if (!g_st.cur.pressure_inhg.valid)
		return '=';

	time_t now = time(NULL);
	if (pres_last == 0 || now - pres_last >= PRES_SAMPLE_S) {
		pres_v[pres_head] = g_st.cur.pressure_inhg.v;
		pres_ok[pres_head] = true;
		pres_head = (pres_head + 1) % PRES_SLOTS;
		pres_last = now;
	}

	/* Oldest retained sample: the slot the head is about to overwrite. */
	int oldest = pres_head;
	for (int i = 0; i < PRES_SLOTS; i++) {
		int k = (pres_head + i) % PRES_SLOTS;
		if (pres_ok[k]) {
			oldest = k;
			break;
		}
	}
	if (!pres_ok[oldest])
		return '=';

	float d = g_st.cur.pressure_inhg.v - pres_v[oldest];
	if (d > PRES_SIGNIF_IN)
		return '^';
	if (d < -PRES_SIGNIF_IN)
		return 'v';
	return '=';
}

static void render_pane_current_plus(void)
{
	/* line[] is 48, not 40, because -Werror=format-truncation reasons about
	 * the WORST case, not the realistic one: "CLOUD " + 15 + " UV " + 15 is
	 * exactly 40 characters, so 40 leaves no room for the terminator. Do not
	 * shrink a[]/b[] to silence this -- a nonsense value should render as
	 * visible nonsense, not as a plausible truncated number.
	 *
	 * The DISPLAY bound is not this buffer, it is the label width, which
	 * clips. See build_pane_current_plus(). */
	char a[16], b[16], line[48];

	/* COLUMN A IS 16 CHARACTERS (256px), COLUMN B IS 15 (240px).
	 *
	 * These were 22 and 22, computed at 8px per character. At the real 16px
	 * the two old columns overlapped each other and column B ran 100px past
	 * the right edge of the pane. Four of the eight format strings below did
	 * not fit the corrected budget and were re-cut; each change is marked.
	 *
	 * worst cases, all measured against the column they are in:
	 *   A0 "WIND  199MPH NNW"  16   6 + 6 + 1 + 3
	 *   A1 "GUST  199MPH"      12   6 + 6
	 *   A2 "RAIN 100% 9.99/H"  16   5 + 4 + 1 + 6   (was "PRECIP ...", 18)
	 *   A3 "CLOUD 100% UV 15"  16   6 + 4 + 4 + 2   (UV was %.1f, 18)
	 *   B0 "PRES  32.00IN ^"   15   6 + 7 + 1 + 1
	 *   B1 "SUNRISE 06:41"     13   8 + 5
	 *   B2 "SUNSET  19:58"     13   8 + 5
	 *   B3 "DAY RAIN 9.99IN"   15   9 + 6           (was "RAIN TODAY ...", 17)
	 */
	fmt_val(a, sizeof(a), g_st.cur.wind_mph, 0, "MPH");
	snprintf(line, sizeof(line), "WIND  %s %s", a,
		 g_st.cur.wind_deg.valid ? compass16(g_st.cur.wind_deg.v) : "--");
	lv_label_set_text(lbl_p2a[0], line);

	fmt_val(a, sizeof(a), g_st.cur.gust_mph, 0, "MPH");
	snprintf(line, sizeof(line), "GUST  %s", a);
	lv_label_set_text(lbl_p2a[1], line);

	/* The amount here is rain_rate_inhr, a RATE, and it sits next to a
	 * probability -- "PRECIP 40% 0.12IN" reads as "0.12 inches expected",
	 * which is a different claim entirely. The "/H" suffix is what keeps
	 * the two apart; the day's accumulation is on the RAIN TODAY row. */
	/* "PRECIP" cost 2 of the 16 characters this column has and bought
	 * nothing "RAIN" does not say. The row is still probability THEN rate;
	 * the "/H" is what keeps them apart and it stays. */
	fmt_val(a, sizeof(a), g_st.cur.precip_prob_pct, 0, "%");
	fmt_val(b, sizeof(b), g_st.cur.rain_rate_inhr, 2, "/H");
	snprintf(line, sizeof(line), "RAIN %s %s", a, b);
	lv_label_set_text(lbl_p2a[2], line);

	/* UV is %.0f, not %.1f: "15.0" is four characters and "15" is two, and
	 * this row needed two back. Nothing anyone acts on is lost -- the WHO
	 * UV Index is defined and published as a whole number. */
	fmt_val(a, sizeof(a), g_st.cur.cloud_pct, 0, "%");
	fmt_val(b, sizeof(b), g_st.cur.uv, 0, "");
	snprintf(line, sizeof(line), "CLOUD %s UV %s", a, b);
	lv_label_set_text(lbl_p2a[3], line);

	/* pressure_trend() also takes the 3-hourly sample, so call it exactly
	 * once per update and outside an argument list. */
	char trend = pressure_trend();
	fmt_val(a, sizeof(a), g_st.cur.pressure_inhg, 2, "IN");
	if (g_st.cur.pressure_inhg.valid)
		snprintf(line, sizeof(line), "PRES  %s %c", a, trend);
	else
		snprintf(line, sizeof(line), "PRES  %s", a);
	lv_label_set_text(lbl_p2b[0], line);

	fmt_hhmm(a, sizeof(a), g_st.sunrise);
	snprintf(line, sizeof(line), "SUNRISE %s", a);
	lv_label_set_text(lbl_p2b[1], line);

	fmt_hhmm(a, sizeof(a), g_st.sunset);
	snprintf(line, sizeof(line), "SUNSET  %s", a);
	lv_label_set_text(lbl_p2b[2], line);

	/* "DAY RAIN", not "RAIN TODAY": two characters shorter, and it reads as
	 * a different quantity from the "RAIN 40% 0.12/H" row in column A, which
	 * is what it is -- that one is odds and a rate, this one is the day's
	 * accumulation. */
	fmt_val(a, sizeof(a), g_st.cur.rain_today_in, 2, "IN");
	snprintf(line, sizeof(line), "DAY RAIN %s", a);
	lv_label_set_text(lbl_p2b[3], line);

	/* Cap 34 characters at unscii_8 = 272px (x8..280). */
	char note[40];
	copy_upper(note, sizeof(note), g_note[0] ? g_note : "NO SOURCE", 34);
	lv_label_set_text(lbl_note, note);
}

static void render_pane_forecast(void)
{
	for (int i = 0; i < WX_FORECAST_DAYS; i++) {
		const wx_day_t *d = &g_st.day[i];

		if (!d->valid) {
			lv_label_set_text(lbl_fc_day[i], "---");
			lv_label_set_text(lbl_fc_hi[i], "HI   --");
			lv_label_set_text(lbl_fc_lo[i], "LO   --");
			lv_label_set_text(lbl_fc_pop[i], "RAIN  --");
			lv_obj_add_flag(img_fc[i], LV_OBJ_FLAG_HIDDEN);
			continue;
		}

		/* 3 characters exactly. strftime %a is locale-dependent; the C
		 * locale this firmware runs in gives "Mon".."Sun". */
		char day[8] = "---";
		if (d->date > 0) {
			struct tm tmv;
			localtime_r(&d->date, &tmv);
			strftime(day, sizeof(day), "%a", &tmv);
			for (int k = 0; day[k]; k++)
				if (day[k] >= 'a' && day[k] <= 'z')
					day[k] = (char)(day[k] - 32);
		}
		lv_label_set_text(lbl_fc_day[i], day);

		/* Formatted with the C library's snprintf, not
		 * lv_label_set_text_fmt. LV_USE_STDLIB_SPRINTF is left at its
		 * LV_STDLIB_BUILTIN default here (nothing in
		 * sdkconfig.defaults selects otherwise), and that is LVGL's own
		 * compact printf, not newlib's. Field widths like %3d are what
		 * hold these columns aligned, so they go through the
		 * implementation this project already relies on everywhere
		 * else in this file. */
		char t[16];

		/* "HI  88F" / "HI -99F" -- 7 characters = 112px in a
		 * 170px column. clamp_temp is what keeps that 7: an
		 * unclamped %3d of a garbage 9999 would print 8
		 * characters, 128px, and a 5-digit one 160px, which is
		 * where the column's own clip would start eating it. */
		snprintf(t, sizeof(t), "HI %3dF", clamp_temp(d->hi_f));
		lv_label_set_text(lbl_fc_hi[i], t);

		snprintf(t, sizeof(t), "LO %3dF", clamp_temp(d->lo_f));
		lv_label_set_text(lbl_fc_lo[i], t);

		/* 8 characters at unscii_8 = 64px in a 170px column. */
		int pop = (int)lroundf(d->precip_prob_pct);
		if (pop < 0)
			pop = 0;
		if (pop > 100)
			pop = 100;
		snprintf(t, sizeof(t), "RAIN%3d%%", pop);
		lv_label_set_text(lbl_fc_pop[i], t);

		wx_icon_t ic = (d->weather_code >= 0)
				? wx_code_to_icon(d->weather_code)
				: WX_ICON_UNKNOWN;
		if (icon_frames_avail(ic) > 0) {
			/* Forecast icons hold frame 0. Three animations
			 * running beside the current-conditions one would be
			 * four image invalidations per timer tick for no
			 * information gain. */
			lv_image_set_src(img_fc[i], &wx_icon_frames[(int)ic][0]);
			lv_obj_remove_flag(img_fc[i], LV_OBJ_FLAG_HIDDEN);
		} else {
			lv_obj_add_flag(img_fc[i], LV_OBJ_FLAG_HIDDEN);
		}
	}
}

/* -------------------------------------------------------------------------
 * Severe-weather takeover
 *
 * wx_code_is_severe() is true for thunderstorms, heavy snow and violent
 * squalls. On the onset of one, a full-screen 536x240 animation covers the
 * whole panel, plays through, and hands the screen back to the normal
 * rotation where it left off.
 *
 * EDGE, NOT LEVEL. wx_ui_update() runs once per fetch cycle and a
 * thunderstorm lasts hours. A level trigger would therefore replay a
 * full-screen takeover every cycle for the entire storm, occluding the
 * temperature, the wind and the pressure trend exactly when someone is
 * standing in front of the panel wanting to read them. It fires on the
 * transition from not-severe to severe and then stays quiet until the
 * condition has actually cleared.
 *
 * The edge alone is not enough. A code sitting on the severity boundary --
 * a cell drifting in and out of range, or the API alternating 95 and 80
 * between polls -- produces one edge per flap, so a second gate refuses to
 * replay within SEVERE_REPLAY_MS of the last play. The cost of that gate is
 * that a genuine second storm inside half an hour animates only once; the
 * cost of not having it is an unreadable display during weather.
 *
 * The rate limit runs off lv_tick_get(), not time(). The wall clock jumps
 * forward when SNTP lands and can step backwards on a resync, either of
 * which would defeat a time()-based window; lv_tick_elaps() is monotonic and
 * handles wraparound.
 *
 * An unknown weather_code (-1) means the API did not answer this cycle. That
 * is not evidence the storm ended, so it leaves the latch alone rather than
 * clearing it -- otherwise a single failed fetch mid-storm would re-arm the
 * edge and replay the takeover on the next good one.
 * ---------------------------------------------------------------------- */

#define SEVERE_FRAME_MS         110
/* The flash frame is a near-solid white field. Held for the full 110ms it
 * reads as the room lights coming on; real strokes are a few tens of ms. */
#define SEVERE_FLASH_MS          40
#define SEVERE_REPLAY_MS        (30u * 60u * 1000u)     /* 30 minutes */
/* Percentage of set bits above which a frame is the flash and not a storm
 * scene. Scenes are mostly dark sky; the flash is nearly all white. */
#define SEVERE_FLASH_FILL_PCT    60

static lv_timer_t *severe_timer;
static bool        severe_playing;
static bool        severe_latched;      /* last KNOWN severity, for the edge */
static bool        severe_played;       /* has severe_tick_at been set yet */
static uint32_t    severe_tick_at;      /* lv_tick_get() when play last began */
static int         severe_frame;
static int         severe_flash_idx = -1;

/* wx_severe_frame_count is generated; 0 means generation failed. Indexing a
 * zero-length table hands LVGL a descriptor with a null data pointer, which
 * is a crash inside the draw unit rather than a blank frame, so every read
 * goes through here. Same rule as icon_frames_avail(). */
static int severe_frames_avail(void)
{
	int n = wx_severe_frame_count;
	return n > 0 ? n : 0;
}

/* Find the lightning-flash frame by measuring it.
 *
 * The contract exports the frame array and its count and nothing else: no
 * per-frame duration, no documented index for the flash. A hardcoded index
 * would be a guess about a generated file that is free to reorder its frames,
 * and getting it wrong would shorten a scene and stretch the flash. So it is
 * measured: I1 is one bit per pixel with a set bit blitting opaque white, the
 * storm scenes are mostly dark, and the flash is a near-solid white field.
 * The brightest frame wins, and only if it clears SEVERE_FLASH_FILL_PCT -- an
 * animation generated without a flash in it shortens no frame at all.
 *
 * data_size includes the 8 palette bytes LVGL requires ahead of the bitmap.
 * They are byte-identical in every frame and 8 bytes against ~16KB, so they
 * cannot change which frame is brightest; skipping them is not worth the
 * arithmetic. Runs once, from wx_ui_init. */
static void severe_find_flash(void)
{
	int n = severe_frames_avail();
	uint32_t best_pct = 0;
	int best = -1;

	for (int i = 0; i < n; i++) {
		const lv_image_dsc_t *d = &wx_severe_frames[i];
		if (!d->data || d->data_size == 0)
			continue;
		uint32_t set = 0;
		for (uint32_t k = 0; k < d->data_size; k++)
			set += (uint32_t)__builtin_popcount(d->data[k]);
		/* set <= 8 * data_size ~ 129k, so *100 stays inside 32 bits. */
		uint32_t pct = (set * 100u) / (d->data_size * 8u);
		if (pct > best_pct) {
			best_pct = pct;
			best = i;
		}
	}

	severe_flash_idx = (best_pct >= SEVERE_FLASH_FILL_PCT) ? best : -1;
	ESP_LOGI(TAG, "severe: %d frames, flash frame %d (%u%% fill)",
		 n, severe_flash_idx, (unsigned)best_pct);
}

/* Built once, hidden, like everything else on this screen. Creating a
 * full-screen image object at the moment a storm arrives would be an
 * lv_obj_create from the main task, which is what overflowed that task's
 * stack in the sibling project.
 *
 * Full panel, 536x240 at (0,0). The frames are exactly panel-sized, and this
 * object deliberately does NOT ride the anti-burn-in shift that root does: it
 * is on screen for about a second at a time a few times a day, so it
 * accumulates no meaningful wear, and offsetting it would expose an unpainted
 * strip down one edge.
 *
 * Parented on scr rather than root so it covers the top bar too, and created
 * before the dimmer so the brightness overlay still composites on top -- a
 * 3am thunderstorm must not blast a night-dimmed panel to full white. */
static void build_severe(lv_obj_t *parent)
{
	severe_ov = mk_box(parent, 0, 0, PANEL_W, PANEL_H);
	lv_obj_set_style_bg_color(severe_ov, WX_BLACK, 0);
	lv_obj_set_style_bg_opa(severe_ov, LV_OPA_COVER, 0);
	lv_obj_add_flag(severe_ov, LV_OBJ_FLAG_HIDDEN);

	/* I1 blits set bits as opaque white and clear bits as opaque black, so
	 * there is no recolour and no opacity to set. There is also no
	 * transform property here and there must never be one -- see the ban at
	 * the top of this file. The frames change by swapping the image source;
	 * nothing about this object's geometry moves. */
	img_severe = lv_image_create(severe_ov);
	lv_obj_remove_style_all(img_severe);
	lv_obj_center(img_severe);
}

static void severe_show_frame(int i)
{
	lv_image_set_src(img_severe, &wx_severe_frames[i]);
	lv_timer_set_period(severe_timer,
			    (i == severe_flash_idx) ? SEVERE_FLASH_MS
						    : SEVERE_FRAME_MS);
}

static void severe_stop(void)
{
	severe_playing = false;
	if (severe_timer)
		lv_timer_pause(severe_timer);
	if (severe_ov)
		lv_obj_add_flag(severe_ov, LV_OBJ_FLAG_HIDDEN);
}

/* One frame per expiry, then hide and hand the screen back.
 *
 * The timer is created paused in wx_ui_init and only ever paused and resumed.
 * It is not created per play and never deleted here: lv_timer_delete from
 * inside the timer's own callback is a use-after-free, and building
 * everything up front is what the rest of this file does anyway. */
static void severe_anim_cb(lv_timer_t *t)
{
	(void)t;
	if (!severe_playing || ++severe_frame >= severe_frames_avail()) {
		severe_stop();
		return;
	}
	severe_show_frame(severe_frame);
}

/* The edge detector. Called once per wx_ui_update with the current code. */
static void severe_consider(int weather_code)
{
	if (weather_code < 0)
		return;                 /* no reading; the latch keeps its value */

	bool sev = wx_code_is_severe(weather_code);
	bool onset = sev && !severe_latched;
	severe_latched = sev;

	if (!onset || severe_playing)
		return;
	/* Generation failed, or the UI was never built. Do nothing at all
	 * rather than index a zero-size table. */
	if (severe_frames_avail() <= 0 || !severe_timer || !img_severe)
		return;
	if (severe_played && lv_tick_elaps(severe_tick_at) < SEVERE_REPLAY_MS)
		return;

	severe_tick_at = lv_tick_get();
	severe_played = true;
	severe_playing = true;
	severe_frame = 0;
	severe_show_frame(0);
	lv_obj_remove_flag(severe_ov, LV_OBJ_FLAG_HIDDEN);
	lv_timer_reset(severe_timer);
	lv_timer_resume(severe_timer);
	ESP_LOGI(TAG, "severe onset: wmo %d", weather_code);
}

static void show_pane(int idx)
{
	for (int i = 0; i < N_PANELS; i++) {
		if (i == idx)
			lv_obj_remove_flag(pane[i], LV_OBJ_FLAG_HIDDEN);
		else
			lv_obj_add_flag(pane[i], LV_OBJ_FLAG_HIDDEN);
	}
}

static void set_mode(ui_mode_t m)
{
	mode = m;
	/* The toast lives in the top bar, which only exists in MODE_MAIN. Drop
	 * it on any mode change so it cannot reappear stale when the main view
	 * comes back. */
	if (lbl_toast) {
		lv_obj_add_flag(lbl_toast, LV_OBJ_FLAG_HIDDEN);
		toast_ticks = 0;
	}
	/* The severe overlay is parented on the screen, above root, so a play
	 * still running when the setup portal comes up would cover it. Any mode
	 * change ends it. */
	if (m != MODE_MAIN)
		severe_stop();
	if (splash) {
		if (m == MODE_SPLASH)
			lv_obj_remove_flag(splash, LV_OBJ_FLAG_HIDDEN);
		else
			lv_obj_add_flag(splash, LV_OBJ_FLAG_HIDDEN);
	}
	if (main_wrap) {
		if (m == MODE_MAIN)
			lv_obj_remove_flag(main_wrap, LV_OBJ_FLAG_HIDDEN);
		else
			lv_obj_add_flag(main_wrap, LV_OBJ_FLAG_HIDDEN);
	}
	if (portal) {
		if (m == MODE_PORTAL)
			lv_obj_remove_flag(portal, LV_OBJ_FLAG_HIDDEN);
		else
			lv_obj_add_flag(portal, LV_OBJ_FLAG_HIDDEN);
	}
}

/* -------------------------------------------------------------------------
 * Icon animation
 *
 * On its own lv_timer rather than off wx_ui_tick: tick is 1Hz, and a one-
 * frame-per-second condition icon reads as a glitch, not an animation.
 * ---------------------------------------------------------------------- */
static void icon_anim_cb(lv_timer_t *t)
{
	(void)t;
	if (mode != MODE_MAIN || cur_pane != 0)
		return;                 /* the only animated icon is on panel 1 */
	if (severe_playing)
		return;                 /* occluded: invalidating a 120x120 box
					 * under an opaque full-screen overlay
					 * costs a redraw and shows nothing */
	if (icon_is_moon)
		return;                 /* the moon is a STILL. wx_moon_frames is
					 * indexed by phase, not by frame, so there
					 * is nothing here to advance -- and the
					 * write below would put a sun back over it
					 * within ICON_ANIM_MS */
	int n = icon_frames_avail(icon_sel);
	if (n <= 1)
		return;                 /* nothing to animate, or nothing at all */
	icon_frame = (icon_frame + 1) % n;
	lv_image_set_src(img_cond, &wx_icon_frames[(int)icon_sel][icon_frame]);
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

void wx_ui_init(lv_display_t *disp)
{
	if (ui_ready)
		return;
	if (!disp)
		disp = lv_display_get_default();
	if (!disp) {
		ESP_LOGE(TAG, "no display; UI not built");
		return;
	}

	/* Every coordinate in this file is hardcoded to PANEL_W x PANEL_H;
	 * nothing here reads the display's real size, so `disp` was previously
	 * accepted and never checked.
	 *
	 * main.c reaches 536x240 by composing TWO independent settings: a
	 * controller-side esp_lcd_panel_swap_xy(panel, true) and an LVGL-side
	 * lv_disp_set_rotation(disp, LV_DISPLAY_ROTATION_90) over a display
	 * created 240x536. That composes correctly on this hardware today -- the
	 * sibling project runs the same pair -- but the two have to agree, and
	 * changing either one alone brings the panel up 240x536 with every
	 * number in the arithmetic block at the top of this file wrong.
	 *
	 * Log, do not abort. A wrong-but-visible layout still shows the
	 * temperature and can be photographed and diagnosed; a device that
	 * refuses to build its UI shows a black panel that looks identical to
	 * dead hardware, a dead backlight or a hung render loop. The whole value
	 * of this check is turning that blank-screen mystery into one line of
	 * serial log. */
	int32_t hres = lv_display_get_horizontal_resolution(disp);
	int32_t vres = lv_display_get_vertical_resolution(disp);
	if (hres != PANEL_W || vres != PANEL_H)
		ESP_LOGE(TAG, "display is %ldx%ld but this layout is fixed at "
			 "%dx%d -- check swap_xy and lv_disp_set_rotation in "
			 "main.c; the layout below will be wrong",
			 (long)hres, (long)vres, PANEL_W, PANEL_H);

	scr = lv_display_get_screen_active(disp);
	lv_obj_set_style_bg_color(scr, WX_BLACK, 0);
	lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
	lv_obj_set_style_pad_all(scr, 0, 0);
	lv_obj_set_style_border_width(scr, 0, 0);
	lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);

	build_styles();

	/* Everything visible lives under root so the anti-burn-in shift is one
	 * lv_obj_set_pos rather than a walk over the whole tree. */
	root = mk_box(scr, 0, 0, ROOT_W, ROOT_H);

	build_splash(root);

	main_wrap = mk_box(root, 0, 0, ROOT_W, ROOT_H);
	lv_obj_add_flag(main_wrap, LV_OBJ_FLAG_HIDDEN);

	lv_obj_t *bar = mk_box(main_wrap, 0, 0, ROOT_W, BAR_H);
	build_topbar(bar);

	for (int i = 0; i < N_PANELS; i++)
		pane[i] = mk_box(main_wrap, 0, AREA_Y, ROOT_W, AREA_H);
	build_pane_current(pane[0]);
	build_pane_current_plus(pane[1]);
	build_pane_forecast(pane[2]);
	show_pane(0);

	build_portal(root);

	/* Severe-weather takeover. Sibling of root so it covers the top bar,
	 * created before the dimmer so brightness still applies over it. */
	build_severe(scr);
	severe_find_flash();

	/* Brightness overlay. Sibling of root and created last, so it is last
	 * in the screen's child list and therefore composited on top of
	 * everything -- including the portal. See wx_ui_set_brightness. */
	dimmer = mk_box(scr, 0, 0, PANEL_W, PANEL_H);
	lv_obj_set_style_bg_color(dimmer, WX_BLACK, 0);
	lv_obj_set_style_bg_opa(dimmer, LV_OPA_TRANSP, 0);
	lv_obj_add_flag(dimmer, LV_OBJ_FLAG_HIDDEN);

	lv_timer_create(icon_anim_cb, ICON_ANIM_MS, NULL);

	/* Created here and left paused for the life of the app. The severe
	 * animation runs seconds at a time, days apart; creating the timer on
	 * demand would be an allocation from the main task at the one moment
	 * the UI is busiest. Period is set per frame in severe_show_frame(). */
	severe_timer = lv_timer_create(severe_anim_cb, SEVERE_FRAME_MS, NULL);
	if (severe_timer)
		lv_timer_pause(severe_timer);

	set_mode(MODE_SPLASH);
	ui_ready = true;
	ESP_LOGI(TAG, "UI built: root %dx%d in panel %dx%d",
		 ROOT_W, ROOT_H, PANEL_W, PANEL_H);
}

void wx_ui_update(const wx_state_t *st)
{
	if (!ui_ready || !st)
		return;

	/* Copy rather than alias: main owns that struct and may refill it from
	 * the fetch task while wx_ui_tick is reading. */
	g_st = *st;
	/* cur_source_note points into wx_fetch.c, which reuses its buffer
	 * between tiers -- keep our own copy. */
	copy_upper(g_note, sizeof(g_note), st->cur_source_note, 34);
	g_have_state = true;

	/* Any successful update means we are past setup, and the header gives
	 * no explicit "hide portal" call, so this is what closes it. */
	if (mode != MODE_MAIN)
		set_mode(MODE_MAIN);

	render_topbar_temp();
	render_pane_current();
	render_pane_current_plus();
	render_pane_forecast();

	char obs[16], obsline[24];
	fmt_hhmm(obs, sizeof(obs), g_st.cur.observed);
	snprintf(obsline, sizeof(obsline), "OBS %s", obs);   /* 9 ch */
	lv_label_set_text(lbl_obs, obsline);

	/* The marker names where the TEMPERATURE came from. Provenance is
	 * per-field in this app, and temperature is the field the top bar is
	 * showing, so anything else would be a claim about a value that is not
	 * on screen. */
	switch (g_st.cur.temp_f.src) {
	case WX_SRC_PWS: lv_label_set_text(lbl_src, "PWS"); break;
	case WX_SRC_API: lv_label_set_text(lbl_src, "O-M"); break;
	default:         lv_label_set_text(lbl_src, "---"); break;
	}

	/* Recompute staleness now instead of waiting up to a second for the
	 * next tick -- a fresh sync should turn the display green immediately.
	 * Deliberately NOT a wx_ui_tick() call: that would advance the panel
	 * dwell and the pixel-shift phase off the fetch schedule. */
	refresh_staleness(time(NULL));

	/* Last, so the panels underneath are already rendered with this cycle's
	 * data by the time the takeover uncovers them. Edge-triggered and
	 * rate-limited inside; see the section comment for why both. */
	severe_consider(g_st.cur.weather_code);
}

void wx_ui_tick(void)
{
	if (!ui_ready)
		return;

	tick_count++;

	time_t now = time(NULL);
	struct tm tmv;
	localtime_r(&now, &tmv);

	/* Clock and date repaint only on the minute. Seconds were dropped on
	 * purpose; this is the check that makes that pay off. */
	if (tmv.tm_min != last_min) {
		last_min = tmv.tm_min;
		char hhmm[8];
		snprintf(hhmm, sizeof(hhmm), "%02d%02d", tmv.tm_hour,
			 tmv.tm_min);
		for (int i = 0; i < 4; i++)
			seg_digit_set(&clk[i], hhmm[i]);

		/* "SAT 05 SEP" -- 10 characters exactly. */
		char date[16];
		strftime(date, sizeof(date), "%a %d %b", &tmv);
		char up[16];
		copy_upper(up, sizeof(up), date, 10);
		lv_label_set_text(lbl_date, up);
	}

	/* Staleness. Recomputed every second because the age is what makes it
	 * move; both setters inside short-circuit when nothing changed. */
	refresh_staleness(now);

	/* Panel rotation. Suspended while the severe takeover is on screen:
	 * pane_dwell is left where it stands rather than reset, so the panel
	 * the user was reading resumes with the rest of its dwell instead of
	 * being cut short by an event it did not ask for. */
	if (mode == MODE_MAIN && !severe_playing) {
		if (++pane_dwell >= PANEL_DWELL_S) {
			pane_dwell = 0;
			cur_pane = (cur_pane + 1) % N_PANELS;
			show_pane(cur_pane);
		}
	}

	/* Transient status expiry. */
	if (toast_ticks > 0 && --toast_ticks == 0)
		lv_obj_add_flag(lbl_toast, LV_OBJ_FLAG_HIDDEN);

	/* Portal cursor blink, 1Hz off this tick. */
	if (mode == MODE_PORTAL && portal_cursor) {
		if (tick_count & 1)
			lv_obj_add_flag(portal_cursor, LV_OBJ_FLAG_HIDDEN);
		else
			lv_obj_remove_flag(portal_cursor, LV_OBJ_FLAG_HIDDEN);
	}

	/* Anti-burn-in shift. Geometry only -- lv_obj_set_pos, never a
	 * transform property. See the note at the top of this file for what
	 * transforms did to the render loop in the sibling project. */
	if (tick_count % SHIFT_PERIOD_S == 0) {
		shift_idx = (shift_idx + 1) % (int)N_SHIFT;
		lv_obj_set_pos(root, shift_x[shift_idx], shift_y[shift_idx]);
	}
}

void wx_ui_status(const char *msg)
{
	if (!ui_ready)
		return;
	/* Cap 30 characters. That is the narrowest of the three destinations:
	 * the portal status row is x40..520 = 480px = 30 characters at
	 * unscii_16. The splash line is the full 530 = 33, and the top-bar
	 * toast is unscii_8 at 20 characters over 3 available lines = 60.
	 *
	 * The cap was 40, derived at 8px per character, and at the true metric
	 * 40 characters is 640px -- wider than the panel. The longest string any
	 * caller actually passes is "CONFIG ERASED -- RESTARTING" from main.c,
	 * 27 characters, so nothing real truncates. */
	char t[48];
	copy_upper(t, sizeof(t), msg, 30);
	lv_label_set_text(lbl_splash_status, t);
	lv_label_set_text(lbl_portal_status, t);
	lv_label_set_text(lbl_toast, t);

	if (mode == MODE_MAIN) {
		lv_obj_remove_flag(lbl_toast, LV_OBJ_FLAG_HIDDEN);
		toast_ticks = TOAST_TICKS;
	}
}

void wx_ui_invalidate_clock(void)
{
	last_min = -1;
}

void wx_ui_next_panel(void)
{
	if (!ui_ready || mode != MODE_MAIN)
		return;
	cur_pane = (cur_pane + 1) % N_PANELS;
	pane_dwell = 0;         /* a manual advance restarts the dwell, so the
				 * panel the user asked for gets a full 8s */
	show_pane(cur_pane);
}

void wx_ui_show_portal(const char *ssid, const char *url)
{
	if (!ui_ready)
		return;

	/* 28 characters = 448px at x64 -> right edge 512, inside 530.
	 *
	 * The 802.11 SSID limit is 32 and wx_cfg_t.ssid[33] carries all of it,
	 * but 32 characters is 512px at unscii_16 and this row is indented to
	 * x64, so a 32-character SSID clips at 28. The default is 15. */
	char t[64];
	copy_upper(t, sizeof(t), ssid && ssid[0] ? ssid : WX_PORTAL_SSID, 28);
	lv_label_set_text(lbl_portal_ssid, t);

	/* 28 characters = 448px at x64 -> 512. Not uppercased blindly: a URL
	 * path can be case-sensitive. Only the scheme and host would be safe
	 * to fold, and splitting the string to do that is not worth it.
	 * The default "http://192.168.4.1" is 18. */
	char u[48];
	snprintf(u, sizeof(u), "%.28s", url && url[0] ? url : "http://192.168.4.1");
	lv_label_set_text(lbl_portal_url, u);

	set_mode(MODE_PORTAL);
}

/* THIS IS NOT PANEL BRIGHTNESS.
 *
 * The RM67162 does accept WRDISBV (0x51) and this board's init sequence sends
 * it once, hardcoded to 175 -- see lilygo_rm67162_spi_cmd[] in
 * components/esp_lcd_panel_rm67162/esp_lcd_panel_rm67162.c. But that
 * component's public header exports exactly one symbol,
 * esp_lcd_new_panel_rm67162(); the panel struct holding the esp_lcd_panel_io
 * handle is private to that .c file, and esp_lcd_panel_t exposes only
 * reset/init/draw_bitmap/mirror/swap_xy/gap/invert/disp_on_off. There is no
 * route from here to command 0x51 without editing the driver or having main.c
 * hand us the IO handle, and this file is not permitted to do either.
 *
 * So this is an LVGL-side dim: a full-screen black overlay whose opacity is
 * the inverse of the requested level. On this specific hardware that is not
 * merely cosmetic -- the panel is a self-emissive AMOLED, so compositing
 * towards black genuinely reduces the light and the current each subpixel
 * produces. It is still not the panel's own brightness register, it does not
 * change the driver's gamma handling, and it costs one alpha blend over the
 * invalidated area on every refresh.
 *
 * At level 255 the overlay is hidden outright rather than left at opacity 0,
 * so the common case costs nothing at all.
 *
 * TO MAKE THIS REAL: the driver needs a
 * esp_lcd_panel_rm67162_set_brightness(panel, level) that does
 * rm67162_cmd_trans(io, 0x51, &level, 1). That is a two-line addition to the
 * component and belongs in the component, not here.
 */
static wx_panel_bright_fn panel_bright_cb;

void wx_ui_set_panel_bright_cb(wx_panel_bright_fn fn)
{
	panel_bright_cb = fn;
}

void wx_ui_set_brightness(uint8_t level)
{
	if (!ui_ready)
		return;
	if (level == bright)
		return;
	bright = level;

	/* Real panel brightness when main has registered it. The overlay below
	 * is a fallback only -- see the note in vaultweather.h for why it is a
	 * poor substitute on this display. */
	if (panel_bright_cb) {
		panel_bright_cb(level);
		if (dimmer)
			lv_obj_add_flag(dimmer, LV_OBJ_FLAG_HIDDEN);
		return;
	}

	if (!dimmer)
		return;

	if (level >= 255) {
		lv_obj_add_flag(dimmer, LV_OBJ_FLAG_HIDDEN);
		return;
	}
	lv_obj_set_style_bg_opa(dimmer, (lv_opa_t)(255 - level), 0);
	lv_obj_remove_flag(dimmer, LV_OBJ_FLAG_HIDDEN);
}
