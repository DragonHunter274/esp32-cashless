#include "FastSyslog.h"

// Global instance definition
FastSyslog fastSyslog;

// Constructor
FastSyslog::FastSyslog() {
    logBuffer = nullptr;
    writeIndex = 0;
    readIndex = 0;
    syslogTaskHandle = nullptr;
    udpClient = nullptr;
    syslog = nullptr;
}

// Destructor
FastSyslog::~FastSyslog() {
    end();
}

// Initialize the fast syslog system
bool FastSyslog::begin(const char* server, uint16_t port, 
                       const char* deviceHostname, 
                       const char* appName) {
    
    // Allocate buffer
    logBuffer = new FastLogMessage[FAST_SYSLOG_BUFFER_SIZE];
    if (!logBuffer) {
        return false;
    }
    
    // Initialize buffer
    memset(logBuffer, 0, sizeof(FastLogMessage) * FAST_SYSLOG_BUFFER_SIZE);
    
    // Create UDP client and syslog instance
    udpClient = new WiFiUDP();
    syslog = new Syslog(*udpClient, SYSLOG_PROTO_IETF);
    
    if (!udpClient || !syslog) {
        return false;
    }
    
    // Configure syslog
    syslog->server(server, port);
    syslog->deviceHostname(deviceHostname);
    syslog->appName(appName);
    syslog->defaultPriority(LOG_INFO);
    
    // Create syslog task
    BaseType_t taskCreated = xTaskCreatePinnedToCore(
        syslogTaskWrapper,           // Task function
        "FastSyslog",               // Task name
        FAST_SYSLOG_TASK_STACK_SIZE, // Stack size
        this,                       // Task parameters (this pointer)
        FAST_SYSLOG_TASK_PRIORITY,  // Task priority
        &syslogTaskHandle,          // Task handle
        FAST_SYSLOG_TASK_CORE       // Core to run on
    );
    
    if (taskCreated != pdPASS) {
        delete[] logBuffer;
        delete udpClient;
        delete syslog;
        logBuffer = nullptr;
        udpClient = nullptr;
        syslog = nullptr;
        return false;
    }
    
    return true;
}

// Stop the syslog system
void FastSyslog::end() {
    // Delete task
    if (syslogTaskHandle) {
        vTaskDelete(syslogTaskHandle);
        syslogTaskHandle = nullptr;
    }
    
    // Clean up memory
    if (logBuffer) {
        delete[] logBuffer;
        logBuffer = nullptr;
    }
    
    if (udpClient) {
        delete udpClient;
        udpClient = nullptr;
    }
    
    if (syslog) {
        delete syslog;
        syslog = nullptr;
    }
    
    writeIndex = 0;
    readIndex = 0;
}

// Static wrapper for task function
void FastSyslog::syslogTaskWrapper(void* parameter) {
    FastSyslog* instance = static_cast<FastSyslog*>(parameter);
    instance->syslogTask();
}

// Actual task implementation
void FastSyslog::syslogTask() {
    uint32_t localReadIndex = 0;
    
    while (true) {
        // Check if there's a message ready
        if (localReadIndex != writeIndex && 
            logBuffer[localReadIndex & FAST_SYSLOG_BUFFER_MASK].ready) {
            
            FastLogMessage* msg = &logBuffer[localReadIndex & FAST_SYSLOG_BUFFER_MASK];
            
            // Only send if WiFi is connected and syslog is valid
            if (WiFi.status() == WL_CONNECTED && syslog) {
                syslog->logf(msg->priority, "%s", msg->message);
            }
            
            // Mark as processed (atomic)
            msg->ready = false;
            localReadIndex++;
            readIndex = localReadIndex;  // Update global read index
        } else {
            // No messages, yield to other tasks
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
}

// Internal fast log implementation
void FastSyslog::internalFastLog(const char* message, uint8_t priority) {
    if (!logBuffer) return;  // Not initialized

    // Check log level filter
    if (priority > FAST_SYSLOG_MAX_LEVEL) {
        return;  // Message filtered out
    }

    uint32_t currentWrite = writeIndex;
    uint32_t nextWrite = (currentWrite + 1) & FAST_SYSLOG_BUFFER_MASK;

    // Check if buffer is full (simple overflow check)
    if (nextWrite == (readIndex & FAST_SYSLOG_BUFFER_MASK)) {
        return; // Drop message if buffer full
    }

    FastLogMessage* msg = &logBuffer[currentWrite & FAST_SYSLOG_BUFFER_MASK];

    // Fast string copy (limit to prevent overflow)
    const char* src = message;
    char* dst = msg->message;
    int i = 0;
    while (*src && i < (FAST_SYSLOG_MESSAGE_SIZE - 1)) {
        *dst++ = *src++;
        i++;
    }
    *dst = '\0';

    msg->priority = priority;
    msg->ready = true;  // Atomic flag set

    writeIndex = nextWrite;  // Update write index
}

// Fast logging function
void FastSyslog::log(const char* message, uint8_t priority) {
    internalFastLog(message, priority);
}

// Formatted logging function
void FastSyslog::logf(uint8_t priority, const char* format, ...) {
    if (!logBuffer) return;  // Not initialized

    // Check log level filter
    if (priority > FAST_SYSLOG_MAX_LEVEL) {
        return;  // Message filtered out
    }

    uint32_t currentWrite = writeIndex;
    uint32_t nextWrite = (currentWrite + 1) & FAST_SYSLOG_BUFFER_MASK;

    if (nextWrite == (readIndex & FAST_SYSLOG_BUFFER_MASK)) {
        return; // Drop if full
    }

    FastLogMessage* msg = &logBuffer[currentWrite & FAST_SYSLOG_BUFFER_MASK];

    va_list args;
    va_start(args, format);
    vsnprintf(msg->message, FAST_SYSLOG_MESSAGE_SIZE, format, args);
    va_end(args);

    msg->priority = priority;
    msg->ready = true;
    writeIndex = nextWrite;
}

// Get buffer statistics
uint32_t FastSyslog::getBufferUsage() {
    return (writeIndex - readIndex) & FAST_SYSLOG_BUFFER_MASK;
}

uint32_t FastSyslog::getDroppedMessages() {
    // This would require additional tracking - simplified version
    return 0;
}

bool FastSyslog::isBufferFull() {
    uint32_t nextWrite = (writeIndex + 1) & FAST_SYSLOG_BUFFER_MASK;
    return nextWrite == (readIndex & FAST_SYSLOG_BUFFER_MASK);
}