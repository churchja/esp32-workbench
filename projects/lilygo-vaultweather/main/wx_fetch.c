/*
 * wx_fetch.c -- everything that talks to the network and turns bytes into
 * wx_val_t. No other module makes an HTTP request; no other module knows a
 * metric unit exists.
 *
 * WHY THE READ LOOP IS HAND-ROLLED
 * api.open-meteo.com answers with Transfer-Encoding: chunked, so
 * esp_http_client_get_content_length() is -1 and esp_http_client_fetch_headers()
 * returns 0. The official esp_http_client example guards its accumulation
 * branch with !esp_http_client_is_chunked_response(); copied verbatim, that
 * guard is false for every Open-Meteo reply and the handler collects ZERO
 * bytes while every esp_err_t stays ESP_OK. It fails completely silently.
 * http_get() below therefore uses the synchronous open/fetch_headers/read
 * pattern with an explicit byte cap and never consults content-length at all.
 *
 * WHY THERE ARE THREE TIERS
 * Not retries. Each tier fails on a DIFFERENT gate, verified live:
 *   1. data.weathercloud.net wants  Accept: application/vnd.weathercloud.v0+json
 *      (v1 answers 401 ERR_APP_CHECK_UNAUTHORIZED -- Firebase App Check, which
 *      an ESP32 cannot mint a token for; no Accept header at all answers 406).
 *   2. app.weathercloud.net wants   X-Requested-With: XMLHttpRequest
 *      and, without it, answers HTTP 200 WITH A ZERO-BYTE BODY. Checking the
 *      status code alone passes that. This code checks the byte count.
 *   3. Open-Meteo, which needs no header at all and always answers.
 * If Weathercloud changes its auth story, tier 2 lives on a different host and
 * a different gate, so it is a real second chance rather than a second attempt.
 *
 * WHY UNITS ARE ASYMMETRIC HERE
 * Weathercloud reports metric base units and is converted in wc_ingest().
 * Open-Meteo is ASKED for imperial via query parameters and is NOT converted
 * in om_current() -- except pressure_msl, which comes back in hPa whatever the
 * unit parameters say. Both sites are commented, because converting the
 * already-imperial side is exactly how a display ends up reading 180 F.
 *
 * Response shapes and header requirements in this file were confirmed by live
 * request against a real Weathercloud station and a real coordinate pair while
 * it was written; the byte counts in the comments are the measured ones, not
 * estimates. The specific device ID is deliberately not recorded here -- this
 * repository is public and that ID identifies someone's home.
 */

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cJSON.h>
#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_log.h"

#include "vaultweather.h"

#define TAG "wx_fetch"

/* -------------------------------------------------------------------------
 * Sizes
 *
 * Measured bodies: Weathercloud v0 354 B, Weathercloud XHR 168 B, Open-Meteo
 * forecast 1698 B, Open-Meteo geocoding 682 B. The caps are the next power of
 * two with room for a station that adds variables, not guesses.
 * ---------------------------------------------------------------------- */
#define WC_CAP          4096
#define OM_CAP          8192
#define URL_CAP         1024

#define HTTP_TIMEOUT_MS 10000
#define HTTP_RX_BUF     1024

/* TRANSMIT buffer -- a SEPARATE field from .buffer_size, which is receive-only.
 *
 * Leaving .buffer_size_tx at 0 defaults it to DEFAULT_HTTP_BUF_SIZE, which is
 * 512 (esp_http_client.h:20). esp_http_client.c:1778 builds the whole request
 * line -- "GET " + path + "?" + query + " HTTP/1.1\r\n" -- into that buffer and
 * bails with ESP_LOGE("Out of buffer") if it does not fit.
 *
 * Open-Meteo's field-selection API produces long URLs by design: ours is 551
 * characters of path and query, so the request line alone needs 566. Every
 * forecast fetch therefore failed with ESP_FAIL before a single byte reached
 * the network -- which read on the panel as a blank 3-day forecast and blank
 * sunrise/sunset/UV/cloud, because those are exactly the API-only fields. The
 * PWS tiers kept working, which is what made it look like a parsing bug rather
 * than a request that was never sent.
 *
 * 1024 holds the 566-byte request line plus Host, User-Agent, Connection and
 * the two Weathercloud request headers with room to spare. */
#define HTTP_TX_BUF 1024
#define USER_AGENT      "vaultweather/1.0 (esp32-s3; esp-idf)"

/* -------------------------------------------------------------------------
 * Unit conversion -- METRIC SOURCES ONLY. See the DESIGN RULE in the header.
 * ---------------------------------------------------------------------- */

/* 1 inHg = 3386.388640341 Pa = 33.86388640341 hPa */
#define HPA_TO_INHG   0.02952998307f
/* 1 m/s = 3600 s/h / 1609.344 m/mi */
#define MPS_TO_MPH    2.23693629205f
/* 1 in = 25.4 mm exactly */
#define MM_TO_IN      0.03937007874f

static inline float c_to_f(double c) { return (float)(c * 9.0 / 5.0 + 32.0); }

