/*
 * lilygo-vaultweather -- Fallout-styled clock and weather station.
 *
 * DISPLAY BRING-UP IS INHERITED VERBATIM from projects/lilygo-pipboy, where it
 * was established by probing. It is the only combination that drives this
 * panel: the Plus is RM67162 over regular 4-wire SPI with GPIO7 as DC and
 * D2/D3 unpopulated, not the QSPI sibling every datasheet describes. Six
 * config permutations failed before this one. Do not tidy it.
 *
 * THREADING -- THE ONE THING TO UNDERSTAND BEFORE EDITING THIS FILE
 * LVGL is not thread-safe, and a single HTTPS fetch to Weathercloud takes
 * seconds: TLS handshake, request, response. Doing that on the render task
 * would freeze the clock for the duration, every ten minutes, forever.
 *
 * So there are two tasks:
 *   - app_main       owns LVGL. Renders, ticks the UI, reads the button.
 *                    Never blocks on the network.
 *   - wx_fetch_task  owns the network. Blocks freely. Writes results into
 *                    `shared` under a mutex and raises `shared_dirty`.
 *
 * The render task notices the flag, copies the state under the same mutex, and
 * calls wx_ui_update. The copy is deliberate: it keeps the mutex held for
 * microseconds rather than for the whole LVGL update, so a slow redraw can
 * never stall a fetch and vice versa.
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <esp_lcd_types.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_ops.h>
#include <esp_heap_caps.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_timer.h>

#include "esp_lcd_panel_rm67162.h"
#include "sdkconfig.h"
#include "lvgl.h"
#include "vaultweather.h"

#define TAG "vaultwx"

#if defined(CONFIG_HWE_DISPLAY_SPI1_HOST)
# define SPIx_HOST SPI1_HOST
#elif defined(CONFIG_HWE_DISPLAY_SPI2_HOST)
# define SPIx_HOST SPI2_HOST
#else
# error "SPI host 1 or 2 must be selected"
#endif

#if defined(CONFIG_HWE_DISPLAY_RST_ACTIVE_LEVEL_LOW)
# define RST_ACTIVE_LEVEL 0
#elif defined(CONFIG_HWE_DISPLAY_RST_ACTIVE_LEVEL_HIGH)
# define RST_ACTIVE_LEVEL 1
#else
# error "RST_ACTIVE_LEVEL must be selected"
#endif

#define SEND_BUF_SIZE ((CONFIG_HWE_DISPLAY_WIDTH * CONFIG_HWE_DISPLAY_HEIGHT \
	* LV_COLOR_FORMAT_GET_SIZE(LV_COLOR_FORMAT_RGB565_SWAPPED)) / 10)

#define LV_TICK_PERIOD_MS 1

/* Physical panel size after the rotation applied in display_bringup(). Named
 * separately from wx_ui.c's PANEL_W/PANEL_H, which are private to that file. */
#define PANEL_W_PX 536
#define PANEL_H_PX 240

/* Poll cadences. Open-Meteo's underlying models update hourly at best and
 * Weathercloud allows 60 requests a minute, so neither of these is anywhere
 * near a limit -- they are chosen to feel live without being wasteful. */
#define CURRENT_PERIOD_S   (10 * 60)
#define FORECAST_PERIOD_S  (30 * 60)

/* Backoff after a failed cycle: 30s, 60s, 2m, 5m, then hold at 10m. A desk
 * clock that hammers a dead endpoint every 30 seconds for a week is a bad
 * citizen; one that gives up entirely never recovers from a router reboot. */
static const int BACKOFF_S[] = { 30, 60, 120, 300, 600 };
#define BACKOFF_N ((int)(sizeof(BACKOFF_S) / sizeof(BACKOFF_S[0])))

/* Portal hand-off. wx_portal_run() blocks for as long as the user takes on
 * their phone, so it runs on its OWN task -- calling it inline froze the whole
 * UI for the entire setup session. The cursor that blinks on the portal screen
 * is the only on-screen proof the firmware is still alive while the user
 * waits, and it is driven by wx_ui_tick() from the render loop; blocking here
 * guaranteed the one liveness indicator was itself dead. */
