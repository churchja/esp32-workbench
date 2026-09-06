/*
 * wx_touch.c -- CST816S touch: a minimal tap-to-wake driver, plus the probe
 * that established it works.
 *
 * TWO THINGS LIVE HERE. wx_touch_init() / wx_touch_take_tap() are the driver
 * the app uses for tap-to-wake. wx_touch_probe() is the diagnostic that proved
 * the hardware works before that driver was written; nothing calls it during
 * normal operation -- it runs when 't' is typed on the serial console, and it
 * stays because the next question about this bus will want it.
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
 * ANSWERED ON THIS UNIT 2026-09-06, by running the probe below:
 *   * register reads work -- chip id 0xB5 = CST816S, vendor 0x22, fw 0x03. An
 *     address ACK had only proved the chip was powered and decoding; this
 *     proves the register interface behaves.
 *   * 34 touch events read with coordinates while the screen was tapped.
 *   * GPIO21 PULSES rather than holding low: it sampled low on 4 of 200
 *     samples across those 34 events. Everything in the driver follows from
 *     that one measurement.
 *   * The documented risk did NOT materialise, and the reason is now known
 *     rather than guessed. CST816 parts sleep aggressively and usually need
 *     RST pulsed; the guess was that GPIO17 (lcd_rst) might be shared. The
 *     official schematic (T-Display-S3-AMOLED-Plus.pdf V1.0, 2024-10-21)
 *     settles it: the touch FPC P3 is a SIX-pin connector carrying SCL, SDA
 *     and GPIO21 INT, and LilyGO's own board struct records the touch pins as
 *     {3 SDA, 2 SCL, 21 IRQ, -1 RST}. There is NO touch reset line on this
 *     board at all. Nothing to pulse, and nothing to add later.
 *
 * STILL OPEN: the coordinate frame. x brushed 240 while y stayed low, which
 * points at the panel's native 240x536 portrait frame, rotated 90 degrees from
 * the 536x240 the app draws in. Tap-to-wake does not care. Anything that maps a
 * touch to a UI element must establish the transform by tapping known corners.
 */
#include <stdio.h>
#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_timer.h>

#include "vaultweather.h"
#include "wx_i2c.h"

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

static esp_err_t   bus_init(void);                 /* defined below, next to */
static esp_err_t   rd(uint8_t reg, uint8_t *out, size_t n);   /* the probe    */
static const char *chip_name(uint8_t id);

/* Set by the ISR, cleared by the render loop. volatile because they are
 * different execution contexts.
 *
 * No lock, and the reason is NOT that a bool cannot tear -- that would license
 * the same pattern for a counter, where it would be wrong. The test-and-clear
 * in wx_touch_take_tap() genuinely can lose an edge that arrives between the
 * load and the store. It is safe here only because the single consumer is
 * IDEMPOTENT: the effect of a tap is "extend the wake deadline", so a lost tap
 * costs at most one repeat and two taps do the same thing as one. */
static volatile bool tap_flag;
static bool          driver_ready;

static void IRAM_ATTR touch_isr(void *arg)
{
	tap_flag = true;
}

/* WHY AN EDGE INTERRUPT AND NOT A POLL.
 *
 * Measured on this unit: while 34 touch events were read, GPIO21 sampled LOW on
 * only 4 of 200 samples at 20Hz. The INT line is a short PULSE, not a level
 * held for the duration of a touch. A poll at any sane rate would therefore
 * miss most taps -- and would look like flaky hardware rather than a wrong
 * assumption. An edge interrupt catches every one.
 *
 * WHY THE TAP IS NOT CONFIRMED BY AN I2C READ.
 * The obvious hardening is to read the finger count after the edge and ignore
 * the interrupt if it reads zero. That is wrong here: the pulse is short, the
 * finger is often already lifted by the time the render loop runs, and the
 * register would read zero for a real tap. It would trade a harmless failure
 * (a spurious wake brightens the screen for 45s) for an annoying one (a real
 * tap does nothing). The edge is trusted on purpose.
 */
