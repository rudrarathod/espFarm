/*******************************************************************************
 * ESP-Agri Debug Log — Ring Buffer
 *
 * Stores the last N log entries (farm, device, command, result) in RAM.
 * Can be dumped to Serial or read for OLED display.
 ******************************************************************************/
#ifndef AGRI_LOG_H
#define AGRI_LOG_H

#include <Arduino.h>
#include "agri_config.h"

struct AgriLogEntry {
    char     farm[AGRI_FARM_ID_LEN];
    char     device[AGRI_DEVICE_ID_LEN];
    char     cmd[12];
    char     result[12];          // "OK", "NACK", "TIMEOUT", "BAD_FARM"...
    uint32_t timestamp;
    bool     valid;
};

/// Append a log entry (ring buffer — oldest overwritten)
void agri_log(const char* farm, const char* device,
              const char* cmd,  const char* result);

/// Get entry by index (0 = newest).  Returns nullptr if out of range.
const AgriLogEntry* agri_log_get(uint8_t index);

/// Dump all entries to Serial
void agri_log_dump();

/// Total entries currently stored (max AGRI_LOG_ENTRIES)
uint8_t agri_log_count();

#endif // AGRI_LOG_H
