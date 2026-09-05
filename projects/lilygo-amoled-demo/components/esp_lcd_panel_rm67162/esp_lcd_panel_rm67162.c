/*
 * SPDX-FileCopyrightText: 2021-2023 Espressif Systems (Shanghai) CO LTD
 * SPDX-FileCopyrightText: 2024-2026 Eugene Crosser
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * This is an ESP-IDF component that adds support for rm67162 OLED chip
 * to the esp_lcd driver framework.
 */

#include <stdlib.h>
#include <string.h>
#include <sys/cdefs.h>
#include "sdkconfig.h"

#if CONFIG_LCD_ENABLE_DEBUG_LOG
// The local log level must be defined before including esp_log.h
// Set the maximum log level for this source file
#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
#endif

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_commands.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_check.h"

#define RM67162_CMD_DSTBON	0x4F	// Deep standby (RESX 0 > 3ms to wake)
#define RM67162_CMD_WRCTRLD	0x53	// Write display control . . B . D . . .
#define RM67162_CMD_RDCTRLD0	0x54	// Read disp contr (B-right, D-imming)
#define RM67162_CMD_RDCTRLD1	0x55	// RAD_ACL Control
#define RM67162_CMD_IMGEHCCTR0	0x58	// Set_color_enhance (three bits)
#define RM67162_CMD_IMGEHCCTR1	0x59	// Read_color_enhance
#define RM67162_CMD_CESLRCTR0	0x5A	// Set_color_enhance1
#define RM67162_CMD_CESLRCTR1	0x5B	// Read_color_enhance1

static const char *TAG = "lcd_panel.rm67162";

typedef struct {
	esp_lcd_panel_t base;
	esp_lcd_panel_io_handle_t io;
	int reset_gpio_num;
	bool reset_active_level;
	int x_gap;
	int y_gap;
	uint8_t fb_bits_per_pixel;
	uint8_t madctl_val;	// save current value of LCD_CMD_MADCTL register
	uint8_t colmod_val;	// save current value of LCD_CMD_COLMOD register
} rm67162_panel_t;

static esp_err_t panel_rm67162_del(esp_lcd_panel_t *panel)
{
	rm67162_panel_t *rm67162 = __containerof(panel, rm67162_panel_t, base);

	if (rm67162->reset_gpio_num >= 0) {
		ESP_LOGD(TAG, "reset \"reset\" gpio pin %d",
				rm67162->reset_gpio_num);
		gpio_reset_pin(rm67162->reset_gpio_num);
	}
	ESP_LOGD(TAG, "del rm67162 panel @%p", rm67162);
	free(rm67162);
	return ESP_OK;
}

/*
 * Without DC pin, this chip uses two bytes per byte of address,
 * and address itself is 16 bit wide. So here we need to construct a
 * 32 bit wide contraption containing high and low bytes of the address
 * interspersed with control bytes, and tell esp-idf's SPI driver that
 * this is our 32 bit long command. Luckily for us, subsequent data is
 * a simple sequence of bytes.
 * In the panel configuration, set .lcd_cmd_bits = 32, .lcd_param_bits = 8.
 *
 * Control byte, hi addr byte, control byte, lo addr byte, control byte, data
 *
 * 1 |R D H 0 0 0 0 0|A A A A A A A A|R D H 0 0 0 0 0|A A A A A A A A|
 * 0 |W C L 0 0 0 0 0|F E D C B A 9 8|W C L 0 0 0 0 0|7 6 5 4 3 2 1 0|
 *
 * Read or Write, data or command, hi or lo byte of 16bit address
 */
static esp_err_t rm67162_cmd_trans(esp_lcd_panel_io_handle_t io, int lcd_cmd,
		const void *param, size_t param_size)
{
#if CONFIG_RM67162_USE_DC_PIN
	/* Conventional 4-wire SPI: a DC line separates command from data, so the
	 * command is a plain 8-bit value and esp_lcd toggles DC around it. This
	 * is how LilyGO drives the T-Display-S3 AMOLED **Plus** -- their
	 * writeCommand() pulls display.d1 (GPIO7) LOW for the command byte and
	 * HIGH for the data bytes.
	 *
	 * The no-DC framing below embeds control bytes in a 32-bit command word
	 * instead. Both are "SPI" and they are NOT interchangeable: with the
	 * wrong one every call still returns ESP_OK and the panel stays dark,
	 * because nothing on this bus reads back. */
	return esp_lcd_panel_io_tx_param(io, lcd_cmd, param, param_size);
#else
	return esp_lcd_panel_io_tx_param(io, 0x02000000 | (lcd_cmd<<8),
			param, param_size);
#endif
}


