/*
 * vaultweather.h -- the contract between every module in this app.
 *
 * DESIGN RULE: UNITS ARE CONVERTED AT THE SOURCE BOUNDARY, ONCE.
 * Everything inside this app is imperial (F, mph, inHg, inches). Weathercloud
 * reports metric base units (C, m/s, hPa, mm) and Open-Meteo is *asked* for
 * imperial via query parameters. Both are normalised in wx_fetch.c before
 * anything else sees them. No function below this line ever has to ask "which
 * unit is this?" -- which is the class of bug that silently shows 27 degrees
 * in January.
 *
 * DESIGN RULE: EVERY READING CARRIES ITS PROVENANCE.
 * The user asked for a per-field merge -- their own weather station wins for
 * what it measures, the forecast API fills what it cannot. That means the same
 * struct field can come from either source depending on what was reachable
 * this cycle, so the source travels WITH the value rather than being inferred
 * from some global "mode" flag that would go stale.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "esp_err.h"
#include "lvgl.h"

/* -------------------------------------------------------------------------
 * Provenance
 * ---------------------------------------------------------------------- */

typedef enum {
	WX_SRC_NONE = 0,   /* no data -- not zero, not stale: absent */
	WX_SRC_PWS,        /* the user's own station, via Weathercloud */
	WX_SRC_API,        /* Open-Meteo model output */
} wx_source_t;

/* A reading, whether it is real, and where it came from.
 *
 * `valid == false` must never be rendered as 0. A barometer that reads 0.00
 * inHg looks like a measurement; a field showing "--" looks like what it is.
 */
typedef struct {
	float       v;
	bool        valid;
	wx_source_t src;
} wx_val_t;

static inline wx_val_t wx_val(float v, wx_source_t s)
{
	return (wx_val_t){ .v = v, .valid = true, .src = s };
}

static inline wx_val_t wx_none(void)
{
	return (wx_val_t){ .v = 0.0f, .valid = false, .src = WX_SRC_NONE };
}

/* -------------------------------------------------------------------------
 * Weather data
 * ---------------------------------------------------------------------- */

/* Current conditions, after the PWS/API merge.
 *
 * The fields marked API-ONLY are ones no consumer-grade personal weather
 * station measures. Cloud cover in particular requires either a ceilometer or
 * a satellite; a backyard station physically cannot report it, so it is always
 * WX_SRC_API when present at all.
 */
typedef struct {
	wx_val_t temp_f;
	wx_val_t feels_f;          /* heat index or wind chill, whichever applies */
	wx_val_t humidity_pct;
	wx_val_t dew_f;
	wx_val_t wind_mph;
	wx_val_t gust_mph;
	wx_val_t wind_deg;         /* meteorological: direction wind comes FROM */
	wx_val_t pressure_inhg;    /* sea-level corrected, NOT station pressure */
	wx_val_t rain_rate_inhr;
	wx_val_t rain_today_in;

	wx_val_t cloud_pct;        /* API-ONLY */
	wx_val_t uv;               /* API-ONLY unless the station has a solar sensor */
	wx_val_t precip_prob_pct;  /* API-ONLY -- a station measures rain, not odds */

	int      weather_code;     /* WMO code for icon selection; -1 unknown. API-ONLY */
	bool     is_day;           /* from the API's is_day flag; picks day/night icons */

	time_t   observed;         /* when the READING was taken, not when fetched.
	                            * Weathercloud returns its own `ts`; using fetch
	                            * time would hide a station that stopped
	                            * uploading hours ago behind a fresh timestamp. */
} wx_current_t;

typedef struct {
	time_t date;               /* local midnight of the forecast day */
	int    weather_code;       /* WMO */
	float  hi_f, lo_f;
	float  precip_prob_pct;
	float  precip_sum_in;
	float  wind_max_mph;
	float  uv_max;
	bool   valid;
} wx_day_t;

#define WX_FORECAST_DAYS 3

/* Everything the UI renders. One struct, owned by main, passed by const
 * pointer to the UI -- the UI never fetches and the fetcher never draws. */
typedef struct {
	wx_current_t cur;
	wx_day_t     day[WX_FORECAST_DAYS];

	time_t       sunrise;      /* today's, drives the brightness schedule */
	time_t       sunset;

	time_t       last_ok;      /* last successful sync from ANY source */
	time_t       last_pws_ok;  /* last time the station specifically answered */
	bool         have_current;
	bool         have_forecast;

	/* Which tier of the PWS fallback chain last succeeded, for the status
	 * line. See wx_fetch.c for why there are three. */
	const char  *cur_source_note;
} wx_state_t;

