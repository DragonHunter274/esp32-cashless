#pragma once

// MDB Protocol Constants
#define ACK 0x00  // Acknowledgment / Checksum correct
#define RET 0xAA  // Retransmit the previously sent data
#define NAK 0xFF  // Negative acknowledge

#define BIT_MODE_SET  0b100000000
#define BIT_ADD_SET   0b011111000
#define BIT_CMD_SET   0b000000111

// Macros
#define to_scale_factor(p, x, y) (p / x / pow(10, -(y)))
#define from_scale_factor(p, x, y) (p * x * pow(10, -(y)))

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
