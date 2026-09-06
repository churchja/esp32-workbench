/*
 * wx_touch.c -- INVESTIGATION, not a driver.
 *
 * This exists to answer one question with measurements instead of reading:
 * can this board's touch controller be talked to, and does it report usable
 * touch events? Nothing in the app calls it during normal operation; it runs
 * only when 't' is typed on the serial console.
 *
 * WHAT IS ALREADY KNOWN, AND HOW
 * boards/e4b0638aec2c.yaml carries a verification_log entry dated 2026-09-04,
 * result: verified -- projects/i2c-variant-scan was flashed onto THIS unit and
 * three devices answered on SDA=3/SCL=2:
 *
 *     0x15  CST816 touch
 *     0x51  PCF85063 RTC
 *     0x6b  BQ25896 PMU
 *
 * That trio is the T-Display-S3 AMOLED Plus signature and nothing else in the
 * family. The same work resolved GPIO21 as the touch INT line -- NOT a button,
 * which is what the older non-touch repo would have told you.
 *
 * So the bus and the address are measured facts. What is NOT known, and what
 * this file is for: whether the controller answers REGISTER reads, what part
 * it actually is, and whether it reports coordinates. An ACK to an address
 * probe proves a chip is powered and decoding its address; it does not prove
 * the register interface behaves.
 *
 * KNOWN RISK, STATED UP FRONT: CST816 parts sleep aggressively and many need
 * their RST line pulsed to come back. No touch-reset pin is recorded in the
 * board profile -- it may be tied to the display reset (GPIO17, already pulsed
 * during panel bring-up) or to nothing reachable. If register reads fail while
 * the address probe succeeds, that is the most likely reason, and it is a
 * finding rather than a failure.
 */
#include <stdio.h>
#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <esp_err.h>
#include <esp_log.h>

#include "vaultweather.h"

#define TAG "wx_touch"

/* Measured on this unit; see the verification_log entry quoted above. */
#define TOUCH_SDA   3
#define TOUCH_SCL   2
#define TOUCH_INT  21
#define TOUCH_ADDR 0x15

/* CST816 register map. */
#define REG_GESTURE   0x01
#define REG_FINGERS   0x02
#define REG_XPOS_H    0x03
#define REG_CHIP_ID   0xA7
#define REG_VENDOR_ID 0xA8
#define REG_FW_VER    0xA9

static i2c_master_bus_handle_t bus;
static i2c_master_dev_handle_t dev;

static const char *chip_name(uint8_t id)
{
	switch (id) {
	case 0xB4: return "CST716";
	case 0xB5: return "CST816S";
	case 0xB6: return "CST816T";
	case 0xB7: return "CST816D";
	default:   return "unknown";
	}
}

static const char *gesture_name(uint8_t g)
{
	switch (g) {
	case 0x00: return "none";
	case 0x01: return "swipe up";
	case 0x02: return "swipe down";
	case 0x03: return "swipe left";
	case 0x04: return "swipe right";
	case 0x05: return "click";
	case 0x0B: return "long press";
	default:   return "?";
	}
}

static esp_err_t rd(uint8_t reg, uint8_t *out, size_t n)
{
	/* Write the register address, then read -- a repeated-START transaction,
	 * which is what i2c_master_transmit_receive() issues. A separate write
	 * then read would put a STOP between them and many controllers reset
	 * their address pointer on STOP. */
	return i2c_master_transmit_receive(dev, &reg, 1, out, n, 200);
}

static esp_err_t bus_init(void)
{
	if (bus)
		return ESP_OK;

	i2c_master_bus_config_t bc = {
		.i2c_port          = I2C_NUM_0,
		.sda_io_num        = TOUCH_SDA,
		.scl_io_num        = TOUCH_SCL,
		.clk_source        = I2C_CLK_SRC_DEFAULT,
		.glitch_ignore_cnt = 7,
		.flags.enable_internal_pullup = true,
	};
	esp_err_t e = i2c_new_master_bus(&bc, &bus);
	if (e != ESP_OK) {
		ESP_LOGE(TAG, "i2c_new_master_bus: %s", esp_err_to_name(e));
		bus = NULL;
		return e;
	}

	i2c_device_config_t dc = {
		.dev_addr_length = I2C_ADDR_BIT_LEN_7,
		.device_address  = TOUCH_ADDR,
		/* 100kHz, not 400. This is a probe on a bus whose trace lengths
		 * and pull-up values are unknown; the slower edge rate removes
		 * signal integrity from the list of things a failure could mean. */
		.scl_speed_hz    = 100000,
	};
	e = i2c_master_bus_add_device(bus, &dc, &dev);
	if (e != ESP_OK) {
		ESP_LOGE(TAG, "add_device: %s", esp_err_to_name(e));
		return e;
	}
	return ESP_OK;
}

