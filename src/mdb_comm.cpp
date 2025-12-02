#include "mdb_comm.h"
#include "FastSyslog.h"
#include <queue.h>

// Global Variables definitions
volatile MACHINE_STATE machine_state = INACTIVE_STATE;
portMUX_TYPE mdb_mux = portMUX_INITIALIZER_UNLOCKED;

volatile bool reset_cashless_todo = false;
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
  pinMode(pin_mdb_rx, INPUT);
  pinMode(pin_mdb_tx, OUTPUT);
  pinMode(pin_mdb_led, OUTPUT);
}

// Read 9-bit data
uint16_t IRAM_ATTR read_9(uint8_t *checksum, bool wait_forever) {
  uint16_t coming_read = 0;
  unsigned long start_wait = micros();

  while (digitalRead(pin_mdb_rx)) {
      if (!wait_forever && (micros() - start_wait > 5000)) { // 5ms timeout
          return 0xFFFF; // Error code
      }
  }

  delayMicroseconds(156);
  for (uint8_t x = 0; x < 9; x++) {
    coming_read |= (digitalRead(pin_mdb_rx) << x);
    delayMicroseconds(104); // 9600bps
  }

  // Only add data bytes (not mode bytes) to checksum
  if(checksum && !(coming_read & BIT_MODE_SET)) {
    *checksum += (coming_read & 0xFF);
  }

  return coming_read;
}

