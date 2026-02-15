# ESP32 Cashless Payment System

MDB (Multi-Drop Bus) cashless payment system for vending machines with secure OTA updates.

## Features

- **MDB Protocol**: Full implementation of MDB cashless payment protocol
- **RFID Card Reader**: MFRC522 with Ultralight C authentication support
- **Backend Integration**: REST API for balance checking and transaction processing
- **Secure OTA Updates**: Remote firmware updates with RSA-4096 signature verification
- **Async Logging**: High-performance syslog integration
- **FreeRTOS Architecture**: Multi-threaded design optimized for real-time performance

## Quick Start

### Initial Configuration

1. Copy `include/secrets.example.h` to `include/secrets.h`:
   ```bash
   cp include/secrets.example.h include/secrets.h
   ```

2. Open `include/secrets.h` and update the values with your actual configuration:
   - `WIFI_SSID`: Your WiFi network name
   - `WIFI_PASSWORD`: Your WiFi password
   - `API_KEY`: Your API key for the backend service
   - `API_BASE_URL`: The base URL of the backend API (e.g., `http://192.168.1.100:8080`)
   - `SYSLOG_SERVER`: Your syslog server IP address
   - `MACHINE_ID`: Unique identifier for this vending machine
   - `OTA_MANIFEST_URL`: URL to the OTA manifest file for firmware updates

### Build and Upload

```bash
# Build the project
pio run

# Upload to device
pio run --target upload

# Monitor serial output
pio device monitor
```

### OTA Firmware Updates

For secure remote firmware updates, see the complete guide: **[OTA_SETUP.md](OTA_SETUP.md)**

Quick workflow:
```bash
# Build firmware (automatically signs after build)
pio run

# Optional: Verify signature
./verify_firmware.sh firmware_release/firmware.img

# Upload firmware_release/ contents to your web server
```

**Note**: Firmware is automatically signed after every build. The version is read from `include/OTA.h`.

## Documentation

- **[CLAUDE.md](CLAUDE.md)** - Architecture and development guide
- **[OTA_SETUP.md](OTA_SETUP.md)** - Complete OTA update system documentation

> [!IMPORTANT]
> `include/secrets.h` and `priv_key.pem` are ignored by git to prevent accidental commit of sensitive information. Do not remove them from `.gitignore`.
