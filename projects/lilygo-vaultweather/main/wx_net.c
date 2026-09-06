/*
 * wx_net.c -- Wi-Fi station, auto-reconnect, and SNTP.
 *
 * This is a desk clock. It is expected to sit on a shelf for months with
 * nobody watching it, so the interesting code in here is not the happy-path
 * join -- it is what happens at 03:00 when the router reboots for a firmware
 * update and takes twenty minutes to come back. Everything below is written
 * for that case: bounded retries with a capped backoff, no unbounded spin, no
 * log flood, and no state that only gets fixed by a human pressing reset.
 *
 * COEXISTENCE WITH THE SETUP PORTAL
 * wx_portal.c brings up a SoftAP from the same chip and the same TCP/IP stack.
 * esp_netif_init(), esp_event_loop_create_default() and esp_wifi_init() are
 * all one-shot: the second caller gets ESP_ERR_INVALID_STATE (or
 * ESP_ERR_WIFI_INIT_STATE) rather than a no-op. Neither module can assume it
 * runs first, so each of those calls is guarded here AND its
 * already-initialised error is treated as success. See stack_init_once().
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>

#include <esp_check.h>
#include <esp_err.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_netif_sntp.h>
#include <esp_timer.h>
#include <esp_wifi.h>

#include "vaultweather.h"

#define TAG "wx_net"

/* Retry ladder for the unattended case. Doubling forever would put the retry
 * interval into the hours after a night of downtime and the clock would stay
 * dark long after the AP came back; holding at 60s costs one association
 * attempt a minute, which is nothing. */
static const uint32_t s_backoff_ms[] = { 1000, 2000, 5000, 15000, 60000 };
#define WX_BACKOFF_STEPS (sizeof(s_backoff_ms) / sizeof(s_backoff_ms[0]))

/* Failed associations tolerated while wx_net_connect() is still blocking.
 * The point is to fail fast on a wrong password (which disconnects almost
 * immediately, over and over) instead of burning the caller's whole timeout
 * on something that will never succeed. */
#define WX_JOIN_MAX_FAIL 5

/* Retry interval during the join phase. Deliberately NOT the ladder above:
 * during the join a human is watching the splash screen, so retry promptly and
 * give up quickly. The patient ladder is for after we have handed control
 * back. */
#define WX_JOIN_RETRY_MS 1000

#define WX_BIT_GOT_IP  BIT0
#define WX_BIT_FAILED  BIT1

static EventGroupHandle_t   s_events;
static esp_netif_t         *s_sta_netif;
static esp_timer_handle_t   s_retry_timer;

static bool s_stack_ready;      /* esp_netif + default event loop done */
static bool s_wifi_ready;       /* esp_wifi_init + handlers done */
static bool s_wifi_started;
static bool s_sntp_ready;       /* esp_netif_sntp_init done; it is one-shot too */

/* Set by SNTP's own sync callback. volatile: written on the SNTP task, read on
 * whichever task called wx_time_sync().
 *
 * This exists because "is the year > 2024" STOPPED being a valid proxy for
 * "SNTP has synced" the moment a battery-backed RTC could set the clock before
 * the network came up. The symptom was subtle and real: wx_time_sync() returned
 * ESP_OK on its first check, logged "clock set" for a time SNTP had never
 * supplied, and the caller then wrote the RTC back with the RTC's own value --
 * a no-op. SNTP corrected the SYSTEM clock a second or two later, and the RTC
 * was left behind by exactly that much. It showed up as a -2s delta on the
 * console's clock command where every earlier reading had been +0s.
 *
 * sntp_get_sync_status() is NOT used for this: it self-clears after one read
 * ("After the update is completed... After that, the status will be reset to
 * SNTP_SYNC_STATUS_RESET"), so polling it races anything else that looks. */
static volatile bool s_sntp_synced;