/* NWS defines the heat index only at or above 80 F and wind chill only at or
 * below 50 F. Weathercloud supplies both `heat` and `chill` and sets each equal
 * to `temp` outside its own validity range (verified: temp 27.1 C, chill 27.1,
 * heat 30.6), so picking by threshold can never pick a placeholder over a real
 * value. Between the thresholds neither applies and the dry-bulb temperature
 * IS what it feels like. */
#define FEELS_HEAT_MIN_F  80.0f
#define FEELS_CHILL_MAX_F 50.0f

/* -------------------------------------------------------------------------
 * Small helpers
 * ---------------------------------------------------------------------- */

/* A key that is absent, null, or non-numeric is ABSENT, never zero. A
 * barometer reading 0.00 inHg looks like a measurement. */
static bool jnum(const cJSON *o, const char *key, double *out)
{
	const cJSON *it = cJSON_GetObjectItemCaseSensitive(o, key);
	if (!cJSON_IsNumber(it)) {
		return false;
	}
	*out = it->valuedouble;
	return true;
}

static bool jarr_num(const cJSON *o, const char *key, int idx, double *out)
{
	const cJSON *arr = cJSON_GetObjectItemCaseSensitive(o, key);
	if (!cJSON_IsArray(arr)) {
		return false;
	}
	const cJSON *it = cJSON_GetArrayItem(arr, idx);
	if (!cJSON_IsNumber(it)) {
		return false;
	}
	*out = it->valuedouble;
	return true;
}

static const char *jarr_str(const cJSON *o, const char *key, int idx)
{
	const cJSON *arr = cJSON_GetObjectItemCaseSensitive(o, key);
	if (!cJSON_IsArray(arr)) {
		return NULL;
	}
	const cJSON *it = cJSON_GetArrayItem(arr, idx);
	return cJSON_IsString(it) ? it->valuestring : NULL;
}

static const char *jstr(const cJSON *o, const char *key)
{
	const cJSON *it = cJSON_GetObjectItemCaseSensitive(o, key);
	return cJSON_IsString(it) ? it->valuestring : NULL;
}

/* Set a field from a metric source with conversion, or leave it absent. */
static void set_conv(wx_val_t *dst, const cJSON *o, const char *key,
                     float scale, wx_source_t src)
{
	double d;
	if (jnum(o, key, &d)) {
		*dst = wx_val((float)d * scale, src);
	}
}

/* Days since 1970-01-01 from a proleptic Gregorian date. Howard Hinnant's
 * days_from_civil. Used instead of mktime()/timegm() because both of those
 * consult the current TZ, and this file has to convert timestamps BEFORE it
 * has learned what the TZ is. */
static long long days_from_civil(int y, int m, int d)
{
	y -= (m <= 2);
	const long long era = (y >= 0 ? y : y - 399) / 400;
	const int yoe = (int)(y - era * 400);
	const int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097LL + doe - 719468LL;
}

/* Parses "YYYY-MM-DD", "YYYY-MM-DDTHH:MM" and "YYYY-MM-DDTHH:MM:SSZ" alike;
 * missing trailing components read as zero and any trailing Z is ignored.
 * `utc_off` is the seconds the string's wall clock is AHEAD of UTC -- pass 0
 * for a Zulu timestamp, utc_offset_seconds for an Open-Meteo local one. */
static bool iso_to_epoch(const char *s, long utc_off, time_t *out)
{
	int Y = 0, Mo = 0, D = 0, h = 0, mi = 0, sec = 0;

	if (!s) {
		return false;
	}
	if (sscanf(s, "%d-%d-%dT%d:%d:%d", &Y, &Mo, &D, &h, &mi, &sec) < 3) {
		return false;
	}
	if (Y < 1970 || Y > 2200 || Mo < 1 || Mo > 12 || D < 1 || D > 31 ||
	    h < 0 || h > 23 || mi < 0 || mi > 59 || sec < 0 || sec > 60) {
		return false;
	}
	*out = (time_t)(days_from_civil(Y, Mo, D) * 86400LL +
	                h * 3600 + mi * 60 + sec - utc_off);
	return true;
}

static void url_encode(const char *in, char *out, size_t n)
{
	static const char HEX[] = "0123456789ABCDEF";
	size_t o = 0;

	for (const unsigned char *p = (const unsigned char *)in; *p && o + 4 < n; p++) {
		if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~') {
			out[o++] = (char)*p;
		} else {
			out[o++] = '%';
			out[o++] = HEX[*p >> 4];
			out[o++] = HEX[*p & 0x0F];
		}
	}
	out[o] = '\0';
}

/* -------------------------------------------------------------------------
 * HTTP
 * ---------------------------------------------------------------------- */

/* GET `url` into `buf`, NUL-terminated. Returns the byte count (which may be
 * 0 -- see tier 2), or -1 if the request itself failed or the status was not
 * 2xx. One optional extra request header; both tiers of Weathercloud need one
 * and are rejected without it. */
