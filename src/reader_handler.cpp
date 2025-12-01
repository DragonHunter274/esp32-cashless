#include "reader_handler.h"
#include "mdb_comm.h"
#include "api_client.h"
#include "FastSyslog.h"
#include "secrets.h"

extern CardReader cardReader;

// Format the UID bytes into a hex string
void formatUidString(const CardReader::Uid& uid, char* uidString, size_t maxLen) {
  snprintf(uidString, maxLen, "");
  for (byte i = 0; i < uid.size; i++) {
      snprintf(uidString + (i * 2), 3, "%02X", uid.uidByte[i]);
  }
}

// Wait until the card is removed from the reader
void waitForCardRemoval() {
  while (cardReader.isCardPresent() && !reader_cancel_todo) {
      vTaskDelay(10 / portTICK_PERIOD_MS);
  }
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
          FAST_LOG_ERROR("Machine didn't enter vend state in time");
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
          Serial.println("Machine didn't enter idle state in time");
      }
      return;
  }

  if (vend_success) {
      confirmPurchase(txId);
      vend_success = false;
  }
}

// Get and verify user balance
bool getAndVerifyBalance(const char* uidString) {
  // Try to get balance up to 3 times
  const int MAX_ATTEMPTS = 3;

  for (int attempts = 0; attempts < MAX_ATTEMPTS; attempts++) {
      current_user_balance = getUserBalance(uidString);

      if (current_user_balance >= 0) {
          Serial.printf("Balance received: %d\n", current_user_balance);
          fastSyslog.logf(LOG_INFO, "Balance received: %d", current_user_balance);
          session_begin_todo = true;
          return true;
      }

      fastSyslog.logf(LOG_ERR, "Failed to get balance (attempt %d/%d)\n", attempts + 1, MAX_ATTEMPTS);
      vTaskDelay(500 / portTICK_PERIOD_MS);  // Wait before retry
  }

  Serial.println("Failed to get balance after all attempts");
  return false;
}

// Process the purchase transaction
int processPurchase(const char* uidString) {
  Serial.printf("Current Item Price: %d\n", current_item_price);

  // Attempt transaction
  int txId = makePurchase(uidString, current_item_price, current_item_number, MACHINE_ID);
  if (txId != -1) {
      Serial.println("Transaction successful");
      vend_approved_todo = true;
      return txId;
  } else {
      Serial.println("Transaction failed");
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

  vTaskDelay(1000 / portTICK_PERIOD_MS);

  for (;;) {
      // Wait for a card to be presented
      if (!cardReader.isCardPresent()) {
          vTaskDelay(50 / portTICK_PERIOD_MS);
          continue;
      }

      Serial.println("Card detected! Waiting before reading...");
      FAST_LOG_INFO("card detected");
      vTaskDelay(100 / portTICK_PERIOD_MS);

      // Try reading the card
      Result readResult = cardReader.read(uid, isUltralightC, secret);
      if (readResult != Result::OK) {
          FAST_LOG_ERROR("Failed to read card");

          // Wait until card is removed before trying again
          waitForCardRemoval();
          continue;
      }

      // Format UID string
      formatUidString(uid, uidString, sizeof(uidString));

      fastSyslog.logf(LOG_INFO, "uid: %s", uidString);
      // Wait for machine to be in enabled state
      if (!waitForMachineState(ENABLED_STATE, 5000) || reader_cancel_todo) {
          FAST_LOG_ERROR("Machine not enabled in time");
          waitForCardRemoval();
          continue;
      }

      // Process the card transaction
      processCardTransaction(uidString, "testitem");

      // Wait for card removal before accepting a new card
      waitForCardRemoval();
      reader_cancel_todo = false;
      session_end_todo = true;

      // Add a small delay before next loop iteration
      vTaskDelay(200 / portTICK_PERIOD_MS);
  }
}
