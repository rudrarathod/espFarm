/*******************************************************************************
 * ESP-Agri Device Map — Implementation
 ******************************************************************************/
#include "agri_devmap.h"

static AgriDevice s_devices[AGRI_MAX_DEVICES];
static uint8_t    s_count = 0;

// ============================================================================
// Init — create empty configurable slots (dashboard assigns IDs at runtime)
// ============================================================================
void agri_devmap_init() {
    s_count = AGRI_MAX_DEVICES;
    memset(s_devices, 0, sizeof(s_devices));

    const uint8_t defaultPins[AGRI_MAX_DEVICES] = {
        AGRI_DEV_PUMP_PIN,
        AGRI_DEV_VALVE_PIN,
        AGRI_DEV_LIGHT_PIN,
        AGRI_DEV_MOTOR_PIN,
        AGRI_DEV_AUX_PIN
    };

    for (uint8_t i = 0; i < s_count; i++) {
        AgriDevice& d = s_devices[i];
        d.id[0]      = '\0';
        d.gpio       = defaultPins[i];
        d.activeHigh = true;
        d.state      = false;
        d.valid      = false;

        pinMode(d.gpio, INPUT);
    }

    const char* csv = AGRI_RELAY_DEVICE_LIST_CSV;
    if (csv && csv[0]) {
        char cfgBuf[256] = {0};
        strlcpy(cfgBuf, csv, sizeof(cfgBuf));

        char* saveptr = nullptr;
        char* token = strtok_r(cfgBuf, ";", &saveptr);
        uint8_t seqIdx = 0;
        while (token) {
            int idx = -1;
            char id[AGRI_DEVICE_ID_LEN] = {0};
            int gpio = -1;
            int ah = 1;

            if (sscanf(token, "%d,%15[^,],%d,%d", &idx, id, &gpio, &ah) == 4) {
                if (idx >= 0 && idx < s_count) {
                    agri_devmap_reconfigure((uint8_t)idx, id, (uint8_t)gpio, ah != 0);
                }
            } else if (sscanf(token, "%15[^,],%d,%d", id, &gpio, &ah) == 3) {
                if (seqIdx < s_count) {
                    agri_devmap_reconfigure(seqIdx, id, (uint8_t)gpio, ah != 0);
                    seqIdx++;
                }
            }

            token = strtok_r(nullptr, ";", &saveptr);
        }
    }

    Serial.printf("[DEVMAP] %d empty slots ready for dashboard config\n", s_count);
}

// ============================================================================
// Look up
// ============================================================================
AgriDevice* agri_devmap_find(const char* deviceId) {
    for (uint8_t i = 0; i < s_count; i++) {
        if (s_devices[i].valid && strcmp(s_devices[i].id, deviceId) == 0)
            return &s_devices[i];
    }
    return nullptr;
}

// ============================================================================
// Set state
// ============================================================================
static void _apply(AgriDevice& d) {
    bool pin = d.activeHigh ? d.state : !d.state;
    digitalWrite(d.gpio, pin ? HIGH : LOW);
    Serial.printf("[DEVMAP] %s → %s  (GPIO%d = %s)\n",
                  d.id, d.state ? "ON" : "OFF", d.gpio, pin ? "HIGH" : "LOW");
}

bool agri_devmap_set(const char* deviceId, bool on) {
    AgriDevice* d = agri_devmap_find(deviceId);
    if (!d) return false;
    d->state = on;
    _apply(*d);
    return true;
}

int agri_devmap_toggle(const char* deviceId) {
    AgriDevice* d = agri_devmap_find(deviceId);
    if (!d) return -1;
    d->state = !d->state;
    _apply(*d);
    return d->state ? 1 : 0;
}

int agri_devmap_state(const char* deviceId) {
    AgriDevice* d = agri_devmap_find(deviceId);
    if (!d) return -1;
    return d->state ? 1 : 0;
}

// ============================================================================
// Table access
// ============================================================================
const AgriDevice* agri_devmap_table() {
    return s_devices;
}

uint8_t agri_devmap_count() {
    return s_count;
}

bool agri_devmap_reconfigure(uint8_t idx, const char* id, uint8_t gpio, bool activeHigh) {
    if (idx >= s_count || !id || !id[0]) return false;

    for (uint8_t i = 0; i < s_count; i++) {
        if (i == idx || !s_devices[i].valid) continue;
        if (strcmp(s_devices[i].id, id) == 0) return false;
        if (s_devices[i].gpio == gpio) return false;
    }

    AgriDevice& d = s_devices[idx];

    bool oldPinLevel = d.activeHigh ? false : true;
    digitalWrite(d.gpio, oldPinLevel ? HIGH : LOW);
    pinMode(d.gpio, INPUT);

    strlcpy(d.id, id, sizeof(d.id));
    d.gpio       = gpio;
    d.activeHigh = activeHigh;
    d.state      = false;
    d.valid      = true;

    pinMode(d.gpio, OUTPUT);
    digitalWrite(d.gpio, d.activeHigh ? LOW : HIGH);

    Serial.printf("[DEVMAP] Recfg #%d  %-8s -> GPIO%d (active %s)\n",
                  idx, d.id, d.gpio, d.activeHigh ? "HIGH" : "LOW");
    return true;
}