/* ------------------------------------------------------------------------
 * LilyGO T-Display-S3 AMOLED **Plus** init sequence.
 *
 * The minimal init below (SLPOUT / MADCTL / COLMOD / WRDISBV) brings up the
 * QSPI-wired boards this driver was written against. It does NOT bring up the
 * Plus, whose panel is wired for regular SPI: LilyGO's own library drives that
 * variant with a separate 18-command sequence (rm67162_spi_cmd) rather than the
 * 12-command one it uses for the QSPI boards. Bus width and init sequence are
 * independent, so configuring a narrower bus alone leaves the panel dark while
 * every call still returns ESP_OK.
 *
 * Sequence and its encoding are from LilyGo-AMOLED-Series, MIT licensed,
 * (c) 2023 Shenzhen Xin Yuan Electronic Technology Co., Ltd -- Lewis He.
 *   src/initSequence.cpp  rm67162_spi_cmd[RM67162_INIT_SPI_SEQUENCE_LENGTH]
 *
 * Encoding, from LilyGo_AMOLED.cpp's own loop:
 *   len & 0x1F  -> parameter byte count
 *   len & 0x80  -> delay 120 ms afterwards
 *   len & 0x20  -> delay 10 ms afterwards
 * and the whole table is sent TWICE, which that loop does deliberately to
 * "prevent initialization failure". Reproduced here rather than tidied away.
 * ------------------------------------------------------------------------ */
#if CONFIG_RM67162_USE_LILYGO_SPI_INIT

#define LILYGO_DEFAULT_BRIGHTNESS 175

typedef struct {
	uint32_t addr;
	uint8_t param[4];
	uint32_t len;
} lilygo_lcd_cmd_t;

static const lilygo_lcd_cmd_t lilygo_rm67162_spi_cmd[] = {
	{0xFE, {0x04}, 0x01},                        /* SET APGE3 */
	{0x6A, {0x00}, 0x01},
	{0xFE, {0x05}, 0x01},                        /* SET APGE4 */
	{0xFE, {0x07}, 0x01},                        /* SET APGE6 */
	{0x07, {0x4F}, 0x01},
	{0xFE, {0x01}, 0x01},                        /* SET APGE0 */
	{0x2A, {0x02}, 0x01},
	{0x2B, {0x73}, 0x01},
	{0xFE, {0x0A}, 0x01},                        /* SET APGE9 */
	{0x29, {0x10}, 0x01},
	{0xFE, {0x00}, 0x01},
	{0x51, {LILYGO_DEFAULT_BRIGHTNESS}, 0x01},
	{0x53, {0x20}, 0x01},
	{0x35, {0x00}, 0x01},
	{0x3A, {0x75}, 0x01},                        /* pixel format, 16 bit */
	{0xC4, {0x80}, 0x01},
	{0x11, {0x00}, 0x01 | 0x80},                 /* sleep out,  +120 ms */
	{0x29, {0x00}, 0x01 | 0x80},                 /* display on, +120 ms */
};

static esp_err_t rm67162_send_lilygo_spi_init(esp_lcd_panel_io_handle_t io)
{
	for (int pass = 0; pass < 2; pass++) {
		for (size_t i = 0; i < sizeof(lilygo_rm67162_spi_cmd) /
				sizeof(lilygo_rm67162_spi_cmd[0]); i++) {
			const lilygo_lcd_cmd_t *t = &lilygo_rm67162_spi_cmd[i];
			ESP_RETURN_ON_ERROR(
				rm67162_cmd_trans(io, t->addr, t->param,
						t->len & 0x1F),
				TAG, "lilygo init cmd 0x%02x failed",
				(unsigned)t->addr);
			if (t->len & 0x80)
				vTaskDelay(pdMS_TO_TICKS(120));
			if (t->len & 0x20)
				vTaskDelay(pdMS_TO_TICKS(10));
		}
	}
	return ESP_OK;
}

#endif /* CONFIG_RM67162_USE_LILYGO_SPI_INIT */

