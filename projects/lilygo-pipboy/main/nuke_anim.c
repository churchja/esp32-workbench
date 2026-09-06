/*
 * NUKE -- a 1950s-cartoon detonation, built to a reference image.
 *
 * WHAT THE REFERENCE CONTAINS that earlier attempts missed:
 *   - a wide CAULIFLOWER cap of many overlapping lobes, not one disc
 *   - a NARROW stem, not a fat column
 *   - a RING around the stem, the condensation torus -- the single most
 *     recognisable element of the icon, and absent from every earlier version
 *   - a spreading GROUND BILLOW of lobes along the base
 *
 * COLOUR, and why it inverts the reference
 * The reference is black line-art on white. This panel is black and an AMOLED
 * renders that as true black, so the cloud is WHITE FILL with a BLACK border:
 * the borders merge into the background and the gaps between lobes read as the
 * heavy cartoon outlines. Same silhouette, inverted medium.
 *
 * MOTION -- keyframes, not tweens
 * Each element pops in on its own delay with overshoot easing, so the cloud
 * assembles lobe by lobe the way cel animation reveals a drawing, rather than
 * gliding smoothly like a UI transition.
 *
 * Everything grows by RESIZING, never transform_scale. Scaling bordered objects
 * through LVGL's transform path stalled the render loop outright: the app
 * stopped logging the instant the animation fired and never recovered, which
 * silently broke three rounds of GPIO scanning before the cause was found.
 * Resizing a LV_RADIUS_CIRCLE object keeps it circular and costs a normal
 * redraw.
 */
#include <string.h>
#include <stdio.h>
#include "lvgl.h"

#define NK_LITE   lv_color_hex(0xFFFFFF)
#define NK_BODY   lv_color_hex(0xE2E2E2)
#define NK_SHADE  lv_color_hex(0xB9B9B9)
#define NK_CAPCOL lv_color_hex(0xFFE8A0)

struct lobe { int16_t x, y, r; uint16_t delay; uint8_t tone; };

/* Cap: overlapping lobes, widest in the middle, so the silhouette reads as one
 * billowing mass rather than a row of circles. */
static const struct lobe CAP[] = {
	{   0, -74, 34, 1000, 0 }, { -40, -64, 27, 1060, 0 },
	{  40, -64, 27, 1060, 0 }, { -70, -46, 22, 1140, 1 },
	{  70, -46, 22, 1140, 1 }, { -26, -44, 26, 1000, 0 },
	{  26, -44, 26, 1000, 0 }, {   0, -46, 30,  960, 0 },
};
static const struct lobe GROUND[] = {
	{   0,  94, 34,  620, 2 }, { -54,  98, 27,  680, 2 },
	{  54,  98, 27,  680, 2 }, { -102, 102, 21, 740, 2 },
	{ 102, 102, 21,  740, 2 },
};
#define N_CAP    (sizeof(CAP) / sizeof(CAP[0]))
#define N_GROUND (sizeof(GROUND) / sizeof(GROUND[0]))
#define N_LOBE   (N_CAP + N_GROUND)

static lv_obj_t *nk_scr, *nk_flash, *nk_label;
static lv_obj_t *nk_lobe[N_LOBE], *nk_stem, *nk_ring, *nk_bomb;
static struct lobe all[N_LOBE];
static char   nk_text[48];
static size_t nk_pos;
static bool   nk_running;

static void cb_opa(void *o, int32_t v) { lv_obj_set_style_opa(o, v, 0); }
static void cb_y  (void *o, int32_t v) { lv_obj_set_y(o, v); }

static void cb_lobe(void *o, int32_t v)
{
	lv_obj_t *ob = (lv_obj_t *)o;
	int i = (int)(intptr_t)lv_obj_get_user_data(ob);
	int d = (all[i].r * 2 * v) / 256;
	if (d < 2) d = 2;
	lv_obj_set_size(ob, d, d);
	lv_obj_align(ob, LV_ALIGN_CENTER, all[i].x, all[i].y);
}