static void sntp_synced_cb(struct timeval *tv)
{
	(void)tv;
	s_sntp_synced = true;
}

static volatile bool s_have_ip;
static bool s_joining;          /* true only while wx_net_connect() blocks */
static int  s_join_fails;
static int  s_backoff_idx;
static bool s_hold_logged;      /* so the 60s hold is announced once, not forever */

/* -------------------------------------------------------------------------
 * Reconnect scheduling
 * ---------------------------------------------------------------------- */

/* esp_wifi_connect() is called from here rather than straight out of the event
 * handler because the retry has to be *delayed*, and sleeping inside the
 * default event loop task would stall every other event in the system --
 * including the IP_EVENT we are waiting for. esp_timer's dispatch task is a
 * normal task context, so calling into the Wi-Fi driver from it is fine. */
static void retry_timer_cb(void *arg)
{
	(void)arg;
	esp_err_t err = esp_wifi_connect();
	if (err == ESP_ERR_WIFI_NOT_INIT || err == ESP_ERR_WIFI_NOT_STARTED) {
		/* Ordering bug on our side, not a network condition. */
		ESP_LOGE(TAG, "esp_wifi_connect: %s", esp_err_to_name(err));
	} else if (err != ESP_OK) {
		/* Anything else here is a transient the driver will re-report
		 * as a WIFI_EVENT_STA_DISCONNECTED with a real reason code, and
		 * that handler already logs. Warning here too would double
		 * every line of an outage. */
		ESP_LOGD(TAG, "esp_wifi_connect: %s", esp_err_to_name(err));
	}
}

static void schedule_retry(uint32_t delay_ms)
{
	if (s_retry_timer == NULL) {
		return;
	}
	/* Idle timer returns ESP_ERR_INVALID_STATE here; that is the normal
	 * case, not a fault. Stopping first makes start_once unconditional. */
	esp_timer_stop(s_retry_timer);

	esp_err_t err = esp_timer_start_once(s_retry_timer,
			(uint64_t)delay_ms * 1000ULL);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "retry timer: %s", esp_err_to_name(err));
	}
}

