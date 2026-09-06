/*
 * NUKE -- frame-played mushroom cloud, 1950s comic style.
 *
 * WHY FRAMES
 * Three earlier versions assembled the explosion from LVGL primitives at
 * runtime: an arc, then tweened circles, then resized lobes. Every one read as
 * a UI widget, because a circle drawn by a widget toolkit has a mathematically
 * perfect edge and nothing in comic art does. The art is now pre-rendered by
 * tools/gen_nuke_frames.py, where each outline is perturbed by summed sine
 * harmonics -- no two lobes alike, none of them circles.
 *
 * WHY THIS CANNOT OVERFLOW THE STACK
 * Playback allocates nothing. One lv_image is built once at startup and the
 * timer swaps its source pointer. The frames are `static const` in flash. The
 * earlier crash came from building ~10 LVGL objects inside app_main and blowing
 * the 3584-byte main task stack; there is nothing here to build.
 *
 * WHY IT CANNOT WEDGE THE RENDER LOOP
 * No transform_scale, which is what stalled LVGL previously -- scaling bordered
 * objects sends each through a transformation layer. Blitting a 1bpp mask is an
 * ordinary redraw.
 *
 * WHY I1 AND NOT A1
 * A1 is a valid LVGL enum but the 9.5 SOFTWARE renderer has no blitter for it
 * -- A1 exists only in the nema_gfx and vg_lite backends, neither of which
 * this chip has. An A1 image draws NOTHING and says so only via a runtime
 * LV_LOG_WARN. I1 is the 1bpp format the software renderer implements, and
 * its blend ignores the palette entirely: set bit -> opaque white, clear bit
 * -> opaque black. That is a comic panel with no recolour needed, so no
 * recolour is applied here.
 *
 * THE CLOUD IS THE SET BITS. The black gaps between overlapping lobes ARE the
 * heavy comic outlines: the ink is the background.
 */
#include <string.h>
#include "lvgl.h"

extern const lv_image_dsc_t nuke_frame[];
extern const int nuke_frame_count;

/* Hold time per frame, ms. Cartoon studios varied exposure rather than running
 * everything on the same beat: the flash is a single snap, the billow holds
 * longer, and the last frame sits while the caption types. */
static const uint16_t HOLD[] = { 300, 70, 150, 170, 190, 210, 240, 1500 };

static lv_obj_t *nk_scr, *nk_img, *nk_label;
static lv_timer_t *nk_timer;
static int      nk_i;
static char     nk_text[48];
static size_t   nk_pos;
static bool     nk_running;

static void nk_type(lv_timer_t *t)
{
	if (nk_pos >= strlen(nk_text)) { lv_timer_del(t); return; }
	nk_pos++;
	char part[48];
	memcpy(part, nk_text, nk_pos);
	part[nk_pos] = '\0';
	lv_label_set_text(nk_label, part);
}

static void nk_step(lv_timer_t *t)
{
	nk_i++;
	if (nk_i >= nuke_frame_count) {
		lv_obj_add_flag(nk_scr, LV_OBJ_FLAG_HIDDEN);
		nk_running = false;
		lv_timer_del(t);
		nk_timer = NULL;
		return;
	}
	lv_image_set_src(nk_img, &nuke_frame[nk_i]);
	lv_timer_set_period(t, HOLD[nk_i]);

	/* Caption starts once the cloud is recognisable, not during the flash. */
	if (nk_i == 5) {
		nk_pos = 0;
		lv_label_set_text(nk_label, "");
		lv_timer_create(nk_type, 40, NULL);
	}
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

	nk_img = lv_image_create(nk_scr);
	lv_obj_set_pos(nk_img, 0, 0);
	lv_image_set_src(nk_img, &nuke_frame[0]);

	nk_label = lv_label_create(nk_scr);
	lv_obj_set_style_text_color(nk_label, lv_color_hex(0xFFE8A0), 0);
	lv_obj_set_style_text_font(nk_label, &lv_font_unscii_16, 0);
	lv_obj_align(nk_label, LV_ALIGN_BOTTOM_LEFT, 8, -4);
}

bool nuke_busy(void) { return nk_running; }

void nuke_fire(const char *caption)
{
	if (nk_running) return;
	nk_running = true;

	strncpy(nk_text, caption, sizeof(nk_text) - 1);
	nk_text[sizeof(nk_text) - 1] = '\0';
	nk_pos = 0;
	lv_label_set_text(nk_label, "");

	nk_i = 0;
	lv_image_set_src(nk_img, &nuke_frame[0]);
	lv_obj_clear_flag(nk_scr, LV_OBJ_FLAG_HIDDEN);
	lv_obj_move_foreground(nk_scr);

	nk_timer = lv_timer_create(nk_step, HOLD[0], NULL);
}