/* Stem climbs: the base stays on the ground line, the top rises. */
static void cb_stem(void *o, int32_t v)
{
	lv_obj_t *ob = (lv_obj_t *)o;
	if (v < 2) v = 2;
	lv_obj_set_size(ob, 24, v);
	lv_obj_align(ob, LV_ALIGN_CENTER, 0, 78 - v / 2);
}

/* Ring spreads horizontally around the stem. */
static void cb_ring(void *o, int32_t v)
{
	lv_obj_t *ob = (lv_obj_t *)o;
	if (v < 4) v = 4;
	lv_obj_set_size(ob, v, 22);
	lv_obj_align(ob, LV_ALIGN_CENTER, 0, 34);
}

static void nk_type(lv_timer_t *t)
{
	if (nk_pos >= strlen(nk_text)) { lv_timer_del(t); return; }
	nk_pos++;
	char part[48];
	memcpy(part, nk_text, nk_pos);
	part[nk_pos] = '\0';
	lv_label_set_text(nk_label, part);
}

static void nk_end(lv_timer_t *t)
{
	lv_obj_add_flag(nk_scr, LV_OBJ_FLAG_HIDDEN);
	nk_running = false;
	lv_timer_del(t);
}

static void anim(lv_obj_t *o, lv_anim_exec_xcb_t cb, int32_t a0, int32_t a1,
		 uint32_t dur, uint32_t delay, lv_anim_path_cb_t path)
{
	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, o);
	lv_anim_set_exec_cb(&a, cb);
	lv_anim_set_values(&a, a0, a1);
	lv_anim_set_duration(&a, dur);
	lv_anim_set_delay(&a, delay);
	if (path) lv_anim_set_path_cb(&a, path);
	lv_anim_start(&a);
}

static lv_color_t tone(uint8_t t)
{
	return t == 0 ? NK_LITE : (t == 1 ? NK_BODY : NK_SHADE);
}

static lv_obj_t *shape(lv_obj_t *par, lv_color_t fill, int radius)
{
	lv_obj_t *o = lv_obj_create(par);
	lv_obj_set_style_radius(o, radius, 0);
	lv_obj_set_style_bg_color(o, fill, 0);
	lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
	lv_obj_set_style_border_color(o, lv_color_black(), 0);
	lv_obj_set_style_border_width(o, 3, 0);
	lv_obj_set_style_pad_all(o, 0, 0);
	lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
	return o;
}