/* -------------------------------------------------------------------------
 * Events
 * ---------------------------------------------------------------------- */

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id,
		void *data)
{
	(void)arg;
	(void)base;

	switch (id) {
	case WIFI_EVENT_STA_START: {
		/* The driver does not auto-connect; this is the first attempt.
		 * If it fails outright there is no DISCONNECTED event to fall
		 * back on, so arm the retry here or the station sits idle
		 * forever. */
		esp_err_t err = esp_wifi_connect();
		if (err != ESP_OK) {
			ESP_LOGW(TAG, "initial connect: %s",
					esp_err_to_name(err));
			schedule_retry(s_backoff_ms[0]);
		}
		break;
	}

	case WIFI_EVENT_STA_CONNECTED:
		/* Associated, but no IP yet. wx_net_is_up() stays false until
		 * DHCP finishes -- an associated station with no address
		 * cannot reach Open-Meteo, and reporting "up" here would send
		 * the fetcher off to fail on a name lookup. */
		ESP_LOGI(TAG, "associated, waiting for DHCP");
		break;

	case WIFI_EVENT_STA_DISCONNECTED: {
		const wifi_event_sta_disconnected_t *d =
			(const wifi_event_sta_disconnected_t *)data;
		unsigned reason = d ? d->reason : 0;

		bool was_up = s_have_ip;
		s_have_ip = false;

		if (s_joining) {
			s_join_fails++;
			if (s_join_fails >= WX_JOIN_MAX_FAIL) {
				ESP_LOGW(TAG, "join failed %d times (reason %u), giving up on the blocking wait",
						s_join_fails, reason);
				/* Hand control back to the caller, but keep
				 * trying in the background -- the AP may simply
				 * not be powered up yet. */
				xEventGroupSetBits(s_events, WX_BIT_FAILED);
				schedule_retry(s_backoff_ms[0]);
				break;
			}
			ESP_LOGW(TAG, "join attempt %d failed (reason %u), retrying",
					s_join_fails, reason);
			schedule_retry(WX_JOIN_RETRY_MS);
			break;
		}

		uint32_t delay = s_backoff_ms[s_backoff_idx];

		if (was_up) {
			/* The transition that matters: we HAD a working link
			 * and lost it. */
			ESP_LOGW(TAG, "link lost (reason %u), reconnecting in %ums",
					reason, (unsigned)delay);
		} else if (s_backoff_idx < (int)WX_BACKOFF_STEPS - 1) {
			ESP_LOGI(TAG, "reconnect failed (reason %u), next attempt in %ums",
					reason, (unsigned)delay);
		} else if (!s_hold_logged) {
			ESP_LOGW(TAG, "AP unreachable (reason %u), holding at %ums between attempts",
					reason, (unsigned)delay);
			s_hold_logged = true;
		} else {
			/* Steady-state outage. One line a minute for a week is
			 * 10k lines of nothing; keep it at DEBUG. */
			ESP_LOGD(TAG, "still down (reason %u)", reason);
		}

		schedule_retry(delay);
		if (s_backoff_idx < (int)WX_BACKOFF_STEPS - 1) {
			s_backoff_idx++;
		}
		break;
	}

	default:
		break;
	}
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id,
		void *data)
{
	(void)arg;
	(void)base;

	switch (id) {
	case IP_EVENT_STA_GOT_IP: {
		const ip_event_got_ip_t *e = (const ip_event_got_ip_t *)data;

		s_have_ip = true;
		s_join_fails = 0;
		s_backoff_idx = 0;
		s_hold_logged = false;
		if (s_retry_timer) {
			esp_timer_stop(s_retry_timer);
		}

		if (e) {
			ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&e->ip_info.ip));
		}
		xEventGroupSetBits(s_events, WX_BIT_GOT_IP);
		break;
	}

	case IP_EVENT_STA_LOST_IP:
		/* Lease expiry without a disassociation. Rare, but it leaves an
		 * associated station with no route, which looks "up" to every
		 * check except this one. */
		s_have_ip = false;
		ESP_LOGW(TAG, "lost ip lease");
		break;

	default:
		break;
	}
}

/* -------------------------------------------------------------------------
 * One-shot initialisation
 * ---------------------------------------------------------------------- */

/* Both of these are process-wide singletons shared with wx_portal.c.
 * ESP_ERR_INVALID_STATE from either means "the other module already did it",
 * which is exactly the outcome we want, so it is folded into success. The
 * static guard covers the case where WE are first; the error check covers the
 * case where the portal was. Either module may run first. */
static esp_err_t stack_init_once(void)
{
	if (s_stack_ready) {
		return ESP_OK;
	}

	esp_err_t err = esp_netif_init();
	if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
		ESP_LOGE(TAG, "esp_netif_init: %s", esp_err_to_name(err));
		return err;
	}

	err = esp_event_loop_create_default();
	if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
		ESP_LOGE(TAG, "esp_event_loop_create_default: %s",
				esp_err_to_name(err));
		return err;
	}

	s_stack_ready = true;
	return ESP_OK;
}

