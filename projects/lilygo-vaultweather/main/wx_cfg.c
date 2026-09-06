/*
 * wx_cfg.c -- persistent configuration and last-good weather cache.
 *
 * Two unrelated records share one NVS namespace, stored differently on
 * purpose:
 *
 *   config -- what the user typed into the setup portal. Written once, read
 *             on every boot. Kept as individual typed keys so a dump of the
 *             partition is readable (components/nvs_flash/nvs_partition_tool/
 *             nvs_tool.py in the IDF tree): when the board will not associate,
 *             "is the SSID actually what I think it is" has to be answerable
 *             without a serial console.
 *
 *   cache  -- the last good weather, so the panel is populated the moment
 *             LVGL starts rather than showing "--" for the twenty-odd seconds
 *             a cold Wi-Fi + SNTP + fetch cycle takes. Written repeatedly, so
 *             it is one versioned blob and it is rate limited.
 *
 * THE WI-FI PASSWORD STORED HERE IS NOT ENCRYPTED. NVS encryption requires
 * flash encryption, which is not enabled on this board -- sdkconfig.defaults
 * sets no CONFIG_NVS_ENCRYPTION, and enabling flash encryption is a one-way
 * efuse burn that would also end casual reflashing of a desk toy. Anyone who
 * can attach a USB cable can `esptool.py read_flash` the nvs partition and
 * read the password in cleartext. That is an accepted risk for a clock on a
 * home network. It is stated here rather than left to be discovered because
 * this is NOT secure storage and nothing else belongs in it.
 */

#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "vaultweather.h"

#define TAG "wx_cfg"

#define WX_NS       "vaultwx"

/* NVS keys are capped at 15 characters (NVS_KEY_NAME_MAX_SIZE - 1). */
#define K_SSID      "ssid"
#define K_PASS      "pass"
#define K_LAT       "lat"
#define K_LON       "lon"
#define K_PLACE     "place"
#define K_PWSID     "pws_id"
#define K_CFG_OK    "cfg_ok"
#define K_CACHE     "wx_cache"

/* -------------------------------------------------------------------------
 * NVS bring-up
 * ---------------------------------------------------------------------- */

/* Every entry point in this file calls this instead of assuming main did it.
 * A missed nvs_flash_init() surfaces as ESP_ERR_NVS_NOT_INITIALIZED during
 * boot -- before the display is up and can say anything about it.
 *
 * NO_FREE_PAGES means the partition is full of stale entries or was truncated
 * by a partition table change; NEW_VERSION_FOUND means it was written by a
 * newer NVS format than this build understands. Neither is recoverable by
 * retrying. Erasing costs the config and drops the user back into the setup
 * portal, which is a far better outcome than a clock that will not boot. */
static esp_err_t nvs_ready(void)
{
	static bool done;

	if (done) {
		return ESP_OK;
	}

	esp_err_t err = nvs_flash_init();
	if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		ESP_LOGW(TAG, "nvs unusable (%s) -- erasing partition", esp_err_to_name(err));
		err = nvs_flash_erase();
		if (err != ESP_OK) {
			ESP_LOGE(TAG, "nvs_flash_erase: %s", esp_err_to_name(err));
			return err;
		}
		err = nvs_flash_init();
	}
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "nvs_flash_init: %s", esp_err_to_name(err));
		return err;
	}

	done = true;
	return ESP_OK;
}

/* -------------------------------------------------------------------------
 * Configuration
 *
 * Coordinates are stored as int32 microdegrees rather than as raw float bits
 * in a u32. NVS has no float type, so something has to be chosen; microdegrees
 * resolve to about 0.11 m, which is four orders of magnitude finer than an
 * Open-Meteo grid cell, and they stay meaningful in a flash dump. A punned
 * float would be an opaque 32-bit number that silently changes meaning if the
 * representation ever does.
 * ---------------------------------------------------------------------- */

/* Largest wx_cfg_t string field, plus room. Checked below rather than trusted. */
#define CFG_STR_MAX 80