/* -------------------------------------------------------------------------
 * Persistent configuration (NVS)
 * ---------------------------------------------------------------------- */

typedef struct {
	char  ssid[33];        /* 32 + NUL, per 802.11 */
	char  pass[65];        /* 64 + NUL, WPA2 max */
	float lat, lon;
	char  place[48];       /* display name, e.g. "EVANSVILLE, IN" */
	char  pws_id[24];      /* Weathercloud device id; empty string = no PWS */
	bool  configured;      /* false until the portal has been completed once */
} wx_cfg_t;

esp_err_t wx_cfg_load(wx_cfg_t *out);
esp_err_t wx_cfg_save(const wx_cfg_t *cfg);
esp_err_t wx_cfg_erase(void);

/* Last-good weather survives a power cut, so the screen is never blank after
 * a reboot. Only the fields the UI needs are persisted; see wx_cfg.c. */
esp_err_t wx_cache_load(wx_state_t *out);
esp_err_t wx_cache_save(const wx_state_t *st);

/* -------------------------------------------------------------------------
 * Network
 * ---------------------------------------------------------------------- */

/* Bring up Wi-Fi in station mode and wait for an IP.
 * Returns ESP_OK on success, ESP_ERR_TIMEOUT if it could not associate. */
esp_err_t wx_net_connect(const wx_cfg_t *cfg, int timeout_ms);

bool wx_net_is_up(void);
int  wx_net_rssi(void);                 /* dBm, or 0 if not associated */
void wx_net_ip_str(char *buf, size_t n);

/* Start SNTP and block until the clock is plausibly set (year > 2024).
 * The TZ is derived from the API response, not hardcoded -- see wx_fetch.c. */
esp_err_t wx_time_sync(int timeout_ms);

/* Apply a POSIX TZ string. MUST be called only from the render task.
 *
 * newlib's tzset() rewrites global _timezone/_tzname in place. The render task
 * calls localtime_r() once a second; if the fetch task called tzset()
 * concurrently, localtime_r could read that state mid-update and render an
 * hour-wrong clock. So the fetch task does not call this -- it calls
 * wx_time_request_tz() and the render task drains it. */
void      wx_time_set_tz(const char *posix_tz);

/* Thread-safe hand-off, fetch task -> render task. request() only stores;
 * take_pending() returns true once per new value and copies it out. */
void wx_time_request_tz(const char *posix_tz);
bool wx_time_take_pending_tz(char *buf, size_t n);

/* -------------------------------------------------------------------------
 * Setup portal
 * ---------------------------------------------------------------------- */

/* Bring up SoftAP + captive DNS + HTTP server and BLOCK until the user has
 * submitted a valid configuration, which is then written to NVS.
 *
 * ORDERING CONSTRAINT: MUST NOT be called after wx_net_connect().
 * wx_net_connect() creates the WIFI_STA_DEF netif and never destroys it;
 * esp_netif_create_default_wifi_sta() asserts rather than returning an error
 * when that key already exists, so a second call aborts the firmware. This is
 * a real abort, not a graceful failure. main.c satisfies the constraint by
 * running the portal only before the first connect, and by rebooting rather
 * than re-entering the portal in place when the user holds the button -- that
 * reboot is a REQUIREMENT, not a convenience.
 *
 * BLOCKING: this call does not return until the user finishes on their phone,
 * which is human-scale time. The caller's task cannot render while it blocks,
 * so main.c runs it on a separate task and keeps pumping LVGL. Do not call it
 * inline from a task that owns the display.
 *
 * `on_progress` is called with short status strings so the caller can show
 * them on the panel while the user is on their phone. May be NULL. IT RUNS ON
 * THE PORTAL'S OWN TASK and must not touch LVGL -- store and forward.
 */
typedef void (*wx_portal_status_cb)(const char *msg);

esp_err_t wx_portal_run(wx_cfg_t *out, wx_portal_status_cb on_progress);

/* The SSID the board advertises while in setup mode. */
#define WX_PORTAL_SSID "VAULT-TEC-SETUP"

/* -------------------------------------------------------------------------
 * Data acquisition
 * ---------------------------------------------------------------------- */

/* Fetch and merge current conditions.
 *
 * Tries, in order:
 *   1. Weathercloud v0   (documented-ish, no key, may be withdrawn)
 *   2. Weathercloud XHR  (undocumented; fails on a DIFFERENT gate, so it is
 *                         genuinely independent of tier 1 rather than a retry)
 *   3. Open-Meteo current block
 * then fills any field the station cannot measure from the API regardless of
 * which tier won.
 *
 * Returns ESP_OK if ANY tier produced usable data.
 */