static esp_err_t panel_rm67162_reset(esp_lcd_panel_t *panel)
{
	rm67162_panel_t *rm67162 = __containerof(panel, rm67162_panel_t, base);

	// perform hardware reset
	if (rm67162->reset_gpio_num >= 0) {
		int delays[] = {200, 300, 200};
#define STEPS (sizeof(delays) / sizeof(delays[0]))
		int lvl = rm67162->reset_active_level;

		ESP_RETURN_ON_ERROR(gpio_set_direction(rm67162->reset_gpio_num,
					GPIO_MODE_OUTPUT),
				TAG, "configure GPIO for RST line failed");
		for (int i = 0; i < STEPS; i++) {
			ESP_LOGD(TAG, "Set RST pin %d to %s",
				rm67162->reset_gpio_num, lvl?"HIGH":"LOW");
			ESP_RETURN_ON_ERROR(gpio_set_level(
					rm67162->reset_gpio_num, lvl),
				TAG, "gpio_set_level for RST error");
			vTaskDelay(pdMS_TO_TICKS(delays[i]));
			lvl = !lvl;
		}
	} else {
		ESP_LOGD(TAG, "Performing software reset");
		ESP_RETURN_ON_ERROR(rm67162_cmd_trans(
			(esp_lcd_panel_io_handle_t)rm67162->io,
			LCD_CMD_SWRESET, NULL, 0),
				TAG, "io tx param LCD_CMD_SWRESET failed");
		// spec, wait at least 5m before sending new command
		vTaskDelay(pdMS_TO_TICKS(20));
	}

	return ESP_OK;
}

static esp_err_t panel_rm67162_init(esp_lcd_panel_t *panel)
{
	rm67162_panel_t *rm67162 = __containerof(panel, rm67162_panel_t, base);
	esp_lcd_panel_io_handle_t io = rm67162->io;

#if CONFIG_RM67162_USE_LILYGO_SPI_INIT
	/* The Plus needs LilyGO's own sequence; the minimal one below does not
	 * light it. Return here rather than sending both -- MADCTL and COLMOD
	 * are already set by the table, and re-sending them after 0x29 has
	 * turned the display on is a different init, not a superset. */
	ESP_LOGI(TAG, "using LilyGO rm67162_spi_cmd init sequence");
	return rm67162_send_lilygo_spi_init(io);
#endif

	// LCD goes into sleep mode and display will be turned off
	// after power on reset, exit sleep mode first
	ESP_RETURN_ON_ERROR(rm67162_cmd_trans(io, LCD_CMD_SLPOUT, NULL, 0),
			TAG, "io tx param LCD_CMD_SLPOUT failed");
	vTaskDelay(pdMS_TO_TICKS(120));
	ESP_RETURN_ON_ERROR(rm67162_cmd_trans(
		io, LCD_CMD_MADCTL, (uint8_t[]) {rm67162->madctl_val,}, 1),
			TAG, "io tx param LCD_CMD_MADCTL failed");
	ESP_RETURN_ON_ERROR(rm67162_cmd_trans(
		io, LCD_CMD_COLMOD, (uint8_t[]) {rm67162->colmod_val,}, 1),
			TAG, "io tx param LCD_CMD_COLMOD failed");
	ESP_RETURN_ON_ERROR(rm67162_cmd_trans(
		io, LCD_CMD_WRDISBV, (uint8_t[]) {0,}, 1),
			TAG, "io tx param LCD_CMD_WRDISBV 0 failed");
	vTaskDelay(pdMS_TO_TICKS(120));
	ESP_RETURN_ON_ERROR(rm67162_cmd_trans(
		io, LCD_CMD_WRDISBV, (uint8_t[]) {0xD0,}, 1),
			TAG, "io tx param LCD_CMD_WRDISBV 0xD0 failed");
	return ESP_OK;
}