static esp_err_t wifi_init_once(void)
{
	if (s_wifi_ready) {
		return ESP_OK;
	}

	/* Guarded, like every allocation below it: a later step in this function
	 * can fail and leave s_wifi_ready false, and the caller is entitled to
	 * try again rather than leak one of these per attempt. */
	if (s_events == NULL) {
		s_events = xEventGroupCreate();
		ESP_RETURN_ON_FALSE(s_events != NULL, ESP_ERR_NO_MEM, TAG,
				"event group alloc failed");
	}

	if (s_sta_netif == NULL) {
		s_sta_netif = esp_netif_create_default_wifi_sta();
		ESP_RETURN_ON_FALSE(s_sta_netif != NULL, ESP_FAIL, TAG,
				"sta netif create failed");
	}

	wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
	esp_err_t err = esp_wifi_init(&ic);
	/* Same coexistence rule as the netif/event-loop singletons: the portal
	 * may already have installed the driver. */
	if (err != ESP_OK && err != ESP_ERR_WIFI_INIT_STATE) {
		ESP_LOGE(TAG, "esp_wifi_init: %s", esp_err_to_name(err));
		return err;
	}

	/* Config lives in our own NVS namespace (wx_cfg.c). Letting the Wi-Fi
	 * driver keep a second copy in its own NVS partition means a flash
	 * write on every join -- pointless wear on a device that re-joins after
	 * every router hiccup, and a second source of truth to disagree with
	 * ours. */
	ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG,
			"esp_wifi_set_storage");

	ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT,
			ESP_EVENT_ANY_ID, on_wifi_event, NULL, NULL), TAG,
			"wifi event register");
	ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT,
			ESP_EVENT_ANY_ID, on_ip_event, NULL, NULL), TAG,
			"ip event register");

	if (s_retry_timer == NULL) {
		const esp_timer_create_args_t targs = {
			.callback = retry_timer_cb,
			.arg = NULL,
			/* ESP_TIMER_TASK, not ISR dispatch: the callback calls
			 * into the Wi-Fi driver, which is not ISR-safe. */
			.dispatch_method = ESP_TIMER_TASK,
			.name = "wx_reconnect",
			.skip_unhandled_events = false,
		};
		ESP_RETURN_ON_ERROR(esp_timer_create(&targs, &s_retry_timer),
				TAG, "retry timer create");
	}

	s_wifi_ready = true;
	return ESP_OK;
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