static volatile bool portal_done;
static esp_err_t     portal_rc;
static char          portal_msg[48];
static volatile bool portal_msg_dirty;

static wx_cfg_t          cfg;
static wx_state_t        shared;          /* written by fetch task, read by render task */
static SemaphoreHandle_t shared_lock;
static volatile bool     shared_dirty;
static volatile bool     force_refresh;   /* set by a long button press */

static esp_lcd_panel_handle_t panel;
static esp_lcd_panel_io_handle_t panel_io;

/* Run LVGL and the 1Hz UI tick for roughly `ms` milliseconds.
 *
 * Every place that used to spin `lv_task_handler()` in a bare loop skipped
 * wx_ui_tick(), which is what blinks the cursor and ages the sync indicator.
 * Routing all of them through here means "waiting" always looks alive. */
static void pump_ms(int ms)
{
	static int64_t last_tick_us;
	int64_t deadline = esp_timer_get_time() + (int64_t)ms * 1000;

	do {
		int64_t now = esp_timer_get_time();
		if (now - last_tick_us >= 1000000) {
			last_tick_us = now;
			wx_ui_tick();
		}
		lv_task_handler();
		vTaskDelay(pdMS_TO_TICKS(20));
	} while (esp_timer_get_time() < deadline);
}

/* Runs on the httpd/portal task, so it must NOT touch LVGL. It only stores;
 * the render task picks the message up in the pump loop below. */
static void portal_progress(const char *msg)
{
	strncpy(portal_msg, msg, sizeof(portal_msg) - 1);
	portal_msg[sizeof(portal_msg) - 1] = '\0';
	portal_msg_dirty = true;
}

static void portal_task(void *arg)
{
	portal_rc  = wx_portal_run(&cfg, portal_progress);
	portal_done = true;
	vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ LVGL */

static bool IRAM_ATTR color_trans_done(esp_lcd_panel_io_handle_t io,
                                       esp_lcd_panel_io_event_data_t *ed,
                                       void *user_ctx)
{
	lv_display_flush_ready((lv_display_t *)user_ctx);
	return false;
}

static void lv_tick_task(void *arg)
{
	lv_tick_inc(LV_TICK_PERIOD_MS);
}

static void disp_flush(lv_display_t *drv, const lv_area_t *area, uint8_t *px_map)
{
	esp_lcd_panel_handle_t p = (esp_lcd_panel_handle_t)lv_display_get_user_data(drv);
	ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(p, area->x1, area->y1,
	                                          area->x2 + 1, area->y2 + 1,
	                                          (uint16_t *)px_map));
}