esp_err_t wx_fetch_current(const wx_cfg_t *cfg, wx_current_t *out,
                           const char **source_note);

/* Fetch the 3-day forecast plus today's sunrise/sunset. Open-Meteo only --
 * no personal weather station forecasts. */
esp_err_t wx_fetch_forecast(const wx_cfg_t *cfg, wx_day_t *days, int n_days,
                            time_t *sunrise, time_t *sunset);

/* Resolve a place name to coordinates, for the setup portal.
 * Uses Open-Meteo's geocoding endpoint (no key). */
esp_err_t wx_geocode(const char *query, float *lat, float *lon,
                     char *place_out, size_t place_n);

/* -------------------------------------------------------------------------
 * WMO weather codes
 * ---------------------------------------------------------------------- */

/* Open-Meteo encodes conditions as WMO 4677 codes. Both the icon chooser and
 * the text label go through here so they can never disagree. */
typedef enum {
	WX_ICON_CLEAR = 0,
	WX_ICON_PARTLY,
	WX_ICON_CLOUDY,
	WX_ICON_FOG,
	WX_ICON_DRIZZLE,
	WX_ICON_RAIN,
	WX_ICON_SNOW,
	WX_ICON_STORM,
	WX_ICON_COUNT,
	WX_ICON_UNKNOWN = WX_ICON_COUNT,
} wx_icon_t;

wx_icon_t   wx_code_to_icon(int wmo_code);
const char *wx_code_to_text(int wmo_code);   /* e.g. "PARTLY CLOUDY" */

/* Severe conditions get a full-screen animation rather than just an icon.
 * Returns true for thunderstorms, heavy snow and violent squalls. */
bool wx_code_is_severe(int wmo_code);

/* Animated icon frames, generated by tools/gen_wx_icons.py into wx_icons.c.
 *
 * These live in the CONTRACT rather than in wx_ui.c because they cross a
 * translation-unit boundary. Declared locally in the consumer, a change to the
 * generator's frame count would produce an extern whose type disagrees with
 * the definition -- which the linker accepts without complaint, and which the
 * consumer would then index out of bounds. The generator emits a
 * _Static_assert against WX_ICON_MAX_FRAMES so that mismatch is a build error.
 *
 * wx_icon_frame_count[] says how many of the MAX slots are real for each icon;
 * an icon that failed to generate has a count of 0, and handing LVGL a
 * zero-size descriptor is a crash, so never index without checking. */
#define WX_ICON_MAX_FRAMES 8

extern const lv_image_dsc_t wx_icon_frames[][WX_ICON_MAX_FRAMES];
extern const int            wx_icon_frame_count[];

/* Moon phases, 96x96, generated by tools/gen_wx_moon.py.
 *
 * At night a clear sky must not show a sun. The phase is computed locally --
 * no API supplies it and none needs to, since it is a function of the date.
 *
 * 48 STEPS, NOT 8. The first version had eight, and both the picture and the
 * name came from that one index. Eight buckets are 3.7 days wide, so a moon
 * 1.7 days past last quarter -- 32% lit and visibly a crescent -- was labelled
 * "LAST QTR". Picking a tile and naming a phase want different resolutions, so
 * they are now separate concerns: wx_moon_step() indexes the art, and
 * wx_moon_name() derives the caption from the continuous fraction.
 *
 * 48 steps are 0.62 days apart, finer than a 96x96 tile can render, and cost
 * 48 * 1160 = 55,680 bytes.
 *
 * NORTHERN HEMISPHERE ORIENTATION: waxing phases are lit on the RIGHT limb.
 * Wrong below the equator; a deliberate limitation for a device configured in
 * Evansville, Indiana, noted so it is a known bound rather than a latent bug. */
#define WX_MOON_STEPS 48

extern const lv_image_dsc_t wx_moon_frames[WX_MOON_STEPS];

/* A cloud with the moon behind it, for PARTLY at night. Phase-independent:
 * the cloud covers most of the disc, so 48 variants would be invisible work. */
extern const lv_image_dsc_t wx_moon_cloud_frame;

int   wx_moon_step(time_t t);    /* 0..WX_MOON_STEPS-1, index into the art */
float wx_moon_illum(time_t t);   /* 0..1, illuminated fraction of the disc */

/* Caption. The four INSTANT names (NEW MOON, FIRST QUARTER, FULL MOON, LAST
 * QUARTER) are held to a ~1.2-day window around the actual event; crescent and
 * gibbous cover the rest. Longest is "WAXING CRESCENT" at 15 characters. */
const char *wx_moon_name(time_t t);