esp_err_t wx_net_connect(const wx_cfg_t *cfg, int timeout_ms)
{
	ESP_RETURN_ON_FALSE(cfg != NULL && cfg->ssid[0] != '\0',
			ESP_ERR_INVALID_ARG, TAG, "no ssid configured");

	/* esp_wifi_init() needs NVS. wx_cfg_load() has necessarily run before
	 * this -- it produced the cfg we were handed -- so nvs_flash_init() has
	 * already happened. Doing it here as well would hide a caller that got
	 * the order wrong. */

	ESP_RETURN_ON_ERROR(stack_init_once(), TAG, "netif/event init");
	ESP_RETURN_ON_ERROR(wifi_init_once(), TAG, "wifi init");

	/* A second call means the config changed (the portal was re-run). Stop
	 * any pending retry so it cannot fire against the old credentials. */
	esp_timer_stop(s_retry_timer);

	wifi_config_t wc = { 0 };
	size_t ssid_len = strnlen(cfg->ssid, sizeof(wc.sta.ssid));
	size_t pass_len = strnlen(cfg->pass, sizeof(wc.sta.password));
	memcpy(wc.sta.ssid, cfg->ssid, ssid_len);
	memcpy(wc.sta.password, cfg->pass, pass_len);

	/* Scan every channel and pick the strongest match. A fast scan stops at
	 * the first beacon carrying the SSID, which on a mesh or an
	 * extender-equipped house is regularly the weakest radio in the
	 * building. This costs about a second at join time, once. */
	wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
	wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

	/* With a password set, the driver silently raises the auth threshold to
	 * WPA2 and will then refuse an open AP. The portal permits an empty
	 * password, so say explicitly which case we are in. */
	wc.sta.threshold.authmode = (pass_len > 0) ? WIFI_AUTH_WPA2_PSK
	                                           : WIFI_AUTH_OPEN;
	/* pmf_cfg.capable is documented as deprecated in IDF 6.0 -- PMF is
	 * negotiated automatically now -- so only `required` is meaningful, and
	 * requiring it would lock out WPA2-only APs. */
	wc.sta.pmf_cfg.required = false;

	/* A zeroed wifi_config_t leaves this at WPA3_SAE_PWE_UNSPECIFIED(0),
	 * which is NOT the documented default of 2. On a router running
	 * WPA2/WPA3 transition mode -- now the shipping default on most
	 * consumer gear -- that can cost the SAE handshake. Set it explicitly. */
	wc.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

	ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG,
			"esp_wifi_set_mode");
	ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wc), TAG,
			"esp_wifi_set_config");

	s_have_ip = false;
	s_join_fails = 0;
	s_backoff_idx = 0;
	s_hold_logged = false;
	s_joining = true;
	xEventGroupClearBits(s_events, WX_BIT_GOT_IP | WX_BIT_FAILED);

	ESP_LOGI(TAG, "joining \"%s\" (timeout %dms)", cfg->ssid, timeout_ms);

	if (!s_wifi_started) {
		ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "esp_wifi_start");
		s_wifi_started = true;
		/* WIFI_EVENT_STA_START drives the first esp_wifi_connect(). */
	} else {
		esp_err_t err = esp_wifi_connect();
		if (err != ESP_OK) {
			ESP_LOGW(TAG, "esp_wifi_connect: %s",
					esp_err_to_name(err));
		}
	}

	/* A non-positive timeout means "kick it off, do not block" rather than
	 * "block forever". An unattended device that parks in an unbounded wait
	 * on a boot path is the one failure this module is here to prevent, so
	 * there is no code path through here that can never return. */
	EventBits_t bits = 0;
	if (timeout_ms > 0) {
		bits = xEventGroupWaitBits(s_events,
				WX_BIT_GOT_IP | WX_BIT_FAILED,
				pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
	}

	s_joining = false;

	if (bits & WX_BIT_GOT_IP) {
		char ip[16];
		wx_net_ip_str(ip, sizeof(ip));
		ESP_LOGI(TAG, "online at %s, rssi %d dBm", ip, wx_net_rssi());
		return ESP_OK;
	}

	/* Failed, timed out, or never waited. In every case the background
	 * reconnect stays armed: an AP that is merely slow to boot (a
	 * whole-house UPS brings the clock up before the router) must not need
	 * a human to press reset. main() polls wx_net_is_up(). */
	if (bits & WX_BIT_FAILED) {
		ESP_LOGW(TAG, "association failed; retrying in the background");
	} else {
		/* The join-phase handler may already have armed a retry; this
		 * covers the case where the attempt is simply still in flight. */
		schedule_retry(s_backoff_ms[0]);
		ESP_LOGW(TAG, "no ip yet; retrying in the background");
	}
	return ESP_ERR_TIMEOUT;
}

bool wx_net_is_up(void)
{
	return s_have_ip;
}

int wx_net_rssi(void)
{
	wifi_ap_record_t ap;

	if (!s_wifi_started) {
		return 0;
	}
	/* ESP_ERR_WIFI_NOT_CONNECT here is the ordinary "not associated" case,
	 * not a fault worth logging every second from the UI tick. */
	if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
		return 0;
	}
	return ap.rssi;
}

void wx_net_ip_str(char *buf, size_t n)
{
	esp_netif_ip_info_t info;

	if (buf == NULL || n == 0) {
		return;
	}

	/* Read the netif rather than caching the address from the GOT_IP
	 * event: after a DHCP renew onto a different subnet the cached string
	 * would be a plausible-looking lie on the status line. */
	if (s_sta_netif != NULL &&
	    esp_netif_get_ip_info(s_sta_netif, &info) == ESP_OK &&
	    info.ip.addr != 0) {
		snprintf(buf, n, IPSTR, IP2STR(&info.ip));
		return;
	}

	snprintf(buf, n, "0.0.0.0");
}

/* -------------------------------------------------------------------------
 * Time
 * ---------------------------------------------------------------------- */

