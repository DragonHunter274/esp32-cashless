#pragma once

#include <Arduino.h>
#include <FreeRTOS.h>
#include "mdb_protocol.h"

// Pin Definitions
#define pin_mdb_rx  GPIO_NUM_4
#define pin_mdb_tx  GPIO_NUM_5
#define pin_mdb_led GPIO_NUM_13

// UART Configuration
#define MDB_UART_NUM UART_NUM_2

// MDB Communication Data Structure
typedef struct {
  uint16_t itemPrice;
  uint16_t itemNumber;
} CashSale_t;

// Global Variables (extern declarations)
extern volatile MACHINE_STATE machine_state;
extern portMUX_TYPE mdb_mux;

extern volatile bool cashless_reset_todo;
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
void write_9(uint16_t nth9);
void write_payload_9(uint8_t *mdb_payload, uint8_t length);
void mdb_loop(void *pvParameters);
void mdb_cashless_loop(void *pvParameters);
