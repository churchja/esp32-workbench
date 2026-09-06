/*
 * PIP-BOY 3000 MK IV -- board identity dashboard and pin verifier
 * LilyGo T-Display S3 AMOLED Plus, 536x240 RM67162.
 *
 * Two jobs:
 *   1. Show what this board actually is, read from silicon at runtime.
 *   2. Verify pins that boards/e4b0638aec2c.yaml marks `unverified`.
 *
 * WHY THIS SCANS RATHER THAN POLLING ONE PIN
 * The first version polled GPIO0 alone, because three sources say button_1 is
 * GPIO0 -- exactly the agreed-but-untested kind of claim the profile flags. A
 * press produced nothing, which is ambiguous: wrong pin, or a 500ms poll too
 * slow to catch it. This version watches every safe candidate at 50ms and
 * names whichever one moves, which answers both questions at once.
 *
 * PINS DELIBERATELY NOT SCANNED
 *   47,18,6,17,7,38,9  display SCK/MOSI/CS/RST/DC/PWR/TE -- driving them
 *                      would fight the panel
 *   33..37             octal PSRAM on this module
 *   43,44              UART0 console
 *   45,46              strapping
 *
 * Green is #1CFF4A; a self-emissive panel gives true black, so the phosphor
 * look works here in a way it does not on a backlit LCD.
 */
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "lvgl.h"

#define TAG "pipboy"

#define PIN_LCD_TE     9
#define PIN_LCD_D2     48
#define PIN_LCD_D3     5

/* Widened after the first 14 all read a steady HIGH under pull-up -- the
 * scanner was working, so the button simply was not on any of them.
 *
 * GPIO5 and GPIO48 are the notable additions. They are lcd_d3/lcd_d2 in the
 * QSPI pinout, which is why the first pass excluded them -- but the Plus does
 * NOT wire them to the panel (RM67162_AMOLED_SPI sets both to -1, the finding
 * that made this display work at all). On this board they are free pins, and
 * therefore plausible button pins. 4, 8, 15 and 39 were simply missed.
 *
 * Still excluded: 19/20 are USB D-/D+ and touching them kills the CDC port. */
static const int cand[] = { 0, 1, 2, 3, 4, 5, 8, 10, 11, 12, 13, 14,
                            15, 16, 21, 39, 40, 41, 42, 48 };
#define N_CAND (sizeof(cand) / sizeof(cand[0]))
static bool cand_low[N_CAND];
static bool cand_seen_low[N_CAND];

#define PB_GREEN       lv_color_hex(0x1CFF4A)
#define PB_DIM         lv_color_hex(0x0E7F25)
#define PB_BLACK       lv_color_hex(0x000000)
#define OVERLAY_MS     4000

static lv_obj_t *lbl_header, *lbl_body, *lbl_status, *lbl_overlay;
static lv_obj_t *scan_bar;
static bool overlay_active;

static uint32_t te_edges;
static bool     te_verified;
static float    te_hz;
static void IRAM_ATTR te_isr(void *arg) { te_edges++; }

/* --------------------------------------------------------------------- */
static void board_facts(char *out, size_t cap)
{
	/* Identity never changes -- read ONCE. esp_flash_get_size() touches the
	 * flash controller and heap_caps_get_total_size() walks heap structures
	 * under a lock; doing both per frame starved the idle task and tripped
	 * the task watchdog. Only heap and uptime are live. */
	static bool cached;
	static esp_chip_info_t ci;
	static uint8_t mac[6];
	static uint32_t flash_sz;
	static size_t psram;
	if (!cached) {
		esp_chip_info(&ci);
		esp_efuse_mac_get_default(mac);
		esp_flash_get_size(NULL, &flash_sz);
		psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
		cached = true;
	}
	size_t freek = heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024;
	int64_t up = esp_timer_get_time() / 1000000;

	/* <=12 characters per line. At 2x scale unscii is 16px/char, so 12 chars
	 * is 192px -- inside the 240px narrow axis whichever way the panel ends
	 * up rotated. The first version used 17-char lines (272px) and ran off
	 * the edge. */
	snprintf(out, cap,
		"%02X%02X%02X%02X%02X%02X\n"
		"S3 rev v%d.%d\n"
		"%" PRIu32 "MB/%uMB\n"
		"HEAP %uK\n"
		"UP %02d:%02d:%02d",
		mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
		ci.revision / 100, ci.revision % 100,
		flash_sz / (1024 * 1024), (unsigned)(psram / (1024 * 1024)),
		(unsigned)freek,
		(int)(up / 3600), (int)((up / 60) % 60), (int)(up % 60));
}

