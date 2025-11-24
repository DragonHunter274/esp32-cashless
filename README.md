# ESP32 Cashless Payment System

## Configuration

This project uses a configuration file for sensitive credentials and settings.

1. Copy `include/secrets.example.h` to `include/secrets.h`:
   ```bash
   cp include/secrets.example.h include/secrets.h
   ```

2. Open `include/secrets.h` and update the values with your actual configuration:
   - `WIFI_SSID`: Your WiFi network name.
   - `WIFI_PASSWORD`: Your WiFi password.
   - `API_KEY`: Your API key for the backend service.
   - `API_BASE_URL`: The base URL of the backend API (e.g., `http://192.168.1.100:8080`).

> [!IMPORTANT]
> `include/secrets.h` is ignored by git to prevent accidental commit of sensitive information. Do not remove it from `.gitignore`.
