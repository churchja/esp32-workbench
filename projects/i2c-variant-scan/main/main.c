/*
 * LilyGo T-Display S3 AMOLED -- variant discriminator.
 *
 * WHY THIS EXISTS
 * ---------------
 * "T-Display S3 AMOLED" is a family of at least six boards. Every one is
 * ESP32-S3R8 with 16MB flash and 8MB OPI PSRAM, so nothing readable over USB
 * separates them -- see boards/e4b0638aec2c.yaml, board.probe_discriminates_variant.
 * The variant decides the panel controller, the bus, and three GPIOs whose
 * functions conflict across builds (38: LED vs panel power enable; 21: button
 * vs touch INT; 4: battery ADC vs SD-card MISO).
 *
 * LilyGO's own library resolves it by I2C probe in LilyGo_AMOLED::begin().
 * This reproduces that sequence and only reports -- it configures no display,
 * enables no rail, and writes nothing.
 *
 * SAFETY NOTES
 * ------------
 * idf-base deliberately drives no GPIO, which is what makes it safe on a board
 * with an unverified pin map. This file breaks that rule on purpose, so the
 * bounds are worth stating:
 *
 *   - None of the scanned pins (1, 2, 3, 6, 7) is one of the three conflicting
 *     GPIOs (4, 21, 38). Checked against the profile before writing this.
 *   - I2C is open-drain with internal pull-ups: the master pulls low and
 *     releases, it never drives high.
 *   - The probes run in LilyGO's order and STOP at the first hit. Pair C (6,7)
 *     is LCD_CS/LCD_D1 on the 1.91in boards, so it is only reached when the
 *     board is already known not to be a 1.91in. Worst case there is a garbled
 *     panel until reset -- display bus lines cannot be damaged by toggling.
 *   - i2c_master_probe() sends an address frame and reads the ACK bit. It
 *     transmits no data bytes to any device.
 */
#include <stdio.h>
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "variant";

/* Addresses LilyGO's autodetect keys on. */
#define ADDR_AXP2101   0x34   /* PMU on the 1.47in Lite            */
#define ADDR_CST816    0x15   /* touch controller on 1.91in Touch/Plus */
#define ADDR_PCF85063  0x51   /* RTC -- present only on the Plus   */
#define ADDR_SY6970    0x6A   /* PMU on the 2.41in T4-S3           */
#define ADDR_BQ25896   0x6B   /* PMU on the 1.91in SPI/Plus        */

typedef struct { const char *name; int sda; int scl; } bus_t;

static bool scan_bus(const bus_t *b, uint8_t *found, int *n_found)
{
    i2c_master_bus_config_t cfg = {
        .i2c_port = -1,
        .sda_io_num = b->sda,
        .scl_io_num = b->scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus = NULL;
    esp_err_t err = i2c_new_master_bus(&cfg, &bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s (SDA=%d SCL=%d): bus init failed: %s",
                 b->name, b->sda, b->scl, esp_err_to_name(err));
        return false;
    }

    *n_found = 0;
    for (uint16_t a = 0x08; a <= 0x77; a++) {
        if (i2c_master_probe(bus, a, 50) == ESP_OK) {
            found[(*n_found)++] = (uint8_t)a;
        }
    }
    i2c_del_master_bus(bus);

    printf("  %s  SDA=%-2d SCL=%-2d  -> ", b->name, b->sda, b->scl);
    if (*n_found == 0) {
        printf("no devices\n");
    } else {
        for (int i = 0; i < *n_found; i++) printf("0x%02x ", found[i]);
        printf("\n");
    }
    return *n_found > 0;
}

static bool has(const uint8_t *f, int n, uint8_t a)
{
    for (int i = 0; i < n; i++) if (f[i] == a) return true;
    return false;
}

void app_main(void)
{
    /* LilyGO's order. Stop at the first hit -- see SAFETY NOTES. */
    const bus_t A = { "pair A", 1, 2 };
    const bus_t B = { "pair B", 3, 2 };
    const bus_t C = { "pair C", 6, 7 };

    uint8_t f[64]; int n = 0;
    const char *verdict = NULL;
    const char *why = NULL;

    vTaskDelay(pdMS_TO_TICKS(1500));   /* let the USB console attach */

    printf("\n==== LilyGo AMOLED variant scan ====\n");

    if (scan_bus(&A, f, &n) && has(f, n, ADDR_AXP2101)) {
        verdict = "T-Display-AMOLED-Lite 1.47in (SH8501)";
        why = "AXP2101 (0x34) on SDA=1/SCL=2";
    } else if (scan_bus(&B, f, &n) && has(f, n, ADDR_CST816)) {
        if (has(f, n, ADDR_PCF85063)) {
            verdict = "T-Display-S3 AMOLED Plus 1.91in (RM67162, single SPI)";
            why = "CST816 (0x15) AND PCF85063 RTC (0x51) on SDA=3/SCL=2";
        } else {
            verdict = "T-Display-S3 AMOLED Touch 1.91in (RM67162, QSPI)";
            why = "CST816 (0x15), no RTC at 0x51, on SDA=3/SCL=2";
        }
    } else if (scan_bus(&C, f, &n) && has(f, n, ADDR_SY6970)) {
        verdict = "T4-S3 2.41in (RM690B0)";
        why = "SY6970 (0x6a) on SDA=6/SCL=7";
    } else {
        verdict = "T-Display-S3 AMOLED 1.91in NON-TOUCH (RM67162, QSPI)";
        why = "no PMU, no touch controller, no RTC on any probed pair "
              "-- LilyGO's fall-through case";
    }

    printf("\n  VARIANT : %s\n", verdict);
    printf("  BASIS   : %s\n", why);
    printf("==== end ====\n\n");

    /* Idle. Nothing here drives a peripheral; the board waits for a restore. */
    while (1) vTaskDelay(pdMS_TO_TICKS(10000));
}
