/*
 * S2 USB-console probe -- the SEPARATE template.
 *
 * This is deliberately NOT the base probe firmware. That one's defining
 * property is that it drives no peripheral at all, which is what makes it safe
 * to flash onto a board whose pin map is unverified. A USB device stack IS a
 * peripheral, so bolting TinyUSB onto it would destroy that guarantee.
 *
 * Why this exists: parts with a USB-Serial/JTAG controller (S3/C3/C6/C5/H2/P4)
 * get a secondary console for free and the base template is readable over USB.
 * The ESP32-S2 has only USB-OTG -- soc_caps.h declares SOC_USB_OTG_SUPPORTED
 * and nothing else -- so it gets no secondary console, and the ROM CDC option
 * (CONFIG_ESP_CONSOLE_USB_CDC) cannot bring USB up from a cold boot into an
 * app. Verified twice on hardware: both produced a board that ran correctly and
 * presented no USB device at all.
 *
 * TinyUSB drives the OTG peripheral itself, which is the missing piece.
 *
 * API taken from the fetched component headers, not from memory:
 *   tinyusb.h                 tinyusb_driver_install(const tinyusb_config_t *)
 *   tinyusb_default_config.h  TINYUSB_DEFAULT_CONFIG() -- separate header;
 *                             it includes tinyusb.h, not the other way round
 *   tinyusb_cdc_acm.h  tinyusb_cdcacm_init(const tinyusb_config_cdcacm_t *)
 *                      TINYUSB_CDC_ACM_0
 *   tinyusb_console.h  tinyusb_console_init(int cdc_intf)
 */

#include <inttypes.h>
#include <stdio.h>

#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_cdc_acm.h"
#include "tinyusb_console.h"

static void report(void)
{
    esp_chip_info_t info;
    esp_chip_info(&info);
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_BASE);
    uint32_t flash = 0;
    esp_flash_get_size(NULL, &flash);
    const esp_app_desc_t *d = esp_app_get_description();

    printf("\n--------------------------------------------\n");
    printf("ESP32 Workbench -- self-report over TinyUSB CDC\n");
    printf("  cores       : %d\n", info.cores);
    printf("  revision    : %d.%d\n", info.revision / 100, info.revision % 100);
    printf("  base MAC    : %02x:%02x:%02x:%02x:%02x:%02x\n",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    printf("  flash       : %" PRIu32 " bytes (%.1f MB)\n", flash, flash / 1048576.0);
    printf("  heap free   : %u bytes\n",
           (unsigned) heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    size_t psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    printf("  PSRAM       : %s\n", psram ? "present and enabled" : "none usable");
    if (d) printf("  project     : %s / idf %s\n", d->project_name, d->idf_ver);
    printf("  idf compiled: %s\n", esp_get_idf_version());
    printf("  reset reason: %d\n", (int) esp_reset_reason());
    printf("--------------------------------------------\n");
}

void app_main(void)
{
    tinyusb_config_t cfg = TINYUSB_DEFAULT_CONFIG();
    ESP_ERROR_CHECK(tinyusb_driver_install(&cfg));

    tinyusb_config_cdcacm_t acm = { .cdc_port = TINYUSB_CDC_ACM_0 };
    ESP_ERROR_CHECK(tinyusb_cdcacm_init(&acm));
    ESP_ERROR_CHECK(tinyusb_console_init(TINYUSB_CDC_ACM_0));

    /* The host may open the port well after boot, and anything printed before
       it attaches is lost. Repeat rather than print once and go silent. */
    uint32_t n = 0;
    while (1) {
        report();
        printf("  [alive] %" PRIu32 "\n", n++);
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}