_Static_assert(sizeof(((wx_cfg_t *)0)->ssid)   <= CFG_STR_MAX, "CFG_STR_MAX too small for ssid");
_Static_assert(sizeof(((wx_cfg_t *)0)->pass)   <= CFG_STR_MAX, "CFG_STR_MAX too small for pass");
_Static_assert(sizeof(((wx_cfg_t *)0)->place)  <= CFG_STR_MAX, "CFG_STR_MAX too small for place");
_Static_assert(sizeof(((wx_cfg_t *)0)->pws_id) <= CFG_STR_MAX, "CFG_STR_MAX too small for pws_id");

/* Returns true when the key was read into `buf`.
 *
 * A missing key is normal on a board that has never been configured and gives
 * an empty string. A stored value longer than the field returns
 * ESP_ERR_NVS_INVALID_LENGTH from NVS and is NOT truncated to fit: half an
 * SSID associates with nothing and would present as a dead router rather than
 * as bad stored data. Only a genuine NVS failure is reported through `fault`. */
static bool load_str(nvs_handle_t h, const char *key, char *buf, size_t cap,
                     esp_err_t *fault)
{
	size_t len = cap;
	esp_err_t err = nvs_get_str(h, key, buf, &len);

	if (err == ESP_OK) {
		buf[cap - 1] = '\0';
		return true;
	}

	buf[0] = '\0';
	if (err == ESP_ERR_NVS_NOT_FOUND) {
		return false;
	}
	ESP_LOGW(TAG, "cfg key '%s': %s", key, esp_err_to_name(err));
	if (err != ESP_ERR_NVS_INVALID_LENGTH) {
		*fault = err;
	}
	return false;
}

/* nvs_set_str requires a NUL-terminated string. These fields are fixed arrays
 * filled by the portal's form parser in another module; one that exactly filled
 * the array with no terminator would make nvs_set_str walk off the end of the
 * caller's struct. Terminate a bounded local copy instead of trusting it. */
static esp_err_t save_str(nvs_handle_t h, const char *key, const char *src, size_t cap)
{
	char tmp[CFG_STR_MAX];

	if (cap == 0 || cap > sizeof(tmp)) {
		return ESP_ERR_INVALID_SIZE;
	}

	size_t n = strnlen(src, cap - 1);
	memcpy(tmp, src, n);
	tmp[n] = '\0';

	return nvs_set_str(h, key, tmp);
}

esp_err_t wx_cfg_load(wx_cfg_t *out)
{
	if (out == NULL) {
		return ESP_ERR_INVALID_ARG;
	}

	memset(out, 0, sizeof(*out));

	esp_err_t err = nvs_ready();
	if (err != ESP_OK) {
		return err;
	}

	nvs_handle_t h;
	err = nvs_open(WX_NS, NVS_READONLY, &h);
	if (err == ESP_ERR_NVS_NOT_FOUND) {
		/* Namespace has never been written. First boot, or post-erase.
		 * "Not configured yet" is a state this app expects to be in, not a
		 * failure -- the caller responds by running the setup portal. */
		ESP_LOGI(TAG, "no stored config");
		return ESP_OK;
	}
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "nvs_open(%s): %s", WX_NS, esp_err_to_name(err));
		return err;
	}

	esp_err_t fault = ESP_OK;   /* a real NVS failure, as opposed to absent data */

	bool have_ssid = load_str(h, K_SSID, out->ssid, sizeof(out->ssid), &fault);

	/* An open network has an empty password and a board with no personal
	 * weather station has an empty pws_id, so neither absence is a problem;
	 * the contract already spells "" as "no PWS". */
	(void)load_str(h, K_PASS,  out->pass,   sizeof(out->pass),   &fault);
	(void)load_str(h, K_PLACE, out->place,  sizeof(out->place),  &fault);
	(void)load_str(h, K_PWSID, out->pws_id, sizeof(out->pws_id), &fault);

	int32_t lat_ud = 0, lon_ud = 0;
	esp_err_t e_lat = nvs_get_i32(h, K_LAT, &lat_ud);
	esp_err_t e_lon = nvs_get_i32(h, K_LON, &lon_ud);
	if (e_lat != ESP_OK && e_lat != ESP_ERR_NVS_NOT_FOUND) {
		fault = e_lat;
	}
	if (e_lon != ESP_OK && e_lon != ESP_ERR_NVS_NOT_FOUND) {
		fault = e_lon;
	}
	bool have_pos = (e_lat == ESP_OK && e_lon == ESP_OK);
	if (have_pos) {
		out->lat = (float)lat_ud / 1000000.0f;
		out->lon = (float)lon_ud / 1000000.0f;
	}

	uint8_t flag = 0;
	esp_err_t e_flag = nvs_get_u8(h, K_CFG_OK, &flag);
	if (e_flag != ESP_OK && e_flag != ESP_ERR_NVS_NOT_FOUND) {
		fault = e_flag;
	}

	nvs_close(h);

	if (fault != ESP_OK) {
		ESP_LOGE(TAG, "config read failed: %s", esp_err_to_name(fault));
		memset(out, 0, sizeof(*out));
		return fault;
	}

	/* The completion flag alone is not enough. A record that claims to be
	 * complete but has no SSID or no coordinates cannot connect or be queried,
	 * and honouring it would loop the app forever instead of opening the
	 * portal that fixes it. Fields already read are left in place; the portal
	 * can use them to pre-fill. */
	out->configured = (flag == 1) && have_ssid && have_pos;
	if (flag == 1 && !out->configured) {
		ESP_LOGW(TAG, "config marked complete but ssid=%d pos=%d -- treating as unconfigured",
		         (int)have_ssid, (int)have_pos);
	}

	ESP_LOGI(TAG, "config: configured=%d ssid='%s' place='%s' pws='%s' %.4f,%.4f",
	         (int)out->configured, out->ssid, out->place, out->pws_id,
	         (double)out->lat, (double)out->lon);

	return ESP_OK;
}

