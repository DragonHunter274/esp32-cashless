#include "mdb_comm.h"
#include "FastSyslog.h"
#include <queue.h>
#include <driver/uart.h>
#include <driver/gpio.h>

// Global Variables definitions
volatile MACHINE_STATE machine_state = INACTIVE_STATE;
portMUX_TYPE mdb_mux = portMUX_INITIALIZER_UNLOCKED;

volatile bool cashless_reset_todo = false;
volatile bool session_begin_todo = false;
volatile bool session_end_todo = false;
volatile bool session_cancel_todo = false;
volatile bool vend_approved_todo = false;
volatile bool vend_denied_todo = false;
volatile bool outsequence_todo = false;
volatile bool reader_cancel_todo = false;

uint16_t current_item_price = 0;
uint16_t current_item_number = 999;
int current_user_balance = 0;
bool vend_success = false;

extern QueueHandle_t cashSaleQueue;

void mdb_init() {
  // Configure GPIO for LED
  gpio_set_direction(pin_mdb_led, GPIO_MODE_OUTPUT);

  // Initialize UART2 driver and configure MDB pins
  uart_config_t uart_config = {
    .baud_rate = 9600,
    .data_bits = UART_DATA_8_BITS,
    .parity = UART_PARITY_EVEN,
    .stop_bits = UART_STOP_BITS_1,
    .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    .rx_flow_ctrl_thresh = 0,
  };

  uart_param_config(MDB_UART_NUM, &uart_config);
  uart_set_pin(MDB_UART_NUM, pin_mdb_tx, pin_mdb_rx, -1, -1);
  uart_driver_install(MDB_UART_NUM, 256, 256, 0, NULL, 0);
}

// Write 9-bit data using parity bit trick
// This is the clever VMflow approach - uses hardware UART parity bit to send the 9th bit
void write_9(uint16_t nth9) {
  uint8_t ones = __builtin_popcount((uint8_t)nth9);

  uart_wait_tx_done(MDB_UART_NUM, pdMS_TO_TICKS(250));

  // Use the parity bit to send the mode bit
  if ((nth9 >> 8) & 1) {
    // Mode bit is 1 - flip parity to encode it
    uart_set_parity(MDB_UART_NUM, ones % 2 ? UART_PARITY_EVEN : UART_PARITY_ODD);
  } else {
    // Mode bit is 0 - keep normal parity
    uart_set_parity(MDB_UART_NUM, ones % 2 ? UART_PARITY_ODD : UART_PARITY_EVEN);
  }

  uart_write_bytes(MDB_UART_NUM, (uint8_t*)&nth9, 1);
}

// Transmit payload via 9-bit UART
void write_payload_9(uint8_t *mdb_payload, uint8_t length) {
  uint8_t checksum = 0x00;

  // Calculate checksum
  for (int x = 0; x < length; x++) {
    checksum += mdb_payload[x];
    write_9(mdb_payload[x]);
  }

  // CHK* ACK*
  write_9(BIT_MODE_SET | checksum);
}

