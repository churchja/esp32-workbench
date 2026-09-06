/*
 * SPDX-FileCopyrightText: 2024-2026 Eugene Crosser
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * This is a demo application that shows how to use OLED display connected
 * to ESP32 SoC via rm67162 SPI adapter, using ESP-IDF LCD framework.
 * It makes use of the driver `esp_lcd_panel_rm67162`.
 */

#include <stdbool.h>
#include <stdio.h>
#include <freertos/FreeRTOS.h>
#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <esp_lcd_types.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_ops.h>
#include <esp_heap_caps.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_sleep.h>
#include <esp_timer.h>
#include "esp_lcd_panel_rm67162.h"
#include "sdkconfig.h"
#include "lvgl.h"

#define TAG "lvgl_demo"

#if defined(CONFIG_HWE_DISPLAY_SPI1_HOST)
# define SPIx_HOST SPI1_HOST
#elif defined(CONFIG_HWE_DISPLAY_SPI2_HOST)
# define SPIx_HOST SPI2_HOST
#else
# error "SPI host 1 or 2 must be selected"
#endif

#if defined(CONFIG_HWE_DISPLAY_SPI_MODE0)
# define SPI_MODEx (0)
#elif defined(CONFIG_HWE_DISPLAY_SPI_MODE3)
# define SPI_MODEx (2)
#else
# error "SPI MODE0 or MODE3 must be selected"
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

extern void pipboy_ui(lv_display_t *disp);

static volatile int stop_request = 0;

/*
static void poweroff(void *arg)
{
	stop_request++;
}
*/

static bool IRAM_ATTR color_trans_done(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
	lv_display_t *disp = (lv_display_t*)user_ctx;
	lv_display_flush_ready(disp);
	// Whether a high priority task has been waken up by this function
	return false; 
}

static void lv_tick_task(void *arg) {
	lv_tick_inc(LV_TICK_PERIOD_MS);
}

static void disp_flush(lv_display_t *disp_drv, const lv_area_t *area,
		uint8_t *px_map)
{
	esp_lcd_panel_handle_t panel_handle =
		(esp_lcd_panel_handle_t)lv_display_get_user_data(disp_drv);
	ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel_handle,
			area->x1, area->y1,
			area->x2 + 1, area->y2 + 1,
			(uint16_t *) px_map));
}

