#define ARDUINO_BEARSSL_DISABLE_SHA1

#include <Arduino.h>
#include <FreeRTOS.h>
#include <task.h>
#include <queue.h>
#include "cardreader.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <Syslog.h>
#include <WiFiUdp.h>

// Define max log level before including FastSyslog.h to filter logs at compile time
// Uncomment one of the following lines to set your desired max log level:
#define FAST_SYSLOG_MAX_LEVEL FAST_SYSLOG_ERR      // Only errors and higher priority
// #define FAST_SYSLOG_MAX_LEVEL FAST_SYSLOG_WARNING  // Warnings, errors, and higher
// #define FAST_SYSLOG_MAX_LEVEL FAST_SYSLOG_INFO     // Info, warnings, errors (no debug)
// #define FAST_SYSLOG_MAX_LEVEL FAST_SYSLOG_DEBUG    // All logs (default)

#include "FastSyslog.h"
#define ESP32_RTOS
const char* api_key = "redacted";
#include <OTA.h>
const char* api_base_url = "http://10.100.1.55:8080"; // Replace with actual API URL

const char* ssid = "redacted";  // Change to your WiFi SSID
const char* password = "redacted";  // Change to your WiFi password
String resolved_api_base_url = "";


WiFiUDP udpClient;
Syslog syslog(udpClient, SYSLOG_PROTO_IETF);

uint16_t current_item_price = 0;
uint16_t current_item_number = 999;
int current_user_balance = 0;
bool vend_success = false;
// Pin Definitions
#define pin_mdb_rx  4
#define pin_mdb_tx  5
#define pin_mdb_led 13
#define pin_button   0

#define MACHINE_ID "getraenkeautomat"

// Macros
#define to_scale_factor(p, x, y) (p / x / pow(10, -(y)))
#define from_scale_factor(p, x, y) (p * x * pow(10, -(y)))

// MDB Protocol Constants
#define ACK 0x00  // Acknowledgment / Checksum correct
#define RET 0xAA  // Retransmit the previously sent data
#define NAK 0xFF  // Negative acknowledge

#define BIT_MODE_SET  0b100000000
#define BIT_ADD_SET   0b011111000
#define BIT_CMD_SET   0b000000111

CardReader cardReader;


typedef struct {
  uint16_t itemPrice;
  uint16_t itemNumber;
} CashSale_t;

QueueHandle_t cashSaleQueue;

// Enumerations
enum MDB_COMMAND {
  RESET = 0x00,
  SETUP = 0x01,
  POLL = 0x02,
  VEND = 0x03,
  READER = 0x04,
  EXPANSION = 0x07
};

enum MDB_SETUP_DATA {
  CONFIG_DATA = 0x00, 
  MAX_MIN_PRICES = 0x01
};

enum MDB_VEND_DATA {
  VEND_REQUEST = 0x00,
  VEND_CANCEL = 0x01,
  VEND_SUCCESS = 0x02,
  VEND_FAILURE = 0x03,
  SESSION_COMPLETE = 0x04,
  CASH_SALE = 0x05
};

enum MDB_READER_DATA {
  READER_DISABLE = 0x00, 
  READER_ENABLE = 0x01, 
  READER_CANCEL = 0x02
};

enum MDB_EXPANSION_DATA {
  REQUEST_ID = 0x00
};

enum MACHINE_STATE {
  INACTIVE_STATE, 
  DISABLED_STATE, 
  ENABLED_STATE, 
  IDLE_STATE, 
  VEND_STATE
};

// Global Variables
volatile MACHINE_STATE machine_state = INACTIVE_STATE;

volatile bool reset_cashless_todo = false;
volatile bool session_begin_todo = false;
volatile bool session_end_todo = false;
volatile bool session_cancel_todo = false;
volatile bool vend_approved_todo = false;
volatile bool vend_denied_todo = false;
volatile bool outsequence_todo = false;
volatile bool reader_cancel_todo = false;

// Function Prototypes
uint16_t read_9(uint8_t *checksum);
void write_9(uint16_t nth9);
void transmitPayloadByUART9(uint8_t *mdb_payload, uint8_t length);
void mdb_loop(void *pvParameters);
void reader_loop(void *pvParameters);
void formatUidString(const CardReader::Uid& uid, char* uidString, size_t maxLen);
void waitForCardRemoval();
bool waitForMachineState(MACHINE_STATE targetState, uint32_t timeoutMs);
void processCardTransaction(const char* uidString);
bool getAndVerifyBalance(const char* uidString);
int processPurchase(const char* uidString);
bool validate_mdb_checksum(uint8_t command, uint8_t *data, uint8_t data_len);