static lv_display_t *display_bringup(void)
{
	if (CONFIG_HWE_DISPLAY_PWR >= 0) {
		ESP_ERROR_CHECK(gpio_set_direction(CONFIG_HWE_DISPLAY_PWR, GPIO_MODE_OUTPUT));
		ESP_ERROR_CHECK(gpio_set_level(CONFIG_HWE_DISPLAY_PWR,
		                               CONFIG_HWE_DISPLAY_PWR_ON_LEVEL));
		vTaskDelay(pdMS_TO_TICKS(500));
	}

	ESP_ERROR_CHECK(spi_bus_initialize(SPIx_HOST,
		&(spi_bus_config_t){
			.data0_io_num = CONFIG_HWE_DISPLAY_SPI_D0,
#if CONFIG_RM67162_USE_DC_PIN
			/* GPIO7 is the DC line here, not a data lane -- the panel
			 * IO drives it. Claiming it as data1 too would hand one
			 * pin to two owners. */
			.data1_io_num = -1,
#else
			.data1_io_num = CONFIG_HWE_DISPLAY_SPI_D1,
#endif
			.sclk_io_num  = CONFIG_HWE_DISPLAY_SPI_SCK,
			.data2_io_num = CONFIG_HWE_DISPLAY_SPI_D2,
			.data3_io_num = CONFIG_HWE_DISPLAY_SPI_D3,
			/* data4..7 MUST be -1. The compound literal zero-fills them
			 * and 0 is a VALID GPIO, so the SPI driver would claim
			 * GPIO0 -- the BOOT strapping pin. The app then drives it
			 * low, the next reset reads it low, and the board comes up
			 * in DOWNLOAD mode instead of running. */
			.data4_io_num = -1,
			.data5_io_num = -1,
			.data6_io_num = -1,
			.data7_io_num = -1,
			.max_transfer_sz = SEND_BUF_SIZE + 8,
			.flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_GPIO_PINS
#if !defined(CONFIG_HWE_DISPLAY_SPI_SPI)
			       | SPICOMMON_BUSFLAG_QUAD
#endif
			       ,
		},
		SPI_DMA_CH_AUTO));

	esp_lcd_panel_io_handle_t io = NULL;
	ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPIx_HOST,
		&(esp_lcd_panel_io_spi_config_t){
			.cs_gpio_num = CONFIG_HWE_DISPLAY_SPI_CS,
			.pclk_hz     = CONFIG_HWE_DISPLAY_SPI_FREQUENCY,
#if CONFIG_RM67162_USE_DC_PIN
			/* Conventional 4-wire SPI: 8-bit commands with esp_lcd
			 * toggling DC. lcd_cmd_bits 32 belongs to the no-DC framing. */
			.dc_gpio_num  = CONFIG_HWE_DISPLAY_SPI_D1,
			.lcd_cmd_bits = 8,
#else
			.dc_gpio_num  = -1,
			.lcd_cmd_bits = 32,
#endif
			.lcd_param_bits = 8,
#if defined(CONFIG_HWE_DISPLAY_SPI_SPI)
			.spi_mode = 0,
#elif defined(CONFIG_HWE_DISPLAY_SPI_QSPI)
			.spi_mode = 0,
			.flags.quad_mode = 1,
#elif defined(CONFIG_HWE_DISPLAY_SPI_OSPI)
			.spi_mode = 3,
			.flags.octal_mode = 1,
#else
# error "SPI single, quad and octal modes are supported"
#endif
			.trans_queue_depth = 17,
		},
		&io));
	panel_io = io;

	ESP_ERROR_CHECK(esp_lcd_new_panel_rm67162(io,
		&(esp_lcd_panel_dev_config_t){
			.reset_gpio_num = CONFIG_HWE_DISPLAY_RST,
			.flags.reset_active_high = RST_ACTIVE_LEVEL,
			.rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
			.bits_per_pixel = 16,
		},
		&panel));

	ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
	ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
	ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, true));
	ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, true, false));

	lv_init();
	lv_display_t *disp = lv_display_create(CONFIG_HWE_DISPLAY_WIDTH,
	                                       CONFIG_HWE_DISPLAY_HEIGHT);
	ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(io,
		&(esp_lcd_panel_io_callbacks_t){ color_trans_done }, disp));
	lv_display_set_user_data(disp, panel);
	lv_display_set_flush_cb(disp, disp_flush);
	lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565_SWAPPED);

	static lv_color_t *buf[2];
	for (int i = 0; i < 2; i++) {
		buf[i] = heap_caps_malloc(SEND_BUF_SIZE, MALLOC_CAP_DMA);
		assert(buf[i] != NULL);
	}
	lv_display_set_buffers(disp, buf[0], buf[1], SEND_BUF_SIZE,
	                       LV_DISPLAY_RENDER_MODE_PARTIAL);
	lv_disp_set_rotation(disp, LV_DISPLAY_ROTATION_90);

	esp_timer_handle_t tick;
	ESP_ERROR_CHECK(esp_timer_create(
		&(esp_timer_create_args_t){ .callback = &lv_tick_task,
		                            .name = "lv_tick" }, &tick));
	ESP_ERROR_CHECK(esp_timer_start_periodic(tick, LV_TICK_PERIOD_MS * 1000));

	ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));
	return disp;
}

