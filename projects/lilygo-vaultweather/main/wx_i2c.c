#include "wx_i2c.h"

#include <esp_log.h>

#define TAG "wx_i2c"

/* Measured on this unit and recorded in boards/e4b0638aec2c.yaml as verified:
 * a scan of SDA=3/SCL=2 answered 0x15, 0x51 and 0x6B. */
#define WX_I2C_SDA 3
#define WX_I2C_SCL 2

static i2c_master_bus_handle_t s_bus;

esp_err_t wx_i2c_bus(i2c_master_bus_handle_t *out)
{
	if (s_bus) {
		*out = s_bus;
		return ESP_OK;
	}

	i2c_master_bus_config_t bc = {
		.i2c_port          = I2C_NUM_0,
		.sda_io_num        = WX_I2C_SDA,
		.scl_io_num        = WX_I2C_SCL,
		.clk_source        = I2C_CLK_SRC_DEFAULT,
		.glitch_ignore_cnt = 7,
		.flags.enable_internal_pullup = true,
	};

	esp_err_t e = i2c_new_master_bus(&bc, &s_bus);
	if (e != ESP_OK) {
		/* Do NOT cache a failed handle. A previous version of this logic
		 * in wx_touch.c cached a bus whose device add had failed, so every
		 * later call took the "already initialised" path and handed NULL
		 * to the transfer function -- which produced a confident and wrong
		 * diagnosis in the one place you would be trusting the output. */
		ESP_LOGE(TAG, "i2c_new_master_bus: %s", esp_err_to_name(e));
		s_bus = NULL;
		return e;
	}

	ESP_LOGI(TAG, "bus up on SDA=%d SCL=%d", WX_I2C_SDA, WX_I2C_SCL);
	*out = s_bus;
	return ESP_OK;
}