/* -----------------------------------------------------------------------
 * VAULT DOOR ANIMATION
 *
 * Three things move at once for the ~4s it holds the screen:
 *   - a heavy ring spins a full turn (the door gear)
 *   - its arc opens from a sliver to the whole circle (the door rolling back)
 *   - the caption types itself out one character at a time
 *
 * The typewriter is the part that reads as Fallout: the terminal games in the
 * series reveal text character by character, and a bitmap font makes each step
 * a visible whole-pixel jump rather than a smooth fade.
 * --------------------------------------------------------------------- */
static lv_obj_t *door_ring;
static char      type_full[64];
static size_t    type_pos;

static void anim_rotate_cb(void *obj, int32_t v)
{
	lv_obj_set_style_transform_rotation((lv_obj_t *)obj, v, 0);
}

static void anim_arc_cb(void *obj, int32_t v)
{
	lv_arc_set_end_angle((lv_obj_t *)obj, v);
}

static void anim_scale_cb(void *obj, int32_t v)
{
	lv_obj_set_style_transform_scale((lv_obj_t *)obj, v, 0);
}

static void typewriter(lv_timer_t *t)
{
	if (type_pos >= strlen(type_full)) {
		lv_timer_del(t);
		return;
	}
	type_pos++;
	char partial[64];
	memcpy(partial, type_full, type_pos);
	partial[type_pos] = '\0';
	lv_label_set_text(lbl_overlay, partial);
}

static void overlay_clear(lv_timer_t *t)
{
	lv_obj_add_flag(lbl_overlay, LV_OBJ_FLAG_HIDDEN);
	lv_obj_add_flag(door_ring, LV_OBJ_FLAG_HIDDEN);
	lv_obj_clear_flag(lbl_body, LV_OBJ_FLAG_HIDDEN);
	overlay_active = false;
	lv_timer_del(t);
}

static void overlay_show(const char *text)
{
	if (overlay_active) return;
	overlay_active = true;

	lv_obj_add_flag(lbl_body, LV_OBJ_FLAG_HIDDEN);
	lv_obj_clear_flag(lbl_overlay, LV_OBJ_FLAG_HIDDEN);
	lv_obj_clear_flag(door_ring, LV_OBJ_FLAG_HIDDEN);

	/* caption types itself in */
	strncpy(type_full, text, sizeof(type_full) - 1);
	type_full[sizeof(type_full) - 1] = '\0';
	type_pos = 0;
	lv_label_set_text(lbl_overlay, "");
	lv_timer_create(typewriter, 45, NULL);

	/* the gear spins one full turn */
	lv_anim_t spin;
	lv_anim_init(&spin);
	lv_anim_set_var(&spin, door_ring);
	lv_anim_set_exec_cb(&spin, anim_rotate_cb);
	lv_anim_set_values(&spin, 0, 3600);          /* 0.1 deg units */
	lv_anim_set_duration(&spin, 3000);
	lv_anim_start(&spin);

	/* the door rolls open: arc sweeps from a sliver to the full circle */
	lv_anim_t open;
	lv_anim_init(&open);
	lv_anim_set_var(&open, door_ring);
	lv_anim_set_exec_cb(&open, anim_arc_cb);
	lv_anim_set_values(&open, 20, 360);
	lv_anim_set_duration(&open, 1400);
	lv_anim_set_path_cb(&open, lv_anim_path_ease_out);
	lv_anim_start(&open);

	/* and it lurches toward the viewer as it releases */
	lv_anim_t push;
	lv_anim_init(&push);
	lv_anim_set_var(&push, door_ring);
	lv_anim_set_exec_cb(&push, anim_scale_cb);
	lv_anim_set_values(&push, 180, 256);
	lv_anim_set_duration(&push, 1400);
	lv_anim_set_path_cb(&push, lv_anim_path_overshoot);
	lv_anim_start(&push);

	lv_timer_t *t = lv_timer_create(overlay_clear, OVERLAY_MS, NULL);
	lv_timer_set_repeat_count(t, 1);
}