/* The year test, not the sync_wait return, is what decides success.
 * esp_netif_sntp_sync_wait() only reports that the sync semaphore was given;
 * it says nothing about the value that landed in the clock. A device that
 * still reads 1970 will render a plausible-looking clock face and compute
 * sunrise/sunset off by half a century. */
static bool clock_is_plausible(void)
{
	time_t now = time(NULL);
	struct tm utc;

	/* gmtime_r, not localtime_r: this must not depend on whether
	 * wx_time_set_tz() has run yet. */
	gmtime_r(&now, &utc);
	return (utc.tm_year + 1900) > 2024;
}

/* The ESP32-S3 RTC keeps running across a soft reset (esp_restart, a watchdog
 * bite, a brownout that does not drop VDD), so the clock survives those. It
 * does NOT survive a power cut: RTC_SLOW_MEM and the RTC timer both lose state
 * when the rail collapses, and the chip comes back at the epoch. A desk clock
 * on a shelf gets unplugged, so every boot must re-sync rather than trusting
 * whatever time() reports. */
esp_err_t wx_time_sync(int timeout_ms)
{
	/* Already correct -- a re-sync on a running device has nothing to do.
	 * Requires a REAL sync, not merely a plausible clock: the RTC restore
	 * makes the clock plausible before SNTP has said anything. */
	if (s_sntp_synced && clock_is_plausible()) {
		return ESP_OK;
	}

	if (!s_sntp_ready) {
		/* One server: CONFIG_LWIP_SNTP_MAX_SERVERS defaults to 1 in
		 * IDF 6.0, and the servers[] array in esp_sntp_config_t is
		 * sized by it. Listing more without raising that Kconfig value
		 * overruns the array. pool.ntp.org is a rotating DNS pool, so
		 * one name is already several hosts. */
		esp_sntp_config_t cfg =
			ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");

		/* Set BEFORE init. esp_netif_sntp_init() installs its own
		 * notification callback unconditionally and chains to this one;
		 * calling sntp_set_time_sync_notification_cb() afterwards would
		 * overwrite the wrapper's and break its semaphore. */
		cfg.sync_cb = sntp_synced_cb;

		esp_err_t err = esp_netif_sntp_init(&cfg);
		if (err != ESP_OK) {
			ESP_LOGE(TAG, "esp_netif_sntp_init: %s",
					esp_err_to_name(err));
			return err;
		}
		s_sntp_ready = true;
		/* Left running on purpose, and it is safe to start it before
		 * the link is up. lwIP retries a failed request (including a
		 * failed DNS lookup) on SNTP_RETRY_TIMEOUT -- 15s, doubling to
		 * a 150s ceiling -- so an outage costs at most a couple of
		 * minutes of extra delay once the AP returns. The one-hour
		 * CONFIG_LWIP_SNTP_UPDATE_DELAY applies only AFTER a success,
		 * where it is exactly what we want: free drift correction over
		 * the months this thing stays powered. Tearing SNTP down after
		 * the first sync would trade that away for nothing. */
	}

	/* Poll in slices instead of one long wait so the year is re-checked
	 * even if a sync arrives with a bad value: a single blocking wait would
	 * return ESP_OK once and never look again.
	 *
	 * Budget the caller's timeout accordingly. lwIP holds the first request
	 * for a random interval up to CONFIG_LWIP_SNTP_MAXIMUM_STARTUP_DELAY
	 * (5000ms as configured), so anything under ~10s here will sometimes
	 * time out on a perfectly healthy network. */
	const int slice_ms = 2000;
	int waited = 0;

	for (;;) {
		/* BOTH conditions. The callback proves SNTP delivered something;
		 * the year test proves what it delivered is not garbage. Either
		 * alone has been wrong here -- the year test was satisfied by the
		 * RTC, and a sync event says nothing about the value. */
		if (s_sntp_synced && clock_is_plausible()) {
			time_t now = time(NULL);
			struct tm utc;
			char stamp[32];

			gmtime_r(&now, &utc);
			strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S",
					&utc);
			ESP_LOGI(TAG, "clock set: %s UTC", stamp);
			return ESP_OK;
		}

		if (waited >= timeout_ms) {
			break;
		}

		int this_slice = (timeout_ms - waited < slice_ms)
				? (timeout_ms - waited) : slice_ms;

		esp_err_t err = esp_netif_sntp_sync_wait(
				pdMS_TO_TICKS(this_slice));
		waited += this_slice;

		if (err == ESP_ERR_INVALID_STATE) {
			/* SNTP is not actually running; more waiting will not
			 * fix that. */
			ESP_LOGE(TAG, "sntp not running");
			return err;
		}
		/* ESP_ERR_TIMEOUT and ESP_ERR_NOT_FINISHED both mean "keep
		 * waiting"; the latter is the smooth-slew mode still slewing. */
	}

	ESP_LOGW(TAG, "no plausible time after %dms", timeout_ms);
	return ESP_ERR_TIMEOUT;
}

