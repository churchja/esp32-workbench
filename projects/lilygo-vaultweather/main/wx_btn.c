/*
 * wx_btn.c -- the board's single user button.
 *
 * GPIO0 was established as this board's only readable button in
 * projects/lilygo-pipboy: it goes LOW against an internal pull-up when
 * pressed. The other button on the case is hardware RESET and is wired to the
 * chip's EN pin, so software cannot see it at all -- there is no second input
 * to map, which is why every gesture here has to come out of one pin.
 *
 * GPIO0 is also the ESP32-S3 BOOT strapping pin. Holding it across a reset
 * puts the chip into download mode. That is the ROM bootloader's behaviour,
 * decided before app code runs, and it does not constrain runtime use -- but
 * it does mean a user holding the button while the board reboots will land in
 * download mode rather than the setup portal, which is worth knowing when a
 * bug report says "it went dead when I held the button".
 */
#include "driver/gpio.h"
#include "esp_timer.h"
#include "vaultweather.h"

/* Press classification, in milliseconds.
 *
 * DEBOUNCE exists because a mechanical contact rings for a few ms; without it
 * one press reads as several. 25ms is comfortably past the bounce of a tactile
 * switch and still imperceptible.
 *
 * HOLD_MS is deliberately long. It erases the saved configuration, which means
 * re-entering Wi-Fi credentials on a phone -- an expensive thing to trigger by
 * accident while picking the board up. Five seconds is hard to do unintentionally.
 */
#define DEBOUNCE_MS    25
#define LONG_MS      1000
#define SNAP_MS      3000
#define HOLD_MS      5000

static int64_t press_start;      /* us, from esp_timer_get_time */
static bool    was_down;
static bool    hold_fired;       /* so a 6-second hold reports HOLD once, not
                                  * HOLD then LONG when the finger comes off */
static bool    snap_fired;       /* same, for the 3s rung: without it, holding
                                  * to 3.5s would dump the screen AND then
                                  * report LONG on release, firing a refresh
                                  * nobody asked for */

void wx_btn_init(void)
{
	gpio_config_t cfg = {
		.pin_bit_mask = 1ULL << WX_BUTTON_GPIO,
		.mode         = GPIO_MODE_INPUT,
		.pull_up_en   = GPIO_PULLUP_ENABLE,
		.pull_down_en = GPIO_PULLDOWN_DISABLE,
		.intr_type    = GPIO_INTR_DISABLE,
	};
	gpio_config(&cfg);
	press_start = 0;
	was_down    = false;
	hold_fired  = false;
}

/* Polled rather than interrupt-driven. The main loop already runs at ~20-50ms
 * for LVGL, which is far finer than human press timing, and polling keeps the
 * whole gesture state machine on one task instead of sharing it with an ISR.
 */
wx_btn_t wx_btn_poll(void)
{
	bool    down = (gpio_get_level(WX_BUTTON_GPIO) == 0);   /* active low */
	int64_t now  = esp_timer_get_time();

	if (down && !was_down) {
		was_down    = true;
		hold_fired  = false;
		snap_fired  = false;
		press_start = now;
		return WX_BTN_NONE;
	}

	if (down && was_down) {
		/* Fire HOLD while the button is still down. Waiting for release
		 * would leave the user pressing a seemingly dead button with no
		 * feedback for five seconds; firing here lets the UI react the
		 * moment the threshold is crossed. */
		int32_t held = (int32_t)((now - press_start) / 1000);
		if (!hold_fired && held >= HOLD_MS) {
			hold_fired = true;
			return WX_BTN_HOLD;
		}
		if (!snap_fired && held >= SNAP_MS) {
			snap_fired = true;
			return WX_BTN_SNAP;
		}
		return WX_BTN_NONE;
	}

	if (!down && was_down) {
		was_down = false;
		int32_t ms = (int32_t)((now - press_start) / 1000);

		if (hold_fired || snap_fired)
			return WX_BTN_NONE;                /* already reported */
		if (ms < DEBOUNCE_MS) return WX_BTN_NONE;  /* contact bounce */
		if (ms >= LONG_MS)    return WX_BTN_LONG;
		return WX_BTN_SHORT;
	}

	return WX_BTN_NONE;
}