esp_err_t wx_cfg_save(const wx_cfg_t *cfg)
{
	if (cfg == NULL) {
		return ESP_ERR_INVALID_ARG;
	}

	/* Reject before writing anything. A NaN from a failed geocode fails both
	 * comparisons and would otherwise be rounded into a meaningless int32 and
	 * persisted as a real location. */
	if (!(cfg->lat >= -90.0f && cfg->lat <= 90.0f) ||
	    !(cfg->lon >= -180.0f && cfg->lon <= 180.0f)) {
		ESP_LOGE(TAG, "refusing to save out-of-range position %.4f,%.4f",
		         (double)cfg->lat, (double)cfg->lon);
		return ESP_ERR_INVALID_ARG;
	}

	esp_err_t err = nvs_ready();
	if (err != ESP_OK) {
		return err;
	}

	nvs_handle_t h;
	err = nvs_open(WX_NS, NVS_READWRITE, &h);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "nvs_open(%s,rw): %s", WX_NS, esp_err_to_name(err));
		return err;
	}

	/* Drop the completion flag first and commit it on its own. If power is
	 * lost partway through the field writes below, the next boot sees an
	 * incomplete record and reruns the portal -- rather than a new SSID paired
	 * with the previous password, which fails to associate and looks for all
	 * the world like a broken router. */
	err = nvs_erase_key(h, K_CFG_OK);
	if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
		goto out;
	}
	err = nvs_commit(h);
	if (err != ESP_OK) {
		goto out;
	}

	err = save_str(h, K_SSID, cfg->ssid, sizeof(cfg->ssid));
	if (err != ESP_OK) {
		goto out;
	}
	err = save_str(h, K_PASS, cfg->pass, sizeof(cfg->pass));
	if (err != ESP_OK) {
		goto out;
	}
	err = save_str(h, K_PLACE, cfg->place, sizeof(cfg->place));
	if (err != ESP_OK) {
		goto out;
	}
	err = save_str(h, K_PWSID, cfg->pws_id, sizeof(cfg->pws_id));
	if (err != ESP_OK) {
		goto out;
	}

	err = nvs_set_i32(h, K_LAT, (int32_t)lroundf(cfg->lat * 1000000.0f));
	if (err != ESP_OK) {
		goto out;
	}
	err = nvs_set_i32(h, K_LON, (int32_t)lroundf(cfg->lon * 1000000.0f));
	if (err != ESP_OK) {
		goto out;
	}

	err = nvs_set_u8(h, K_CFG_OK, cfg->configured ? 1 : 0);
	if (err != ESP_OK) {
		goto out;
	}

	err = nvs_commit(h);