int getUserBalance(const char* uid) {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi not connected!");
        return -1;
    }

    // Use resolved URL instead of hardcoded one
    if (resolved_api_base_url.length() == 0) {
        Serial.println("❌ No valid API base URL available!");
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
        Serial.println("❌ No valid API base URL available!");
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
        Serial.println("❌ No valid API base URL available!");
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
        Serial.println("❌ No valid API base URL available!");
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


// Read 9-bit data
uint16_t read_9(uint8_t *checksum) {
  uint16_t coming_read = 0;

  while (digitalRead(pin_mdb_rx))
    ;

  delayMicroseconds(156);
  for (uint8_t x = 0; x < 9; x++) {
    coming_read |= (digitalRead(pin_mdb_rx) << x);
    delayMicroseconds(104); // 9600bps
  }

  // FIXED: Only add data bytes (not mode bytes) to checksum
  if(checksum && !(coming_read & BIT_MODE_SET)) {
    *checksum += (coming_read & 0xFF);
  }

  return coming_read;
}


void resolveServerHostname() {
    Serial.println("🔍 Resolving server hostname via mDNS...");
    
    // Try to resolve k3s-node1.local
    IPAddress serverIP = MDNS.queryHost("k3s-node1");
    
    if (serverIP != INADDR_NONE) {
        // Successfully resolved
        resolved_api_base_url = "http://" + serverIP.toString() + ":8080";
        Serial.print("✅ Resolved k3s-node1.local to: ");
        Serial.println(serverIP);
        Serial.print("✅ API Base URL set to: ");
        Serial.println(resolved_api_base_url);
        
        fastSyslog.logf(LOG_INFO, "Resolved k3s-node1.local to %s", serverIP.toString().c_str());
    } else {
        // Failed to resolve, fallback to hardcoded IP
        Serial.println("❌ Failed to resolve k3s-node1.local via mDNS");
        Serial.println("🔄 Falling back to hardcoded IP address");
        resolved_api_base_url = String(api_base_url); // Use the original hardcoded URL
        
        fastSyslog.logf(LOG_WARNING, "mDNS resolution failed, using fallback IP");
    }
}

void connectToWiFi() {
    Serial.print("🔄 Connecting to WiFi...");
    WiFi.begin(ssid, password);

    // Wait for connection
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 30) { 
        Serial.print(".");
        delay(1000);  // 1-second delay
        attempts++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.print("\n✅ Connected to WiFi! IP Address: ");
        Serial.println(WiFi.localIP());
        
        // Initialize mDNS responder
        if (!MDNS.begin(MACHINE_ID)) {
            Serial.println("Error setting up MDNS responder!");
            while(1) {
                delay(1000);
            }
        }
        Serial.println("✅ mDNS responder started");
        
        // Initialize FastSyslog with hardcoded IP first (will be updated by resolveServerHostname)
        if (!fastSyslog.begin("10.100.219.138", 5140, "getraenkeautomat", "getraenkeautomat")) {
            Serial.println("Failed to initialize FastSyslog!");
        }
        
        // Resolve the server hostnames using mDNS
        resolveServerHostname();
    } else {
        Serial.println("\n❌ WiFi Connection Failed!");
    }
}





// Write 9-bit data
void write_9(uint16_t nth9) {
  digitalWrite(pin_mdb_tx, LOW); // start
  delayMicroseconds(104);

  for (uint8_t x = 0; x < 9; x++) {
    digitalWrite(pin_mdb_tx, (nth9 >> x) & 1);
    delayMicroseconds(104); // 9600bps
  }

  digitalWrite(pin_mdb_tx, HIGH); // stop
  delayMicroseconds(104);
}

// Transmit payload via 9-bit UART
void transmitPayloadByUART9(uint8_t *mdb_payload, uint8_t length) {
  uint8_t checksum = 0;
  for (int x = 0; x < length; x++) {
    checksum += mdb_payload[x];
    write_9(mdb_payload[x]);
  }

  // CHK* ACK*
  write_9(BIT_MODE_SET | checksum);
}



// Helper functions for cleaner code

// Format the UID bytes into a hex string
void formatUidString(const CardReader::Uid& uid, char* uidString, size_t maxLen) {
  snprintf(uidString, maxLen, "");
  for (byte i = 0; i < uid.size; i++) {
      snprintf(uidString + (i * 2), 3, "%02X", uid.uidByte[i]);
  }
}

// Wait until the card is removed from the reader
void waitForCardRemoval() {
  //Serial.println("Waiting for card removal...");
  while (cardReader.isCardPresent() && !reader_cancel_todo) {
      vTaskDelay(10 / portTICK_PERIOD_MS);
  }
  //Serial.println("Card removed");
}

// Wait for a specific machine state with timeout
bool waitForMachineState(MACHINE_STATE targetState, uint32_t timeoutMs) {
  uint32_t startTime = millis();
  while (machine_state != targetState) {
      // Check for global cancellation first
      if (reader_cancel_todo) {
          return false;  // Exit immediately on global cancel
      }
      
      if (millis() - startTime > timeoutMs) {
          return false;  // Timeout
      }
      vTaskDelay(10 / portTICK_PERIOD_MS);
  }
  return true;  // Success
}

