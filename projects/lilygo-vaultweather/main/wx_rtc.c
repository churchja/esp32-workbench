/*
 * wx_rtc.c -- PCF85063ATL battery-backed real-time clock.
 *
 * WHY THIS EXISTS
 * The ESP32-S3's internal RTC survives a soft reset but NOT a power cut, so
 * before this the clock came only from SNTP: unplug the board, or lose the
 * router, and it booted showing 1970 until the network came back. The
 * PCF85063ATL (schematic U7) has its own backup cell (U8, MS412FE-FL26E) and
 * keeps time with the ESP32 completely unpowered.
 *
 * The board shipped with this part unused -- the factory firmware image
 * contains zero references to it.
 *
 * ORDER OF OPERATIONS, AND WHY
 * The RTC is read and applied BEFORE SNTP starts. That is not a style choice:
 * settimeofday() and an SNTP callback racing each other would give one winner
 * rather than a corrupt clock, but restoring first makes the question moot and
 * means the correct time is on screen within a second of boot instead of after
 * a DHCP lease and an NTP round trip. SNTP then overwrites it with something
 * better, and we write that back.
 *
 * ============================ DATASHEET FACTS ============================
 * All of this is from NXP PCF85063A Rev. 7.3 (14 July 2026), fetched rather
 * than recalled. Section references are to that document.
 *
 * REGISTERS (Table 4, p.5). Time occupies 04h..0Ah:
 *      04h Seconds   bits 6:0 BCD, and bit 7 is the OS flag
 *      05h Minutes   bits 6:0 BCD
 *      06h Hours     bits 5:0 BCD in 24-hour mode
 *      07h Days      bits 5:0 BCD, 1..31
 *      08h Weekdays  bits 2:0 PLAIN BINARY, 0..6 -- NOT BCD
 *      09h Months    bits 4:0 BCD, 1..12
 *      0Ah Years     all 8 bits BCD, 0..99
 *
 * THE OS FLAG (04h bit 7, sec 7.3.1) is the whole reason this is safe. Quoting:
 * 0 means "Clock integrity is guaranteed"; 1 means "Clock integrity is not
 * guaranteed; the oscillator has stopped or has been interrupted". It is set by
 * hardware on every power-on and stays set until software clears it, which
 * happens as a side effect of writing the seconds register with bit 7 clear.
 * A reading with OS set is discarded ENTIRELY -- not used as a lower bound, not
 * partially trusted. A clock that is confidently wrong is worse than one that
 * admits it does not know.
 *
 * ONE TRANSACTION, NOT SEVERAL (sec 7.4, p.23-24). This is the trap. The part
 * blocks its counters for the duration of an access, so a single burst cannot
 * tear -- but the datasheet is explicit that splitting it is a bug: "setting or
 * reading seconds through to years must be made in one single access. Failing
 * to comply with this method could result in the time becoming corrupted... A
 * roll-over can occur between reads, therefore giving the minutes from one
 * moment and the hours from the next." Both paths below are one transaction.
 *
 * THE 1-SECOND DEADLINE (sec 7.4, and Table 41 note 2, p.40). Only ONE pending
 * increment is stored while the counters are blocked, so an access lasting over
 * a second makes the RTC silently lose the extra seconds -- permanently, and
 * only ever losing, never gaining. Seven bytes at 100 kHz is under a
 * millisecond, so the margin is three orders of magnitude; the risk would be a
 * caller blocking mid-transaction, which is why nothing here logs or allocates
 * between the START and the STOP.
 *
 * WEEKDAY IS NOT DERIVED BY THE CHIP (Table 24 footnote). It stores whatever
 * you put there and will never correct it, so this driver computes it.
 *
 * CENTURY: the Years register holds 00..99 with no century bit anywhere in the
 * part. 2000-2099 is assumed. Leap years are handled by the chip as
 * divisible-by-4 with no 100/400 rule (Table 22 footnote), which is wrong in
 * 2100 and irrelevant to a device that will not see it.
 */
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include <esp_err.h>
#include <esp_log.h>

