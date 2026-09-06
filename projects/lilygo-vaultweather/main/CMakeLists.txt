set(srcs
	"main.c"
	"wx_btn.c"
	"wx_touch.c"
	"wx_astro.c"
	"wx_moon_frames.c"
	"wx_cfg.c"
	"wx_net.c"
	"wx_portal.c"
	"wx_fetch.c"
	"wx_ui.c"
	"wx_icons.c"
	"wx_severe_frames.c"
)

# REQUIRES is explicit rather than relying on transitive pulls:
#   nvs_flash        -- saved credentials, location and the weather cache
#   esp_wifi/esp_netif/esp_event -- STA plus the SoftAP setup portal
#   esp_http_client  -- Open-Meteo and Weathercloud
#   esp_http_server  -- the setup portal's web page
#   mbedtls          -- esp_crt_bundle.h lives here, and Weathercloud is
#                       HTTPS-only, so the bundle is not optional
#   lwip             -- raw sockets for the captive-portal DNS responder
#   esp_lcd/driver/esp_timer -- panel, GPIO/SPI, LVGL tick
#   esp_lcd_panel_rm67162 -- the VENDORED driver in components/. The Pip-Boy
#                       project got this transitively because it declared no
#                       REQUIRES at all; naming any requirement at all turns
#                       off that fallback, so it has to be listed here.
# cJSON comes from the espressif/cjson managed component declared in
# idf_component.yml; ESP-IDF 6.0 removed the built-in `json` component, so
# listing `json` here would fail to resolve.
idf_component_register(
	SRCS ${srcs}
	INCLUDE_DIRS "."
	REQUIRES
		nvs_flash
		esp_wifi
		esp_netif
		esp_event
		esp_http_client
		esp_http_server
		mbedtls
		lwip
		esp_lcd
		esp_lcd_panel_rm67162
		driver
		esp_timer
		esp_driver_usb_serial_jtag
		esp_driver_i2c
)