static int http_get(const char *url, const char *hdr_key, const char *hdr_val,
                    char *buf, size_t cap)
{
	esp_http_client_config_t cfg = {
		.url            = url,
		.method         = HTTP_METHOD_GET,
		.timeout_ms     = HTTP_TIMEOUT_MS,
		.user_agent     = USER_AGENT,
		.buffer_size    = HTTP_RX_BUF,
		.buffer_size_tx = HTTP_TX_BUF,
	};

	/* Weathercloud is HTTPS-only (port 80 answers 301). Open-Meteo serves
	 * plain HTTP with no redirect and no HSTS, so it skips the TLS handshake
	 * and the ~30 KB of mbedTLS session buffers entirely. */
	if (strncmp(url, "https://", 8) == 0) {
		cfg.crt_bundle_attach = esp_crt_bundle_attach;
	}

	esp_http_client_handle_t c = esp_http_client_init(&cfg);
	if (!c) {
		ESP_LOGE(TAG, "client init failed");
		return -1;
	}

	int out = -1;
	esp_err_t err;

	if (hdr_key) {
		err = esp_http_client_set_header(c, hdr_key, hdr_val);
		if (err != ESP_OK) {
			ESP_LOGE(TAG, "set_header %s: %s", hdr_key, esp_err_to_name(err));
			goto cleanup;
		}
	}

	err = esp_http_client_open(c, 0);
	if (err != ESP_OK) {
		ESP_LOGW(TAG, "open %s: %s", url, esp_err_to_name(err));
		goto cleanup;
	}

	/* Returns 0 for a chunked body. That is not an error and the value is
	 * deliberately discarded: it must not be used to size the read loop. */
	(void)esp_http_client_fetch_headers(c);

	int status = esp_http_client_get_status_code(c);
	if (status < 200 || status > 299) {
		ESP_LOGW(TAG, "HTTP %d from %s", status, url);
		goto close;
	}

	size_t n = 0;
	while (n + 1 < cap) {
		int r = esp_http_client_read(c, buf + n, (int)(cap - 1 - n));
		if (r <= 0) {
			break;    /* 0 = end of body, negative = transport error */
		}
		n += (size_t)r;
	}
	buf[n] = '\0';
	if (n + 1 >= cap) {
		ESP_LOGW(TAG, "response hit the %u byte cap, likely truncated",
		         (unsigned)cap);
	}
	out = (int)n;

close:
	esp_http_client_close(c);
cleanup:
	esp_http_client_cleanup(c);
	return out;
}

/* -------------------------------------------------------------------------
 * Time zone
 *
 * Open-Meteo returns an IANA name ("America/Chicago") and a numeric
 * utc_offset_seconds. newlib on the ESP32 has no tzdata database, so the IANA
 * name is unusable; timezone_abbreviation ("GMT-5") contains digits and is not
 * a legal POSIX zone name either. What IS portable is the classic
 * `std offset` form from the ESP-IDF docs ("CST-8"), so the numeric offset is
 * rendered into that with a placeholder three-letter tag.
 *
 * POSIX SIGNS ARE INVERTED: the offset is how far the zone is WEST of UTC, so
 * utc_offset_seconds -18000 (US Central, DST in effect) becomes "LOC5".
 *
 * DST: utc_offset_seconds already has DST folded in as of the request, and
 * this is re-derived on every poll, so a spring-forward is picked up within
 * one 10-minute cycle instead of being six months wrong. A synthesized rule
 * would be a guess; a re-read number is a measurement.
 * ---------------------------------------------------------------------- */
static void apply_tz(long utc_off)
{
	static long applied = LONG_MIN;
	char tz[24];

	if (utc_off == applied) {
		return;
	}

	long west = -utc_off;
	long mag  = west < 0 ? -west : west;
	const char *sign = west < 0 ? "-" : "";
	int h = (int)(mag / 3600);
	int m = (int)((mag % 3600) / 60);

	if (m) {
		snprintf(tz, sizeof tz, "LOC%s%d:%02d", sign, h, m);
	} else {
		snprintf(tz, sizeof tz, "LOC%s%d", sign, h);
	}

	/* REQUEST, do not set. This function runs on the fetch task; tzset()
	 * must happen on the render task, which is the one calling localtime_r
	 * every second. See wx_net.c for the full reasoning. */
	wx_time_request_tz(tz);
	applied = utc_off;
	ESP_LOGI(TAG, "TZ=%s (utc_offset_seconds=%ld)", tz, utc_off);
}

/* -------------------------------------------------------------------------
 * Weathercloud
 * ---------------------------------------------------------------------- */

/* Ingest one Weathercloud reading object.
 *
 * Tier 1 nests the values under "values" and timestamps with an ISO string;
 * tier 2 puts them at the top level and timestamps with a unix "epoch". The
 * MEASUREMENTS are identically named and identically metric in both -- checked
 * live, both endpoints returning temp 27.1 / bar 995.8 / hum 87 within the same
 * minute -- so one ingest serves both. Anything tier 2 renames is simply not
 * recognised and stays absent, which is the correct outcome for an
 * undocumented endpoint.
 *
 * Returns false when there is no temperature, which is the one field that
 * makes a station reading worth having; the caller then falls through a tier.
 */