out:
	nvs_close(h);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "config save failed: %s", esp_err_to_name(err));
	} else {
		/* %.*s, not %s: the caller's ssid is a fixed array that may not be
		 * terminated -- which is the whole reason save_str exists, and this
		 * line read past the end of it until a host test caught it. The
		 * password is never logged. */
		ESP_LOGI(TAG, "config saved (ssid='%.*s', %.4f,%.4f)",
		         (int)(sizeof(cfg->ssid) - 1), cfg->ssid,
		         (double)cfg->lat, (double)cfg->lon);
	}
	return err;
}

esp_err_t wx_cfg_erase(void)
{
	esp_err_t err = nvs_ready();
	if (err != ESP_OK) {
		return err;
	}

	/* NVS_READWRITE_PURGE + nvs_purge_all reclaim the superseded copies rather
	 * than only marking them deleted. Since the password is stored in
	 * cleartext (see the file header), an "erase config" that left the old
	 * bytes recoverable with esptool would be a lie. Purging rewrites pages,
	 * which is not free, but this runs at most once per five-second button
	 * hold. */
	nvs_handle_t h;
	err = nvs_open(WX_NS, NVS_READWRITE_PURGE, &h);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "nvs_open(%s,purge): %s", WX_NS, esp_err_to_name(err));
		return err;
	}

	/* This takes the weather cache with it, which is correct: the cache
	 * describes a location that is about to be replaced. */
	err = nvs_erase_all(h);
	if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
		goto out;
	}

	esp_err_t perr = nvs_purge_all(h);
	if (perr != ESP_OK) {
		/* The erase itself stands; only the scrub of the old copies failed.
		 * Report it, do not fail the operation -- the user pressed the button
		 * to get back to the portal. */
		ESP_LOGW(TAG, "nvs_purge_all: %s (old copies may remain readable)",
		         esp_err_to_name(perr));
	}

	err = nvs_commit(h);

out:
	nvs_close(h);
	if (err == ESP_ERR_NVS_NOT_FOUND) {
		err = ESP_OK;   /* nothing was stored; the end state is what was asked for */
	}
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "config erase failed: %s", esp_err_to_name(err));
	} else {
		ESP_LOGW(TAG, "config and weather cache erased");
	}
	return err;
}

/* -------------------------------------------------------------------------
 * Weather cache
 * ---------------------------------------------------------------------- */

#define CACHE_MAGIC   0x57584331u   /* 'W' 'X' 'C' '1' */
#define CACHE_VERSION 1

/* The on-flash record. Deliberately not wx_state_t itself:
 *
 *  - cur_source_note is a `const char *` into a string literal. Its numeric
 *    value is meaningless after any rebuild and dereferencing a stale one is a
 *    crash, so it is not stored at all. wx_cache_load points it at a literal
 *    of its own.
 *  - the four timestamps are int64_t rather than time_t, so the record cannot
 *    silently change size if the toolchain's time_t width changes.
 *
 * wx_current_t and wx_day_t are embedded verbatim -- hand-copying twenty
 * fields in two directions is its own bug farm -- so their sizes go in the
 * header and are checked on load. That catches the change that will actually
 * happen (a field added to wx_current_t) even when whoever added it forgot to
 * bump CACHE_VERSION. It does NOT catch a change that keeps the size the same,
 * such as swapping two floats or redefining what a field means: bump
 * CACHE_VERSION for those.
 *
 * Not packed. The size is validated on both sides, so padding is harmless,
 * and unaligned members would cost more than the handful of bytes saved. */
typedef struct {
	uint32_t magic;
	uint8_t  version;
	uint8_t  n_days;
	uint16_t sz_current;
	uint16_t sz_day;
	uint8_t  have_current;
	uint8_t  have_forecast;

	int64_t  sunrise;
	int64_t  sunset;
	int64_t  last_ok;
	int64_t  last_pws_ok;

	wx_current_t cur;
	wx_day_t     day[WX_FORECAST_DAYS];
} cache_rec_t;