// Process the full card transaction
void processCardTransaction(const char* uidString, const char* itemType) {
  // Try to get balance
  if (!getAndVerifyBalance(uidString)) {
      FAST_LOG_ERROR("Failed to check balance.");
      return;
  }
  
  // Wait for vend state - will exit immediately if reader_cancel_todo is set
  if (!waitForMachineState(VEND_STATE, 10000)) {
      if (reader_cancel_todo) {
          FAST_LOG_INFO("Transaction cancelled");
      } else {
          FAST_LOG_ERROR("❌ Machine didn't enter vend state in time");
      }
      return;
  }
  
  // Process the purchase
  int txId = processPurchase(uidString);
  FAST_LOG_DEBUG("processing purchase");
  
  // Wait for idle state - will exit immediately if reader_cancel_todo is set
  if (!waitForMachineState(IDLE_STATE, 10000)) {
      if (reader_cancel_todo) {
          FAST_LOG_INFO("Transaction cancelled");
      } else {
          Serial.println("❌ Machine didn't enter idle state in time");
      }
      return;
  }
  
  if (vend_success) {
      confirmPurchase(txId);
      vend_success = false;
  }
}


bool validate_mdb_checksum(uint8_t command, uint8_t *data, uint8_t data_len) {
    uint8_t calculated_checksum = command;
    
    // Add all data bytes except the last one (which is the received checksum)
    for (int i = 0; i < data_len - 1; i++) {
        calculated_checksum += data[i];
    }
    calculated_checksum &= 0xFF;
    
    // Compare with received checksum (last byte)
    uint8_t received_checksum = data[data_len - 1];
    
    if (calculated_checksum != received_checksum) {
        fastSyslog.logf(LOG_DEBUG, "Checksum error: calc=0x%02X, recv=0x%02X\n", 
               calculated_checksum, received_checksum);
        return false;
    }
    
    return true;
}


// Get and verify user balance
bool getAndVerifyBalance(const char* uidString) {
  // Try to get balance up to 3 times
  const int MAX_ATTEMPTS = 3;
  
  for (int attempts = 0; attempts < MAX_ATTEMPTS; attempts++) {
      current_user_balance = getUserBalance(uidString);
      
      if (current_user_balance >= 0) {
          Serial.printf("✅ Balance received: %d\n", current_user_balance);
          fastSyslog.logf(LOG_INFO, "✅ Balance received: %d", current_user_balance);
          session_begin_todo = true;
          return true;
      }
      
      fastSyslog.logf(LOG_ERR, "❌ Failed to get balance (attempt %d/%d)\n", attempts + 1, MAX_ATTEMPTS);
      vTaskDelay(500 / portTICK_PERIOD_MS);  // Wait before retry
  }
  
  Serial.println("❌ Failed to get balance after all attempts");
  return false;
}

// Process the purchase transaction
int processPurchase(const char* uidString) {
  //if (current_user_balance < current_item_price) {
  //    Serial.println("❌ Insufficient balance");
  //    vend_denied_todo = true;
  //    return -1;
  //}
  
  Serial.printf("Current Item Price: %d\n", current_item_price);
  
  // Attempt transaction
  int txId = makePurchase(uidString, current_item_price, current_item_number, MACHINE_ID);
  if (txId != -1) {
      Serial.println("✅ Transaction successful");
      vend_approved_todo = true;
      return txId;
  } else {
      Serial.println("❌ Transaction failed");
      vend_denied_todo = true;
      return -1;
  }
}


// Button handling task
void reader_loop(void *pvParameters) {
  CardReader::Uid uid;
  CardReader::CardSecret secret;
  bool isUltralightC;
  char uidString[21];

  Serial.println("Entering button_task...");
  FAST_LOG_DEBUG("entering button task");
  //reset_cashless_todo = true;
  vTaskDelay(1000 / portTICK_PERIOD_MS);

  for (;;) {
      // Wait for a card to be presented
      if (!cardReader.isCardPresent()) {
          vTaskDelay(50 / portTICK_PERIOD_MS);
          continue;
      }
      
      Serial.println("🔷 Card detected! Waiting before reading...");
      FAST_LOG_INFO("card detected");
      vTaskDelay(100 / portTICK_PERIOD_MS);

      // Try reading the card
      Result readResult = cardReader.read(uid, isUltralightC, secret);
      if (readResult != Result::OK) {
          FAST_LOG_ERROR("❌ Failed to read card");
          
          // Wait until card is removed before trying again
          waitForCardRemoval();
          continue;
      }

      //Serial.println("✅ Card Read Successfully!");

      // Format UID string
      formatUidString(uid, uidString, sizeof(uidString));
      //Serial.print("UID as String: ");
      fastSyslog.logf(LOG_INFO, "uid: %s", uidString);
      //Serial.println(uidString);
      //Serial.println(getUserBalance("0486A5DA826180"));
      // Wait for machine to be in enabled state
      if (!waitForMachineState(ENABLED_STATE, 5000) || reader_cancel_todo) {
          //Serial.println("❌ Machine not enabled in time");
          FAST_LOG_ERROR("❌ Machine not enabled in time");
          waitForCardRemoval();
          continue;
      }  // Return to default facility

      // Process the card transaction
      processCardTransaction(uidString, "testitem");
      
      // Wait for card removal before accepting a new card
      waitForCardRemoval();
      reader_cancel_todo = false;
      session_end_todo = true;
      //Serial.println("🔄 Ready for next read...");
      
      // Add a small delay before next loop iteration
      vTaskDelay(200 / portTICK_PERIOD_MS);
  }
}

