static bool wc_ingest(const cJSON *v, time_t observed, wx_current_t *out)
{
	double t_c;

	if (!cJSON_IsObject(v) || !jnum(v, "temp", &t_c)) {
		return false;
	}

	/* METRIC SOURCE -- converted here, once. Do not convert again anywhere. */
	float t_f = c_to_f(t_c);
	out->temp_f = wx_val(t_f, WX_SRC_PWS);

	double d;
	if (jnum(v, "dew", &d))  out->dew_f       = wx_val(c_to_f(d), WX_SRC_PWS);
	if (jnum(v, "hum", &d))  out->humidity_pct = wx_val((float)d, WX_SRC_PWS);
	if (jnum(v, "wdir", &d)) out->wind_deg     = wx_val((float)d, WX_SRC_PWS);

	set_conv(&out->pressure_inhg,  v, "bar",      HPA_TO_INHG, WX_SRC_PWS);
	set_conv(&out->wind_mph,       v, "wspd",     MPS_TO_MPH,  WX_SRC_PWS);
	set_conv(&out->gust_mph,       v, "wspdhi",   MPS_TO_MPH,  WX_SRC_PWS);
	set_conv(&out->rain_rate_inhr, v, "rainrate", MM_TO_IN,    WX_SRC_PWS);
	set_conv(&out->rain_today_in,  v, "rain",     MM_TO_IN,    WX_SRC_PWS);

	/* See FEELS_* above for why a threshold pick is safe here. */
	double raw;
	if (t_f >= FEELS_HEAT_MIN_F && jnum(v, "heat", &raw)) {
		out->feels_f = wx_val(c_to_f(raw), WX_SRC_PWS);
	} else if (t_f <= FEELS_CHILL_MAX_F && jnum(v, "chill", &raw)) {
		out->feels_f = wx_val(c_to_f(raw), WX_SRC_PWS);
	} else {
		out->feels_f = wx_val(t_f, WX_SRC_PWS);
	}

	out->observed = observed;
	return true;
}

/* Tier 1: documented-ish v0 API. ~354 bytes. */
static bool pws_v0(const char *pws_id, char *buf, char *url, wx_current_t *out)
{
	snprintf(url, URL_CAP, "https://data.weathercloud.net/devices/%s/values", pws_id);

	/* v1 in this header answers 401 ERR_APP_CHECK_UNAUTHORIZED (Firebase App
	 * Check); omitting the header entirely answers 406 ERR_NOT_ACCEPTABLE. */
	int n = http_get(url, "Accept", "application/vnd.weathercloud.v0+json", buf, WC_CAP);
	if (n <= 0) {
		return false;
	}

	cJSON *root = cJSON_Parse(buf);
	if (!root) {
		ESP_LOGW(TAG, "tier1: unparseable body (%d bytes)", n);
		return false;
	}

	const cJSON *values = cJSON_GetObjectItemCaseSensitive(root, "values");

	/* The station's OWN timestamp. Substituting fetch time here would hide a
	 * station that stopped uploading hours ago behind a fresh clock. */
	time_t obs = 0;
	if (!iso_to_epoch(jstr(values, "ts"), 0, &obs)) {
		ESP_LOGW(TAG, "tier1: no usable ts; reading will read as stale");
	}

	bool ok = wc_ingest(values, obs, out);
	cJSON_Delete(root);
	return ok;
}

/* Tier 2: the undocumented XHR endpoint the web dashboard uses. ~168 bytes. */
static bool pws_xhr(const char *pws_id, char *buf, char *url, wx_current_t *out)
{
	snprintf(url, URL_CAP, "https://app.weathercloud.net/device/values/%s", pws_id);

	int n = http_get(url, "X-Requested-With", "XMLHttpRequest", buf, WC_CAP);

	/* WITHOUT that header this endpoint answers HTTP 200 with a ZERO-BYTE
	 * BODY -- no error status, no error object. Verified. A status-only check
	 * would read that as success and then quietly merge nothing, so the byte
	 * count is the gate. */
	if (n <= 0) {
		ESP_LOGW(TAG, "tier2: %d bytes; treating as failure, not as no-change", n);
		return false;
	}

	cJSON *root = cJSON_Parse(buf);
	if (!root) {
		ESP_LOGW(TAG, "tier2: unparseable body (%d bytes)", n);
		return false;
	}

	/* Timestamped with a unix epoch here rather than tier 1's ISO string. */
	time_t obs = 0;
	double e;
	if (jnum(root, "epoch", &e)) {
		obs = (time_t)e;
	} else {
		ESP_LOGW(TAG, "tier2: no epoch; reading will read as stale");
	}

	bool ok = wc_ingest(root, obs, out);
	cJSON_Delete(root);
	return ok;
}

/* -------------------------------------------------------------------------
 * Open-Meteo
 * ---------------------------------------------------------------------- */

/* pressure_msl, not surface_pressure: surface_pressure is corrected to station
 * elevation (994.7 against 1015.5 for the same site) and is not what a
 * barometer readout means. sea_level_pressure is rejected outright by the API.
 * timezone=auto makes the response carry timezone and utc_offset_seconds.
 * The unit parameters make everything except pressure_msl imperial already. */
