/*******************************************************************************
 * ESP-Agri Debug Log — Implementation
 ******************************************************************************/
#include "agri_log.h"

static AgriLogEntry s_log[AGRI_LOG_ENTRIES];
static uint8_t      s_logHead  = 0;   // Next write position
static uint8_t      s_logCount = 0;

void agri_log(const char* farm, const char* device,
              const char* cmd,  const char* result)
{
    AgriLogEntry& e = s_log[s_logHead];
    e.valid = true;
    strlcpy(e.farm,   farm   ? farm   : "?", sizeof(e.farm));
    strlcpy(e.device, device ? device : "?", sizeof(e.device));
    strlcpy(e.cmd,    cmd    ? cmd    : "?", sizeof(e.cmd));
    strlcpy(e.result, result ? result : "?", sizeof(e.result));
    e.timestamp = millis();

    s_logHead = (s_logHead + 1) % AGRI_LOG_ENTRIES;
    if (s_logCount < AGRI_LOG_ENTRIES) s_logCount++;

    Serial.printf("[LOG] %s | %s | %s | %s\n", farm, device, cmd, result);
}

const AgriLogEntry* agri_log_get(uint8_t index) {
    if (index >= s_logCount) return nullptr;
    // index 0 = newest
    int pos = ((int)s_logHead - 1 - (int)index + AGRI_LOG_ENTRIES * 2) % AGRI_LOG_ENTRIES;
    return &s_log[pos];
}

void agri_log_dump() {
    Serial.println("===== ESP-Agri Debug Log =====");
    for (uint8_t i = 0; i < s_logCount; i++) {
        const AgriLogEntry* e = agri_log_get(i);
        if (!e) break;
        Serial.printf("  [%u] %8s | %-8s | %-8s | %s\n",
                      e->timestamp / 1000, e->farm, e->device, e->cmd, e->result);
    }
    Serial.printf("  (%d entries)\n", s_logCount);
    Serial.println("==============================");
}

uint8_t agri_log_count() {
    return s_logCount;
}