void wx_touch_probe(int seconds)
{
	if (bus_init() != ESP_OK) {
		printf("TOUCH: bus init failed\n");
		return;
	}

	printf("\nTOUCH PROBE  SDA=%d SCL=%d addr=0x%02X int=GPIO%d\n",
	       TOUCH_SDA, TOUCH_SCL, TOUCH_ADDR, TOUCH_INT);

	/* Full bus scan first. If the expected trio does not appear, every
	 * subsequent result is suspect and it is worth knowing that immediately
	 * rather than debugging a register read on a bus that is not working. */
	printf("scan:");
	int found = 0;
	for (uint16_t a = 0x08; a < 0x78; a++) {
		if (i2c_master_probe(bus, a, 50) == ESP_OK) {
			printf(" 0x%02X", a);
			found++;
		}
	}
	printf("   (%d device%s)\n", found, found == 1 ? "" : "s");

	uint8_t id = 0, vend = 0, fw = 0;
	esp_err_t e1 = rd(REG_CHIP_ID, &id, 1);
	esp_err_t e2 = rd(REG_VENDOR_ID, &vend, 1);
	esp_err_t e3 = rd(REG_FW_VER, &fw, 1);

	if (e1 != ESP_OK) {
		printf("chip id read FAILED: %s\n", esp_err_to_name(e1));
		printf("  the address ACKs but registers do not read. Most likely\n"
		       "  the controller is asleep and needs its RST pulsed; no\n"
		       "  touch-reset pin is recorded for this board.\n");
	} else {
		printf("chip id 0x%02X (%s)  vendor 0x%02X%s  fw 0x%02X%s\n",
		       id, chip_name(id),
		       vend, e2 == ESP_OK ? "" : "(read failed)",
		       fw,   e3 == ESP_OK ? "" : "(read failed)");
	}

	/* GPIO21 as an input, so the INT line can be watched alongside the
	 * register data. If INT toggles but registers stay empty, the chip is
	 * sensing and the register path is the problem -- a distinction worth
	 * having before writing a driver. */
	gpio_config_t ic = {
		.pin_bit_mask = 1ULL << TOUCH_INT,
		.mode         = GPIO_MODE_INPUT,
		.pull_up_en   = GPIO_PULLUP_ENABLE,
		.pull_down_en = GPIO_PULLDOWN_DISABLE,
		.intr_type    = GPIO_INTR_DISABLE,
	};
	gpio_config(&ic);

	printf("polling %ds -- TOUCH THE SCREEN NOW\n", seconds);

	int      ticks   = seconds * 20;      /* 50ms period */
	int      events  = 0;
	int      int_low = 0;
	uint8_t  last[6] = { 0 };

	for (int i = 0; i < ticks; i++) {
		if (gpio_get_level(TOUCH_INT) == 0)
			int_low++;

		uint8_t b[6];
		if (rd(REG_GESTURE, b, sizeof(b)) == ESP_OK) {
			uint8_t fingers = b[1] & 0x0F;
			if (fingers && memcmp(b, last, sizeof(b)) != 0) {
				int x = ((b[2] & 0x0F) << 8) | b[3];
				int y = ((b[4] & 0x0F) << 8) | b[5];
				printf("  touch  x=%3d y=%3d  fingers=%d  gesture=%s\n",
				       x, y, fingers, gesture_name(b[0]));
				events++;
				memcpy(last, b, sizeof(b));
			}
			if (!fingers)
				memset(last, 0, sizeof(last));
		}
		vTaskDelay(pdMS_TO_TICKS(50));
	}

	printf("TOUCH RESULT: %d event%s, INT low on %d/%d samples\n",
	       events, events == 1 ? "" : "s", int_low, ticks);
	if (!events && int_low)
		printf("  INT moved but no coordinates -- the chip senses; the\n"
		       "  register path or the register map is wrong.\n");
	if (!events && !int_low)
		printf("  nothing at all. Either the screen was not touched, or the\n"
		       "  controller is asleep and needs a reset line we do not have.\n");
}
