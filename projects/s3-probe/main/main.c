/*
 * Safe first firmware for a board whose pin map you have NOT verified.
 *
 * It configures no GPIO, starts no radios, and touches no peripheral. There is
 * no pin it can get wrong, because it drives none -- which is what makes it
 * safe to flash onto genuinely unknown hardware.
 *
 * It complements the offline probe rather than repeating it. esptool reads the
 * chip from outside via the bootloader; this reads it from inside, after the
 * heap allocator, PSRAM controller and flash driver have actually initialised.
 * Where the two disagree, the disagreement IS the finding -- usually a build
 * config claiming memory the silicon does not have.
 *
 * APIs verified against ESP-IDF v6.0.3 headers on disk.
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

static void rule(void) { printf("--------------------------------------------\n"); }

static void report_chip(void)
{
    esp_chip_info_t info;
    esp_chip_info(&info);

    rule();
    printf("CHIP\n");
    printf("  cores         : %d\n", info.cores);
    printf("  revision      : %d.%d\n", info.revision / 100, info.revision % 100);

    /* Flags from esp_chip_info.h; read from eFuse, so authoritative for what
       radios and embedded memory the silicon actually has. */
    printf("  features      : ");
    if (info.features & CHIP_FEATURE_WIFI_BGN)  printf("WiFi2.4G ");
    if (info.features & CHIP_FEATURE_BT)        printf("BT-Classic ");
    if (info.features & CHIP_FEATURE_BLE)       printf("BLE ");
    if (info.features & CHIP_FEATURE_IEEE802154) printf("802.15.4 ");
    if (info.features & CHIP_FEATURE_EMB_FLASH) printf("embedded-flash ");
    if (info.features & CHIP_FEATURE_EMB_PSRAM) printf("embedded-PSRAM ");
    printf("\n");

    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_BASE) == ESP_OK) {
        printf("  base MAC      : %02x:%02x:%02x:%02x:%02x:%02x\n",
               mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        printf("                  (this is the workbench's profile/backup key)\n");
    }
}

static void report_memory(void)
{
    rule();
    printf("MEMORY\n");

    uint32_t flash_size = 0;
    if (esp_flash_get_size(NULL, &flash_size) == ESP_OK) {
        printf("  flash         : %" PRIu32 " bytes (%.1f MB)\n",
               flash_size, flash_size / 1048576.0);
    } else {
        printf("  flash         : size could not be read\n");
    }

    printf("  heap free/tot : %u / %u bytes\n",
           (unsigned) heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned) heap_caps_get_total_size(MALLOC_CAP_INTERNAL));

    /* No esp_psram requirement needed -- see main/CMakeLists.txt. */
    size_t psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    if (psram > 0) {
        printf("  PSRAM         : %u bytes (%.1f MB), %u free\n",
               (unsigned) psram, psram / 1048576.0,
               (unsigned) heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    } else {
        printf("  PSRAM         : none usable\n");
        printf("                  If the probed profile says PSRAM is present,\n");
        printf("                  it is not enabled in sdkconfig.\n");
    }
}

static void report_build(void)
{
    const esp_app_desc_t *d = esp_app_get_description();
    rule();
    printf("BUILD\n");
    if (d) {
        printf("  project       : %s\n", d->project_name);
        printf("  version       : %s\n", d->version);
        printf("  built         : %s %s\n", d->date, d->time);
        printf("  idf (app_desc): %s\n", d->idf_ver);
    }
    printf("  idf (compiled): %s\n", esp_get_idf_version());

    rule();
    printf("BOOT\n");
    printf("  reset reason  : %d\n", (int) esp_reset_reason());
    printf("    1=power-on 3=software 4=panic 5=int-wdt 6=task-wdt\n");
    printf("    8=deep-sleep  9=brownout  <- brownout means POWER, not code\n");
}

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(300));   /* let the console settle */

    printf("\nESP32 Workbench -- capability self-report (ESP-IDF)\n");
    printf("No GPIO configured. No radios started. Nothing driven.\n");

    report_chip();
    report_memory();
    report_build();
    rule();
    printf("Reconcile these against the probed profile in boards/.\n");
    printf("Disagreements are findings, not noise.\n");
    rule();

    uint32_t s = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        printf("[alive] up %" PRIu32 "s  heap %u\n", (s += 10),
               (unsigned) heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    }
}