/* Full-screen severe-weather animation, 536x240, generated by
 * tools/gen_wx_severe.py. Played when wx_code_is_severe() is true, at most
 * once per condition onset -- a full-screen takeover every 8-second panel
 * rotation during a thunderstorm would make the device unusable exactly when
 * you most want to read it. */
extern const lv_image_dsc_t wx_severe_frames[];
extern const int            wx_severe_frame_count;

/* -------------------------------------------------------------------------
 * UI
 * ---------------------------------------------------------------------- */

/* Pip-Boy phosphor, carried over from projects/lilygo-pipboy so the two apps
 * look like the same device. */
#define WX_GREEN   lv_color_hex(0x1CFF4A)
#define WX_DIM     lv_color_hex(0x0E7F25)
#define WX_AMBER   lv_color_hex(0xFFE8A0)
#define WX_BLACK   lv_color_hex(0x000000)

void wx_ui_init(lv_display_t *disp);

/* Push new data to the screen. Safe to call with a partially-filled state;
 * anything not valid renders as "--". */
void wx_ui_update(const wx_state_t *st);

/* Called once a second by main; drives the clock, the staleness indicator,
 * the panel rotation and the anti-burn-in pixel shift. */
void wx_ui_tick(void);

/* Boot / status text on the splash screen before data exists. */
void wx_ui_status(const char *msg);

void wx_ui_next_panel(void);

/* Which of the rotating panels is showing, 0-based.
 *
 * Exists so a screen capture can SAY which panel it caught. The host tool
 * first guessed from pixels -- "is there a big 7-segment temperature on the
 * left" -- and the text rows of another panel tripped it. The firmware knows
 * the answer exactly; there is no reason to infer it. */
int wx_ui_current_panel(void);

/* Force the clock to repaint on the next tick.
 *
 * The clock redraws only when the minute NUMBER changes, which is right for
 * burn-in and wrong exactly once: a whole-hour timezone shift leaves tm_min
 * identical, so applying the timezone at boot left the hour digits showing UTC
 * until the next minute rolled over -- up to a full minute of visibly wrong
 * time, caught in a screen capture. Call this after changing the timezone. */
void wx_ui_invalidate_clock(void);
void wx_ui_show_portal(const char *ssid, const char *url);

/* Panel brightness, 0-255. main drives this from sunrise/sunset.
 *
 * PREFER REAL PANEL BRIGHTNESS. The RM67162 has a WRDISBV command (0x51) that
 * actually reduces emission. The fallback here composites a black overlay,
 * which on an AMOLED is the wrong mechanism in every respect: every pixel
 * stays lit, the image only washes toward grey, it costs a full-screen alpha
 * blend on every redraw of an already DMA-bound panel, and it does nothing for
 * power or burn-in. main owns the panel IO handle, so it registers the real
 * one here and the overlay is only used if nothing was registered. */
typedef void (*wx_panel_bright_fn)(uint8_t level);
void wx_ui_set_panel_bright_cb(wx_panel_bright_fn fn);

void wx_ui_set_brightness(uint8_t level);

/* -------------------------------------------------------------------------
 * Button
 * ---------------------------------------------------------------------- */

/* GPIO0 -- verified in projects/lilygo-pipboy as the board's only user button
 * (active low against an internal pull-up). The other button on the board is
 * hardware RESET and is not readable by software.
 *
 * GPIO0 is also the BOOT strapping pin: holding it across a reset puts the
 * chip in download mode. That is a bootloader behaviour, not ours, and does
 * not affect runtime use.
 */
#define WX_BUTTON_GPIO 0

/* One button has to carry four actions, so they are separated by press
 * duration. SHORT stays instant -- it is the only one used daily and adding a
 * double-press window would have put a ~400ms lag on every panel advance to
 * serve a gesture used once a month.
 *
 *   25ms .. 1s    SHORT   next panel        (on release)
 *    1s  .. 3s    LONG    force a refresh   (on release)
 *    3s           SNAP    dump the screen   (fires while held)
 *    5s           HOLD    erase config      (fires while held)
 *
 * SNAP and HOLD fire while the button is still down, so the user gets feedback
 * at the moment the threshold passes rather than pressing a seemingly dead
 * button. Holding through to an erase also takes a screenshot on the way,
 * which is harmless. */
typedef enum {
	WX_BTN_NONE = 0,
	WX_BTN_SHORT,      /* next panel */
	WX_BTN_LONG,       /* force a refresh */
	WX_BTN_SNAP,       /* 3s -- dump the screen over serial */
	WX_BTN_HOLD,       /* 5s -- erase config and re-enter the setup portal */
} wx_btn_t;

void     wx_btn_init(void);
wx_btn_t wx_btn_poll(void);
