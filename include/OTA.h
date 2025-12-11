#ifndef OTA_H
#define OTA_H

#include <esp32fota.h>
#include "pub_key.h"
#include "FastSyslog.h"

// Firmware version - increment these for new releases
#define FIRMWARE_VERSION_MAJOR 1
#define FIRMWARE_VERSION_MINOR 0
#define FIRMWARE_VERSION_PATCH 0

// OTA configuration
#define FIRMWARE_NAME "mdb-cashless"
#define OTA_CHECK_INTERVAL_MS 3600000  // Check for updates every hour (3600000ms)

// External declaration of FOTA object
extern esp32FOTA FOTA;

// Function declarations
void setupOTA(const char* manifest_url);
void ota_task(void* parameter);

#endif // OTA_H