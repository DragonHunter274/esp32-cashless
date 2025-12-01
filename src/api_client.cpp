#include "api_client.h"
#include "mdb_comm.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include "FastSyslog.h"
#include "secrets.h"

extern const char* api_key;
extern const char* api_base_url;
extern const char* ssid;
extern const char* password;
extern String resolved_api_base_url;
extern QueueHandle_t cashSaleQueue;

int getUserBalance(const char* uid) {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi not connected!");
        return -1;
    }

    // Use resolved URL instead of hardcoded one
    if (resolved_api_base_url.length() == 0) {
        Serial.println("No valid API base URL available!");
        return -1;
    }

    HTTPClient http;
    String url = String(api_base_url) + "/getBalance";
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("X-API-Key", api_key);

    StaticJsonDocument<200> jsonRequest;
    jsonRequest["uid"] = uid;
    String requestBody;
    serializeJson(jsonRequest, requestBody);

    int httpResponseCode = http.POST(requestBody);
    if (httpResponseCode == 200) {
        String response = http.getString();
        StaticJsonDocument<200> jsonResponse;
        deserializeJson(jsonResponse, response);
        int balance = jsonResponse["balance"];
        http.end();
        return balance;
    } else {
        Serial.print("Error fetching balance: ");
        Serial.println(httpResponseCode);
        fastSyslog.logf(LOG_ERR, "Error fetching Balance %d", httpResponseCode);
        http.end();
        return -1;
    }
}

int makePurchase(const char* uid, int amount, int product, const char* machine_id) {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi not connected!");
        return -1;
    }

    if (resolved_api_base_url.length() == 0) {
        Serial.println("No valid API base URL available!");
        return -1;
    }

    FAST_LOG_DEBUG("entering makePurchase function");
    HTTPClient http;
    String url = String(api_base_url) + "/makePurchase";
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("X-API-Key", api_key);

    StaticJsonDocument<256> jsonRequest;
    jsonRequest["uid"] = uid;
    jsonRequest["amount"] = amount;
    jsonRequest["product"] = product;
    jsonRequest["machine_id"] = machine_id;
    String requestBody;
    serializeJson(jsonRequest, requestBody);

    int httpResponseCode = http.POST(requestBody);
    if (httpResponseCode == 200) {
        StaticJsonDocument<128> jsonResponse;
        DeserializationError error = deserializeJson(jsonResponse, http.getString());
        http.end();

        if (error) {
            FAST_LOG_ERROR("Failed to parse response.");
            return -1;
        }

        int transactionId = jsonResponse["transaction_id"];
        return transactionId;
    } else {
        http.end();
        return -1;
    }
}

int makeCashPurchase(int amount, int product, const char* machine_id) {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi not connected!");
        return -1;
    }

    if (resolved_api_base_url.length() == 0) {
        Serial.println("No valid API base URL available!");
        return -1;
    }

    FAST_LOG_DEBUG("entering makeCashPurchase function");
    HTTPClient http;
    String url = String(api_base_url) + "/makeCashPurchase";
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("X-API-Key", api_key);

    StaticJsonDocument<256> jsonRequest;
    jsonRequest["amount"] = amount;
    jsonRequest["product"] = product;
    jsonRequest["machine_id"] = machine_id;
    String requestBody;
    serializeJson(jsonRequest, requestBody);

    int httpResponseCode = http.POST(requestBody);
    if (httpResponseCode == 201) {
        StaticJsonDocument<128> jsonResponse;
        DeserializationError error = deserializeJson(jsonResponse, http.getString());
        http.end();

        if (error) {
            FAST_LOG_ERROR("Failed to parse response.");
            return -1;
        }
        return 1;
    } else {
        Serial.print("Purchase failed. HTTP code: ");
        fastSyslog.logf(LOG_ERR, "Purchase Failed. HTTP code: %d", httpResponseCode);
        http.end();
        return -1;
    }
}

bool confirmPurchase(int transactionId) {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi not connected!");
        return false;
    }

    if (resolved_api_base_url.length() == 0) {
        Serial.println("No valid API base URL available!");
        return false;
    }

    HTTPClient http;
    String url = String(api_base_url) + "/confirmPurchase";
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("X-API-Key", api_key);

    StaticJsonDocument<128> jsonRequest;
    jsonRequest["transaction_id"] = transactionId;
    String requestBody;
    serializeJson(jsonRequest, requestBody);

    int httpResponseCode = http.POST(requestBody);
    http.end();

    if (httpResponseCode == 200) {
        return true;
    } else {
        return false;
    }
}

void resolveServerHostname() {
    Serial.println("Resolving server hostname via mDNS...");

    // Try to resolve k3s-node1.local
    IPAddress serverIP = MDNS.queryHost("k3s-node1");

    if (serverIP != INADDR_NONE) {
        // Successfully resolved
        extern String resolved_api_base_url;
        resolved_api_base_url = "http://" + serverIP.toString() + ":8080";
        Serial.print("Resolved k3s-node1.local to: ");
        Serial.println(serverIP);
        Serial.print("API Base URL set to: ");
        Serial.println(resolved_api_base_url);

        fastSyslog.logf(LOG_INFO, "Resolved k3s-node1.local to %s", serverIP.toString().c_str());
    } else {
        // Failed to resolve, fallback to hardcoded IP
        Serial.println("Failed to resolve k3s-node1.local via mDNS");
        Serial.println("Falling back to hardcoded IP address");
        resolved_api_base_url = String(api_base_url); // Use the original hardcoded URL

        fastSyslog.logf(LOG_WARNING, "mDNS resolution failed, using fallback IP");
    }
}

void connectToWiFi() {
    Serial.print("Connecting to WiFi...");
    WiFi.begin(ssid, password);

    // Wait for connection
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 30) {
        Serial.print(".");
        delay(1000);  // 1-second delay
        attempts++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.print("\nConnected to WiFi! IP Address: ");
        Serial.println(WiFi.localIP());

        // Initialize mDNS responder
        if (!MDNS.begin(MACHINE_ID)) {
            Serial.println("Error setting up MDNS responder!");
            while(1) {
                delay(1000);
            }
        }
        Serial.println("mDNS responder started");

        // Initialize FastSyslog with hardcoded IP first (will be updated by resolveServerHostname)
        if (!fastSyslog.begin(SYSLOG_SERVER, SYSLOG_PORT, MACHINE_ID, MACHINE_ID)) {
            Serial.println("Failed to initialize FastSyslog!");
        }

        // Resolve the server hostnames using mDNS
        resolveServerHostname();
    } else {
        Serial.println("\nWiFi Connection Failed!");
    }
}

void wifi_loop(void *pvParameters) {
    for (;;) {
        // Periodic check
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("WiFi Disconnected! Attempting reconnect...");
            connectToWiFi();
        }
        //ArduinoOTA.handle();
        vTaskDelay(10000 / portTICK_PERIOD_MS);  // Check every 10 seconds
    }
}

void cashsale_handler(void *pvParameters){
  CashSale_t cashsale_data;
  for(;;){
    if (xQueueReceive(cashSaleQueue, &cashsale_data, portMAX_DELAY) == pdPASS){
      fastSyslog.logf(LOG_INFO, "cashsale item: %d cashsale_price: %d",cashsale_data.itemNumber,cashsale_data.itemPrice);
      makeCashPurchase(cashsale_data.itemPrice, cashsale_data.itemNumber, MACHINE_ID);
    }
  }
}
