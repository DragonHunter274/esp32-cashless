#define ARDUINO_BEARSSL_DISABLE_SHA1

#include <Arduino.h>
#include <FreeRTOS.h>
#include <task.h>
#include <queue.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <Syslog.h>
#include <OTA.h>

// Define max log level before including FastSyslog.h to filter logs at compile time
#define FAST_SYSLOG_MAX_LEVEL FAST_SYSLOG_ERR      // Only errors and higher priority

#include "FastSyslog.h"
#define ESP32_RTOS
#include "secrets.h"

// Include modular headers
#include "mdb_protocol.h"
#include "mdb_comm.h"
#include "api_client.h"
#include "reader_handler.h"
#include "cardreader.h"

// Configuration constants
const char* api_key = API_KEY;
const char* api_base_url = API_BASE_URL;
const char* ssid = WIFI_SSID;
const char* password = WIFI_PASSWORD;

String resolved_api_base_url = "";

// Global objects
WiFiUDP udpClient;
Syslog syslog(udpClient, SYSLOG_PROTO_IETF);
CardReader cardReader;

QueueHandle_t cashSaleQueue;

void setup() {
  // Pin modes
  mdb_init();

  cashSaleQueue = xQueueCreate(10, sizeof(CashSale_t));

  Serial.begin(115200);

  xTaskCreatePinnedToCore(
    mdb_loop,     // Task function
    "mdb_loop",   // Task name
    16384,        // Stack size
    NULL,         // Parameters
    3,            // Priority
    NULL,         // Task handle
    1
  );

  Result initResult = cardReader.begin();
  if (initResult == Result::OK) {
    Serial.println("Card Reader initialized successfully.");
  } else {
    Serial.println("Card Reader initialization failed.");
  }

  connectToWiFi();

  if (!fastSyslog.begin(SYSLOG_SERVER, SYSLOG_PORT, MACHINE_ID, MACHINE_ID)) {
    Serial.println("Failed to initialize FastSyslog!");
    return;
  }

  Serial.println("starting up");
  FAST_LOG_INFO("starting up");

  // Initialize OTA update system with signed firmware verification
  setupOTA(OTA_MANIFEST_URL);

  // Create WiFi Monitoring Task
  xTaskCreatePinnedToCore(
    wifi_loop,      // Task function
    "wifi_loop",    // Task name
    4096,           // Stack size
    NULL,           // Parameters
    1,              // Priority
    NULL,           // Task handle
    0
  );

  xTaskCreatePinnedToCore(
    reader_loop,  // Task function
    "reader_loop",// Task name
    8192,         // Stack size
    NULL,         // Parameters
    1,            // Priority
    NULL,         // Task handle
    0
  );

  xTaskCreatePinnedToCore(
    cashsale_handler,
    "cashsale_handler",
    4096,
    NULL,
    1,
    NULL,
    0
  );

  // Create OTA Update Task
  xTaskCreatePinnedToCore(
    ota_task,       // Task function
    "ota_task",     // Task name
    8192,           // Stack size (larger for HTTPS operations)
    NULL,           // Parameters
    1,              // Priority (low priority, background task)
    NULL,           // Task handle
    0               // Core 0
  );
}

void loop() {
  // Empty - all functionality is in FreeRTOS tasks
}