esp_err_t wx_touch_init(void)
{
	if (driver_ready)
		return ESP_OK;

	/* NOTE THE ORDER. Tap-to-wake needs the INTERRUPT LINE and nothing else
	 * -- it never reads a register. An earlier version brought up I2C first
	 * and bailed if that failed, which meant an unrelated bus fault took the
	 * whole feature down and left the panel pinned at BRIGHT_NIGHT all night
	 * with no way to brighten it. That is exactly the escape hatch that made
	 * a night level of 30 acceptable in the first place. The bus is now
	 * best-effort and only used to report what the chip says it is. */
	gpio_config_t ic = {
		.pin_bit_mask = 1ULL << TOUCH_INT,
		.mode         = GPIO_MODE_INPUT,
		.pull_up_en   = GPIO_PULLUP_ENABLE,
		.pull_down_en = GPIO_PULLDOWN_DISABLE,
		.intr_type    = GPIO_INTR_NEGEDGE,
	};
	esp_err_t e = gpio_config(&ic);
	if (e != ESP_OK) {
		ESP_LOGE(TAG, "gpio_config: %s", esp_err_to_name(e));
		return e;
	}

	/* Another component may already own the ISR service. That is not an
	 * error for us -- we only want a handler on one pin. */
	e = gpio_install_isr_service(0);
	if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
		ESP_LOGE(TAG, "isr service: %s", esp_err_to_name(e));
		return e;
	}

	e = gpio_isr_handler_add(TOUCH_INT, touch_isr, NULL);
	if (e != ESP_OK) {
		ESP_LOGE(TAG, "isr add: %s", esp_err_to_name(e));
		return e;
	}

	driver_ready = true;
	tap_flag = false;      /* discard any edge from configuring the pin */

	/* Identity is best-effort and is READ, not asserted. The previous log
	 * line said "(CST816S)" without a single transaction having happened --
	 * a claim about hardware made by code that had not looked. */
	uint8_t id = 0;
	if (bus_init() == ESP_OK && rd(REG_CHIP_ID, &id, 1) == ESP_OK)
		ESP_LOGI(TAG, "tap-to-wake armed on GPIO%d (chip 0x%02X %s)",
			 TOUCH_INT, id, chip_name(id));
	else
		ESP_LOGI(TAG, "tap-to-wake armed on GPIO%d "
			      "(chip id unread; the edge does not need it)",
			 TOUCH_INT);
	return ESP_OK;
}

bool wx_touch_take_tap(void)
{
	if (!tap_flag)
		return false;
	tap_flag = false;
	return true;
}

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
	/* 50ms, not 200. This runs on the render loop; four reads at 200ms would
	 * be most of a second of frozen UI on a bus that is not answering. */
	return i2c_master_transmit_receive(dev, &reg, 1, out, n, 50);
}

static esp_err_t bus_init(void)
{
	if (dev)
		return ESP_OK;

	/* The bus is SHARED -- the RTC and the PMU sit on it too. Creating one
	 * here would have been wrong the moment a second module needed it:
	 * i2c_new_master_bus() on an open port returns ESP_ERR_INVALID_STATE. */
	esp_err_t e = wx_i2c_bus(&bus);
	if (e != ESP_OK)
		return e;

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
		/* Do not cache a half-open state. Keeping a non-NULL bus with a
		 * NULL device made every later call take the "already
		 * initialised" early return, hand NULL to
		 * i2c_master_transmit_receive, and print the confident and wrong
		 * diagnosis "the controller is asleep and needs its RST pulsed"
		 * -- misleading output in the one place you would be trusting it.
		 *
		 * The BUS is not torn down: it is shared with the RTC, and this
		 * module does not own it. The early return above now keys off
		 * `dev`, which is this module's own handle. */
		dev = NULL;
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

	/* Watch the INT line alongside the register data: if INT toggles but the
	 * registers stay empty, the chip senses and the register path is wrong.
	 *
	 * Configure the pin ONLY if the driver has not already claimed it --
	 * reconfiguring with intr_type DISABLE would silently disarm tap-to-wake
	 * and leave it dead until the next reboot. */
	if (!driver_ready) {
		gpio_config_t ic = {
			.pin_bit_mask = 1ULL << TOUCH_INT,
			.mode         = GPIO_MODE_INPUT,
			.pull_up_en   = GPIO_PULLUP_ENABLE,
			.pull_down_en = GPIO_PULLDOWN_DISABLE,
			.intr_type    = GPIO_INTR_DISABLE,
		};
		gpio_config(&ic);
	}

	printf("polling %ds -- TOUCH THE SCREEN NOW\n", seconds);

	/* HARD WALL-CLOCK BOUND, not just an iteration count.
	 *
	 * This runs ON THE RENDER LOOP, so for its whole duration the clock does
	 * not update, taps are not consumed and the 45s wake timer does not
	 * expire. An iteration count alone does not bound that: each rd() can
	 * burn its I2C timeout on a sick bus, so "10 seconds" could become far
	 * longer exactly when something is wrong and you are staring at it. */
	const int64_t deadline = esp_timer_get_time() + (int64_t)seconds * 1000000;
	int      ticks   = seconds * 20;      /* 50ms period */
	int      events  = 0;
	int      int_low = 0;
	uint8_t  last[6] = { 0 };

	for (int i = 0; i < ticks && esp_timer_get_time() < deadline; i++) {
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