/* --- write rate limit ----------------------------------------------------
 *
 * The refresh loop calls wx_cache_save every 10 minutes: 6/h, 144/day, about
 * 52,600 a year. What that actually costs the flash, with the real constants
 * (nvs_constants.h in the IDF tree: 32-byte entries, 126 entries per 4096-byte
 * page; partitions.csv gives the nvs partition 0x6000 = 6 pages, one of which
 * NVS holds back for compaction):
 *
 *   record is ~350 bytes  -> ~11 data entries, plus header and blob index
 *   126 / 13              -> ~9 writes fill a page
 *   52,600 / 9            -> ~5,800 page erases a year
 *   over 6 sectors        -> ~1,000 erase cycles per sector per year
 *   100,000-cycle spec    -> on the order of a century
 *
 * So this is not an imminent wear-out and a comment claiming otherwise would
 * be scaremongering. It is still worth limiting, for three reasons that do not
 * depend on the endurance number: those figures scale linearly with the
 * refresh interval and stop being comfortable if it is ever shortened; the
 * same partition holds the Wi-Fi driver's own calibration data, so this churn
 * drags its pages through compaction too; and the overwhelming majority of
 * those writes would be storing bytes identical to the ones already there.
 *
 * Policy:
 *   - never more often than MIN_WRITE_S. The cache is only ever read on a cold
 *     boot, so a copy half an hour behind is invisible.
 *   - only when a reading moved further than its own noise floor, or the
 *     forecast, the day boundaries or the have_ flags changed.
 *   - but at least every HEARTBEAT_S regardless, because last_ok is what the
 *     UI uses to decide whether data is stale. Skipping writes through a
 *     settled afternoon would leave last_ok hours behind and make perfectly
 *     fresh data look ancient after a power cut.
 *
 * Worst case with a 30-minute floor: ~17,500 writes a year. Settled weather:
 * ~1,500 a year, the heartbeat alone.
 */
#define MIN_WRITE_S   (30 * 60)
#define HEARTBEAT_S   (6 * 60 * 60)

/* Not guarded by a lock. NVS has its own internal locking, so the flash is
 * never corrupted; the exposure is limited to two tasks racing on the state
 * below, whose worst outcome is one redundant write. main is the only caller
 * in this app, and a mutex here would be protecting nothing worth the code. */
static bool        s_have_last;
static cache_rec_t s_last;            /* mirror of what is on flash, ~400B of DRAM */
static int64_t     s_last_write_us;   /* esp_timer, NOT time(): the first SNTP
                                       * sync steps the wall clock by decades
                                       * and would make one comparison
                                       * nonsense at exactly the wrong moment */

/* Sources and validity are compared exactly -- a value that switched from the
 * station to the API is a different reading even at the same number, and the
 * status line says so. */
static bool val_moved(wx_val_t a, wx_val_t b, float eps)
{
	if (a.valid != b.valid || a.src != b.src) {
		return true;
	}
	if (!a.valid) {
		return false;
	}
	return fabsf(a.v - b.v) >= eps;
}

/* Per-field noise floors. A single global epsilon would be wrong in both
 * directions at once: 0.5 spans a third of the useful range of a barometer in
 * inHg and sits below the resolution of a humidity reading.
 *
 * `observed` is deliberately NOT a trigger. It advances on every successful
 * fetch by definition, so treating it as a change would defeat the whole rate
 * limit while looking like it worked. Same reasoning excludes last_ok and
 * last_pws_ok in the caller; the heartbeat above is what keeps them current. */
static bool current_moved(const wx_current_t *a, const wx_current_t *b)
{
	return a->weather_code != b->weather_code ||
	       a->is_day       != b->is_day ||
	       val_moved(a->temp_f,         b->temp_f,         0.5f) ||
	       val_moved(a->feels_f,        b->feels_f,        0.5f) ||
	       val_moved(a->dew_f,          b->dew_f,          0.5f) ||
	       val_moved(a->humidity_pct,   b->humidity_pct,   2.0f) ||
	       val_moved(a->wind_mph,       b->wind_mph,       1.0f) ||
	       val_moved(a->gust_mph,       b->gust_mph,       2.0f) ||
	       /* Linear compare on a circular quantity: 350 deg vs 10 deg reads as
	        * 340 rather than 20. The only cost is an extra write on a wind
	        * that crosses north, so it is not worth the modular arithmetic. */
	       val_moved(a->wind_deg,       b->wind_deg,       15.0f) ||
	       val_moved(a->pressure_inhg,  b->pressure_inhg,  0.02f) ||
	       val_moved(a->rain_rate_inhr, b->rain_rate_inhr, 0.01f) ||
	       val_moved(a->rain_today_in,  b->rain_today_in,  0.01f) ||
	       val_moved(a->cloud_pct,      b->cloud_pct,      5.0f) ||
	       val_moved(a->uv,             b->uv,             0.5f) ||
	       val_moved(a->precip_prob_pct, b->precip_prob_pct, 5.0f);
}

