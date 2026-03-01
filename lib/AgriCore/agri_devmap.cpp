/*******************************************************************************
 * ESP-Agri Device Map — Implementation
 ******************************************************************************/
#include "agri_devmap.h"

static AgriDevice s_devices[AGRI_MAX_DEVICES];
static uint8_t    s_count = 0;

// ============================================================================
// Internal: add a device slot
// ============================================================================
static void _add(const char* id, uint8_t gpio, bool activeHigh) {
    if (s_count >= AGRI_MAX_DEVICES) return;
    AgriDevice& d = s_devices[s_count];
    strlcpy(d.id, id, sizeof(d.id));
    d.gpio       = gpio;
    d.activeHigh = activeHigh;
    d.state      = false;
    d.valid      = true;

    pinMode(gpio, OUTPUT);
    digitalWrite(gpio, activeHigh ? LOW : HIGH);   // default OFF

    Serial.printf("[DEVMAP] #%d  %-8s → GPIO%d  (active %s)\n",
                  s_count, id, gpio, activeHigh ? "HIGH" : "LOW");
    s_count++;
}

// ============================================================================
// Init — register all test devices
// ============================================================================
void agri_devmap_init() {
    s_count = 0;
    memset(s_devices, 0, sizeof(s_devices));

    _add("PUMP_01",  AGRI_DEV_PUMP_PIN,  true);
    _add("VALVE_01", AGRI_DEV_VALVE_PIN, true);
    _add("LIGHT_01", AGRI_DEV_LIGHT_PIN, true);
    _add("MOTOR_01", AGRI_DEV_MOTOR_PIN, true);
    _add("AUX_01",   AGRI_DEV_AUX_PIN,   true);

    Serial.printf("[DEVMAP] %d devices registered\n", s_count);
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