static esp_err_t panel_rm67162_draw_bitmap(esp_lcd_panel_t *panel, int x_start,
					   int y_start, int x_end, int y_end,
					   const void *color_data)
{
	rm67162_panel_t *rm67162 = __containerof(panel, rm67162_panel_t, base);
	esp_lcd_panel_io_handle_t io = rm67162->io;

	x_start += rm67162->x_gap;
	x_end += rm67162->x_gap;
	y_start += rm67162->y_gap;
	y_end += rm67162->y_gap;

	// define an area of frame memory where MCU can access
	ESP_RETURN_ON_ERROR(rm67162_cmd_trans(
		io, LCD_CMD_CASET, (uint8_t[]) {
			(x_start >> 8) & 0xFF, x_start & 0xFF,
			((x_end - 1) >> 8) & 0xFF, (x_end - 1) & 0xFF,
		}, 4), TAG, "io tx param LCD_CMD_CASET failed");
	ESP_RETURN_ON_ERROR(rm67162_cmd_trans(
		io, LCD_CMD_RASET, (uint8_t[]) {
			(y_start >> 8) & 0xFF, y_start & 0xFF,
			((y_end - 1) >> 8) & 0xFF, (y_end - 1) & 0xFF,
		}, 4), TAG, "io tx param LCD_CMD_RASET failed");
	// transfer frame buffer
	size_t len = (x_end - x_start) * (y_end - y_start)
			* rm67162->fb_bits_per_pixel / 8;
	ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_color(
#if CONFIG_RM67162_USE_DC_PIN
		io, LCD_CMD_RAMWR, color_data, len),
#else
		io, 0x32000000 | (LCD_CMD_RAMWR<<8), color_data, len),
#endif
			TAG, "io tx color LCD_CMD_RAMWR failed");
	return ESP_OK;
}

static esp_err_t panel_rm67162_invert_color(esp_lcd_panel_t *panel,
					    bool invert_color_data)
{
	rm67162_panel_t *rm67162 = __containerof(panel, rm67162_panel_t, base);
	ESP_RETURN_ON_ERROR(rm67162_cmd_trans(
		(esp_lcd_panel_io_handle_t)rm67162->io,
		invert_color_data ? LCD_CMD_INVON : LCD_CMD_INVOFF,
		NULL, 0),
			TAG, "io tx param LCD_CMD_INVx failed");
	return ESP_OK;
}

static esp_err_t panel_rm67162_mirror(esp_lcd_panel_t *panel, bool mirror_x,
				      bool mirror_y)
{
	rm67162_panel_t *rm67162 = __containerof(panel, rm67162_panel_t, base);
	if (mirror_x) {
		rm67162->madctl_val |= LCD_CMD_MX_BIT;
	} else {
		rm67162->madctl_val &= ~LCD_CMD_MX_BIT;
	}
	if (mirror_y) {
		rm67162->madctl_val |= LCD_CMD_MY_BIT;
	} else {
		rm67162->madctl_val &= ~LCD_CMD_MY_BIT;
	}
	ESP_RETURN_ON_ERROR(rm67162_cmd_trans(
		(esp_lcd_panel_io_handle_t)rm67162->io,
		LCD_CMD_MADCTL,
		(uint8_t[]) {rm67162->madctl_val,}, 1),
			TAG, "io tx param LCD_CMD_MADCTL failed");
	return ESP_OK;
}

static esp_err_t panel_rm67162_swap_xy(esp_lcd_panel_t *panel, bool swap_axes)
{
	rm67162_panel_t *rm67162 = __containerof(panel, rm67162_panel_t, base);
	if (swap_axes) {
		rm67162->madctl_val |= LCD_CMD_MV_BIT;
	} else {
		rm67162->madctl_val &= ~LCD_CMD_MV_BIT;
	}
	ESP_RETURN_ON_ERROR(rm67162_cmd_trans(
		(esp_lcd_panel_io_handle_t)rm67162->io,
		LCD_CMD_MADCTL,
		(uint8_t[]) {rm67162->madctl_val}, 1),
			TAG, "io tx param LCD_CMD_MADCTL failed");
	return ESP_OK;
}

static esp_err_t panel_rm67162_set_gap(esp_lcd_panel_t *panel, int x_gap,
				       int y_gap)
{
	rm67162_panel_t *rm67162 = __containerof(panel, rm67162_panel_t, base);
	rm67162->x_gap = x_gap;
	rm67162->y_gap = y_gap;
	return ESP_OK;
}

static esp_err_t panel_rm67162_disp_on_off(esp_lcd_panel_t *panel, bool on_off)
{
	rm67162_panel_t *rm67162 = __containerof(panel, rm67162_panel_t, base);
	ESP_RETURN_ON_ERROR(rm67162_cmd_trans(
		(esp_lcd_panel_io_handle_t)rm67162->io,
		on_off ? LCD_CMD_DISPON : LCD_CMD_DISPOFF, NULL, 0),
			TAG, "io tx param LCD_CMD_DISPx failed");
	return ESP_OK;
}