void app_main(void)
{
	if (CONFIG_HWE_DISPLAY_PWR >= 0) {
		ESP_LOGI(TAG, "Turn on display power");
		ESP_ERROR_CHECK(gpio_set_direction(CONFIG_HWE_DISPLAY_PWR,
					GPIO_MODE_OUTPUT));
		ESP_ERROR_CHECK(gpio_set_level(CONFIG_HWE_DISPLAY_PWR,
					CONFIG_HWE_DISPLAY_PWR_ON_LEVEL));
		vTaskDelay(pdMS_TO_TICKS(500));
	}

	ESP_LOGI(TAG, "Initialize SPI bus");
	ESP_ERROR_CHECK(spi_bus_initialize(SPIx_HOST,
		& (spi_bus_config_t) {
			.data0_io_num = CONFIG_HWE_DISPLAY_SPI_D0,
#if CONFIG_RM67162_USE_DC_PIN
			/* GPIO7 is the DC line here, not a data lane -- the
			 * panel IO drives it. Claiming it as data1 as well
			 * would hand the same pin to two owners. */
			.data1_io_num = -1,
#else
			.data1_io_num = CONFIG_HWE_DISPLAY_SPI_D1,
#endif
			.sclk_io_num = CONFIG_HWE_DISPLAY_SPI_SCK,
			.data2_io_num = CONFIG_HWE_DISPLAY_SPI_D2,
			.data3_io_num = CONFIG_HWE_DISPLAY_SPI_D3,
			// data4..data7 MUST be -1 when unused. The compound literal
			// zero-initialises them, and 0 is a VALID GPIO -- so the SPI
			// driver claims GPIO0, which is the ESP32-S3 BOOT strapping
			// pin. The app then drives it low and the next reset reads it
			// low, so the board comes up in DOWNLOAD mode instead of
			// running. Symptom: "spi_common: GPIO 0 is conflict with
			// others and be overwritten" at init, then boot:0x21
			// (DOWNLOAD(USB/UART0)) on every subsequent reset.
			.data4_io_num = -1,
			.data5_io_num = -1,
			.data6_io_num = -1,
			.data7_io_num = -1,
			.max_transfer_sz = SEND_BUF_SIZE + 8,
			.flags = SPICOMMON_BUSFLAG_MASTER
				| SPICOMMON_BUSFLAG_GPIO_PINS
#if !defined(CONFIG_HWE_DISPLAY_SPI_SPI)
				/* QUAD is unconditional upstream, which forces
				 * a 4-lane bus even when the panel IO is
				 * configured single-line. On the Plus, D2/D3
				 * are not wired to the display at all
				 * (RM67162_AMOLED_SPI sets both to -1), so
				 * claiming them makes the bus disagree with
				 * the panel. */
				| SPICOMMON_BUSFLAG_QUAD
#endif
				,
		},
		SPI_DMA_CH_AUTO
	));
	ESP_LOGI(TAG, "Attach panel IO handle to SPI");
	esp_lcd_panel_io_handle_t io_handle = NULL;
	ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
		(esp_lcd_spi_bus_handle_t)SPIx_HOST,
	       	& (esp_lcd_panel_io_spi_config_t) {
			.cs_gpio_num = CONFIG_HWE_DISPLAY_SPI_CS,
			.pclk_hz = CONFIG_HWE_DISPLAY_SPI_FREQUENCY,
#if CONFIG_RM67162_USE_DC_PIN
			/* Conventional 4-wire SPI: plain 8-bit commands with
			 * esp_lcd toggling DC, matching how LilyGO drives the
			 * Plus. lcd_cmd_bits 32 belongs to the no-DC framing. */
			.dc_gpio_num = CONFIG_HWE_DISPLAY_SPI_D1,
			.lcd_cmd_bits = 8,
#else
			.dc_gpio_num = -1,
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
	       	&io_handle
	));
	ESP_LOGI(TAG, "Attach vendor specific module");
	esp_lcd_panel_handle_t panel_handle = NULL;
	ESP_ERROR_CHECK(esp_lcd_new_panel_rm67162(
		io_handle,
		& (esp_lcd_panel_dev_config_t) {
			.reset_gpio_num = CONFIG_HWE_DISPLAY_RST,
			.flags.reset_active_high = RST_ACTIVE_LEVEL,
			.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
			.bits_per_pixel = 16,
		},
		&panel_handle
	));
	ESP_LOGI(TAG, "Reset panel");
	ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
	ESP_LOGI(TAG, "Init panel");
	ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
	// ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, true));
	// Rotate 90 degrees clockwise:
	ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel_handle, true));
	ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, true, false));

	/* The bring-up solid-fill test lives in lilygo-amoled-demo and has
	 * served its purpose: the pin map, bus and init below are the verified
	 * ones it established. Straight to LVGL here. */
	lv_init();
	lv_display_t *disp = lv_display_create(CONFIG_HWE_DISPLAY_WIDTH,
					       CONFIG_HWE_DISPLAY_HEIGHT);
	ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(
		io_handle,
		&(esp_lcd_panel_io_callbacks_t) { color_trans_done },
		disp));
	lv_display_set_user_data(disp, panel_handle);
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

	esp_timer_handle_t periodic_timer;
	ESP_ERROR_CHECK(esp_timer_create(
		&(esp_timer_create_args_t) {
			.callback = &lv_tick_task,
			.name = "periodic_gui",
		},
		&periodic_timer));
	ESP_ERROR_CHECK(esp_timer_start_periodic(periodic_timer,
					         LV_TICK_PERIOD_MS * 1000));

	ESP_LOGI(TAG, "Pip-Boy online");
	pipboy_ui(disp);
	ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

	/* NOTE: the demo exits to deep sleep when button_1 goes low. This app
	 * uses button_1 as its verification trigger, so that exit path is
	 * deliberately absent -- otherwise the first press under test would
	 * shut the board down instead of proving the pin. pipboy_ui() owns
	 * GPIO0 configuration; nothing here touches it. */
	while (1) {
		/* Sleep for as long as LVGL says it has nothing to do, floored
		 * at 10ms so the idle task always gets the CPU. A flat 5ms delay
		 * here starved IDLE0 and tripped the task watchdog. */
		uint32_t next = lv_task_handler();
		if (next == LV_NO_TIMER_READY || next > 50)
			next = 50;
		if (next < 10)
			next = 10;
		vTaskDelay(pdMS_TO_TICKS(next));
	}
}