static const char OM_URL_FMT[] =
	"http://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
	"&current=temperature_2m,apparent_temperature,relative_humidity_2m,"
	"dew_point_2m,wind_speed_10m,wind_direction_10m,wind_gusts_10m,"
	"pressure_msl,precipitation,precipitation_probability,weather_code,"
	"cloud_cover,uv_index,is_day"
	"&daily=weather_code,temperature_2m_max,temperature_2m_min,"
	"precipitation_probability_max,precipitation_sum,wind_speed_10m_max,"
	"wind_direction_10m_dominant,sunrise,sunset,uv_index_max"
	"&forecast_days=%d&timezone=auto&temperature_unit=fahrenheit"
	"&wind_speed_unit=mph&precipitation_unit=inch";

/* Fetch and parse the Open-Meteo forecast document. On ESP_OK the caller owns
 * *root and must cJSON_Delete it. Also applies the reported time zone. */
static esp_err_t om_fetch(const wx_cfg_t *cfg, int days, char *buf, char *url,
                          cJSON **root_out, long *utc_off_out)
{
	*root_out = NULL;

	snprintf(url, URL_CAP, OM_URL_FMT, (double)cfg->lat, (double)cfg->lon, days);

	int n = http_get(url, NULL, NULL, buf, OM_CAP);
	if (n <= 0) {
		return ESP_FAIL;
	}

	cJSON *root = cJSON_Parse(buf);
	if (!root) {
		ESP_LOGW(TAG, "open-meteo: unparseable body (%d bytes)", n);
		return ESP_FAIL;
	}

	/* Errors are a top-level {"error":true,"reason":"..."}. In practice they
	 * arrive with HTTP 400 and http_get has already rejected them, so this is
	 * the guard for an error body served with a 2xx. The reason string can be
	 * a full type dump of every accepted variable name, so it is deliberately
	 * neither parsed nor logged. */
	const cJSON *e = cJSON_GetObjectItemCaseSensitive(root, "error");
	if (cJSON_IsTrue(e)) {
		ESP_LOGW(TAG, "open-meteo returned an error object; check lat/lon");
		cJSON_Delete(root);
		return ESP_FAIL;
	}

	double off = 0;
	(void)jnum(root, "utc_offset_seconds", &off);
	apply_tz((long)off);

	*root_out = root;
	*utc_off_out = (long)off;
	return ESP_OK;
}

/* Parse the "current" block. EVERYTHING HERE IS ALREADY IMPERIAL because of
 * the temperature_unit / wind_speed_unit / precipitation_unit query
 * parameters -- do NOT convert it. The single exception is pressure_msl, which
 * the API returns in hPa regardless of those parameters. */
static void om_current(const cJSON *root, long utc_off, wx_current_t *out)
{
	const cJSON *c = cJSON_GetObjectItemCaseSensitive(root, "current");
	if (!cJSON_IsObject(c)) {
		return;
	}

	double d;
	if (jnum(c, "temperature_2m", &d))        out->temp_f        = wx_val((float)d, WX_SRC_API);
	if (jnum(c, "apparent_temperature", &d))  out->feels_f       = wx_val((float)d, WX_SRC_API);
	if (jnum(c, "relative_humidity_2m", &d))  out->humidity_pct  = wx_val((float)d, WX_SRC_API);
	if (jnum(c, "dew_point_2m", &d))          out->dew_f         = wx_val((float)d, WX_SRC_API);
	if (jnum(c, "wind_speed_10m", &d))        out->wind_mph      = wx_val((float)d, WX_SRC_API);
	if (jnum(c, "wind_gusts_10m", &d))        out->gust_mph      = wx_val((float)d, WX_SRC_API);
	if (jnum(c, "wind_direction_10m", &d))    out->wind_deg      = wx_val((float)d, WX_SRC_API);
	if (jnum(c, "cloud_cover", &d))           out->cloud_pct     = wx_val((float)d, WX_SRC_API);
	if (jnum(c, "uv_index", &d))              out->uv            = wx_val((float)d, WX_SRC_API);
	if (jnum(c, "precipitation_probability", &d))
		out->precip_prob_pct = wx_val((float)d, WX_SRC_API);

	/* THE ONE UNIT CONVERSION on this side. hPa -> inHg. */
	if (jnum(c, "pressure_msl", &d))
		out->pressure_inhg = wx_val((float)d * HPA_TO_INHG, WX_SRC_API);

	/* `precipitation` is a BACKWARD-LOOKING SUM over `interval` seconds
	 * (900 in every response seen), already in inches -- not a rate. Turning
	 * it into in/hr is a time conversion, not a unit conversion: reporting
	 * 0.02 in of the last quarter hour as 0.02 in/hr understates by 4x. */
	double interval_s;
	if (jnum(c, "precipitation", &d) && jnum(c, "interval", &interval_s) &&
	    interval_s > 0.0) {
		out->rain_rate_inhr = wx_val((float)(d * 3600.0 / interval_s), WX_SRC_API);
	}

	if (jnum(c, "weather_code", &d)) out->weather_code = (int)d;
	if (jnum(c, "is_day", &d))       out->is_day = (d != 0.0);

	/* rain_today_in is DELIBERATELY left absent when only the API answered.
	 * The nearest thing Open-Meteo offers is daily precipitation_sum[0], which
	 * is a whole-day total with the rest of today still forecast -- at 08:00
	 * that reads as rain that has not fallen. "--" is honest; a forecast
	 * wearing an observation's label is not. */

	time_t t;
	if (iso_to_epoch(jstr(c, "time"), utc_off, &t)) {
		out->observed = t;
	}

	/* No current precipitation_probability from some models; today's daily
	 * max is the honest stand-in and comes from the same document. */
	if (!out->precip_prob_pct.valid) {
		const cJSON *daily = cJSON_GetObjectItemCaseSensitive(root, "daily");
		if (cJSON_IsObject(daily) &&
		    jarr_num(daily, "precipitation_probability_max", 0, &d)) {
			out->precip_prob_pct = wx_val((float)d, WX_SRC_API);
		}
	}
}