/* ------------------------------------------------------------- screenshot */

/* Dump the live screen over serial as base64 RGB565.
 *
 * There is no other way to see what is actually on this panel. When a layout
 * artefact was reported, re-reading the layout arithmetic proved nothing --
 * every number in it checked out while the artefact was still there. This
 * renders the real composited tree, including whatever is drawn underneath.
 *
 * The buffer is 536*240*2 = 257,280 bytes and comes from PSRAM (allocations
 * above CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384 go there), so it does not
 * touch the scarce internal DRAM that Wi-Fi and TLS need. */
static void screen_dump(void)
{
	static const char B64[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

	/* lv_snapshot_take() allocates from LVGL's own 64KB pool
	 * (CONFIG_LV_MEM_SIZE_KILOBYTES=64), and one RGB565 frame of this panel
	 * is 257,280 bytes -- it fails outright. Supply a PSRAM buffer instead
	 * and use the _to_draw_buf variant. Growing the LVGL pool was the wrong
	 * fix: that pool lives in scarce internal DRAM shared with Wi-Fi. */
	const uint32_t W = PANEL_W_PX, H = PANEL_H_PX;
	const uint32_t stride = W * 2;                 /* RGB565 */
	const uint32_t nbytes = stride * H;

	uint8_t *mem = heap_caps_malloc(nbytes, MALLOC_CAP_SPIRAM);
	if (!mem) {
		ESP_LOGE(TAG, "SNAP: %lu byte PSRAM alloc failed",
		         (unsigned long)nbytes);
		return;
	}

	lv_draw_buf_t buf;
	if (lv_draw_buf_init(&buf, W, H, LV_COLOR_FORMAT_RGB565, stride,
	                     mem, nbytes) != LV_RESULT_OK) {
		ESP_LOGE(TAG, "SNAP: draw_buf init failed");
		free(mem);
		return;
	}

	lv_obj_t *s = lv_display_get_screen_active(lv_display_get_default());
	if (lv_snapshot_take_to_draw_buf(s, LV_COLOR_FORMAT_RGB565, &buf)
	    != LV_RESULT_OK) {
		ESP_LOGE(TAG, "SNAP: render failed");
		free(mem);
		return;
	}

	lv_draw_buf_t *db = &buf;
	const uint8_t *d = db->data;
	uint32_t n = nbytes;
	printf("\nSNAP_BEGIN w=%lu h=%lu stride=%lu bytes=%lu\n",
	       (unsigned long)W, (unsigned long)H,
	       (unsigned long)stride, (unsigned long)n);

	/* 57 input bytes -> 76 output chars per line. Chunked so nothing large
	 * is allocated a second time. */
	char line[80];
	uint32_t i = 0;
	while (i < n) {
		int o = 0;
		for (int k = 0; k < 19 && i < n; k++) {
			uint32_t v = (uint32_t)d[i] << 16;
			int have = 1;
			if (i + 1 < n) { v |= (uint32_t)d[i + 1] << 8; have++; }
			if (i + 2 < n) { v |= (uint32_t)d[i + 2];      have++; }
			line[o++] = B64[(v >> 18) & 63];
			line[o++] = B64[(v >> 12) & 63];
			line[o++] = have > 1 ? B64[(v >> 6) & 63] : '=';
			line[o++] = have > 2 ? B64[v & 63]        : '=';
			i += have;
		}
		line[o] = 0;
		printf("%s\n", line);
	}
	printf("SNAP_END\n");
	free(mem);
}

/* ----------------------------------------------------------- fetch task */

static void publish(const wx_state_t *st)
{
	xSemaphoreTake(shared_lock, portMAX_DELAY);
	shared = *st;
	xSemaphoreGive(shared_lock);
	shared_dirty = true;
}

static void wx_fetch_task(void *arg)
{
	wx_state_t st;

	/* Start from whatever survived the last power cycle, so the very first
	 * publish already has something to show rather than blanks. */
	xSemaphoreTake(shared_lock, portMAX_DELAY);
	st = shared;
	xSemaphoreGive(shared_lock);

	time_t next_current  = 0;      /* 0 = due immediately */
	time_t next_forecast = 0;
	int    fail_streak   = 0;

	for (;;) {
		time_t now = time(NULL);

		if (force_refresh) {
			force_refresh = false;
			next_current = next_forecast = 0;
			fail_streak  = 0;
			now = time(NULL);
		}

		if (!wx_net_is_up()) {
			/* Nothing to do but wait. The reconnect logic lives in
			 * wx_net.c; hammering a fetch while disassociated just
			 * burns time in DNS timeouts. */
			vTaskDelay(pdMS_TO_TICKS(2000));
			continue;
		}

		bool progressed = false;

		if (now >= next_current) {
			const char *note = NULL;
			if (wx_fetch_current(&cfg, &st.cur, &note) == ESP_OK) {
				st.have_current     = true;
				st.cur_source_note  = note;
				st.last_ok          = time(NULL);
				if (st.cur.temp_f.src == WX_SRC_PWS)
					st.last_pws_ok = st.last_ok;
				next_current = st.last_ok + CURRENT_PERIOD_S;
				progressed   = true;
				/* Which tier won, and whether the API-only fields
				 * actually arrived. A silent ESP_OK is not evidence:
				 * the forecast fetch failed for a week's worth of
				 * cycles with nothing in the log but an absence. */
				ESP_LOGI(TAG, "current ok via %s (uv=%d cloud=%d code=%d)",
				         note ? note : "?",
				         st.cur.uv.valid, st.cur.cloud_pct.valid,
				         st.cur.weather_code);
			} else {
				ESP_LOGW(TAG, "current fetch FAILED");
			}
		}

		if (now >= next_forecast) {
			time_t sr = 0, ss = 0;
			if (wx_fetch_forecast(&cfg, st.day, WX_FORECAST_DAYS,
			                      &sr, &ss) == ESP_OK) {
				st.have_forecast = true;
				st.sunrise = sr;
				st.sunset  = ss;
				st.last_ok = time(NULL);
				next_forecast = st.last_ok + FORECAST_PERIOD_S;
				progressed    = true;

				int nd = 0;
				for (int i = 0; i < WX_FORECAST_DAYS; i++)
					if (st.day[i].valid) nd++;
				ESP_LOGI(TAG, "forecast ok: %d/%d days valid, sunrise=%lld sunset=%lld",
				         nd, WX_FORECAST_DAYS, (long long)sr, (long long)ss);
			} else {
				ESP_LOGW(TAG, "forecast fetch FAILED");
			}
		}

		if (progressed) {
			fail_streak = 0;
			publish(&st);
			/* wx_cache_save rate-limits itself; calling it on every
			 * successful cycle is safe. See wx_cfg.c. */
			wx_cache_save(&st);
		} else if (now >= next_current || now >= next_forecast) {
			int idx = fail_streak < BACKOFF_N ? fail_streak : BACKOFF_N - 1;
			int wait = BACKOFF_S[idx];
			if (fail_streak < BACKOFF_N) fail_streak++;
			ESP_LOGW(TAG, "fetch failed, retry in %ds", wait);
			/* Push BOTH deadlines out so a failing forecast does not
			 * spin the loop on the current-conditions deadline. */
			next_current  = time(NULL) + wait;
			next_forecast = time(NULL) + wait;
			/* Still publish: the UI needs to see last_ok age. */
			publish(&st);
		}

		vTaskDelay(pdMS_TO_TICKS(1000));
	}
}

/* --------------------------------------------------------- brightness */

/* RM67162 WRDISBV -- write display brightness, 0x00..0xFF.
 *
 * The vendored driver's disp_on_off leaves the panel at 0xD0 (208/255, 82%)
 * and nothing ever raises it, so nearly a quarter of the panel's output was
 * simply unused. This drives it directly.
 *
 * This is a REAL brightness change: the AMOLED reduces emission. The UI's
 * fallback path instead composites black over the image, which leaves every
 * pixel lit and only washes the picture grey. */
static void panel_set_brightness(uint8_t level)
{
	if (!panel_io)
		return;
	esp_err_t e = esp_lcd_panel_io_tx_param(panel_io, 0x51,
	                                        (uint8_t[]){ level }, 1);
	if (e != ESP_OK)
		ESP_LOGW(TAG, "WRDISBV %u failed: %s", level, esp_err_to_name(e));
}

/* Ramp the panel down after local sunset and back up at sunrise. The times
 * come from the forecast payload we already fetch, so "night" means actual
 * darkness wherever the board is pointed rather than a hardcoded clock hour.
 *
 * Returns 0-255. The floor is deliberately not 0: a clock you cannot read in
 * the dark is not a clock. */
#define BRIGHT_DAY   255      /* was effectively 208: the driver's 0xD0 default */
#define BRIGHT_NIGHT 150      /* was 60 THROUGH A BLACK OVERLAY, which is why it
                               * looked murky rather than merely dim. A real
                               * WRDISBV level of 150 is legible at night without
                               * lighting the room. */

static uint8_t brightness_for(const wx_state_t *st, time_t now)
{
	if (st->sunrise == 0 || st->sunset == 0)
		return BRIGHT_DAY;            /* no sun data yet */

	struct tm tm_now, tm_rise, tm_set;
	localtime_r(&now, &tm_now);
	localtime_r(&st->sunrise, &tm_rise);
	localtime_r(&st->sunset,  &tm_set);

	int mins      = tm_now.tm_hour  * 60 + tm_now.tm_min;
	int rise_mins = tm_rise.tm_hour * 60 + tm_rise.tm_min;
	int set_mins  = tm_set.tm_hour  * 60 + tm_set.tm_min;

	/* Compare minutes-of-day, not absolute times: sunrise/sunset in the
	 * payload are for the forecast's days, and by late evening "today's"
	 * sunset may already be in the past as an absolute timestamp while
	 * still being the right daily boundary. */
	return (mins >= rise_mins && mins < set_mins) ? BRIGHT_DAY : BRIGHT_NIGHT;
}

/* --------------------------------------------------------------- main */

void app_main(void)
{
	shared_lock = xSemaphoreCreateMutex();
	assert(shared_lock);

	memset(&shared, 0, sizeof(shared));
	shared.cur_source_note = "";

	ESP_ERROR_CHECK(wx_cfg_load(&cfg));

	lv_display_t *disp = display_bringup();
	wx_ui_init(disp);
	wx_ui_set_panel_bright_cb(panel_set_brightness);
	/* esp_lcd_panel_disp_on_off() set 0xD0 during bring-up; go to full so the
	 * splash and the setup portal are at the same brightness as the running
	 * app rather than dimmer than it. */
	panel_set_brightness(BRIGHT_DAY);
	wx_btn_init();
	wx_ui_status("VAULT-TEC WEATHER STATION");

	/* Get the splash actually onto the panel before anything blocks.
	 * Without this the first thing the user sees is the result of whatever
	 * happens next, several seconds later. */
	pump_ms(400);

	if (!cfg.configured) {
		ESP_LOGI(TAG, "no configuration -- entering setup portal");
		wx_ui_show_portal(WX_PORTAL_SSID, "http://192.168.4.1");
		pump_ms(400);

		/* The portal owns a task; this task keeps rendering. Setup can
		 * take minutes of human time and the screen must stay alive for
		 * all of it. */
		portal_done = false;
		portal_msg_dirty = false;
		xTaskCreate(portal_task, "wx_portal", 8192, NULL, 4, NULL);

		while (!portal_done) {
			if (portal_msg_dirty) {
				portal_msg_dirty = false;
				wx_ui_status(portal_msg);
			}
			pump_ms(40);
		}
		ESP_ERROR_CHECK(portal_rc);
	}

	wx_ui_status("CONNECTING...");
	pump_ms(200);

	if (wx_net_connect(&cfg, 30000) != ESP_OK)
		ESP_LOGW(TAG, "initial association failed; wx_net will keep retrying");

	wx_ui_status("SYNCING CLOCK...");
	pump_ms(200);
	wx_time_sync(15000);

	/* Show whatever survived the power cycle immediately, clearly marked
	 * stale by its age, rather than an empty screen while the first fetch
	 * completes. */
	{
		wx_state_t cached;
		memset(&cached, 0, sizeof(cached));
		if (wx_cache_load(&cached) == ESP_OK && cached.have_current) {
			publish(&cached);
			ESP_LOGI(TAG, "restored cached weather from NVS");
		}
	}

	xTaskCreate(wx_fetch_task, "wx_fetch", 8192, NULL, 4, NULL);

	int64_t last_tick_us = 0;
	uint8_t cur_bright   = 0xFF;

	for (;;) {
		if (shared_dirty) {
			wx_state_t snap;
			xSemaphoreTake(shared_lock, portMAX_DELAY);
			snap = shared;
			shared_dirty = false;
			xSemaphoreGive(shared_lock);
			wx_ui_update(&snap);

			uint8_t b = brightness_for(&snap, time(NULL));
			if (b != cur_bright) { cur_bright = b; wx_ui_set_brightness(b); }
		}

		/* Apply any timezone the fetch task asked for. This happens HERE,
		 * on the render task, because newlib's tzset() rewrites global
		 * _timezone/_tzname in place and this same task calls
		 * localtime_r() once a second -- a concurrent tzset() from the
		 * fetch task could be read mid-update and render an hour-wrong
		 * clock. Rare (it changes twice a year) and therefore exactly the
		 * kind of bug nobody would ever reproduce on demand. */
		{
			char tz[40];
			if (wx_time_take_pending_tz(tz, sizeof(tz))) {
				ESP_LOGI(TAG, "timezone -> %s", tz);
				wx_time_set_tz(tz);
				/* A whole-hour offset does not change tm_min, so
				 * the clock's minute-change guard would hold the
				 * old UTC hour on screen until the next minute. */
				wx_ui_invalidate_clock();
			}
		}

		int64_t now_us = esp_timer_get_time();
		if (now_us - last_tick_us >= 1000000) {
			last_tick_us = now_us;
			wx_ui_tick();
		}

		switch (wx_btn_poll()) {
		case WX_BTN_SHORT:
			wx_ui_next_panel();
			break;
		case WX_BTN_LONG:
			wx_ui_status("REFRESHING...");
			force_refresh = true;
			break;
		case WX_BTN_SNAP:
			/* Deliberate gesture only. This used to fire three times
			 * automatically at every boot, pushing ~750KB over serial
			 * -- fine while iterating on the layout, wrong for a
			 * device that runs unattended for months. */
			wx_ui_status("SCREEN DUMP");
			screen_dump();
			break;
		case WX_BTN_HOLD:
			/* Erase and reboot rather than re-entering the portal in
			 * place: the Wi-Fi stack is already up in STA mode and
			 * tearing it down cleanly enough to start an AP is more
			 * failure surface than a reset, which costs a second. */
			ESP_LOGW(TAG, "button held -- erasing configuration");
			wx_ui_status("CONFIG ERASED -- RESTARTING");
			pump_ms(800);
			wx_cfg_erase();
			esp_restart();
			break;
		default:
			break;
		}

		/* Sleep for as long as LVGL says it has nothing to do, floored at
		 * 20ms. A flat 5ms delay starved IDLE0 and tripped the task
		 * watchdog in the sibling project; a full-screen redraw here is
		 * DMA-bound and needs the headroom. */
		uint32_t next = lv_task_handler();
		if (next == LV_NO_TIMER_READY || next > 50) next = 50;
		if (next < 20) next = 20;
		vTaskDelay(pdMS_TO_TICKS(next));
	}
}
