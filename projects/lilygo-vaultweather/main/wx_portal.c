/*
 * wx_portal.c -- first-run setup: SoftAP + captive DNS + HTTP form.
 *
 * The board becomes its own access point, a phone joins it, the phone's own
 * connectivity probe gets redirected to our form, the user types their home
 * Wi-Fi credentials and a location, and wx_portal_run() returns with NVS
 * already written. It exists for roughly a minute per board lifetime.
 *
 * DESIGN RULE: THE AP IS OPEN, ON PURPOSE.
 * A WPA2 password on a setup AP has to be printed on the device or typed from
 * a manual, and the user has to enter it before they can enter the real one.
 * It buys nothing here: the thing being protected is exactly one HTTP form
 * that hands out no secrets. The real consequence is stated plainly so nobody
 * mistakes this for an oversight -- ANYONE IN RADIO RANGE DURING THE SETUP
 * WINDOW CAN JOIN THE AP AND POST TO /save. The mitigation is the window, not
 * the crypto: the portal runs only when `configured` is false or after a 5s
 * button hold, and it tears the radio down the instant a config is accepted.
 *
 * DESIGN RULE: SPLIT ON RAW SEPARATORS, THEN DECODE.
 * A Wi-Fi passphrase may legally contain & = % and +. Every one of those is a
 * separator or an escape in application/x-www-form-urlencoded, so the field
 * splitter runs over the RAW body and only the extracted value is
 * percent-decoded. Decoding first turns a password's own %26 into a '&' and
 * silently cuts the field in half -- and the user never finds out, because the
 * board just fails to associate a week later.
 */

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "cJSON.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"

#include "lwip/sockets.h"

#include "vaultweather.h"

#define TAG "wx_portal"

/* Every bound in one place so they can be argued about as a set. */
#define PORTAL_POST_MAX   1024   /* the whole form is ~200 bytes; 1KB is slack */
#define PORTAL_SCAN_MAX   24     /* picklist entries after dedup */
#define PORTAL_DNS_PORT   53
#define PORTAL_DNS_BUF    512    /* RFC 1035 4.2.1 UDP message size limit */
#define PORTAL_RECV_TRIES 4      /* httpd recv_wait_timeout is 5s each */

/* form_field() return codes; >= 0 is the decoded byte count. */
#define FF_MISSING (-1)
#define FF_TOOLONG (-2)

typedef struct {
	char   ssid[33];
	int8_t rssi;
} portal_ap_t;

static httpd_handle_t       s_httpd;
static esp_netif_t         *s_ap_netif;
static esp_netif_t         *s_sta_netif;
static SemaphoreHandle_t    s_done;      /* given by /save once NVS is written */
static SemaphoreHandle_t    s_dns_gone;  /* given by the DNS task as it exits */
static TaskHandle_t         s_dns_task;
static volatile bool        s_dns_stop;
static wx_cfg_t             s_result;
static wx_portal_status_cb  s_cb;

/* httpd_resp_set_hdr() stores the POINTER, not a copy (verified in
 * esp_http_server/src/httpd_txrx.c), so anything handed to it must outlive the
 * response. File scope, not stack. */
static char     s_root_url[32];   /* "http://192.168.4.1/" */
static uint32_t s_ap_ip;          /* already in network byte order, from lwip */

static portal_ap_t s_scan[PORTAL_SCAN_MAX];
static int         s_scan_n;

/* -------------------------------------------------------------------------
 * The page
 *
 * One string, no external resources. At this point the board is an island: it
 * has no uplink, so a CDN font or a Google-hosted stylesheet is not "slow", it
 * is a blank page. Attribute quoting is dropped everywhere HTML5 permits it,
 * purely to keep the C escaping legible.
 * ---------------------------------------------------------------------- */

static const char PORTAL_HTML[] =
"<!DOCTYPE html><html><head><meta charset=utf-8>"
"<meta name=viewport content=\"width=device-width,initial-scale=1\">"
"<title>VAULT-TEC SETUP</title><style>"
"*{box-sizing:border-box}"
"body{background:#000;color:#1CFF4A;font-family:monospace;margin:0;padding:14px;font-size:16px}"
"h1{font-size:19px;letter-spacing:3px;border-bottom:2px solid #1CFF4A;padding-bottom:5px;margin:0}"
"p.s{color:#0E7F25;font-size:12px;letter-spacing:1px;margin:5px 0 16px}"
"label{display:block;margin:13px 0 3px;font-size:12px;letter-spacing:1px;color:#0E7F25}"
"input,select,button{width:100%;background:#000;color:#1CFF4A;border:1px solid #1CFF4A;"
"font-family:monospace;font-size:16px;padding:9px}"
"button{background:#1CFF4A;color:#000;font-weight:bold;letter-spacing:2px}"
".r{display:flex;gap:6px}.r input{flex:1}.r button{width:96px}"
"#st{color:#FFE8A0;font-size:12px;min-height:16px;margin:10px 0}"
"</style></head><body>"
"<h1>VAULT-TEC</h1><p class=s>WEATHER TERMINAL // INITIAL CONFIG</p>"
"<form method=post action=/save>"
"<label>DETECTED NETWORKS</label>"
"<select id=sel onchange=\"pick(this)\"><option>-- SCANNING --</option></select>"
"<label>SSID</label><input name=ssid id=ss maxlength=32 required autocapitalize=off>"
"<label>PASSPHRASE</label><input name=pass id=pw type=password maxlength=64>"
"<label>LOCATION (PLACE NAME, OR LAT,LON)</label>"
"<div class=r><input id=q placeholder=\"EVANSVILLE, IN\"><button type=button onclick=geo()>FIND</button></div>"
"<input type=hidden name=lat id=la><input type=hidden name=lon id=lo>"
"<input type=hidden name=place id=pl>"
"<label>WEATHERCLOUD DEVICE ID (OPTIONAL)</label>"
/* Deliberately NOT prefilled. This repository is public, and a hardcoded
 * device ID publishes a pointer to one specific person's weather station --
 * whose dashboard shows their location and live conditions at home. The
 * placeholder documents the format instead. Find yours in the URL of your
 * own station page: app.weathercloud.net/dNNNNNNNNNN */