// New VMflow-style MDB communication task using hardware UART
void mdb_cashless_loop(void *pvParameters) {
  uint16_t fundsAvailable = 0;
  uint16_t itemPrice = 0;
  uint16_t itemNumber = 0;

  // Payload buffer and available transmission flag
  uint8_t mdb_payload_tx[36];
  uint8_t available_tx = 0;

  uint8_t mdb_payload_rx[36];
  uint8_t available_rx = 0;

  for (;;) {
    // Read first byte with infinite timeout
    available_rx = uart_read_bytes(MDB_UART_NUM, mdb_payload_rx, 1, portMAX_DELAY);

    // Skip ACK/NAK/RET single-byte responses
    if (mdb_payload_rx[0] == ACK) {
      continue;
    } else if (mdb_payload_rx[0] == NAK) {
      continue;
    } else if (mdb_payload_rx[0] == RET) {
      continue;
    }

    // Read remaining bytes with 10ms timeout per byte
    size_t len;
    while ((len = uart_read_bytes(MDB_UART_NUM, mdb_payload_rx + available_rx, 1, pdMS_TO_TICKS(10))) > 0) {
      available_rx += len;
    }

    // Check if this message is addressed to us
    // 0x10 = Cashless Device #1
    // 0x18 = Communications Gateway
    uint8_t device_address = mdb_payload_rx[0] & BIT_ADD_SET;

    if (device_address == 0x10 || device_address == 0x18) {

      // Validate checksum
      // MDB checksum = sum of all bytes before the checksum byte
      // Even single-byte commands (RESET, POLL) send a checksum (which equals the command)
      // Packet format: [command] [subcommand?] [data...?] [checksum]
      if (available_rx < 2) {
        fastSyslog.logf(LOG_ERR, "MDB: Incomplete packet len=%d cmd=0x%02X", available_rx, mdb_payload_rx[0]);
        continue;
      }

      uint8_t chk = 0x00;
      for (int x = 0; x < (available_rx - 1); x++) {
        chk += mdb_payload_rx[x];
      }

      if (chk != mdb_payload_rx[available_rx - 1]) {
        fastSyslog.logf(LOG_ERR, "MDB: CHK invalid calc=0x%02X recv=0x%02X len=%d cmd=0x%02X",
                        chk, mdb_payload_rx[available_rx - 1], available_rx, mdb_payload_rx[0]);
        continue;
      }

      // Intended address - turn on LED
      gpio_set_level(pin_mdb_led, 1);

      available_tx = 0;

      // Handle Communications Gateway (0x18)
      if (device_address == 0x18) {
        uint8_t command = mdb_payload_rx[0] & BIT_CMD_SET;

        switch (command) {
          case RESET:
            fastSyslog.logf(LOG_INFO, "MDB: Gateway RESET");
            // Reset gateway state if needed
            break;

          case SETUP: {
            // Parse VMC SETUP command
            uint8_t vmcFeatureLevel = mdb_payload_rx[2];     // Y1
            uint8_t vmcScaleFactor = mdb_payload_rx[3];      // Y2
            uint8_t vmcDecimalPlaces = mdb_payload_rx[4];    // Y3

            fastSyslog.logf(LOG_INFO, "MDB: Gateway SETUP feat=%d scale=%d dec=%d",
                           vmcFeatureLevel, vmcScaleFactor, vmcDecimalPlaces);

            // Respond with Gateway Configuration
            mdb_payload_tx[0] = 0x01;   // Z1: COMMS GATEWAY CONFIGURATION
            mdb_payload_tx[1] = 0x03;   // Z2: Gateway feature level (level 3)
            mdb_payload_tx[2] = 0x00;   // Z3: Application max response time high byte
            mdb_payload_tx[3] = 0x05;   // Z4: Application max response time (5 seconds)
            available_tx = 4;
            break;
          }

          case POLL:
            // Gateway POLL - respond with ACK (no data) or status if needed
            fastSyslog.logf(LOG_DEBUG, "MDB: Gateway POLL");
            break;

          case EXPANSION: {
            // Check for REQUEST_ID subcommand (0x00)
            if (mdb_payload_rx[1] == 0x00) {
              fastSyslog.logf(LOG_INFO, "MDB: Gateway EXPANSION REQUEST_ID");

              // Respond with Peripheral ID
              mdb_payload_tx[0] = 0x06;   // Z1: PERIPHERAL ID

              // Z2-Z4: Manufacturer code (3 bytes ASCII)
              strncpy((char*)&mdb_payload_tx[1], "FAB", 3);

              // Z5-Z16: Serial number (12 bytes numeric ASCII)
              strncpy((char*)&mdb_payload_tx[4], "202501000001", 12);

              // Z17-Z28: Model number (12 bytes ASCII)
              strncpy((char*)&mdb_payload_tx[16], "GWMDB-ESP32 ", 12);

              // Z29-Z30: Software version (2 bytes BCD)
              mdb_payload_tx[28] = 0x01;  // Version 1.0
              mdb_payload_tx[29] = 0x00;

              // Z31-Z34: Optional Features (4 bytes, little-endian 32-bit)
              // b0: File transport layer support (0 = not supported)
              // b1: Verbose mode (1 = supported)
              // b2-b31: Reserved (0)
              mdb_payload_tx[30] = 0b00000010;  // Bit 1 set = Verbose mode enabled
              mdb_payload_tx[31] = 0x00;
              mdb_payload_tx[32] = 0x00;
              mdb_payload_tx[33] = 0x00;

              available_tx = 34;
            } else {
              fastSyslog.logf(LOG_INFO, "MDB: Gateway EXPANSION unknown subcmd=0x%02X", mdb_payload_rx[1]);
            }
            break;
          }

          case 0x03: {  // REPORT command (1BH)
            // REPORT command has variable length with no predefined end
            // The timeout-based read (already done) determines the end of the command
            // Report data is in mdb_payload_rx[1] to mdb_payload_rx[available_rx-2]

            uint8_t report_len = available_rx - 2;  // Exclude command and checksum

            fastSyslog.logf(LOG_INFO, "MDB: Gateway REPORT len=%d", report_len);

            // Log report data for debugging (first few bytes)
            if (report_len > 0) {
              fastSyslog.logf(LOG_DEBUG, "MDB: REPORT data: %02X %02X %02X...",
                             mdb_payload_rx[1],
                             report_len > 1 ? mdb_payload_rx[2] : 0,
                             report_len > 2 ? mdb_payload_rx[3] : 0);
            }

            // Process report data here if needed
            // For now, just acknowledge receipt
            // available_tx = 0 means we'll send ACK
            break;
          }

          default:
            fastSyslog.logf(LOG_INFO, "MDB: Gateway unknown cmd=0x%02X", command);
            break;
        }

        // Send prepared response or ACK
        // available_tx = 0 means we'll send ACK

      } else {
        // Handle Cashless Device #1 (0x10) - existing logic

      switch (mdb_payload_rx[0] & BIT_CMD_SET) {

      case RESET: {
        if (machine_state == VEND_STATE) {
          // Reset during VEND_STATE is interpreted as VEND_SUCCESS
        }

        cashless_reset_todo = true;
        machine_state = INACTIVE_STATE;

        fastSyslog.logf(LOG_INFO, "MDB: RESET");
        break;
      }

      case SETUP: {
        switch (mdb_payload_rx[1]) {

        case CONFIG_DATA: {
          machine_state = DISABLED_STATE;

          uint8_t vmcFeatureLevel = mdb_payload_rx[2];
          uint8_t vmcColumnsOnDisplay = mdb_payload_rx[3];
          uint8_t vmcRowsOnDisplay = mdb_payload_rx[4];
          uint8_t vmcDisplayInfo = mdb_payload_rx[5];

          mdb_payload_tx[0] = 0x01;        // Reader Config Data
          mdb_payload_tx[1] = 1;           // Reader Feature Level
          mdb_payload_tx[2] = 0xff;        // Country Code High
          mdb_payload_tx[3] = 0xff;        // Country Code Low
          mdb_payload_tx[4] = 1;           // Scale Factor
          mdb_payload_tx[5] = 2;           // Decimal Places
          mdb_payload_tx[6] = 3;           // Maximum Response Time (5s)
          mdb_payload_tx[7] = 0b00001001;  // Miscellaneous Options
          available_tx = 8;

          fastSyslog.logf(LOG_INFO, "MDB: CONFIG_DATA");
          break;
        }
        case MAX_MIN_PRICES: {
          uint16_t maxPrice = (mdb_payload_rx[2] << 8) | mdb_payload_rx[3];
          uint16_t minPrice = (mdb_payload_rx[4] << 8) | mdb_payload_rx[5];

          fastSyslog.logf(LOG_INFO, "MDB: MAX_MIN_PRICES");
          break;
        }
        }

        break;
      }

      case POLL: {
        if (cashless_reset_todo) {
          // Just reset
          cashless_reset_todo = false;
          mdb_payload_tx[0] = 0x00;
          available_tx = 1;

        } else if (machine_state == ENABLED_STATE && session_begin_todo) {
          // Begin session
          session_begin_todo = false;
          machine_state = IDLE_STATE;

          fundsAvailable = current_user_balance > 0 ? current_user_balance : 1;

          mdb_payload_tx[0] = 0x03;
          mdb_payload_tx[1] = fundsAvailable >> 8;
          mdb_payload_tx[2] = fundsAvailable;
          available_tx = 3;

        } else if (session_cancel_todo) {
          // Cancel session
          session_cancel_todo = false;

          mdb_payload_tx[0] = 0x04;
          available_tx = 1;

        } else if (vend_approved_todo) {
          // Vend approved
          vend_approved_todo = false;

          mdb_payload_tx[0] = 0x05;
          mdb_payload_tx[1] = itemPrice >> 8;
          mdb_payload_tx[2] = itemPrice;
          available_tx = 3;

        } else if (vend_denied_todo) {
          // Vend denied
          vend_denied_todo = false;
          machine_state = IDLE_STATE;

          mdb_payload_tx[0] = 0x06;
          available_tx = 1;

        } else if (session_end_todo) {
          // End session
          session_end_todo = false;
          machine_state = ENABLED_STATE;

          mdb_payload_tx[0] = 0x07;
          available_tx = 1;

        } else if (outsequence_todo) {
          // Command out of sequence
          outsequence_todo = false;

          mdb_payload_tx[0] = 0x0b;
          available_tx = 1;
        }

        break;
      }

      case VEND: {
        switch (mdb_payload_rx[1]) {
        case VEND_REQUEST: {
          machine_state = VEND_STATE;

          itemPrice = (mdb_payload_rx[2] << 8) | mdb_payload_rx[3];
          itemNumber = (mdb_payload_rx[4] << 8) | mdb_payload_rx[5];

          current_item_price = itemPrice;
          current_item_number = itemNumber;

          if (fundsAvailable && (fundsAvailable != 0xffff)) {
            if (itemPrice <= fundsAvailable) {
              vend_approved_todo = true;
            } else {
              vend_denied_todo = true;
            }
          }

          fastSyslog.logf(LOG_INFO, "MDB: VEND_REQUEST price=%d num=%d", itemPrice, itemNumber);
          break;
        }
        case VEND_CANCEL: {
          vend_denied_todo = true;

          fastSyslog.logf(LOG_INFO, "MDB: VEND_CANCEL");
          break;
        }
        case VEND_SUCCESS: {
          machine_state = IDLE_STATE;

          itemNumber = (mdb_payload_rx[2] << 8) | mdb_payload_rx[3];
          vend_success = true;

          fastSyslog.logf(LOG_INFO, "MDB: VEND_SUCCESS");
          break;
        }
        case VEND_FAILURE: {
          machine_state = IDLE_STATE;
          vend_success = false;

          fastSyslog.logf(LOG_INFO, "MDB: VEND_FAILURE");
          break;
        }
        case SESSION_COMPLETE: {
          session_end_todo = true;

          fastSyslog.logf(LOG_INFO, "MDB: SESSION_COMPLETE");
          break;
        }
        case CASH_SALE: {
          uint16_t itemPrice = (mdb_payload_rx[2] << 8) | mdb_payload_rx[3];
          uint16_t itemNumber = (mdb_payload_rx[4] << 8) | mdb_payload_rx[5];

          CashSale_t cashsale_data;
          cashsale_data.itemNumber = itemNumber;
          cashsale_data.itemPrice = itemPrice;
          xQueueSend(cashSaleQueue, &cashsale_data, 0);

          fastSyslog.logf(LOG_INFO, "MDB: CASH_SALE");
          break;
        }
        }

        break;
      }

      case READER: {
        switch (mdb_payload_rx[1]) {
        case READER_DISABLE: {
          machine_state = DISABLED_STATE;

          fastSyslog.logf(LOG_INFO, "MDB: READER_DISABLE");
          break;
        }
        case READER_ENABLE: {
          machine_state = ENABLED_STATE;

          fastSyslog.logf(LOG_INFO, "MDB: READER_ENABLE");
          break;
        }
        case READER_CANCEL: {
          mdb_payload_tx[0] = 0x08; // Canceled
          available_tx = 1;

          fastSyslog.logf(LOG_INFO, "MDB: READER_CANCEL");
          break;
        }
        }

        break;
      }

      case EXPANSION: {
        switch (mdb_payload_rx[1]) {
        case REQUEST_ID: {
          mdb_payload_tx[0] = 0x09; // Peripheral ID

          strncpy((char*)&mdb_payload_tx[1], "   ", 3);              // Manufacture code
          strncpy((char*)&mdb_payload_tx[4], "            ", 12);    // Serial number
          strncpy((char*)&mdb_payload_tx[16], "            ", 12);   // Model number
          strncpy((char*)&mdb_payload_tx[28], "  ", 2);              // Software version

          available_tx = 30;

          fastSyslog.logf(LOG_INFO, "MDB: REQUEST_ID");
          break;
        }
        }

        break;
      }
      }

      } // End of cashless device (0x10) handler

      // Transmit the prepared payload via UART
      write_payload_9(mdb_payload_tx, available_tx);

    } else {
      gpio_set_level(pin_mdb_led, 0); // Not the intended address
    }
  }
}

// Wrapper for backward compatibility - calls the new implementation
void mdb_loop(void *pvParameters) {
  mdb_cashless_loop(pvParameters);
}
