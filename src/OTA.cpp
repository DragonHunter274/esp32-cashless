#include "OTA.h"
#include <Arduino.h>
#include <FreeRTOS.h>
#include <task.h>

// Global FOTA object
esp32FOTA FOTA;

// Certificate for HTTPS connections (if using HTTPS server)
// This is optional - if your server uses HTTP only, you can leave this empty
const char* root_ca = "";

/**
 * @brief Initialize and configure the OTA update system
 * @param manifest_url URL to the JSON manifest file containing firmware info
 *
 * The manifest JSON should have this format:
 * {
 *   "type": "esp32-fota-http",
 *   "version": "1.0.1",
 *   "bin": "firmware.img"
 * }
 *
 * The firmware.img file contains the signature embedded at the beginning:
 * Format: [512-byte RSA signature][firmware binary]
 */
void setupOTA(const char* manifest_url) {
  FAST_LOG_INFO("Initializing OTA update system...");

  // Create RSA public key asset from progmem
  CryptoMemAsset *MyRSAKey = new CryptoMemAsset("RSA Public Key", pub_key, strlen(pub_key) + 1);

  // Get and configure FOTA settings
  auto cfg = FOTA.getConfig();
  cfg.name = (char*)FIRMWARE_NAME;
  cfg.manifest_url = (char*)manifest_url;
  cfg.sem = SemverClass(FIRMWARE_VERSION_MAJOR, FIRMWARE_VERSION_MINOR, FIRMWARE_VERSION_PATCH);
  cfg.check_sig = true;   // REQUIRED: Verify firmware signatures
  cfg.unsafe = false;     // REQUIRED: Enforce security
  cfg.pub_key = MyRSAKey; // Public key for signature verification

  // Configure for embedded signature format (signature at beginning of file)
  cfg.use_device_id = false;

  // Optional: Add root CA for HTTPS connections
  // If your server uses HTTPS, uncomment and configure:
  // CryptoMemAsset *MyRootCA = new CryptoMemAsset("Root CA", root_ca, strlen(root_ca) + 1);
  // cfg.root_ca = MyRootCA;

  FOTA.setConfig(cfg);

  Serial.println("=== OTA Configuration ===");
  Serial.printf("Firmware: %s v%d.%d.%d\n",
    FIRMWARE_NAME,
    FIRMWARE_VERSION_MAJOR,
    FIRMWARE_VERSION_MINOR,
    FIRMWARE_VERSION_PATCH);
  Serial.printf("Manifest URL: %s\n", manifest_url);
  Serial.println("Signature format: EMBEDDED (prepended to firmware)");
  Serial.println("Signature verification: ENABLED");
  Serial.println("Security enforcement: ENABLED");
  Serial.println("========================");

  FAST_LOG_INFO("OTA system initialized successfully");
}

/**
 * @brief FreeRTOS task that periodically checks for and applies firmware updates
 * @param parameter Task parameter (unused)
 *
 * This task runs in the background and:
 * 1. Checks the manifest URL for new firmware versions
 * 2. Downloads and verifies signature if update is available
 * 3. Applies the update and reboots if successful
 * 4. Waits for the configured interval before checking again
 */
void ota_task(void* parameter) {
  // Wait for WiFi to be connected before starting OTA checks
  vTaskDelay(pdMS_TO_TICKS(10000)); // 10 second initial delay

  FAST_LOG_INFO("OTA update task started");

  for (;;) {
    Serial.println("[OTA] Checking for firmware updates...");
    FAST_LOG_INFO("Checking for firmware updates");

    bool updateNeeded = false;

    // Check if update is available
    // This will download the manifest and compare versions
    try {
      updateNeeded = FOTA.execHTTPcheck();

      if (updateNeeded) {
        Serial.println("[OTA] New firmware version available!");
        FAST_LOG_INFO("New firmware version available, starting download");

        // Download and verify the firmware
        // This will:
        // 1. Download the .bin file
        // 2. Download the .sig signature file
        // 3. Verify the signature using the public key
        // 4. Flash the firmware if signature is valid
        // 5. Reboot the device
        FOTA.execOTA();

        // If we reach here, the update failed
        Serial.println("[OTA] Update failed or was rejected");
        FAST_LOG_ERROR("OTA update failed");

      } else {
        Serial.println("[OTA] Firmware is up to date");
        FAST_LOG_DEBUG("No firmware update available");
      }

    } catch (...) {
      Serial.println("[OTA] Error checking for updates");
      FAST_LOG_ERROR("Error during OTA update check");
    }

    // Wait before next check
    vTaskDelay(pdMS_TO_TICKS(OTA_CHECK_INTERVAL_MS));
  }
}