// Write 9-bit data
void IRAM_ATTR write_9(uint16_t nth9) {
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

// MDB communication task
void mdb_loop(void *pvParameters) {
  uint8_t mdb_payload[256];  // Declared as a single array
  uint8_t available_tx = 0;

  for (;;) {
    uint8_t checksum = 0x00;
    // MDB requires response within 5ms - use minimal delay to prevent task starvation
    vTaskDelay(1/portTICK_PERIOD_MS);  // Allow other tasks to run if needed, but don't block

    // Wait for command byte (wait_forever = true)
    uint16_t coming_read = read_9(&checksum, true);

    // ENTER CRITICAL SECTION
    portENTER_CRITICAL(&mdb_mux);

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
            // RESET is a single-byte command - no additional data to read
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
            uint16_t sub_cmd_raw = read_9((uint8_t*) 0, false);
            if (sub_cmd_raw == 0xFFFF) {
                fastSyslog.logf(LOG_ERR, "MDB: SETUP sub_cmd timeout");
                break;
            }
            uint8_t sub_cmd = (uint8_t)(sub_cmd_raw & 0xFF);

            switch (sub_cmd) {
              case CONFIG_DATA: {
                uint8_t setup_data[5]; // 4 data bytes + 1 checksum byte
                bool timeout = false;

                // Read all data including received checksum (don't use auto-accumulation)
                for (int i = 0; i < 5; i++) {
                    uint16_t raw = read_9((uint8_t*) 0, false);
                    if (raw == 0xFFFF) {
                        timeout = true;
                        break;
                    }
                    setup_data[i] = (uint8_t)(raw & 0xFF);
                }
                if (timeout) {
                    fastSyslog.logf(LOG_ERR, "MDB: CONFIG_DATA timeout");
                    break;
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
                bool timeout = false;

                // Read all data including received checksum (don't use auto-accumulation)
                for (int i = 0; i < 5; i++) {
                    uint16_t raw = read_9((uint8_t*) 0, false);
                    if (raw == 0xFFFF) {
                        timeout = true;
                        break;
                    }
                    price_data[i] = (uint8_t)(raw & 0xFF);
                }
                if (timeout) {
                    fastSyslog.logf(LOG_ERR, "MDB: MAX_MIN_PRICES timeout");
                    break;
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
            // POLL is a single-byte command - no additional data to read

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
            uint16_t sub_cmd_raw = read_9((uint8_t*) 0, false);
            if (sub_cmd_raw == 0xFFFF) {
                fastSyslog.logf(LOG_ERR, "MDB: VEND sub_cmd timeout");
                break;
            }
            uint8_t sub_cmd = (uint8_t)(sub_cmd_raw & 0xFF);

            switch(sub_cmd) {
              case VEND_REQUEST: {
                uint8_t vend_data[5]; // 4 data bytes + 1 checksum byte
                bool timeout = false;

                // Read all data including received checksum (don't use auto-accumulation)
                for (int i = 0; i < 5; i++) {
                    uint16_t raw = read_9((uint8_t*) 0, false);
                    if (raw == 0xFFFF) {
                        timeout = true;
                        break;
                    }
                    vend_data[i] = (uint8_t)(raw & 0xFF);
                }
                if (timeout) {
                    fastSyslog.logf(LOG_ERR, "MDB: VEND_REQUEST timeout");
                    break;
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

                uint16_t raw = read_9((uint8_t*) 0, false);
                if (raw == 0xFFFF) {
                    fastSyslog.logf(LOG_ERR, "MDB: VEND_CANCEL timeout");
                    break;
                }
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
                bool timeout = false;

                // Read all data including received checksum (don't use auto-accumulation)
                for (int i = 0; i < 3; i++) {
                    uint16_t raw = read_9((uint8_t*) 0, false);
                    if (raw == 0xFFFF) {
                        timeout = true;
                        break;
                    }
                    success_data[i] = (uint8_t)(raw & 0xFF);
                }
                if (timeout) {
                    fastSyslog.logf(LOG_ERR, "MDB: VEND_SUCCESS timeout");
                    break;
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
                machine_state = IDLE_STATE;
                break;
              }
              case VEND_FAILURE: {
                uint8_t checksum_data[1];

                uint16_t raw = read_9((uint8_t*) 0, false);
                if (raw == 0xFFFF) {
                    fastSyslog.logf(LOG_ERR, "MDB: VEND_FAILURE timeout");
                    break;
                }
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

                uint16_t raw = read_9((uint8_t*) 0, false);
                if (raw == 0xFFFF) {
                    fastSyslog.logf(LOG_ERR, "MDB: SESSION_COMPLETE timeout");
                    break;
                }
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
                bool timeout = false;

                // Read all data including received checksum (don't use auto-accumulation)
                for (int i = 0; i < 5; i++) {
                    uint16_t temp = read_9((uint8_t*) 0, false);
                    if (temp == 0xFFFF) {
                        timeout = true;
                        break;
                    }
                    cash_data[i] = (uint8_t)(temp & 0xFF);
                }
                if (timeout) {
                    fastSyslog.logf(LOG_ERR, "MDB: CASH_SALE timeout");
                    break;
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
            uint16_t sub_cmd_raw = read_9((uint8_t*) 0, false);
            if (sub_cmd_raw == 0xFFFF) {
                fastSyslog.logf(LOG_ERR, "MDB: READER sub_cmd timeout");
                break;
            }
            uint8_t sub_cmd = (uint8_t)(sub_cmd_raw & 0xFF);

            switch(sub_cmd) {
              case READER_DISABLE: {
                uint8_t checksum_data[1];

                uint16_t raw = read_9((uint8_t*) 0, false);
                if (raw == 0xFFFF) {
                    fastSyslog.logf(LOG_ERR, "MDB: READER_DISABLE timeout");
                    break;
                }
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

                uint16_t raw = read_9((uint8_t*) 0, false);
                if (raw == 0xFFFF) {
                    fastSyslog.logf(LOG_ERR, "MDB: READER_ENABLE timeout");
                    break;
                }
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

                uint16_t raw = read_9((uint8_t*) 0, false);
                if (raw == 0xFFFF) {
                    fastSyslog.logf(LOG_ERR, "MDB: READER_CANCEL timeout");
                    break;
                }
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
            uint16_t sub_cmd_raw = read_9((uint8_t*) 0, false);
            if (sub_cmd_raw == 0xFFFF) {
                fastSyslog.logf(LOG_ERR, "MDB: EXPANSION sub_cmd timeout");
                break;
            }
            uint8_t sub_cmd = (uint8_t)(sub_cmd_raw & 0xFF);

            switch(sub_cmd) {
              case REQUEST_ID: {
                uint8_t id_data[30]; // 29 data bytes + 1 checksum byte
                bool timeout = false;

                // Read all data including received checksum (don't use auto-accumulation)
                for (int i = 0; i < 30; i++) {
                    uint16_t raw = read_9((uint8_t*) 0, false);
                    if (raw == 0xFFFF) {
                        timeout = true;
                        break;
                    }
                    id_data[i] = (uint8_t)(raw & 0xFF);
                }
                if (timeout) {
                    fastSyslog.logf(LOG_ERR, "MDB: REQUEST_ID timeout");
                    break;
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

    // EXIT CRITICAL SECTION
    portEXIT_CRITICAL(&mdb_mux);
  }
}