/* The TZ string is NOT hardcoded. It arrives in the Open-Meteo response
 * (`timezone_abbreviation` / the timezone the API resolved for the
 * coordinates), so moving the device to another state or country fixes itself
 * on the next fetch instead of needing a rebuild. wx_fetch.c passes whatever
 * came back straight through to here.
 *
 * THE SIGN TRAP: POSIX TZ offsets run OPPOSITE to the usual notation. The
 * offset in a TZ string is what you ADD to local time to get UTC, so US
 * Central -- UTC-6 -- is written "CST6CDT" with a POSITIVE 6, and China --
 * UTC+8 -- is written "CST-8" with a NEGATIVE 8. Copying a "UTC-6" from a
 * timezone table into this string produces a clock that is twelve hours wrong
 * and, worse, right twice a day.
 */
/* Pending-TZ hand-off, fetch task -> render task.
 *
 * tzset() rewrites newlib's global _timezone/_tzname in place, and the render
 * task calls localtime_r() once a second to draw the clock. Calling tzset()
 * from the fetch task could therefore be read mid-update and produce an
 * hour-wrong time. It changes twice a year, which makes it precisely the bug
 * that never reproduces on demand and gets dismissed as "someone misread it".
 *
 * A mutex is deliberately NOT used: it would have to be held around every
 * localtime_r() in the UI as well, which is a lock on the hot render path to
 * protect a semi-annual event. Storing the request and letting the render task
 * apply it costs a 40-byte buffer and removes the race entirely.
 *
 * The flag is written last on the producer side and cleared first on the
 * consumer side, so the worst interleaving loses a request rather than reading
 * a half-written string -- and the caller re-requests on the next fetch cycle
 * because it only suppresses duplicates by VALUE. */
static char          s_pending_tz[40];
static volatile bool s_pending_tz_set;

void wx_time_request_tz(const char *posix_tz)
{
	if (posix_tz == NULL || posix_tz[0] == '\0') {
		return;
	}
	strlcpy(s_pending_tz, posix_tz, sizeof(s_pending_tz));
	s_pending_tz_set = true;
}

bool wx_time_take_pending_tz(char *buf, size_t n)
{
	if (!s_pending_tz_set || buf == NULL || n == 0) {
		return false;
	}
	s_pending_tz_set = false;
	strlcpy(buf, s_pending_tz, n);
	return true;
}

void wx_time_set_tz(const char *posix_tz)
{
	if (posix_tz == NULL || posix_tz[0] == '\0') {
		return;
	}

	/* setenv copies the string into its own storage, so a caller may pass a
	 * stack buffer. tzset() is what actually reparses it; without that call
	 * localtime() keeps using the previous rules. */
	setenv("TZ", posix_tz, 1);
	tzset();

	ESP_LOGI(TAG, "tz set to %s", posix_tz);
}