static esp_err_t panel_rm67162_sleep(esp_lcd_panel_t *panel, bool sleep)
{
	rm67162_panel_t *rm67162 = __containerof(panel, rm67162_panel_t, base);
	ESP_RETURN_ON_ERROR(rm67162_cmd_trans(
		(esp_lcd_panel_io_handle_t)rm67162->io,
		sleep ? LCD_CMD_SLPIN : LCD_CMD_SLPOUT, NULL, 0),
			TAG, "io tx param LCD_CMD_SLP%s failed",
			sleep ? "IN" : "OUT");
	vTaskDelay(pdMS_TO_TICKS(100));

	return ESP_OK;
}

static const esp_lcd_panel_t rm67162_base = {
	.reset = panel_rm67162_reset,
	.init = panel_rm67162_init,
	.del = panel_rm67162_del,
	.draw_bitmap = panel_rm67162_draw_bitmap,
	.invert_color = panel_rm67162_invert_color,
	.set_gap = panel_rm67162_set_gap,
	.mirror = panel_rm67162_mirror,
	.swap_xy = panel_rm67162_swap_xy,
	.disp_on_off = panel_rm67162_disp_on_off,
	.disp_sleep = panel_rm67162_sleep,
};

esp_err_t
esp_lcd_new_panel_rm67162(const esp_lcd_panel_io_handle_t io,
			  const esp_lcd_panel_dev_config_t *panel_dev_config,
			  esp_lcd_panel_handle_t *ret_panel)
{
#if CONFIG_LCD_ENABLE_DEBUG_LOG
	esp_log_level_set(TAG, ESP_LOG_DEBUG);
#endif
	esp_err_t ret = ESP_OK;
	rm67162_panel_t *rm67162 = NULL;
	ESP_GOTO_ON_FALSE(io && panel_dev_config && ret_panel,
		ESP_ERR_INVALID_ARG, err, TAG, "invalid argument");
	rm67162 = calloc(1, sizeof(rm67162_panel_t));
	ESP_GOTO_ON_FALSE(rm67162,
		ESP_ERR_NO_MEM, err, TAG, "no mem for rm67162 panel");

	switch (panel_dev_config->rgb_ele_order) {
	/*
	 * Spec sheet says that LCD_CMD_MV_BIT is reversed for rm67162,
	 * but in reality it is not. I.e. in portrait orientation scanning
	 * goes left to right, top to bottom.
	 */
	case LCD_RGB_ELEMENT_ORDER_RGB:
		rm67162->madctl_val = 0;
		break;
	case LCD_RGB_ELEMENT_ORDER_BGR:
		rm67162->madctl_val = LCD_CMD_BGR_BIT;
		break;
	default:
		ESP_GOTO_ON_FALSE(false,
			ESP_ERR_NOT_SUPPORTED, err, TAG,
			"unsupported RGB element order");
		break;
	}

	uint8_t fb_bits_per_pixel = 0;
	switch (panel_dev_config->bits_per_pixel) {
	case 16:		// RGB565
		rm67162->colmod_val = 0x55;
		fb_bits_per_pixel = 16;
		break;
	case 18:		// RGB666
		rm67162->colmod_val = 0x66;
		// each color component (R/G/B) should occupy
		// the 6 high bits of a byte, which means 3 full bytes
		// are required for a pixel
		fb_bits_per_pixel = 24;
		break;
	case 24:		// RGB888
		rm67162->colmod_val = 0x77;
		fb_bits_per_pixel = 24;
		break;
	default:
		ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG,
				  "unsupported pixel width");
		break;
	}

	rm67162->fb_bits_per_pixel = fb_bits_per_pixel;
	rm67162->reset_gpio_num = panel_dev_config->reset_gpio_num;
	rm67162->reset_active_level =
			panel_dev_config->flags.reset_active_high;
	rm67162->io = io;
	rm67162->base = rm67162_base;

	*ret_panel = &(rm67162->base);
	ESP_LOGD(TAG, "new rm67162 panel @%p", rm67162);

	return ESP_OK;

 err:
	if (rm67162) {
		if (panel_dev_config->reset_gpio_num >= 0) {
			gpio_reset_pin(panel_dev_config->reset_gpio_num);
		}
		free(rm67162);
	}
	return ret;
}