static bool day_moved(const wx_day_t *a, const wx_day_t *b)
{
	if (a->valid != b->valid) {
		return true;
	}
	if (!a->valid) {
		return false;
	}
	return a->date         != b->date ||
	       a->weather_code != b->weather_code ||
	       fabsf(a->hi_f - b->hi_f) >= 1.0f ||
	       fabsf(a->lo_f - b->lo_f) >= 1.0f ||
	       fabsf(a->precip_prob_pct - b->precip_prob_pct) >= 5.0f ||
	       fabsf(a->precip_sum_in - b->precip_sum_in) >= 0.05f ||
	       fabsf(a->wind_max_mph - b->wind_max_mph) >= 2.0f ||
	       fabsf(a->uv_max - b->uv_max) >= 0.5f;
}

esp_err_t wx_cache_load(wx_state_t *out)
{
	if (out == NULL) {
		return ESP_ERR_INVALID_ARG;
	}

	memset(out, 0, sizeof(*out));

	/* Never left NULL. The UI module dereferences this for the status line and
	 * a cold boot with no cache is the exact case where it would be reached. */
	out->cur_source_note = "";

	esp_err_t err = nvs_ready();
	if (err != ESP_OK) {
		return err;
	}

	nvs_handle_t h;
	err = nvs_open(WX_NS, NVS_READONLY, &h);
	if (err != ESP_OK) {
		if (err != ESP_ERR_NVS_NOT_FOUND) {
			ESP_LOGE(TAG, "nvs_open(%s): %s", WX_NS, esp_err_to_name(err));
		}
		return err;
	}

	cache_rec_t rec;
	size_t len = sizeof(rec);
	err = nvs_get_blob(h, K_CACHE, &rec, &len);
	nvs_close(h);

	if (err != ESP_OK) {
		/* NOT_FOUND on a board that has never synced; INVALID_LENGTH if the
		 * record grew in a firmware update, which is the case this design is
		 * here to catch. Neither is a fault -- the app fetches instead. */
		if (err != ESP_ERR_NVS_NOT_FOUND) {
			ESP_LOGW(TAG, "cache read: %s -- discarding", esp_err_to_name(err));
		}
		return err;
	}

	if (len != sizeof(rec) ||
	    rec.magic      != CACHE_MAGIC ||
	    rec.version    != CACHE_VERSION ||
	    rec.n_days     != WX_FORECAST_DAYS ||
	    rec.sz_current != (uint16_t)sizeof(wx_current_t) ||
	    rec.sz_day     != (uint16_t)sizeof(wx_day_t)) {
		ESP_LOGW(TAG, "cache layout mismatch (magic %08" PRIx32 " v%u %ub cur/%ub day/%u days,"
		              " want %08" PRIx32 " v%u %ub/%ub/%u) -- discarding",
		         rec.magic, (unsigned)rec.version, (unsigned)rec.sz_current,
		         (unsigned)rec.sz_day, (unsigned)rec.n_days,
		         (uint32_t)CACHE_MAGIC, (unsigned)CACHE_VERSION,
		         (unsigned)sizeof(wx_current_t), (unsigned)sizeof(wx_day_t),
		         (unsigned)WX_FORECAST_DAYS);
		return ESP_ERR_INVALID_VERSION;
	}

	out->cur = rec.cur;
	memcpy(out->day, rec.day, sizeof(out->day));
	out->sunrise      = (time_t)rec.sunrise;
	out->sunset       = (time_t)rec.sunset;
	out->last_ok      = (time_t)rec.last_ok;
	out->last_pws_ok  = (time_t)rec.last_pws_ok;
	out->have_current = (rec.have_current != 0);
	out->have_forecast = (rec.have_forecast != 0);

	/* A literal, not the stored tier name: which of the three PWS tiers won
	 * last time says nothing about now, and the UI should show that this came
	 * off flash rather than the wire. */
	out->cur_source_note = "CACHED";

	ESP_LOGI(TAG, "cache restored (current=%d forecast=%d last_ok=%lld)",
	         (int)out->have_current, (int)out->have_forecast, (long long)out->last_ok);

	return ESP_OK;
}