/* -------------------------------------------------------------------------
 * Public: current conditions
 * ---------------------------------------------------------------------- */

esp_err_t wx_fetch_current(const wx_cfg_t *cfg, wx_current_t *out,
                           const char **source_note)
{
	if (!cfg || !out) {
		return ESP_ERR_INVALID_ARG;
	}

	/* Absent, not zero. */
	memset(out, 0, sizeof *out);
	out->temp_f = out->feels_f = out->humidity_pct = out->dew_f = wx_none();
	out->wind_mph = out->gust_mph = out->wind_deg = wx_none();
	out->pressure_inhg = out->rain_rate_inhr = out->rain_today_in = wx_none();
	out->cloud_pct = out->uv = out->precip_prob_pct = wx_none();
	out->weather_code = -1;
	out->is_day = true;   /* without the API we cannot know; a lit screen reads */

	/* One allocation, reused by all three tiers in sequence -- they never
	 * overlap. Sized to OM_CAP because that is the larger of the two caps;
	 * the Weathercloud reads still stop themselves at WC_CAP. Heap rather
	 * than stack: 9 KB does not fit in a FreeRTOS task stack. */
	char *buf = malloc(OM_CAP + URL_CAP);
	if (!buf) {
		ESP_LOGE(TAG, "out of memory for %d byte fetch buffer", OM_CAP + URL_CAP);
		return ESP_ERR_NO_MEM;
	}
	char *url = buf + OM_CAP;

	/* An empty pws_id means the user has no station; do not spend two TLS
	 * handshakes discovering that every ten minutes. */
	const char *note = NULL;
	if (cfg->pws_id[0] != '\0') {
		if (pws_v0(cfg->pws_id, buf, url, out)) {
			note = "PWS";
		} else if (pws_xhr(cfg->pws_id, buf, url, out)) {
			note = "PWS/ALT";
		} else {
			ESP_LOGW(TAG, "both PWS tiers failed for id %s", cfg->pws_id);
		}
	}

	/* The API runs regardless of which tier won: it is the only source for
	 * cloud cover, UV, precipitation odds, the WMO code and day/night, none of
	 * which a backyard station measures. */
	cJSON *root = NULL;
	long utc_off = 0;
	wx_current_t api;
	bool have_api = false;

	memset(&api, 0, sizeof api);
	api.weather_code = -1;
	api.is_day = true;

	if (om_fetch(cfg, WX_FORECAST_DAYS, buf, url, &root, &utc_off) == ESP_OK) {
		om_current(root, utc_off, &api);
		cJSON_Delete(root);
		have_api = api.temp_f.valid || api.weather_code >= 0;
	}

	free(buf);

	if (!note && !have_api) {
		if (source_note) {
			*source_note = "NONE";
		}
		return ESP_FAIL;
	}

	if (have_api) {
		/* API-ONLY fields: the station never contests these. */
		out->cloud_pct       = api.cloud_pct;
		out->uv              = api.uv;
		out->precip_prob_pct = api.precip_prob_pct;
		out->weather_code    = api.weather_code;
		out->is_day          = api.is_day;

		/* PWS wins where it measured; the API fills the gaps. Field by field,
		 * because which fields a station reports varies by model and by
		 * whether a given sensor has a dead battery this week. */
		if (!out->temp_f.valid)         out->temp_f         = api.temp_f;
		if (!out->feels_f.valid)        out->feels_f        = api.feels_f;
		if (!out->humidity_pct.valid)   out->humidity_pct   = api.humidity_pct;
		if (!out->dew_f.valid)          out->dew_f          = api.dew_f;
		if (!out->wind_mph.valid)       out->wind_mph       = api.wind_mph;
		if (!out->gust_mph.valid)       out->gust_mph       = api.gust_mph;
		if (!out->wind_deg.valid)       out->wind_deg       = api.wind_deg;
		if (!out->pressure_inhg.valid)  out->pressure_inhg  = api.pressure_inhg;
		if (!out->rain_rate_inhr.valid) out->rain_rate_inhr = api.rain_rate_inhr;
		if (!out->rain_today_in.valid)  out->rain_today_in  = api.rain_today_in;

		/* Only take the model's timestamp when no station reading anchors it;
		 * the station's own ts is what makes a stale station visible. */
		if (!note) {
			out->observed = api.observed;
		}
	}

	/* Literal, never heap: wx_cfg.c stores this pointer past our return. */
	if (source_note) {
		*source_note = note ? note : "OPEN-METEO";
	}
	return ESP_OK;
}