// MDB communication task
void mdb_loop(void *pvParameters) {
  uint8_t mdb_payload[256];  // Declared as a single array
  uint8_t available_tx = 0;

  for (;;) {
    uint8_t checksum = 0x00;
    // MDB requires response within 5ms - use minimal delay to prevent task starvation
    taskYIELD();  // Allow other tasks to run if needed, but don't block

    uint16_t coming_read = read_9(&checksum);
    unsigned long cmd_received_time = micros();  // Time when we have the command byte

    if (coming_read & BIT_MODE_SET) {
      if ((uint8_t) coming_read == ACK) {
        // ACK handling
      } else if ((uint8_t) coming_read == RET) {
        // Retransmit handling
      } else if ((uint8_t) coming_read == NAK) {
        // Negative Acknowledge handling
      } else if((coming_read & BIT_ADD_SET) == 0x10) {
        digitalWrite(pin_mdb_led, HIGH);
        available_tx = 0;

        switch (coming_read & BIT_CMD_SET) {
          case RESET: {
            read_9((uint8_t*) 0);

            MACHINE_STATE prev_state = machine_state;
            machine_state = INACTIVE_STATE;
            reset_cashless_todo = true;

            // Clear any pending flags to ensure clean state
            outsequence_todo = false;
            vend_approved_todo = false;
            vend_denied_todo = false;
            session_end_todo = false;
            session_begin_todo = false;
            session_cancel_todo = false;

            // Log after state change to avoid timing delays
            if (prev_state == VEND_STATE) {
              fastSyslog.logf(LOG_WARNING, "MDB: RESET during VEND (cmd:0x%03X)", coming_read);
            } else {
              fastSyslog.logf(LOG_INFO, "MDB: RESET from VMC (cmd:0x%03X prev:%d)", coming_read, prev_state);
            }
            break;
          }
          case SETUP: {
            uint16_t sub_cmd_raw = read_9((uint8_t*) 0);  // Don't accumulate checksum
            uint8_t sub_cmd = (uint8_t)(sub_cmd_raw & 0xFF);

            switch (sub_cmd) {
              case CONFIG_DATA: {
                uint8_t setup_data[5]; // 4 data bytes + 1 checksum byte

                // Read all data including received checksum (don't use auto-accumulation)
                for (int i = 0; i < 5; i++) {
                    uint16_t raw = read_9((uint8_t*) 0);
                    setup_data[i] = (uint8_t)(raw & 0xFF);
                }

                // Calculate checksum manually: command + subcommand + data bytes
                // MDB spec: checksum = sum of all bytes (command + subcommand + data)
                uint8_t command_byte = (uint8_t)(coming_read & 0xFF);
                uint8_t calc_checksum = command_byte + sub_cmd;
                for (int i = 0; i < 4; i++) {
                    calc_checksum += setup_data[i];
                }
                calc_checksum &= 0xFF;

                // Validate checksum
                if (calc_checksum != setup_data[4]) {
                    // Log AFTER NAK is sent to avoid timing delays
                    write_9(NAK);
                    fastSyslog.logf(LOG_ERR, "MDB: CONFIG_DATA fail cmd:0x%03X data:[%02X %02X %02X %02X] calc:0x%02X recv:0x%02X",
                        coming_read, setup_data[0], setup_data[1], setup_data[2], setup_data[3], calc_checksum, setup_data[4]);
                    break;
                }

                uint8_t vmcFeatureLevel = setup_data[0];
                uint8_t vmcColumnsOnDisplay = setup_data[1];
                uint8_t vmcRowsOnDisplay = setup_data[2];
                uint8_t vmcDisplayInfo = setup_data[3];

                machine_state = DISABLED_STATE;

                // Peripheral Configuration Response
                mdb_payload[0] = 0x01;       // Reader Config Data
                mdb_payload[1] = 1;          // Reader Feature Level (1, 2, 3)
                mdb_payload[2] = 0xff;       // Country Code High
                mdb_payload[3] = 0xff;       // Country Code Low
                mdb_payload[4] = 1;          // Scale Factor
                mdb_payload[5] = 2;          // Decimal Places
                mdb_payload[6] = 5;          // Application Maximum Response Time (5s)
                mdb_payload[7] = 0b00001001; // Miscellaneous Options

                available_tx = 8;
                break;
              }
              case MAX_MIN_PRICES: {
                uint8_t price_data[5]; // 4 data bytes + 1 checksum byte

                // Read all data including received checksum (don't use auto-accumulation)
                for (int i = 0; i < 5; i++) {
                    uint16_t raw = read_9((uint8_t*) 0);
                    price_data[i] = (uint8_t)(raw & 0xFF);
                }

                // Calculate checksum manually: command + subcommand + data bytes
                uint8_t calc_checksum = (uint8_t)(coming_read & 0xFF) + sub_cmd;
                for (int i = 0; i < 4; i++) {
                    calc_checksum += price_data[i];
                }
                calc_checksum &= 0xFF;

                // Validate checksum
                if (calc_checksum != price_data[4]) {
                    fastSyslog.logf(LOG_ERR, "MDB: MAX_MIN_PRICES checksum fail calc:0x%02X recv:0x%02X", calc_checksum, price_data[4]);
                    write_9(NAK);
                    break;
                }
                
                uint16_t maxPrice = (price_data[0] << 8) | price_data[1];
                uint16_t minPrice = (price_data[2] << 8) | price_data[3];
                break;
              }
            }
            break;
          }
          case POLL: {
            read_9((uint8_t*) 0);
            
            // Periodic state logging and stuck state detection
            static uint16_t poll_counter = 0;
            static unsigned long last_state_change = 0;
            poll_counter++;
            
            if (poll_counter % 500 == 0) {
                fastSyslog.logf(LOG_INFO, "MDB: POLL #%d State:%d", poll_counter, machine_state);
            }
            
            // Detect if stuck in DISABLED_STATE for too long
            if (machine_state == DISABLED_STATE && (millis() - last_state_change > 60000)) {
                fastSyslog.logf(LOG_ERR, "MDB: Stuck in DISABLED_STATE 60s - forcing recovery");
                outsequence_todo = true;
                last_state_change = millis();
            }

            if (outsequence_todo) {
              outsequence_todo = false;
              mdb_payload[0] = 0x0b; // Command Out of Sequence
              available_tx = 1;
            } else if (reset_cashless_todo) {
              reset_cashless_todo = false;
              mdb_payload[0] = 0x00; // Just Reset
              available_tx = 1;
              last_state_change = millis();
              fastSyslog.logf(LOG_DEBUG, "MDB: Just Reset sent (s:%d)", machine_state);
            } else if (vend_approved_todo) {
              vend_approved_todo = false;
              uint16_t vendAmount = to_scale_factor(0.00, 1, 2);
              mdb_payload[0] = 0x05;              // Vend Approved
              mdb_payload[1] = vendAmount >> 8;   // Vend Amount High
              mdb_payload[2] = vendAmount;        // Vend Amount Low
              available_tx = 3;
            } else if (vend_denied_todo) {
              vend_denied_todo = false;
              mdb_payload[0] = 0x06; // Vend Denied
              available_tx = 1;
              machine_state = IDLE_STATE;
              last_state_change = millis();
            } else if (session_end_todo) {
              session_end_todo = false;
              mdb_payload[0] = 0x07; // End Session
              available_tx = 1;
              machine_state = ENABLED_STATE;
              last_state_change = millis();
            } else if (session_begin_todo) {
              session_begin_todo = false;
              machine_state = IDLE_STATE;
              last_state_change = millis();

              uint16_t fundsAvailable = 0;
              if (current_user_balance > 0) { 
                fundsAvailable = current_user_balance;
              }
              else fundsAvailable = 1;
              mdb_payload[0] = 0x03; // Begin Session
              mdb_payload[1] = fundsAvailable >> 8;
              mdb_payload[2] = fundsAvailable;
              available_tx = 3;
            } else if (session_cancel_todo) {
              session_cancel_todo = false;
              mdb_payload[0] = 0x04; // Session Cancel Request
              available_tx = 1;
            }
            break;
          }
          case VEND: {
            uint16_t sub_cmd_raw = read_9((uint8_t*) 0);  // Don't accumulate checksum
            uint8_t sub_cmd = (uint8_t)(sub_cmd_raw & 0xFF);

            switch(sub_cmd) {
              case VEND_REQUEST: {
                uint8_t vend_data[5]; // 4 data bytes + 1 checksum byte

                // Read all data including received checksum (don't use auto-accumulation)
                for (int i = 0; i < 5; i++) {
                    uint16_t raw = read_9((uint8_t*) 0);
                    vend_data[i] = (uint8_t)(raw & 0xFF);
                }

                // Calculate checksum manually: command + subcommand + data bytes
                uint8_t calc_checksum = (uint8_t)(coming_read & 0xFF) + sub_cmd;
                for (int i = 0; i < 4; i++) {
                    calc_checksum += vend_data[i];
                }
                calc_checksum &= 0xFF;

                // Validate checksum
                if (calc_checksum != vend_data[4]) {
                    fastSyslog.logf(LOG_ERR, "MDB: VEND_REQUEST checksum fail calc:0x%02X recv:0x%02X", calc_checksum, vend_data[4]);
                    write_9(NAK);
                    break;
                }
                
                uint16_t itemPrice = (vend_data[0] << 8) | vend_data[1];
                uint16_t itemNumber = (vend_data[2] << 8) | vend_data[3];
                current_item_price = itemPrice;
                current_item_number = itemNumber;
                machine_state = VEND_STATE;
                break;
              }
              case VEND_CANCEL: {
                uint8_t checksum_data[1];

                uint16_t raw = read_9((uint8_t*) 0);
                checksum_data[0] = (uint8_t)(raw & 0xFF);

                // Calculate checksum manually: command + subcommand
                uint8_t calc_checksum = ((uint8_t)(coming_read & 0xFF) + sub_cmd) & 0xFF;

                if (calc_checksum != checksum_data[0]) {
                    fastSyslog.logf(LOG_ERR, "MDB: VEND_CANCEL checksum fail calc:0x%02X recv:0x%02X", calc_checksum, checksum_data[0]);
                    write_9(NAK);
                    break;
                }

                vend_denied_todo = true;
                break;
              }
              case VEND_SUCCESS: {
                uint8_t success_data[3]; // 2 data bytes + 1 checksum byte

                // Read all data including received checksum (don't use auto-accumulation)
                for (int i = 0; i < 3; i++) {
                    uint16_t raw = read_9((uint8_t*) 0);
                    success_data[i] = (uint8_t)(raw & 0xFF);
                }

                // Calculate checksum manually: command + subcommand + data bytes
                uint8_t calc_checksum = (uint8_t)(coming_read & 0xFF) + sub_cmd;
                for (int i = 0; i < 2; i++) {
                    calc_checksum += success_data[i];
                }
                calc_checksum &= 0xFF;

                // Validate checksum
                if (calc_checksum != success_data[2]) {
                    fastSyslog.logf(LOG_ERR, "MDB: VEND_SUCCESS checksum fail calc:0x%02X recv:0x%02X", calc_checksum, success_data[2]);
                    write_9(NAK);
                    break;
                }
                
                uint16_t itemNumber = (success_data[0] << 8) | success_data[1];
                vend_success = true;
                cardReader.endCard();
                machine_state = IDLE_STATE;
                break;
              }
              case VEND_FAILURE: {
                uint8_t checksum_data[1];

                uint16_t raw = read_9((uint8_t*) 0);
                checksum_data[0] = (uint8_t)(raw & 0xFF);

                // Calculate checksum manually: command + subcommand
                uint8_t calc_checksum = ((uint8_t)(coming_read & 0xFF) + sub_cmd) & 0xFF;

                if (calc_checksum != checksum_data[0]) {
                    fastSyslog.logf(LOG_ERR, "MDB: VEND_FAILURE checksum fail calc:0x%02X recv:0x%02X", calc_checksum, checksum_data[0]);
                    write_9(NAK);
                    break;
                }

                vend_success = false;
                machine_state = IDLE_STATE;
                break;
              }
              case SESSION_COMPLETE: {
                uint8_t checksum_data[1];

                uint16_t raw = read_9((uint8_t*) 0);
                checksum_data[0] = (uint8_t)(raw & 0xFF);

                // Calculate checksum manually: command + subcommand
                uint8_t calc_checksum = ((uint8_t)(coming_read & 0xFF) + sub_cmd) & 0xFF;

                if (calc_checksum != checksum_data[0]) {
                    fastSyslog.logf(LOG_ERR, "MDB: SESSION_COMPLETE checksum fail calc:0x%02X recv:0x%02X", calc_checksum, checksum_data[0]);
                    write_9(NAK);
                    break;
                }

                session_end_todo = true;
                break;
              }
              case CASH_SALE: {
                uint8_t cash_data[5]; // 4 data bytes + 1 checksum byte

                // Read all data including received checksum (don't use auto-accumulation)
                for (int i = 0; i < 5; i++) {
                    uint16_t temp = read_9((uint8_t*) 0);
                    cash_data[i] = (uint8_t)(temp & 0xFF);
                }

                // Calculate checksum manually: command + subcommand + data bytes
                uint8_t calc_checksum = (uint8_t)(coming_read & 0xFF) + sub_cmd;
                for (int i = 0; i < 4; i++) {
                    calc_checksum += cash_data[i];
                }
                calc_checksum &= 0xFF;

                // Validate checksum
                if (calc_checksum != cash_data[4]) {
                    fastSyslog.logf(LOG_ERR, "MDB: CASH_SALE checksum fail calc:0x%02X recv:0x%02X", calc_checksum, cash_data[4]);
                    write_9(NAK);
                    break;
                }

                uint16_t itemPrice = (cash_data[0] << 8) | cash_data[1];
                uint16_t itemNumber = (cash_data[2] << 8) | cash_data[3];
                CashSale_t cashsale_data;
                cashsale_data.itemNumber = itemNumber;
                cashsale_data.itemPrice = itemPrice;
                xQueueSend(cashSaleQueue, &cashsale_data, 0);
                break;
              }
            }
            break;
          }
          case READER: {
            uint16_t sub_cmd_raw = read_9((uint8_t*) 0);  // Don't accumulate checksum
            uint8_t sub_cmd = (uint8_t)(sub_cmd_raw & 0xFF);

            switch(sub_cmd) {
              case READER_DISABLE: {
                uint8_t checksum_data[1];

                uint16_t raw = read_9((uint8_t*) 0);
                checksum_data[0] = (uint8_t)(raw & 0xFF);

                // Calculate checksum manually: command + subcommand
                uint8_t calc_checksum = ((uint8_t)(coming_read & 0xFF) + sub_cmd) & 0xFF;

                if (calc_checksum != checksum_data[0]) {
                    fastSyslog.logf(LOG_ERR, "MDB: READER_DISABLE checksum fail calc:0x%02X recv:0x%02X", calc_checksum, checksum_data[0]);
                    write_9(NAK);
                    break;
                }

                machine_state = DISABLED_STATE;
                break;
              }
              case READER_ENABLE: {
                uint8_t checksum_data[1];

                uint16_t raw = read_9((uint8_t*) 0);
                checksum_data[0] = (uint8_t)(raw & 0xFF);

                // Calculate checksum manually: command + subcommand
                uint8_t calc_checksum = ((uint8_t)(coming_read & 0xFF) + sub_cmd) & 0xFF;

                if (calc_checksum != checksum_data[0]) {
                    fastSyslog.logf(LOG_ERR, "MDB: READER_ENABLE checksum fail calc:0x%02X recv:0x%02X", calc_checksum, checksum_data[0]);
                    write_9(NAK);
                    break;
                }

                machine_state = ENABLED_STATE;
                break;
              }
              case READER_CANCEL: {
                uint8_t checksum_data[1];

                uint16_t raw = read_9((uint8_t*) 0);
                checksum_data[0] = (uint8_t)(raw & 0xFF);

                // Calculate checksum manually: command + subcommand
                uint8_t calc_checksum = ((uint8_t)(coming_read & 0xFF) + sub_cmd) & 0xFF;

                if (calc_checksum != checksum_data[0]) {
                    fastSyslog.logf(LOG_ERR, "MDB: READER_CANCEL checksum fail calc:0x%02X recv:0x%02X", calc_checksum, checksum_data[0]);
                    write_9(NAK);
                    break;
                }

                mdb_payload[0] = 0x08; // Canceled
                available_tx = 1;
                break;
              }
            }
            break;
          }
          case EXPANSION: {
            uint16_t sub_cmd_raw = read_9((uint8_t*) 0);  // Don't accumulate checksum
            uint8_t sub_cmd = (uint8_t)(sub_cmd_raw & 0xFF);

            switch(sub_cmd) {
              case REQUEST_ID: {
                uint8_t id_data[30]; // 29 data bytes + 1 checksum byte

                // Read all data including received checksum (don't use auto-accumulation)
                for (int i = 0; i < 30; i++) {
                    uint16_t raw = read_9((uint8_t*) 0);
                    id_data[i] = (uint8_t)(raw & 0xFF);
                }

                // Log first few bytes and checksum for debugging
                fastSyslog.logf(LOG_DEBUG, "MDB: REQUEST_ID cmd:0x%03X sub:0x%02X data:[%02X %02X %02X...] chk:0x%02X",
                    coming_read, sub_cmd, id_data[0], id_data[1], id_data[2], id_data[29]);

                // Calculate checksum manually: command + subcommand + data bytes
                // MDB spec: checksum includes full command byte (address + command bits)
                uint8_t command_byte = (uint8_t)(coming_read & 0xFF);
                uint8_t calc_checksum = command_byte + sub_cmd;
                for (int i = 0; i < 29; i++) {
                    calc_checksum += id_data[i];
                }
                calc_checksum &= 0xFF;

                // Validate checksum
                if (calc_checksum != id_data[29]) {
                    fastSyslog.logf(LOG_ERR, "MDB: REQUEST_ID checksum fail cmd_byte:0x%02X calc:0x%02X recv:0x%02X",
                        command_byte, calc_checksum, id_data[29]);
                    write_9(NAK);
                    break;
                }

                fastSyslog.logf(LOG_INFO, "MDB: REQUEST_ID success");

                // Peripheral ID Response
                mdb_payload[0] = 0x09; // Peripheral ID

                // Fill with spaces as per MDB protocol
                // Manufacturer Code
                mdb_payload[1] = ' ';
                mdb_payload[2] = ' ';
                mdb_payload[3] = ' ';

                // Serial Number
                for (int i = 0; i < 12; i++) {
                  mdb_payload[4 + i] = ' ';
                }

                // Model Number
                for (int i = 0; i < 12; i++) {
                  mdb_payload[16 + i] = ' ';
                }

                // Software Version
                mdb_payload[28] = ' ';
                mdb_payload[29] = ' ';

                available_tx = 30;
                break;
              }
            }
            break;
          }
        }

        // Send response: either data payload or ACK
        if (available_tx > 0) {
          // Send data response
          transmitPayloadByUART9(mdb_payload, available_tx);
        } else {
          // No data to send - send ACK to acknowledge command
          write_9(BIT_MODE_SET | ACK);
        }

        // Log response time for POLL commands (every 1000th to avoid spam)
        static uint16_t timing_counter = 0;
        if ((coming_read & BIT_CMD_SET) == POLL && (++timing_counter % 1000) == 0) {
          unsigned long response_time = micros() - cmd_received_time;
          fastSyslog.logf(LOG_INFO, "MDB: POLL response time: %lu us", response_time);
        }
      } else {
        digitalWrite(pin_mdb_led, LOW);
      }
    }
  }
}
void wifi_loop(void *pvParameters) {
    for (;;) {
        // Periodic check
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("🔴 WiFi Disconnected! Attempting reconnect...");
            connectToWiFi();
        } 
        //ArduinoOTA.handle();
        vTaskDelay(10000 / portTICK_PERIOD_MS);  // Check every 10 seconds
    }
}


