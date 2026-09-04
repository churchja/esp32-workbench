/*
 * Safe first firmware for a board whose pin map you have NOT yet verified.
 *
 * It drives no GPIO, enables no radios, and touches no peripheral. All it does
 * is ask the runtime what it is standing on and print the answer. That makes it
 * safe to flash onto genuinely unknown hardware -- there is no pin it can get
 * wrong, because it configures none.
 *
 * It complements offline probing rather than repeating it. esptool reads the
 * chip from outside via the bootloader; this reads it from inside, where the
 * heap allocator, the PSRAM controller, and the flash driver have all actually
 * initialised. Where the two disagree, the disagreement is itself the finding:
 * usually a build config claiming memory the silicon does not have.
 */

#include <Arduino.h>
#include <esp_system.h>
#include <esp_chip_info.h>
#include <esp_flash.h>
#include <esp_heap_caps.h>

static void line() { Serial.println(F("--------------------------------------------")); }

static void reportChip() {
  esp_chip_info_t info;
  esp_chip_info(&info);

  line();
  Serial.println(F("CHIP"));
  Serial.printf("  model         : %s\n", ESP.getChipModel());
  Serial.printf("  revision      : %d\n", ESP.getChipRevision());
  Serial.printf("  cores         : %d\n", info.cores);
  Serial.printf("  cpu freq      : %lu MHz\n", (unsigned long)getCpuFrequencyMhz());

  Serial.print(F("  features      : "));
  if (info.features & CHIP_FEATURE_WIFI_BGN) Serial.print(F("WiFi "));
  if (info.features & CHIP_FEATURE_BT)       Serial.print(F("BT-Classic "));
  if (info.features & CHIP_FEATURE_BLE)      Serial.print(F("BLE "));
  if (info.features & CHIP_FEATURE_IEEE802154) Serial.print(F("802.15.4 "));
  if (info.features & CHIP_FEATURE_EMB_FLASH)  Serial.print(F("embedded-flash "));
  if (info.features & CHIP_FEATURE_EMB_PSRAM)  Serial.print(F("embedded-PSRAM "));
  Serial.println();

  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  Serial.printf("  base MAC      : %02x:%02x:%02x:%02x:%02x:%02x\n",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void reportMemory() {
  line();
  Serial.println(F("MEMORY"));

  uint32_t flashSize = 0;
  esp_flash_get_size(NULL, &flashSize);
  Serial.printf("  flash         : %lu bytes (%.1f MB)\n",
                (unsigned long)flashSize, flashSize / 1048576.0);
  Serial.printf("  sketch used   : %lu of %lu bytes (%.1f%%)\n",
                (unsigned long)ESP.getSketchSize(),
                (unsigned long)(ESP.getSketchSize() + ESP.getFreeSketchSpace()),
                100.0 * ESP.getSketchSize() /
                  (ESP.getSketchSize() + ESP.getFreeSketchSpace()));
  Serial.printf("  heap free     : %lu of %lu bytes\n",
                (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getHeapSize());

  size_t psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
  if (psram > 0) {
    Serial.printf("  PSRAM         : %u bytes (%.1f MB), %u free\n",
                  (unsigned)psram, psram / 1048576.0,
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  } else {
    Serial.println(F("  PSRAM         : none usable"));
    Serial.println(F("                  If the profile probed PSRAM present, it is"));
    Serial.println(F("                  not enabled in the build. See flashing.md."));
  }
}

static void reportBoot() {
  line();
  Serial.println(F("BOOT"));
  Serial.printf("  reset reason  : %d\n", (int)esp_reset_reason());
  Serial.println(F("    1=power-on 3=software 4=watchdog 5=deep-sleep"));
  Serial.println(F("    6=brownout  <- brownout means POWER, not code"));
  Serial.printf("  IDF version   : %s\n", esp_get_idf_version());
  Serial.printf("  uptime        : %lu ms\n", (unsigned long)millis());
}

void setup() {
  Serial.begin(115200);
  const uint32_t start = millis();
  while (!Serial && (millis() - start) < 3000) { delay(10); }
  delay(200);

  Serial.println();
  Serial.println(F("ESP32 Workbench -- capability self-report"));
  Serial.println(F("No GPIO configured. No radios started. Nothing driven."));

  reportChip();
  reportMemory();
  reportBoot();
  line();
  Serial.println(F("Feed this into the board profile, then compare against the"));
  Serial.println(F("probed values. Disagreements are findings, not noise."));
  line();
}

void loop() {
  static uint32_t last = 0;
  if (millis() - last > 10000) {
    last = millis();
    Serial.printf("[alive] up %lus  heap %lu\n",
                  (unsigned long)(millis() / 1000),
                  (unsigned long)ESP.getFreeHeap());
  }
  delay(50);
}
