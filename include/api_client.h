#pragma once

#include <Arduino.h>

// Function declarations for API communication
void connectToWiFi();
void resolveServerHostname();
void wifi_loop(void *pvParameters);

int getUserBalance(const char* uid);
int makePurchase(const char* uid, int amount, int product, const char* machine_id);
int makeCashPurchase(int amount, int product, const char* machine_id);
bool confirmPurchase(int transactionId);

// Cash sale handler task
void cashsale_handler(void *pvParameters);