"<input name=pws value='' placeholder='1234567890' maxlength=23 inputmode=numeric>"
"<div id=st></div><button type=submit>COMMIT TO NVS</button>"
"</form><script>"
"function g(i){return document.getElementById(i)}"
"var st=g('st');"
"function pick(s){if(s.selectedIndex>0)g('ss').value=s.options[s.selectedIndex].value}"
"fetch('/scan').then(r=>r.json()).then(a=>{var s=g('sel');"
"s.innerHTML='<option>-- SELECT --</option>';"
"a.forEach(n=>{var o=document.createElement('option');o.value=n.s;"
"o.text=n.s+'  ['+n.r+' dBm]';s.add(o)})})"
".catch(e=>{g('sel').innerHTML='<option>-- SCAN FAILED --</option>'});"
/* Raw "lat,lon" is accepted without a round trip because /geo needs an uplink
 * the board does not have yet -- see the note above h_geo(). */
"function geo(){var v=g('q').value.trim();var m=v.match(/^(-?\\d+\\.?\\d*)[ ,]+(-?\\d+\\.?\\d*)$/);"
"if(m){set(m[1],m[2],v.toUpperCase());return}"
"st.textContent='LOCATING...';"
"fetch('/geo?q='+encodeURIComponent(v)).then(r=>r.json()).then(d=>{"
"if(!d.ok){st.textContent='NO MATCH (NO UPLINK YET? ENTER LAT,LON)';return}"
"set(d.lat,d.lon,d.place)})"
".catch(e=>{st.textContent='LOOKUP FAILED -- ENTER LAT,LON'})}"
"function set(a,o,p){g('la').value=a;g('lo').value=o;g('pl').value=p;"
"st.textContent=p+'  '+a+', '+o}"
"</script></body></html>";

static const char OK_HTML[] =
"<!DOCTYPE html><html><head><meta charset=utf-8>"
"<meta name=viewport content=\"width=device-width,initial-scale=1\">"
"<title>SAVED</title></head>"
"<body style=\"background:#000;color:#1CFF4A;font-family:monospace;padding:20px\">"
"<h1 style=\"letter-spacing:3px\">CONFIG STORED</h1>"
"<p>VAULT-TEC-SETUP IS SHUTTING DOWN.</p>"
"<p style=\"color:#0E7F25\">THE TERMINAL WILL JOIN YOUR NETWORK NOW.<br>"
"THIS PHONE WILL DROP BACK TO ITS USUAL WI-FI.</p></body></html>";

/* `why` is always one of our own literals -- never echoed user input -- so
 * there is nothing here to HTML-escape. */
static const char ERR_HTML_FMT[] =
"<!DOCTYPE html><html><head><meta charset=utf-8>"
"<meta name=viewport content=\"width=device-width,initial-scale=1\">"
"<title>REJECTED</title></head>"
"<body style=\"background:#000;color:#1CFF4A;font-family:monospace;padding:20px\">"
"<h1 style=\"letter-spacing:3px;color:#FFE8A0\">REJECTED</h1>"
"<p>%s</p><p><a href=\"/\" style=\"color:#1CFF4A\">&lt;&lt; BACK</a></p></body></html>";

/* -------------------------------------------------------------------------
 * Small helpers
 * ---------------------------------------------------------------------- */

static void progress(const char *msg)
{
	ESP_LOGI(TAG, "%s", msg);
	if (s_cb) {
		s_cb(msg);
	}
}

/* For calls whose failure degrades the portal but does not sink it -- a missed
 * power-save tweak, a DHCP option the phone will survive without. The board
 * runs unattended for months, so nothing gets to fail silently, but not every
 * failure is worth aborting setup over. */
static void soft(esp_err_t err, const char *what)
{
	if (err != ESP_OK) {
		ESP_LOGW(TAG, "%s: %s", what, esp_err_to_name(err));
	}
}