void mdb_state_watchdog() {
  static unsigned long last_poll_time = 0;
  static unsigned long last_state_change = 0;
  
  // If no POLL received for 10 seconds, something is wrong
  if (millis() - last_poll_time > 10000) {
    Serial.println("MDB: No POLL for 10s - forcing state reset");
    machine_state = INACTIVE_STATE;
    reset_cashless_todo = true;
  }
  
  // If stuck in same state for too long (except ENABLED which is normal)
  if (machine_state != ENABLED_STATE && millis() - last_state_change > 30000) {
    Serial.println("MDB: Stuck in state - forcing reset");
    machine_state = INACTIVE_STATE;
    reset_cashless_todo = true;
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

void setup() {

  
  // Pin modes
  pinMode(pin_mdb_rx, INPUT);
  pinMode(pin_mdb_tx, OUTPUT);
  pinMode(pin_mdb_led, OUTPUT);

cashSaleQueue = xQueueCreate(10, sizeof(CashSale_t));

  Serial.begin(115200);

    xTaskCreatePinnedToCore(
    mdb_loop,     // Task function
    "mdb_loop",   // Task name
    16384,        // Stack size 
    NULL,         // Parameters
    3,            // Priority
    NULL,          // Task handle
    1
  );

    Result initResult = cardReader.begin();
      if (initResult == Result::OK) {
    Serial.println("Card Reader initialized successfully.");
  } else {
    Serial.println("Card Reader initialization failed.");
  }

  connectToWiFi();
  setupOTA("TemplateSketch", ssid, password);
  //syslog.server("10.100.217.225", 5140);
  //syslog.deviceHostname(MACHINE_ID);
  //syslog.appName(MACHINE_ID);
  //syslog.defaultPriority(LOG_KERN);

      if (!fastSyslog.begin("10.100.1.55", 5140, "getraenkeautomat", "getraenkeautomat")) {
        Serial.println("Failed to initialize FastSyslog!");
        return;
    }

  Serial.println("starting up");
  FAST_LOG_INFO("starting up");
  // Create tasks


 



    // Create WiFi Monitoring Task
    xTaskCreatePinnedToCore(
        wifi_loop,      // Task function
        "wifi_loop",    // Task name
        4096,           // Stack size
        NULL,           // Parameters
        1,              // Priority
        NULL,            // Task handle
        0
    );
 


  xTaskCreatePinnedToCore(
    reader_loop,  // Task function
    "reader_loop",// Task name
    8192,         // Stack size
    NULL,         // Parameters
    1,            // Priority
    NULL,          // Task handle
    0
  );

xTaskCreate(
  cashsale_handler,
  "cashsale_handler",
  4096,
  NULL,
  1,
  NULL
);
}

void loop() {
  
}