esp_err_t wx_cache_save(const wx_state_t *st)
{
	if (st == NULL) {
		return ESP_ERR_INVALID_ARG;
	}

	cache_rec_t rec;

	/* Zero the padding as well as the fields. The struct is written to flash
	 * verbatim, and leaving stack residue in the gaps both makes a hex dump
	 * unreadable and puts whatever was on the stack into persistent storage. */
	memset(&rec, 0, sizeof(rec));

	rec.magic         = CACHE_MAGIC;
	rec.version       = CACHE_VERSION;
	rec.n_days        = WX_FORECAST_DAYS;
	rec.sz_current    = (uint16_t)sizeof(wx_current_t);
	rec.sz_day        = (uint16_t)sizeof(wx_day_t);
	rec.have_current  = st->have_current ? 1 : 0;
	rec.have_forecast = st->have_forecast ? 1 : 0;
	rec.sunrise       = (int64_t)st->sunrise;
	rec.sunset        = (int64_t)st->sunset;
	rec.last_ok       = (int64_t)st->last_ok;
	rec.last_pws_ok   = (int64_t)st->last_pws_ok;
	rec.cur           = st->cur;
	memcpy(rec.day, st->day, sizeof(rec.day));

	int64_t now_us = esp_timer_get_time();

	/* The first save after a boot always writes. What is on flash at that
	 * point could be from ten minutes or ten weeks ago and there is no cheap
	 * way to tell, so it gets refreshed once. Reboots are rare enough that
	 * this does not move the arithmetic above. */
	if (s_have_last) {
		int64_t age_us = now_us - s_last_write_us;

		bool changed = rec.have_current  != s_last.have_current ||
		               rec.have_forecast != s_last.have_forecast ||
		               llabs(rec.sunrise - s_last.sunrise) >= 60 ||
		               llabs(rec.sunset  - s_last.sunset)  >= 60 ||
		               current_moved(&rec.cur, &s_last.cur);

		for (int i = 0; !changed && i < WX_FORECAST_DAYS; i++) {
			changed = day_moved(&rec.day[i], &s_last.day[i]);
		}

		if (age_us < (int64_t)HEARTBEAT_S * 1000000) {
			if (!changed || age_us < (int64_t)MIN_WRITE_S * 1000000) {
				/* Nothing here is worth a flash write. Reported as success
				 * because it is: the caller has no decision to make about it,
				 * and s_last is left alone so a change suppressed by the
				 * floor is still pending on the next call. */
				return ESP_OK;
			}
		}
	}

	esp_err_t err = nvs_ready();
	if (err != ESP_OK) {
		return err;
	}

	nvs_handle_t h;
	err = nvs_open(WX_NS, NVS_READWRITE, &h);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "nvs_open(%s,rw): %s", WX_NS, esp_err_to_name(err));
		return err;
	}

	err = nvs_set_blob(h, K_CACHE, &rec, sizeof(rec));
	if (err == ESP_OK) {
		err = nvs_commit(h);
	}
	nvs_close(h);

	if (err != ESP_OK) {
		ESP_LOGE(TAG, "cache save failed: %s", esp_err_to_name(err));
		return err;
	}

	s_last          = rec;
	s_last_write_us = now_us;
	s_have_last     = true;

	/* Logged once per boot so the entry arithmetic in the comment above can be
	 * checked against the build rather than taken on faith. */
	static bool sized;
	if (!sized) {
		sized = true;
		ESP_LOGI(TAG, "cache record %u bytes (%u NVS entries)",
		         (unsigned)sizeof(rec), (unsigned)((sizeof(rec) + 31) / 32 + 2));
	}

	return ESP_OK;
}