/* -------------------------------------------------------------------------
 * Public: forecast
 * ---------------------------------------------------------------------- */

esp_err_t wx_fetch_forecast(const wx_cfg_t *cfg, wx_day_t *days, int n_days,
                            time_t *sunrise, time_t *sunset)
{
	if (!cfg || !days || n_days <= 0) {
		return ESP_ERR_INVALID_ARG;
	}
	if (n_days > WX_FORECAST_DAYS) {
		n_days = WX_FORECAST_DAYS;
	}

	memset(days, 0, sizeof(wx_day_t) * (size_t)n_days);

	char *buf = malloc(OM_CAP + URL_CAP);
	if (!buf) {
		ESP_LOGE(TAG, "out of memory for %d byte fetch buffer", OM_CAP + URL_CAP);
		return ESP_ERR_NO_MEM;
	}
	char *url = buf + OM_CAP;

	cJSON *root = NULL;
	long utc_off = 0;
	esp_err_t err = om_fetch(cfg, n_days, buf, url, &root, &utc_off);
	if (err != ESP_OK) {
		free(buf);
		return err;
	}

	const cJSON *d = cJSON_GetObjectItemCaseSensitive(root, "daily");
	if (!cJSON_IsObject(d)) {
		cJSON_Delete(root);
		free(buf);
		return ESP_FAIL;
	}

	int good = 0;
	for (int i = 0; i < n_days; i++) {
		wx_day_t *o = &days[i];
		double v;

		/* Dates come back as local "YYYY-MM-DD"; subtracting the reported
		 * offset gives the epoch of local midnight without asking libc, whose
		 * TZ may not have been set yet on the first cycle after boot. */
		if (!iso_to_epoch(jarr_str(d, "time", i), utc_off, &o->date)) {
			continue;
		}

		o->weather_code = jarr_num(d, "weather_code", i, &v) ? (int)v : -1;

		/* ALREADY IMPERIAL -- see om_current(). No conversion below. */
		if (!jarr_num(d, "temperature_2m_max", i, &v)) continue;
		o->hi_f = (float)v;
		if (!jarr_num(d, "temperature_2m_min", i, &v)) continue;
		o->lo_f = (float)v;

		o->precip_prob_pct = jarr_num(d, "precipitation_probability_max", i, &v) ? (float)v : 0.0f;
		o->precip_sum_in   = jarr_num(d, "precipitation_sum", i, &v)             ? (float)v : 0.0f;
		o->wind_max_mph    = jarr_num(d, "wind_speed_10m_max", i, &v)            ? (float)v : 0.0f;
		o->uv_max          = jarr_num(d, "uv_index_max", i, &v)                  ? (float)v : 0.0f;

		o->valid = true;
		good++;
	}

	/* Today's, index 0. Also local wall-clock strings. */
	if (sunrise) {
		iso_to_epoch(jarr_str(d, "sunrise", 0), utc_off, sunrise);
	}
	if (sunset) {
		iso_to_epoch(jarr_str(d, "sunset", 0), utc_off, sunset);
	}

	cJSON_Delete(root);
	free(buf);
	return good > 0 ? ESP_OK : ESP_FAIL;
}

/* -------------------------------------------------------------------------
 * Public: geocoding
 * ---------------------------------------------------------------------- */

esp_err_t wx_geocode(const char *query, float *lat, float *lon,
                     char *place_out, size_t place_n)
{
	if (!query || !query[0] || !lat || !lon) {
		return ESP_ERR_INVALID_ARG;
	}

	char *buf = malloc(OM_CAP + URL_CAP);
	if (!buf) {
		return ESP_ERR_NO_MEM;
	}
	char *url = buf + OM_CAP;

	/* Percent-encode: a space or an accented character in the raw query
	 * produces a malformed request line, not a 400. */
	/* Percent-encoding expands up to 3x. The caller (wx_portal.c) passes a
	 * 96-byte query, so 160 silently dropped everything past ~52 source
	 * characters -- and a real place name like "SAINT-DENIS, ILE-DE-FRANCE"
	 * is mostly escapes. The user would have seen a WRONG match, with no
	 * error anywhere. 3*96+1 cannot truncate. */
	char enc[3 * 96 + 1];
	url_encode(query, enc, sizeof enc);
	snprintf(url, URL_CAP,
	         "https://geocoding-api.open-meteo.com/v1/search"
	         "?name=%s&count=1&language=en&format=json", enc);

	int n = http_get(url, NULL, NULL, buf, OM_CAP);
	if (n <= 0) {
		free(buf);
		return ESP_FAIL;
	}

	cJSON *root = cJSON_Parse(buf);
	free(buf);
	if (!root) {
		return ESP_FAIL;
	}

	esp_err_t err = ESP_ERR_NOT_FOUND;

	/* A query that matches nothing omits "results" ENTIRELY rather than
	 * returning an empty array, so an absent key is the no-match case, not a
	 * malformed response. */
	const cJSON *r0 = cJSON_GetArrayItem(
		cJSON_GetObjectItemCaseSensitive(root, "results"), 0);
	if (!cJSON_IsObject(r0)) {
		goto done;
	}

	double la, lo;
	if (!jnum(r0, "latitude", &la) || !jnum(r0, "longitude", &lo)) {
		err = ESP_FAIL;
		goto done;
	}
	*lat = (float)la;
	*lon = (float)lo;

	if (place_out && place_n) {
		const char *name = jstr(r0, "name");
		const char *adm  = jstr(r0, "admin1");
		if (adm && adm[0]) {
			snprintf(place_out, place_n, "%s, %s", name ? name : "", adm);
		} else {
			snprintf(place_out, place_n, "%s", name ? name : "");
		}
		/* The Pip-Boy face is uppercase-only. Bytes >= 0x80 are left alone so
		 * a UTF-8 name survives instead of being cut in half. */
		for (char *p = place_out; *p; p++) {
			*p = (char)toupper((unsigned char)*p);
		}
	}
	err = ESP_OK;

done:
	cJSON_Delete(root);
	return err;
}

