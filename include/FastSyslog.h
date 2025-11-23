#ifndef FAST_SYSLOG_H
#define FAST_SYSLOG_H

#include <Arduino.h>
#include <Syslog.h>
#include <WiFiUdp.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// Configuration - modify these as needed
#define FAST_SYSLOG_BUFFER_SIZE 64    // Must be power of 2
#define FAST_SYSLOG_MESSAGE_SIZE 128  // Max message length
#define FAST_SYSLOG_BUFFER_MASK (FAST_SYSLOG_BUFFER_SIZE - 1)
#define FAST_SYSLOG_TASK_STACK_SIZE 2048
#define FAST_SYSLOG_TASK_PRIORITY 1
#define FAST_SYSLOG_TASK_CORE 0       // Run on core 0 (opposite of main)

// Log level definitions (lower number = higher priority)
#define FAST_SYSLOG_EMERG   0  // System is unusable
#define FAST_SYSLOG_ALERT   1  // Action must be taken immediately
#define FAST_SYSLOG_CRIT    2  // Critical conditions
#define FAST_SYSLOG_ERR     3  // Error conditions
#define FAST_SYSLOG_WARNING 4  // Warning conditions
#define FAST_SYSLOG_NOTICE  5  // Normal but significant condition
#define FAST_SYSLOG_INFO    6  // Informational messages
#define FAST_SYSLOG_DEBUG   7  // Debug-level messages

// Max log level - define in main.cpp before including FastSyslog.h to filter logs
// Example: #define FAST_SYSLOG_MAX_LEVEL FAST_SYSLOG_INFO
// Only logs with priority <= FAST_SYSLOG_MAX_LEVEL will be sent
#ifndef FAST_SYSLOG_MAX_LEVEL
#define FAST_SYSLOG_MAX_LEVEL FAST_SYSLOG_DEBUG  // Default: all logs enabled
#endif

// Log message structure
struct FastLogMessage {
    char message[FAST_SYSLOG_MESSAGE_SIZE];
    uint8_t priority;
    volatile bool ready;
};

class FastSyslog {
public:
    // Ring buffer (public for macro access)
    FastLogMessage* logBuffer;
    volatile uint32_t writeIndex;
    volatile uint32_t readIndex;

private:
    // FreeRTOS task
    TaskHandle_t syslogTaskHandle;
    
    // Syslog instance
    WiFiUDP* udpClient;
    Syslog* syslog;
    
    // Task function (static wrapper)
    static void syslogTaskWrapper(void* parameter);
    
    // Actual task implementation
    void syslogTask();
    
    // Internal fast log implementation
    void internalFastLog(const char* message, uint8_t priority);

public:
    // Constructor
    FastSyslog();
    
    // Destructor
    ~FastSyslog();
    
    // Initialize the fast syslog system
    bool begin(const char* server, uint16_t port, 
               const char* deviceHostname = "ESP32", 
               const char* appName = "FastApp");
    
    // Stop the syslog system
    void end();
    
    // Fast logging functions
    void log(const char* message, uint8_t priority = 6);  // Default to INFO level
    void logf(uint8_t priority, const char* format, ...);
    
    // Get buffer statistics
    uint32_t getBufferUsage();
    uint32_t getDroppedMessages();
    bool isBufferFull();
};

// Global instance (optional - you can create your own)
extern FastSyslog fastSyslog;

// Ultra-fast macro for constant strings (uses global instance)
// Now includes compile-time log level filtering
#define FAST_LOG(msg, priority_val) do { \
    if ((priority_val) <= FAST_SYSLOG_MAX_LEVEL) { \
        uint32_t cw = fastSyslog.writeIndex; \
        uint32_t nw = (cw + 1) & FAST_SYSLOG_BUFFER_MASK; \
        if (nw != (fastSyslog.readIndex & FAST_SYSLOG_BUFFER_MASK)) { \
            FastLogMessage* m = &fastSyslog.logBuffer[cw & FAST_SYSLOG_BUFFER_MASK]; \
            strcpy(m->message, msg); \
            m->priority = (uint8_t)(priority_val); \
            m->ready = true; \
            fastSyslog.writeIndex = nw; \
        } \
    } \
} while(0)

// Simplified macros using named constants (avoiding LOG_* macro conflicts)
#define FAST_LOG_EMERG(msg)   FAST_LOG(msg, FAST_SYSLOG_EMERG)
#define FAST_LOG_ALERT(msg)   FAST_LOG(msg, FAST_SYSLOG_ALERT)
#define FAST_LOG_CRIT(msg)    FAST_LOG(msg, FAST_SYSLOG_CRIT)
#define FAST_LOG_ERROR(msg)   FAST_LOG(msg, FAST_SYSLOG_ERR)
#define FAST_LOG_WARNING(msg) FAST_LOG(msg, FAST_SYSLOG_WARNING)
#define FAST_LOG_NOTICE(msg)  FAST_LOG(msg, FAST_SYSLOG_NOTICE)
#define FAST_LOG_INFO(msg)    FAST_LOG(msg, FAST_SYSLOG_INFO)
#define FAST_LOG_DEBUG(msg)   FAST_LOG(msg, FAST_SYSLOG_DEBUG)

#endif // FAST_SYSLOG_H