#include "vaultweather.h"
#include "wx_i2c.h"

#define TAG "wx_rtc"

#define RTC_ADDR      0x51    /* fixed, not configurable (sec 8.5.1) */
#define REG_CONTROL_1 0x00
#define REG_SECONDS   0x04    /* first of the seven time registers */
#define TIME_REGS     7       /* 04h..0Ah */

#define OS_FLAG       0x80    /* Seconds bit 7 */
#define CTRL1_12_24   0x02    /* 0 = 24-hour, and 24-hour is the reset default */

/* Plausibility floor. The part's own power-on reset value is 2000-01-01, so a
 * cell that has gone flat reads as a valid-looking date two decades in the
 * past. OS should catch that, but a floor costs one comparison and turns a
 * subtle wrong-year bug into an obvious rejection. */
#define YEAR_FLOOR    2024

static i2c_master_dev_handle_t dev;

static inline uint8_t bcd2bin(uint8_t v) { return (v & 0x0F) + 10 * (v >> 4); }
static inline uint8_t bin2bcd(uint8_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

/* Days from 1970-01-01 to a given civil date, proleptic Gregorian.
 *
 * Written out rather than using mktime() because mktime interprets its input in
 * the CURRENT timezone, and this app changes TZ at runtime from the weather
 * API. The RTC holds UTC; converting it through a local-time function would
 * shift the clock by the offset -- and would do so only after the first
 * forecast arrived, which is the kind of bug that looks like a network problem.
 * timegm() would be correct but is not guaranteed present.
 *
 * Hinnant's algorithm: shift the year so March is month 1, which puts the leap
 * day at the end of the year and removes every special case. */
static int64_t days_from_civil(int y, unsigned m, unsigned d)
{
	y -= m <= 2;
	const int64_t era = (y >= 0 ? y : y - 399) / 400;
	const unsigned yoe = (unsigned)(y - era * 400);              /* 0..399 */
	const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;  /* 0..146096 */
	return era * 146097 + (int64_t)doe - 719468;
}

static void civil_from_days(int64_t z, int *y, unsigned *m, unsigned *d)
{
	z += 719468;
	const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
	const unsigned doe = (unsigned)(z - era * 146097);
	const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	const int64_t yy = (int64_t)yoe + era * 400;
	const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	const unsigned mp = (5 * doy + 2) / 153;
	*d = doy - (153 * mp + 2) / 5 + 1;
	*m = mp + (mp < 10 ? 3 : -9);
	*y = (int)(yy + (*m <= 2));
}

esp_err_t wx_rtc_init(void)
{
	if (dev)
		return ESP_OK;

	i2c_master_bus_handle_t bus;
	esp_err_t e = wx_i2c_bus(&bus);
	if (e != ESP_OK)
		return e;

	i2c_device_config_t dc = {
		.dev_addr_length = I2C_ADDR_BIT_LEN_7,
		.device_address  = RTC_ADDR,
		/* The part supports 400kHz; 100kHz matches the other device on
		 * this bus and keeps a seven-byte burst three orders of magnitude
		 * inside the 1-second access deadline either way. */
		.scl_speed_hz    = 100000,
	};
	e = i2c_master_bus_add_device(bus, &dc, &dev);
	if (e != ESP_OK) {
		ESP_LOGE(TAG, "add_device: %s", esp_err_to_name(e));
		dev = NULL;          /* never cache a half-open handle */
		return e;
	}

	/* Confirm 24-hour mode rather than assuming the reset default survived.
	 * If some previous firmware left the part in 12-hour mode, every hour
	 * read would be wrong by up to twelve, and the AMPM bit would land
	 * inside the field this driver masks as hours. */
	uint8_t reg = REG_CONTROL_1, ctrl = 0;
	if (i2c_master_transmit_receive(dev, &reg, 1, &ctrl, 1, 100) == ESP_OK &&
	    (ctrl & CTRL1_12_24)) {
		uint8_t fix[2] = { REG_CONTROL_1, (uint8_t)(ctrl & ~CTRL1_12_24) };
		ESP_LOGW(TAG, "RTC was in 12-hour mode; switching to 24");
		i2c_master_transmit(dev, fix, sizeof(fix), 100);
	}
	return ESP_OK;
}

esp_err_t wx_rtc_read(time_t *out)
{
	if (!dev || !out)
		return ESP_ERR_INVALID_STATE;

	/* ONE transaction across all seven registers -- see the header. */
	uint8_t reg = REG_SECONDS, b[TIME_REGS];
	esp_err_t e = i2c_master_transmit_receive(dev, &reg, 1, b, sizeof(b), 100);
	if (e != ESP_OK)
		return e;

	if (b[0] & OS_FLAG) {
		/* The oscillator stopped at some point. Everything in this
		 * reading is meaningless, including the parts that look fine. */
		ESP_LOGW(TAG, "OS flag set -- RTC lost time, discarding reading");
		return ESP_ERR_INVALID_STATE;
	}

	unsigned sec = bcd2bin(b[0] & 0x7F);
	unsigned min = bcd2bin(b[1] & 0x7F);
	unsigned hor = bcd2bin(b[2] & 0x3F);   /* 24-hour mode: 6 bits */
	unsigned day = bcd2bin(b[3] & 0x3F);
	/* b[4] is the weekday: plain binary, and the chip never validates it
	 * against the date. It is deliberately not read -- the epoch fully
	 * determines it, so trusting stored state would only add a way to be
	 * wrong. */
	unsigned mon = bcd2bin(b[5] & 0x1F);
	unsigned yr  = bcd2bin(b[6]);          /* full byte, 00..99 */

	int year = 2000 + (int)yr;

	if (sec > 59 || min > 59 || hor > 23 ||
	    day < 1 || day > 31 || mon < 1 || mon > 12 || year < YEAR_FLOOR) {
		ESP_LOGW(TAG, "implausible RTC reading %04d-%02u-%02u %02u:%02u:%02u",
			 year, mon, day, hor, min, sec);
		return ESP_ERR_INVALID_RESPONSE;
	}

	*out = (time_t)(days_from_civil(year, mon, day) * 86400
			+ (int64_t)hor * 3600 + (int64_t)min * 60 + sec);
	return ESP_OK;
}

esp_err_t wx_rtc_write(time_t t)
{
	if (!dev)
		return ESP_ERR_INVALID_STATE;

	int64_t days = (int64_t)t / 86400;
	int64_t rem  = (int64_t)t % 86400;
	if (rem < 0) { rem += 86400; days -= 1; }

	int      year;
	unsigned mon, day;
	civil_from_days(days, &year, &mon, &day);

	/* 1970-01-01 was a Thursday, and the chip's own encoding is Sunday=0
	 * (Table 24). The chip stores this without checking it, so it is
	 * computed here from the same epoch as everything else -- writing a
	 * weekday that disagrees with the date would be a defect nothing
	 * detects. */
	unsigned wday = (unsigned)(((days % 7) + 11) % 7);

	uint8_t w[1 + TIME_REGS] = {
		REG_SECONDS,
		/* Bit 7 clear here is what CLEARS the OS flag -- the datasheet
		 * offers no other mechanism. Writing the time is what tells the
		 * part its own clock is trustworthy again. */
		(uint8_t)(bin2bcd((uint8_t)(rem % 60)) & 0x7F),
		bin2bcd((uint8_t)((rem / 60) % 60)),
		bin2bcd((uint8_t)(rem / 3600)),
		bin2bcd((uint8_t)day),
		(uint8_t)(wday & 0x07),
		bin2bcd((uint8_t)mon),
		bin2bcd((uint8_t)(year - 2000)),
	};

	/* One transaction, as with the read. */
	esp_err_t e = i2c_master_transmit(dev, w, sizeof(w), 100);
	if (e != ESP_OK)
		ESP_LOGW(TAG, "write failed: %s", esp_err_to_name(e));
	return e;
}