void nuke_build(lv_obj_t *parent, int32_t w, int32_t h)
{
	nk_scr = lv_obj_create(parent);
	lv_obj_set_size(nk_scr, w, h);
	lv_obj_set_pos(nk_scr, 0, 0);
	lv_obj_set_style_bg_color(nk_scr, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(nk_scr, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(nk_scr, 0, 0);
	lv_obj_set_style_pad_all(nk_scr, 0, 0);
	lv_obj_clear_flag(nk_scr, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag(nk_scr, LV_OBJ_FLAG_HIDDEN);

	for (size_t i = 0; i < N_CAP; i++)    all[i] = CAP[i];
	for (size_t i = 0; i < N_GROUND; i++) all[N_CAP + i] = GROUND[i];

	/* Layered like cels: ground, then stem, then ring, then cap. Later
	 * children paint over earlier ones, so the cap occludes the stem and the
	 * ring sits in front of it. */
	for (size_t i = N_CAP; i < N_LOBE; i++) {
		nk_lobe[i] = shape(nk_scr, tone(all[i].tone), LV_RADIUS_CIRCLE);
		lv_obj_set_user_data(nk_lobe[i], (void *)(intptr_t)i);
	}
	nk_stem = shape(nk_scr, NK_BODY, 6);
	nk_ring = shape(nk_scr, NK_LITE, LV_RADIUS_CIRCLE);
	for (size_t i = 0; i < N_CAP; i++) {
		nk_lobe[i] = shape(nk_scr, tone(all[i].tone), LV_RADIUS_CIRCLE);
		lv_obj_set_user_data(nk_lobe[i], (void *)(intptr_t)i);
	}

	nk_bomb = shape(nk_scr, NK_BODY, LV_RADIUS_CIRCLE);
	lv_obj_set_size(nk_bomb, 16, 30);

	nk_label = lv_label_create(nk_scr);
	lv_obj_set_style_text_color(nk_label, NK_CAPCOL, 0);
	lv_obj_set_style_text_font(nk_label, &lv_font_unscii_16, 0);
	lv_obj_align(nk_label, LV_ALIGN_BOTTOM_LEFT, 8, -4);

	nk_flash = lv_obj_create(nk_scr);
	lv_obj_set_size(nk_flash, w, h);
	lv_obj_set_pos(nk_flash, 0, 0);
	lv_obj_set_style_radius(nk_flash, 0, 0);
	lv_obj_set_style_bg_color(nk_flash, lv_color_white(), 0);
	lv_obj_set_style_bg_opa(nk_flash, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(nk_flash, 0, 0);
}

bool nuke_busy(void) { return nk_running; }

void nuke_fire(const char *caption)
{
	if (nk_running) return;
	nk_running = true;

	lv_obj_clear_flag(nk_scr, LV_OBJ_FLAG_HIDDEN);
	lv_obj_move_foreground(nk_scr);

	for (size_t i = 0; i < N_LOBE; i++) {
		cb_lobe(nk_lobe[i], 0);
		lv_obj_set_style_opa(nk_lobe[i], LV_OPA_COVER, 0);
	}
	cb_stem(nk_stem, 0);
	cb_ring(nk_ring, 0);
	lv_obj_set_style_opa(nk_stem, LV_OPA_COVER, 0);
	lv_obj_set_style_opa(nk_ring, LV_OPA_COVER, 0);
	lv_obj_set_style_opa(nk_flash, LV_OPA_TRANSP, 0);
	lv_obj_set_style_opa(nk_bomb, LV_OPA_COVER, 0);
	lv_label_set_text(nk_label, "");

	/* 0.00  the bomb falls in from above */
	lv_obj_align(nk_bomb, LV_ALIGN_TOP_MID, 0, 0);
	anim(nk_bomb, cb_y, -40, 190, 460, 0, lv_anim_path_ease_in);
	anim(nk_bomb, cb_opa, 255, 0, 60, 450, NULL);

	/* 0.46  detonation: flash on hard, decay */
	anim(nk_flash, cb_opa, 0, 255, 40, 460, NULL);
	anim(nk_flash, cb_opa, 255, 0, 420, 520, lv_anim_path_ease_out);

	/* 0.62 ground erupts -> 0.80 stem climbs -> 0.96 cap billows */
	for (size_t i = 0; i < N_LOBE; i++)
		anim(nk_lobe[i], cb_lobe, 0, 256, 560, all[i].delay,
		     lv_anim_path_overshoot);
	anim(nk_stem, cb_stem, 0, 132, 700, 800, lv_anim_path_ease_out);

	/* 1.30  the ring snaps outward -- the icon's signature */
	anim(nk_ring, cb_ring, 0, 168, 520, 1300, lv_anim_path_overshoot);

	/* 3.00  the whole cloud drifts away */
	for (size_t i = 0; i < N_LOBE; i++)
		anim(nk_lobe[i], cb_opa, 255, 0, 800, 3000, NULL);
	anim(nk_stem, cb_opa, 255, 0, 800, 3000, NULL);
	anim(nk_ring, cb_opa, 255, 0, 800, 3000, NULL);

	strncpy(nk_text, caption, sizeof(nk_text) - 1);
	nk_text[sizeof(nk_text) - 1] = '\0';
	nk_pos = 0;
	lv_timer_create(nk_type, 40, NULL);

	lv_timer_t *e = lv_timer_create(nk_end, 4200, NULL);
	lv_timer_set_repeat_count(e, 1);
}