static int hexv(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

/* Percent-decode `n` bytes of `src` into `dst`. Returns the decoded length, or
 * -1 if it would not fit -- the caller reports that rather than truncating,
 * because a silently shortened passphrase is a config that looks fine and
 * never associates. */
static int url_decode(const char *src, size_t n, char *dst, size_t dst_n)
{
	size_t o = 0;
	size_t i;

	if (dst_n == 0) {
		return -1;
	}
	for (i = 0; i < n; i++) {
		char c = src[i];

		if (c == '+') {
			/* x-www-form-urlencoded emits a bare '+' only where a space
			 * was; a literal '+' in the value arrives as %2B and is
			 * handled by the branch below. */
			c = ' ';
		} else if (c == '%' && i + 2 < n) {
			int hi = hexv(src[i + 1]);
			int lo = hexv(src[i + 2]);

			if (hi >= 0 && lo >= 0) {
				c = (char)((hi << 4) | lo);
				i += 2;
			}
		}
		if (o + 1 >= dst_n) {
			return -1;
		}
		dst[o++] = c;
	}
	dst[o] = '\0';
	return (int)o;
}

/* Find `key` in a form-urlencoded body and write its decoded value to `out`.
 * The scan runs over the raw bytes: '&' and '=' are located BEFORE any
 * decoding, so an encoded %26 or %3D inside a value cannot be mistaken for a
 * separator. See the DESIGN RULE at the top of this file. */
static int form_field(const char *body, size_t blen, const char *key,
                      char *out, size_t out_n)
{
	size_t klen = strlen(key);
	const char *p = body;
	const char *end = body + blen;

	if (out_n) {
		out[0] = '\0';
	}
	while (p < end) {
		const char *amp = memchr(p, '&', (size_t)(end - p));
		const char *pend = amp ? amp : end;
		const char *eq = memchr(p, '=', (size_t)(pend - p));

		if (eq && (size_t)(eq - p) == klen && memcmp(p, key, klen) == 0) {
			int n = url_decode(eq + 1, (size_t)(pend - eq - 1), out, out_n);
			return (n < 0) ? FF_TOOLONG : n;
		}
		if (!amp) {
			break;
		}
		p = amp + 1;
	}
	return FF_MISSING;
}

static bool parse_coord(const char *body, size_t blen, const char *key,
                        float lo, float hi, float *out)
{
	char txt[24];
	char *endp = txt;
	float v;

	if (form_field(body, blen, key, txt, sizeof(txt)) <= 0) {
		return false;
	}
	v = strtof(txt, &endp);
	if (endp == txt || v < lo || v > hi) {
		return false;
	}
	*out = v;
	return true;
}

/* -------------------------------------------------------------------------
 * Scan
 *
 * CAVEAT, and it is the reason this is cached rather than run per request:
 * esp_wifi_scan_start() does work in APSTA mode, but the radio has exactly one
 * channel. esp_wifi.h documents this under esp_wifi_set_config attention 3
 * ("ESP devices are limited to only one channel, so when in the soft-AP+
 * station mode, the soft-AP will adjust its channel automatically"), and
 * docs/en/api-guides/wifi-driver/overview.rst spells out the same home-channel
 * rule. An all-channel sweep therefore drags the SoftAP off its home channel
 * for the duration -- roughly 14 channels x 150ms -- and any phone already
 * associated sees missed beacons and stalled TCP for a second or two.
 *
 * So the scan is done ONCE, before the HTTP server is even listening and
 * before a phone has any reason to have joined. /scan serves the cache;
 * /scan?fresh=1 re-runs it and accepts the disruption, which is the right
 * trade for a user who is staring at an empty list anyway.
 * ---------------------------------------------------------------------- */

static void portal_scan(void)
{
	wifi_scan_config_t sc = {
		.show_hidden      = false,
		.scan_type        = WIFI_SCAN_TYPE_ACTIVE,
		.scan_time.active = { .min = 60, .max = 150 },
	};
	wifi_ap_record_t *recs;
	uint16_t want = PORTAL_SCAN_MAX * 2;   /* room for duplicate BSSIDs */
	esp_err_t err;

	s_scan_n = 0;

	err = esp_wifi_scan_start(&sc, true);
	if (err != ESP_OK) {
		ESP_LOGW(TAG, "scan_start: %s", esp_err_to_name(err));
		/* Best-effort on every error path below: the driver leaks its AP list
		 * if nobody claims it, and there is nothing useful to do if the
		 * cleanup itself fails. */
		(void)esp_wifi_clear_ap_list();
		return;
	}

	/* wifi_ap_record_t is a large struct (it carries wifi_country_t and an HE
	 * info block), so the raw list is heap and transient. What survives is
	 * portal_ap_t: 34 bytes, static, no allocation on the /scan path. */
	recs = calloc(want, sizeof(*recs));
	if (!recs) {
		(void)esp_wifi_clear_ap_list();
		return;
	}

	err = esp_wifi_scan_get_ap_records(&want, recs);
	if (err != ESP_OK) {
		ESP_LOGW(TAG, "scan_get_ap_records: %s", esp_err_to_name(err));
		(void)esp_wifi_clear_ap_list();
		free(recs);
		return;
	}
	/* esp_wifi_scan_get_ap_records() frees the driver's list itself -- calling
	 * esp_wifi_clear_ap_list() after a successful fetch would be a second
	 * free, so it only appears on the error paths above. */

	for (uint16_t i = 0; i < want && s_scan_n < PORTAL_SCAN_MAX; i++) {
		const char *ssid = (const char *)recs[i].ssid;
		bool dup = false;

		if (ssid[0] == '\0') {
			continue;   /* hidden network; nothing to put in a picklist */
		}
		/* The driver already returns the list sorted by descending RSSI
		 * (documented on esp_wifi_scan_get_ap_records), so the FIRST
		 * occurrence of an SSID is its strongest BSSID and later ones are
		 * just other radios on the same mesh. Keep the first, drop the rest.
		 * That also means no sort of our own is needed. */
		for (int j = 0; j < s_scan_n; j++) {
			if (strcmp(s_scan[j].ssid, ssid) == 0) {
				dup = true;
				break;
			}
		}
		if (dup) {
			continue;
		}
		/* wifi_ap_record_t.ssid is uint8_t[33] and the driver NUL-terminates
		 * it, but strlcpy-style bounding costs nothing. */
		snprintf(s_scan[s_scan_n].ssid, sizeof(s_scan[s_scan_n].ssid), "%s", ssid);
		s_scan[s_scan_n].rssi = recs[i].rssi;
		s_scan_n++;
	}
	free(recs);
	ESP_LOGI(TAG, "scan: %d unique SSIDs", s_scan_n);
}

/* -------------------------------------------------------------------------
 * Captive DNS
 *
 * ESP-IDF ships a DNS *client* and a DHCP server but no DNS server, so this
 * hand-parses just enough of RFC 1035 section 4 to answer every A lookup with
 * the SoftAP address. That is what turns a phone's connectivity probe
 * (captive.apple.com, connectivitycheck.gstatic.com, ...) into a request that
 * reaches our 404 handler and gets redirected to the form.
 *
 * Wire offsets this depends on, all from the start of the 12-byte header:
 *   [0..1]   ID          -- echoed verbatim
 *   [2]      QR(0x80) OPCODE(0x78) AA(0x04) TC(0x02) RD(0x01)
 *   [3]      RA(0x80) Z(0x70) RCODE(0x0F)
 *   [4..5]   QDCOUNT
 *   [6..7]   ANCOUNT
 *   [8..9]   NSCOUNT
 *   [10..11] ARCOUNT
 *   [12..]   QNAME: length-prefixed labels, terminated by a 0x00 root label,
 *            then QTYPE (2 bytes) and QCLASS (2 bytes).
 * The answer we append is 16 bytes: a 0xC00C compression pointer back to the
 * question name at offset 12, TYPE, CLASS, TTL, RDLENGTH, and 4 bytes of IP.
 * ---------------------------------------------------------------------- */

static void dns_task(void *arg)
{
	struct sockaddr_in me = {
		.sin_family      = AF_INET,
		.sin_port        = htons(PORTAL_DNS_PORT),
		.sin_addr.s_addr = htonl(INADDR_ANY),
	};
	struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
	uint8_t buf[PORTAL_DNS_BUF];
	int sock;

	sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sock < 0) {
		ESP_LOGE(TAG, "dns socket: errno %d", errno);
		goto out;
	}
	/* A blocking recvfrom() would pin this task past teardown. The 1s timeout
	 * is what makes s_dns_stop observable. */
	setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	if (bind(sock, (struct sockaddr *)&me, sizeof(me)) < 0) {
		ESP_LOGE(TAG, "dns bind :53: errno %d", errno);
		close(sock);
		goto out;
	}
	ESP_LOGI(TAG, "captive DNS up on :53");

	while (!s_dns_stop) {
		struct sockaddr_in from;
		socklen_t flen = sizeof(from);
		int qend, alen = 0;
		int n;

		n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &flen);
		if (n < 17) {
			continue;   /* 12 header + 1 root label + 4 = smallest legal query */
		}
		if (buf[2] & 0x80) {
			continue;   /* QR set: someone sent us a reply, not a question */
		}
		if ((buf[2] >> 3) & 0x0F) {
			continue;   /* opcode != 0: not a standard query */
		}
		if (buf[4] != 0 || buf[5] != 1) {
			continue;   /* QDCOUNT != 1; the 0xC00C pointer assumes exactly one */
		}

		/* Walk QNAME. A compression pointer (top two bits set) is illegal in a
		 * question section, so a length byte >= 0xC0 is a malformed packet. */
		qend = 12;
		while (qend < n && buf[qend]) {
			if (buf[qend] & 0xC0) {
				qend = -1;
				break;
			}
			qend += buf[qend] + 1;
		}
		if (qend < 0 || qend >= n) {
			continue;
		}
		qend += 1 + 4;   /* root label, then QTYPE and QCLASS */
		if (qend > n) {
			continue;
		}

		buf[2] = 0x84 | (buf[2] & 0x01);   /* QR + AA, preserve the client's RD */
		buf[3] = 0x00;                     /* RA clear, RCODE 0 = NOERROR */
		buf[6] = 0; buf[7] = 0;            /* ANCOUNT, raised below if we answer */
		buf[8] = 0; buf[9] = 0;            /* NSCOUNT */
		buf[10] = 0; buf[11] = 0;          /* ARCOUNT */

		/* QTYPE sits at qend-4. Answer A (type 1) only: a phone that asks AAAA
		 * gets an empty NOERROR and falls back to A, which is correct. Stuffing
		 * a v4 address into a v6 answer confuses resolvers rather than helping
		 * them. */
		if (buf[qend - 4] == 0x00 && buf[qend - 3] == 0x01 &&
		    qend + 16 <= (int)sizeof(buf)) {
			uint8_t *a = buf + qend;

			a[0] = 0xC0; a[1] = 0x0C;          /* NAME: pointer to offset 12 */
			a[2] = 0x00; a[3] = 0x01;          /* TYPE  A */
			a[4] = 0x00; a[5] = 0x01;          /* CLASS IN */
			a[6] = 0; a[7] = 0; a[8] = 0; a[9] = 0;
			/* TTL 0. A longer TTL would leave 192.168.4.1 cached against
			 * captive.apple.com on the user's phone after the portal is gone,
			 * which breaks their normal Wi-Fi, not ours. */
			a[10] = 0x00; a[11] = 0x04;        /* RDLENGTH */
			memcpy(a + 12, &s_ap_ip, 4);       /* RDATA; lwip keeps it in
			                                    * network byte order already */
			alen = 16;
			buf[7] = 1;                        /* ANCOUNT = 1 */
		}
		sendto(sock, buf, (size_t)(qend + alen), 0,
		       (struct sockaddr *)&from, flen);
	}
	close(sock);