/* --------------------------------------------------------------------- */
/* 50ms: fast enough that a human press cannot fall between samples. The 500ms
 * tick used before could miss a short press outright, which is one of the two
 * reasons the first result was ambiguous. */
static void scan_pins(lv_timer_t *t)
{
	for (size_t i = 0; i < N_CAND; i++) {
		bool low = (gpio_get_level(cand[i]) == 0);
		if (low && !cand_low[i]) {
			char msg[192];
			if (!cand_seen_low[i]) {
				cand_seen_low[i] = true;
				ESP_LOGI(TAG,
					 "VERIFY button on GPIO%d: went LOW with "
					 "internal pull-up -- CONCLUSIVE, a button "
					 "is wired here", cand[i]);
			}
			snprintf(msg, sizeof(msg),
				 ">> DOOR OPEN GPIO%d", cand[i]);
			overlay_show(msg);
		}
		cand_low[i] = low;
	}
}

/* --------------------------------------------------------------------- */
static void tick(lv_timer_t *t)
{
	static int ticks;
	char buf[256];
	ticks++;

	if (ticks % 4 == 0) {                /* ~2s at 500ms */
		uint32_t n = te_edges;
		te_edges = 0;
		te_hz = n / 2.0f;
		if (!te_verified && n > 20) {
			te_verified = true;
			ESP_LOGI(TAG,
				 "VERIFY lcd_te (GPIO%d): %.1f Hz over 2s -- pin "
				 "CONFIRMED driven by the panel; the RATE is "
				 "unexplained (frames? lines? edge count?) and "
				 "must NOT be recorded as a frame rate",
				 PIN_LCD_TE, (double)te_hz);
		}
	}

	if (!overlay_active) {
		board_facts(buf, sizeof(buf));
		lv_label_set_text(lbl_body, buf);
	}

	char st[192], pins[128] = "";
	for (size_t i = 0; i < N_CAND; i++) {
		if (cand_seen_low[i]) {
			char one[16];
			snprintf(one, sizeof(one), "G%d ", cand[i]);
			strncat(pins, one, sizeof(pins) - strlen(pins) - 1);
		}
	}
	snprintf(st, sizeof(st), "TE %.0fHz  BTN %s",
		 (double)te_hz, pins[0] ? pins : "press one");
	lv_label_set_text(lbl_status, st);

	/* Raw level dump. If a press changes nothing here, the button is not on
	 * any scanned pin -- and if every pin reads the same constant, the
	 * scanner is broken rather than the board. */
	if (ticks % 4 == 0) {
		char lv[160] = "";
		for (size_t i = 0; i < N_CAND; i++) {
			char one[16];
			snprintf(one, sizeof(one), "%d=%d ",
				 cand[i], gpio_get_level(cand[i]));
			strncat(lv, one, sizeof(lv) - strlen(lv) - 1);
		}
		ESP_LOGI(TAG, "levels %s", lv);
	}

	lv_obj_set_y(scan_bar, (ticks * 17) % lv_display_get_vertical_resolution(NULL));
}