/* -------------------------------------------------------------------------
 * WMO 4677 codes
 *
 * The subset Open-Meteo actually emits, transcribed from its own published
 * interpretation table (open-meteo.com/en/docs, "WMO Weather interpretation
 * codes (WW)"), read while this was written. Codes it never emits are not
 * here, and an unrecognised code renders as UNKNOWN rather than as clear sky.
 *
 * ONE TABLE, three accessors: the header's stated reason for routing both the
 * icon and the label through this file is that they can then never disagree.
 * Labels are budgeted at 16 characters for the fixed-width face.
 * ---------------------------------------------------------------------- */
static const struct {
	int16_t   code;
	uint8_t   icon;
	bool      severe;
	const char *text;
} WMO[] = {
	{  0, WX_ICON_CLEAR,   false, "CLEAR"           },
	{  1, WX_ICON_CLEAR,   false, "MOSTLY CLEAR"    },
	{  2, WX_ICON_PARTLY,  false, "PARTLY CLOUDY"   },
	{  3, WX_ICON_CLOUDY,  false, "OVERCAST"        },
	{ 45, WX_ICON_FOG,     false, "FOG"             },
	{ 48, WX_ICON_FOG,     false, "RIME FOG"        },
	{ 51, WX_ICON_DRIZZLE, false, "LIGHT DRIZZLE"   },
	{ 53, WX_ICON_DRIZZLE, false, "DRIZZLE"         },
	{ 55, WX_ICON_DRIZZLE, false, "HEAVY DRIZZLE"   },
	{ 56, WX_ICON_DRIZZLE, false, "FRZ DRIZZLE"     },
	{ 57, WX_ICON_DRIZZLE, false, "FRZ DRIZZLE HVY" },
	{ 61, WX_ICON_RAIN,    false, "LIGHT RAIN"      },
	{ 63, WX_ICON_RAIN,    false, "RAIN"            },
	{ 65, WX_ICON_RAIN,    false, "HEAVY RAIN"      },
	{ 66, WX_ICON_RAIN,    false, "FRZ RAIN"        },
	{ 67, WX_ICON_RAIN,    false, "FRZ RAIN HVY"    },
	{ 71, WX_ICON_SNOW,    false, "LIGHT SNOW"      },
	{ 73, WX_ICON_SNOW,    false, "SNOW"            },
	{ 75, WX_ICON_SNOW,    true,  "HEAVY SNOW"      },
	{ 77, WX_ICON_SNOW,    false, "SNOW GRAINS"     },
	{ 80, WX_ICON_RAIN,    false, "LIGHT SHOWERS"   },
	{ 81, WX_ICON_RAIN,    false, "SHOWERS"         },
	{ 82, WX_ICON_RAIN,    true,  "VIOLENT SHOWERS" },
	{ 85, WX_ICON_SNOW,    false, "SNOW SHOWERS"    },
	{ 86, WX_ICON_SNOW,    false, "HEAVY SNOW SHWR" },
	{ 95, WX_ICON_STORM,   true,  "THUNDERSTORM"    },
	{ 96, WX_ICON_STORM,   true,  "STORM + HAIL"    },
	{ 99, WX_ICON_STORM,   true,  "STORM + LG HAIL" },
};

static int wmo_index(int code)
{
	for (size_t i = 0; i < sizeof WMO / sizeof WMO[0]; i++) {
		if (WMO[i].code == code) {
			return (int)i;
		}
	}
	return -1;
}

wx_icon_t wx_code_to_icon(int wmo_code)
{
	int i = wmo_index(wmo_code);
	return i < 0 ? WX_ICON_UNKNOWN : (wx_icon_t)WMO[i].icon;
}

const char *wx_code_to_text(int wmo_code)
{
	int i = wmo_index(wmo_code);
	return i < 0 ? "UNKNOWN" : WMO[i].text;
}

bool wx_code_is_severe(int wmo_code)
{
	int i = wmo_index(wmo_code);
	return i < 0 ? false : WMO[i].severe;
}