out:
	xSemaphoreGive(s_dns_gone);
	vTaskDelete(NULL);
}

/* -------------------------------------------------------------------------
 * HTTP handlers
 * ---------------------------------------------------------------------- */

static esp_err_t err_page(httpd_req_t *req, const char *why)
{
	char page[640];

	snprintf(page, sizeof(page), ERR_HTML_FMT, why);
	httpd_resp_set_status(req, "400 Bad Request");
	httpd_resp_set_type(req, "text/html");
	return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t h_root(httpd_req_t *req)
{
	httpd_resp_set_type(req, "text/html");
	/* A cached copy of this page would be served back on the next setup run,
	 * possibly after a firmware change. */
	httpd_resp_set_hdr(req, "Cache-Control", "no-store");
	return httpd_resp_send(req, PORTAL_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t h_scan(httpd_req_t *req)
{
	char qs[48], fresh[4];
	cJSON *arr;
	char *out;
	esp_err_t err;

	if (httpd_req_get_url_query_str(req, qs, sizeof(qs)) == ESP_OK &&
	    httpd_query_key_value(qs, "fresh", fresh, sizeof(fresh)) == ESP_OK &&
	    fresh[0] == '1') {
		portal_scan();
	}

	/* cJSON rather than snprintf: an SSID is arbitrary bytes and routinely
	 * contains a quote or a backslash. Hand-rolled JSON escaping is exactly
	 * where a picklist breaks on somebody's "Bob's Wi-Fi". */
	arr = cJSON_CreateArray();
	if (!arr) {
		return httpd_resp_send_500(req);
	}
	for (int i = 0; i < s_scan_n; i++) {
		cJSON *o = cJSON_CreateObject();

		if (!o) {
			break;
		}
		cJSON_AddStringToObject(o, "s", s_scan[i].ssid);
		cJSON_AddNumberToObject(o, "r", s_scan[i].rssi);
		cJSON_AddItemToArray(arr, o);
	}
	out = cJSON_PrintUnformatted(arr);
	cJSON_Delete(arr);
	if (!out) {
		return httpd_resp_send_500(req);
	}
	httpd_resp_set_type(req, "application/json");
	err = httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
	cJSON_free(out);
	return err;
}

/* NOTE, and it is a sequencing problem this module cannot fix alone:
 * wx_geocode() talks to api.open-meteo.com, and while the portal is running
 * the STA side is associated with nothing. This endpoint will therefore fail
 * on a genuinely first-run board. It is wired up as specified, and the page
 * offers a raw "lat,lon" path so setup is completable without it. See the
 * report accompanying this file. */
static esp_err_t h_geo(httpd_req_t *req)
{
	char qs[192], raw[96], q[96], place[48], num[24];
	float lat = 0.0f, lon = 0.0f;
	cJSON *o;
	char *out;
	esp_err_t err;

	o = cJSON_CreateObject();
	if (!o) {
		return httpd_resp_send_500(req);
	}
	place[0] = '\0';

	if (httpd_req_get_url_query_str(req, qs, sizeof(qs)) != ESP_OK ||
	    httpd_query_key_value(qs, "q", raw, sizeof(raw)) != ESP_OK ||
	    url_decode(raw, strlen(raw), q, sizeof(q)) <= 0) {
		cJSON_AddBoolToObject(o, "ok", 0);
	} else if (wx_geocode(q, &lat, &lon, place, sizeof(place)) != ESP_OK) {
		cJSON_AddBoolToObject(o, "ok", 0);
	} else {
		cJSON_AddBoolToObject(o, "ok", 1);
		/* Coordinates go out as fixed-4 STRINGS, not JSON numbers: a float
		 * promoted to double prints as 37.974800109863281, which is noise on
		 * the panel and in the form. 4 places is ~11m, far finer than any
		 * weather grid cell. */
		snprintf(num, sizeof(num), "%.4f", (double)lat);
		cJSON_AddStringToObject(o, "lat", num);
		snprintf(num, sizeof(num), "%.4f", (double)lon);
		cJSON_AddStringToObject(o, "lon", num);
		cJSON_AddStringToObject(o, "place", place);
	}

	out = cJSON_PrintUnformatted(o);
	cJSON_Delete(o);
	if (!out) {
		return httpd_resp_send_500(req);
	}
	httpd_resp_set_type(req, "application/json");
	err = httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
	cJSON_free(out);
	return err;
}

static esp_err_t h_save(httpd_req_t *req)
{
	const char *why = NULL;
	wx_cfg_t cfg;
	char *body;
	size_t want;
	int got = 0, tries = 0, n;

	if (req->content_len == 0 || req->content_len > PORTAL_POST_MAX) {
		return err_page(req, "FORM BODY MISSING OR OVER 1024 BYTES");
	}
	want = req->content_len;

	body = malloc(want + 1);
	if (!body) {
		return err_page(req, "OUT OF MEMORY");
	}
	while ((size_t)got < want) {
		n = httpd_req_recv(req, body + got, want - (size_t)got);
		if (n == HTTPD_SOCK_ERR_TIMEOUT) {
			/* Not an error: the body arrived in more TCP segments than one
			 * recv covered. The cap is on CONSECUTIVE stalls, so a slow but
			 * live phone is not cut off, while a half-open socket cannot park
			 * an httpd worker forever. */
			if (++tries >= PORTAL_RECV_TRIES) {
				break;
			}
			continue;
		}
		if (n <= 0) {
			break;
		}
		got += n;
		tries = 0;
	}
	if ((size_t)got != want) {
		free(body);
		return err_page(req, "REQUEST BODY TRUNCATED -- RETRY");
	}
	body[got] = '\0';

	memset(&cfg, 0, sizeof(cfg));

	n = form_field(body, (size_t)got, "ssid", cfg.ssid, sizeof(cfg.ssid));
	if (n == FF_TOOLONG) {
		why = "SSID TOO LONG -- 32 BYTES MAX";
	} else if (n <= 0) {
		why = "SSID IS REQUIRED";
	}

	if (!why) {
		n = form_field(body, (size_t)got, "pass", cfg.pass, sizeof(cfg.pass));
		if (n == FF_TOOLONG) {
			why = "PASSPHRASE TOO LONG -- 64 BYTES MAX";
		}
		/* FF_MISSING and 0 are both fine: an open network has no passphrase. */
	}

	if (!why && !parse_coord(body, (size_t)got, "lat", -90.0f, 90.0f, &cfg.lat)) {
		why = "LATITUDE MISSING OR OUT OF RANGE -- USE FIND, OR TYPE LAT,LON";
	}
	if (!why && !parse_coord(body, (size_t)got, "lon", -180.0f, 180.0f, &cfg.lon)) {
		why = "LONGITUDE MISSING OR OUT OF RANGE -- USE FIND, OR TYPE LAT,LON";
	}

	if (!why) {
		n = form_field(body, (size_t)got, "place", cfg.place, sizeof(cfg.place));
		if (n == FF_TOOLONG) {
			why = "PLACE NAME TOO LONG -- 47 BYTES MAX";
		} else if (n <= 0) {
			/* Purely a display label; a coordinate pair is a fine fallback. */
			snprintf(cfg.place, sizeof(cfg.place), "%.3f, %.3f",
			         (double)cfg.lat, (double)cfg.lon);
		}
	}

	if (!why) {
		n = form_field(body, (size_t)got, "pws", cfg.pws_id, sizeof(cfg.pws_id));
		if (n == FF_TOOLONG) {
			why = "WEATHERCLOUD ID TOO LONG -- 23 BYTES MAX";
		} else if (n < 0) {
			cfg.pws_id[0] = '\0';   /* empty means "no PWS", per the header */
		} else {
			/* Weathercloud device ids are decimal. Catching a pasted URL here
			 * is worth it -- otherwise the failure surfaces months later as a
			 * PWS tier that silently never wins. */
			for (int i = 0; cfg.pws_id[i]; i++) {
				if (!isdigit((unsigned char)cfg.pws_id[i])) {
					why = "WEATHERCLOUD ID MUST BE DIGITS ONLY";
					break;
				}
			}
		}
	}

	free(body);
	if (why) {
		ESP_LOGW(TAG, "rejected config: %s", why);
		return err_page(req, why);
	}

	cfg.configured = true;
	if (wx_cfg_save(&cfg) != ESP_OK) {
		return err_page(req, "NVS WRITE FAILED -- POWER CYCLE AND RETRY");
	}
	s_result = cfg;

	/* Send result deliberately ignored: NVS is already written, so a phone
	 * that walked away mid-response has still configured the board. Failing
	 * the portal here would throw away a good config to report a dead socket. */
	httpd_resp_set_type(req, "text/html");
	httpd_resp_set_hdr(req, "Connection", "close");
	(void)httpd_resp_send(req, OK_HTML, HTTPD_RESP_USE_STRLEN);

	/* Only now. wx_portal_run() tears the AP down the moment this fires, so
	 * the confirmation page has to be on the wire first. */
	xSemaphoreGive(s_done);
	return ESP_OK;
}

/* The catch-all. Every unknown URI 302s to the root, which is what makes a
 * phone's captive-portal detector decide the network is gated and pop the
 * sign-in sheet with our form in it. Returning ESP_FAIL closes the socket,
 * which matters: a probing phone opens several at once and max_open_sockets
 * is finite. */
static esp_err_t h_404_redirect(httpd_req_t *req, httpd_err_code_t err)
{
	httpd_resp_set_status(req, "302 Found");
	httpd_resp_set_hdr(req, "Location", s_root_url);
	httpd_resp_set_hdr(req, "Connection", "close");
	httpd_resp_send(req, NULL, 0);
	return ESP_FAIL;
}

/* -------------------------------------------------------------------------
 * Bring-up and teardown
 * ---------------------------------------------------------------------- */

static const httpd_uri_t URI_ROOT = { .uri = "/",     .method = HTTP_GET,  .handler = h_root };
static const httpd_uri_t URI_SCAN = { .uri = "/scan", .method = HTTP_GET,  .handler = h_scan };
static const httpd_uri_t URI_GEO  = { .uri = "/geo",  .method = HTTP_GET,  .handler = h_geo  };
static const httpd_uri_t URI_SAVE = { .uri = "/save", .method = HTTP_POST, .handler = h_save };

static void portal_teardown(void)
{
	if (s_httpd) {
		httpd_stop(s_httpd);
		s_httpd = NULL;
	}
	if (s_dns_task) {
		s_dns_stop = true;
		/* The task's recv timeout is 1s; 3s is slack, not a guess. */
		if (xSemaphoreTake(s_dns_gone, pdMS_TO_TICKS(3000)) != pdTRUE) {
			ESP_LOGW(TAG, "dns task did not exit within 3s");
		}
		s_dns_task = NULL;
	}

	/* Both return ESP_ERR_WIFI_NOT_INIT when bring-up failed before
	 * esp_wifi_init(); teardown runs on that path too, so it is expected and
	 * not worth logging. */
	(void)esp_wifi_stop();
	(void)esp_wifi_deinit();
	/* Both netifs go. wx_net_connect() creates its own WIFI_STA_DEF netif and
	 * esp_netif asserts on a duplicate if_key, so leaving ours behind turns a
	 * successful setup into a boot loop. */
	esp_netif_destroy_default_wifi(s_ap_netif);
	esp_netif_destroy_default_wifi(s_sta_netif);
	s_ap_netif = NULL;
	s_sta_netif = NULL;

	if (s_done) {
		vSemaphoreDelete(s_done);
		s_done = NULL;
	}
	if (s_dns_gone) {
		vSemaphoreDelete(s_dns_gone);
		s_dns_gone = NULL;
	}
	s_cb = NULL;
}

esp_err_t wx_portal_run(wx_cfg_t *out, wx_portal_status_cb on_progress)
{
	wifi_init_config_t wic = WIFI_INIT_CONFIG_DEFAULT();
	httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
	esp_netif_ip_info_t ip;
	wifi_config_t apc;
	esp_err_t err;
	char msg[64];

	/* esp_netif_dhcps_option() treats this as a BOOLEAN, not a bitmask: see
	 * esp_netif_lwip.c, case DOMAIN_NAME_SERVER, which does
	 * `if (*(uint8_t *)opt->val) *opt_info |= OFFER_DNS`. The official
	 * softap_sta example passes 0x02 via a locally #defined DHCPS_OFFER_DNS
	 * -- that symbol is not exported by IDF, and its value is incidental. */
	uint8_t dhcps_offer_dns = 1;

	if (!out) {
		return ESP_ERR_INVALID_ARG;
	}

	memset(&s_result, 0, sizeof(s_result));
	s_scan_n = 0;
	s_dns_stop = false;
	s_cb = on_progress;

	s_done = xSemaphoreCreateBinary();
	s_dns_gone = xSemaphoreCreateBinary();
	if (!s_done || !s_dns_gone) {
		err = ESP_ERR_NO_MEM;
		goto fail;
	}

	progress("PORTAL: RADIO UP");

	/* esp_netif and the default event loop are process-global. main may have
	 * brought them up already, so ESP_ERR_INVALID_STATE here means "already
	 * done", not a failure. */
	err = esp_netif_init();
	if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
		goto fail;
	}
	err = esp_event_loop_create_default();
	if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
		goto fail;
	}

	err = esp_wifi_init(&wic);
	/* ESP_ERR_WIFI_INIT_STATE means the driver is ALREADY installed --
	 * wx_net.c may have run first. wx_net.c folds this into success and
	 * says "neither module can assume it runs first"; treating it as fatal
	 * here honoured that contract in only one direction and would panic
	 * through ESP_ERROR_CHECK in app_main. */
	if (err != ESP_OK && err != ESP_ERR_WIFI_INIT_STATE) {
		goto fail;
	}
	/* RAM-only storage: this AP config is a throwaway and has no business
	 * being resurrected from NVS on the next boot, where wx_net_connect()
	 * expects a clean STA setup. */
	soft(esp_wifi_set_storage(WIFI_STORAGE_RAM), "set_storage RAM");

	/* APSTA, not AP. STA is not here to connect -- it is what makes
	 * esp_wifi_scan_start() legal, so the page can offer a picklist instead of
	 * demanding the user type an SSID from memory. */
	err = esp_wifi_set_mode(WIFI_MODE_APSTA);
	if (err != ESP_OK) {
		goto fail;
	}

	s_ap_netif = esp_netif_create_default_wifi_ap();
	s_sta_netif = esp_netif_create_default_wifi_sta();
	if (!s_ap_netif || !s_sta_netif) {
		err = ESP_ERR_NO_MEM;
		goto fail;
	}

	memset(&apc, 0, sizeof(apc));
	snprintf((char *)apc.ap.ssid, sizeof(apc.ap.ssid), "%s", WX_PORTAL_SSID);
	apc.ap.ssid_len = (uint8_t)strlen(WX_PORTAL_SSID);
	apc.ap.channel = 1;
	apc.ap.max_connection = 4;
	apc.ap.beacon_interval = 100;
	/* OPEN. See the DESIGN RULE at the top of this file: the window is the
	 * mitigation, not a passphrase. */
	apc.ap.authmode = WIFI_AUTH_OPEN;

	err = esp_wifi_set_config(WIFI_IF_AP, &apc);
	if (err != ESP_OK) {
		goto fail;
	}
	err = esp_wifi_start();
	if (err != ESP_OK) {
		goto fail;
	}
	/* Modem sleep on the STA side adds latency to every form submit for no
	 * benefit during a one-minute setup. */
	soft(esp_wifi_set_ps(WIFI_PS_NONE), "set_ps NONE");

	err = esp_netif_get_ip_info(s_ap_netif, &ip);
	if (err != ESP_OK) {
		goto fail;
	}
	s_ap_ip = ip.ip.addr;   /* network byte order, straight from lwip */
	snprintf(s_root_url, sizeof(s_root_url), "http://" IPSTR "/", IP2STR(&ip.ip));

	/* Hand the DHCP client our own address as its DNS server, otherwise the
	 * phone keeps whatever resolver it had and the hand-rolled responder below
	 * never sees a query. Option 114 (RFC 8910) additionally tells a modern
	 * client the portal URL outright. Both have to bracket a dhcps stop/start
	 * because the server refuses option changes while running. */
	/* The SET path returns ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED unless the
	 * server is stopped first -- verified in esp_netif_lwip.c, the
	 * ESP_NETIF_OP_SET branch of esp_netif_dhcps_option_api(). */
	soft(esp_netif_dhcps_stop(s_ap_netif), "dhcps stop");
	soft(esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET,
	                            ESP_NETIF_DOMAIN_NAME_SERVER,
	                            &dhcps_offer_dns, sizeof(dhcps_offer_dns)),
	     "dhcps offer DNS");
	{
		esp_netif_dns_info_t dns = { 0 };

		dns.ip.type = ESP_IPADDR_TYPE_V4;
		dns.ip.u_addr.ip4.addr = s_ap_ip;
		soft(esp_netif_set_dns_info(s_ap_netif, ESP_NETIF_DNS_MAIN, &dns),
		     "set_dns_info");
	}
	/* s_root_url is file scope precisely because the DHCP server keeps the
	 * pointer for the life of the lease, not a copy. */
	soft(esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET,
	                            ESP_NETIF_CAPTIVEPORTAL_URI,
	                            s_root_url, (uint32_t)strlen(s_root_url)),
	     "dhcps captive-portal URI");
	soft(esp_netif_dhcps_start(s_ap_netif), "dhcps start");

	/* Before the HTTP server exists, so nobody is mid-request when the radio
	 * leaves the home channel. See the caveat above portal_scan(). */
	progress("PORTAL: SCANNING");
	portal_scan();

	if (xTaskCreate(dns_task, "wx_dns", 3072, NULL, 4, &s_dns_task) != pdPASS) {
		s_dns_task = NULL;
		err = ESP_ERR_NO_MEM;
		goto fail;
	}

	/* 8192: cJSON printing plus, on /geo, a whole esp_http_client fetch runs
	 * inside this task. The 4096 default is not enough for that path. */
	hc.stack_size = 8192;
	hc.max_uri_handlers = 6;
	hc.lru_purge_enable = true;   /* probing phones open sockets in bursts */
	hc.server_port = 80;

	err = httpd_start(&s_httpd, &hc);
	if (err != ESP_OK) {
		goto fail;
	}
	if ((err = httpd_register_uri_handler(s_httpd, &URI_ROOT)) != ESP_OK ||
	    (err = httpd_register_uri_handler(s_httpd, &URI_SCAN)) != ESP_OK ||
	    (err = httpd_register_uri_handler(s_httpd, &URI_GEO))  != ESP_OK ||
	    (err = httpd_register_uri_handler(s_httpd, &URI_SAVE)) != ESP_OK) {
		goto fail;
	}
	err = httpd_register_err_handler(s_httpd, HTTPD_404_NOT_FOUND, h_404_redirect);
	if (err != ESP_OK) {
		goto fail;
	}

	snprintf(msg, sizeof(msg), "JOIN %s", WX_PORTAL_SSID);
	progress(msg);
	snprintf(msg, sizeof(msg), "THEN %s", s_root_url);
	progress(msg);

	/* Blocks until h_save() has written NVS and answered the phone. */
	xSemaphoreTake(s_done, portMAX_DELAY);

	progress("PORTAL: CONFIG SAVED");
	/* Let the confirmation page and its FIN actually leave before the radio
	 * goes. Without this the phone shows a connection reset on the very
	 * request that succeeded. */
	vTaskDelay(pdMS_TO_TICKS(700));

	portal_teardown();
	*out = s_result;
	return ESP_OK;

fail:
	ESP_LOGE(TAG, "portal bring-up failed: %s", esp_err_to_name(err));
	portal_teardown();
	return err;
}
