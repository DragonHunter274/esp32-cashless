#pragma once

#include <Arduino.h>
#include "cardreader.h"
#include "mdb_protocol.h"

// Function declarations for reader handling
void formatUidString(const CardReader::Uid& uid, char* uidString, size_t maxLen);
void waitForCardRemoval();
bool waitForMachineState(MACHINE_STATE targetState, uint32_t timeoutMs);
void processCardTransaction(const char* uidString, const char* itemType);
bool getAndVerifyBalance(const char* uidString);
int processPurchase(const char* uidString);
void reader_loop(void *pvParameters);
