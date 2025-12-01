#pragma once

#include <Arduino.h>
#include <FreeRTOS.h>
#include "mdb_protocol.h"

// Pin Definitions
#define pin_mdb_rx  4
#define pin_mdb_tx  5
#define pin_mdb_led 13

// MDB Communication Data Structure
typedef struct {
  uint16_t itemPrice;
  uint16_t itemNumber;
} CashSale_t;

// Global Variables (extern declarations)
extern volatile MACHINE_STATE machine_state;
extern portMUX_TYPE mdb_mux;

extern volatile bool reset_cashless_todo;
extern volatile bool session_begin_todo;
extern volatile bool session_end_todo;
extern volatile bool session_cancel_todo;
extern volatile bool vend_approved_todo;
extern volatile bool vend_denied_todo;
extern volatile bool outsequence_todo;
extern volatile bool reader_cancel_todo;

extern uint16_t current_item_price;
extern uint16_t current_item_number;
extern int current_user_balance;
extern bool vend_success;

// Function declarations
void mdb_init();
uint16_t read_9(uint8_t *checksum, bool wait_forever = true);
void write_9(uint16_t nth9);
void transmitPayloadByUART9(uint8_t *mdb_payload, uint8_t length);
void mdb_loop(void *pvParameters);
bool validate_mdb_checksum(uint8_t command, uint8_t *data, uint8_t data_len);
void mdb_state_watchdog();