/* --------------------------------------------------------------------- */
static void boot_done(lv_timer_t *t)
{
	lv_obj_clear_flag(lbl_body, LV_OBJ_FLAG_HIDDEN);
	lv_obj_clear_flag(lbl_status, LV_OBJ_FLAG_HIDDEN);
	lv_label_set_text(lbl_header, "PIP-BOY 3000  STAT");
	lv_timer_create(tick, 500, NULL);
	lv_timer_create(scan_pins, 50, NULL);
	lv_timer_del(t);
}

void pipboy_ui(lv_display_t *disp)
{
	lv_obj_t *scr = lv_display_get_screen_active(disp);
	lv_obj_set_style_bg_color(scr, PB_BLACK, 0);
	lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
	lv_obj_set_style_pad_all(scr, 0, 0);

	/* Ask the display for its size rather than hardcoding it. The panel is
	 * 240x536 NATIVE and rotated 90deg, so "536 wide" was an assumption --
	 * and the wrong one for laying text out against. */
	int32_t hres = lv_display_get_horizontal_resolution(disp);
	int32_t vres = lv_display_get_vertical_resolution(disp);
	ESP_LOGW(TAG, "LVGL canvas after rotation: %" PRId32 " x %" PRId32
		      " (native %dx%d, 1.91in RM67162)",
		 hres, vres, CONFIG_HWE_DISPLAY_WIDTH, CONFIG_HWE_DISPLAY_HEIGHT);

	scan_bar = lv_obj_create(scr);
	lv_obj_set_size(scan_bar, hres, 3);
	lv_obj_set_style_bg_color(scan_bar, PB_DIM, 0);
	lv_obj_set_style_border_width(scan_bar, 0, 0);
	lv_obj_set_style_bg_opa(scan_bar, LV_OPA_30, 0);

	lbl_header = lv_label_create(scr);
	lv_obj_set_style_text_color(lbl_header, PB_GREEN, 0);
	lv_obj_set_style_text_font(lbl_header, &lv_font_unscii_16, 0);
	lv_obj_align(lbl_header, LV_ALIGN_TOP_LEFT, 10, 6);

	/* BODY AT 2x. unscii is a bitmap face, so LVGL's transform scales it by
	 * whole pixels: 8x16 becomes a chunky 16x32 with hard edges. That is
	 * both far more legible at arm's length and truer to the look than
	 * swapping in a smooth proportional face like Montserrat. */
	lbl_body = lv_label_create(scr);
	lv_obj_set_style_text_color(lbl_body, PB_GREEN, 0);
	lv_obj_set_style_text_font(lbl_body, &lv_font_unscii_16, 0);
	lv_obj_set_style_transform_scale(lbl_body, 512, 0);   /* 256 = 100% */
	lv_obj_set_style_transform_pivot_x(lbl_body, 0, 0);
	lv_obj_set_style_transform_pivot_y(lbl_body, 0, 0);
	lv_obj_align(lbl_body, LV_ALIGN_TOP_LEFT, 14, 44);
	lv_obj_add_flag(lbl_body, LV_OBJ_FLAG_HIDDEN);

	lbl_overlay = lv_label_create(scr);
	lv_obj_set_style_text_color(lbl_overlay, PB_GREEN, 0);
	lv_obj_set_style_text_font(lbl_overlay, &lv_font_unscii_16, 0);
	lv_obj_set_style_transform_scale(lbl_overlay, 512, 0);
	lv_obj_set_style_transform_pivot_x(lbl_overlay, 0, 0);
	lv_obj_set_style_transform_pivot_y(lbl_overlay, 0, 0);
	lv_obj_align(lbl_overlay, LV_ALIGN_BOTTOM_MID, 0, -40);
	lv_obj_add_flag(lbl_overlay, LV_OBJ_FLAG_HIDDEN);

	/* The vault door itself: a heavy ring, centred, hidden until triggered.
	 * lv_arc rather than an image so it can be spun and swept by animation
	 * rather than by cycling frames. */
	door_ring = lv_arc_create(scr);
	lv_obj_set_size(door_ring, 150, 150);
	lv_obj_center(door_ring);
	lv_obj_remove_style(door_ring, NULL, LV_PART_KNOB);
	lv_obj_clear_flag(door_ring, LV_OBJ_FLAG_CLICKABLE);
	lv_arc_set_bg_angles(door_ring, 0, 360);
	lv_obj_set_style_arc_color(door_ring, PB_DIM, LV_PART_MAIN);
	lv_obj_set_style_arc_width(door_ring, 6, LV_PART_MAIN);
	lv_obj_set_style_arc_color(door_ring, PB_GREEN, LV_PART_INDICATOR);
	lv_obj_set_style_arc_width(door_ring, 14, LV_PART_INDICATOR);
	lv_obj_set_style_transform_pivot_x(door_ring, 75, 0);
	lv_obj_set_style_transform_pivot_y(door_ring, 75, 0);
	lv_obj_add_flag(door_ring, LV_OBJ_FLAG_HIDDEN);

	lbl_status = lv_label_create(scr);
	lv_obj_set_style_text_color(lbl_status, PB_DIM, 0);
	lv_obj_set_style_text_font(lbl_status, &lv_font_unscii_16, 0);
	lv_obj_align(lbl_status, LV_ALIGN_BOTTOM_LEFT, 10, -6);
	lv_obj_add_flag(lbl_status, LV_OBJ_FLAG_HIDDEN);

	/* --- pins ------------------------------------------------------ */
	uint64_t mask = 0;
	for (size_t i = 0; i < N_CAND; i++)
		mask |= 1ULL << cand[i];
	/* Check the return. A single invalid pin in the mask fails the WHOLE
	 * call, leaving every candidate unconfigured -- which reads exactly
	 * like "no button is wired anywhere". Verify the instrument before
	 * trusting a null result from it. */
	esp_err_t cfg = gpio_config(&(gpio_config_t) {
		.pin_bit_mask = mask,
		.mode = GPIO_MODE_INPUT,
		.pull_up_en = GPIO_PULLUP_ENABLE,
		.pull_down_en = GPIO_PULLDOWN_DISABLE,
		.intr_type = GPIO_INTR_DISABLE,
	});
	ESP_LOGW(TAG, "gpio_config(scan mask 0x%llx) -> %s",
		 (unsigned long long)mask, esp_err_to_name(cfg));
	for (size_t i = 0; i < N_CAND; i++)
		cand_low[i] = (gpio_get_level(cand[i]) == 0);

	gpio_config(&(gpio_config_t) {
		.pin_bit_mask = 1ULL << PIN_LCD_TE,
		.mode = GPIO_MODE_INPUT,
		.intr_type = GPIO_INTR_POSEDGE,
	});
	gpio_install_isr_service(0);
	gpio_isr_handler_add(PIN_LCD_TE, te_isr, NULL);

	/* d2/d3 cannot be settled here: the Plus drives the panel over 4-wire
	 * SPI, so an idle level on GPIO48/GPIO5 looks identical whether or not
	 * the panel is wired to them. A PASS here would be fabricated. */
	ESP_LOGI(TAG, "VERIFY lcd_d2 (GPIO%d) / lcd_d3 (GPIO%d): no test exists "
		      "on this transport -- NOT conclusive, leave unverified",
		 PIN_LCD_D2, PIN_LCD_D3);

	for (size_t i = 0; i < N_CAND; i++)
		if (cand_low[i])
			ESP_LOGW(TAG, "GPIO%d reads LOW at boot -- held, or not "
				      "a button", cand[i]);

	lv_label_set_text(lbl_header,
		"ROBCO INDUSTRIES\n"
		"TERMLINK\n\n"
		" PANEL .. ONLINE\n"
		" EFUSE .. ONLINE\n"
		" SCAN ... ARMED\n\n"
		" PRESS A BUTTON");
	lv_timer_t *t = lv_timer_create(boot_done, 4500, NULL);
	lv_timer_set_repeat_count(t, 1);
}
