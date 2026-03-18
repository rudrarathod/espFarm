/*******************************************************************************
 * ESP-Agri Device Map — Multi-Device GPIO Table
 *
 * Maps logical device IDs (e.g. "PUMP_01") to physical GPIO pins on the
 * relay node, and tracks each device's ON/OFF state.
 ******************************************************************************/
#ifndef AGRI_DEVMAP_H
#define AGRI_DEVMAP_H

#include <Arduino.h>
#include "agri_config.h"

struct AgriDevice {
    char    id[AGRI_DEVICE_ID_LEN];     // e.g. "PUMP_01"
    uint8_t gpio;                        // Physical pin
    bool    activeHigh;                  // Pin polarity
    bool    state;                       // Current ON/OFF
    bool    valid;                       // Slot in use
};

/// Initialise the device table with compile-time test entries and set GPIOs
void agri_devmap_init();

/// Look up a device by ID.  Returns nullptr if not found.
AgriDevice* agri_devmap_find(const char* deviceId);

/// Set a device ON or OFF by ID.  Returns false if device unknown.
bool agri_devmap_set(const char* deviceId, bool on);

/// Toggle a device.  Returns new state, or -1 if unknown.
int  agri_devmap_toggle(const char* deviceId);

/// Get device state.  Returns -1 if unknown.
int  agri_devmap_state(const char* deviceId);

/// Get pointer to the internal device array (for display iteration)
const AgriDevice* agri_devmap_table();

/// Number of configured device slots
uint8_t agri_devmap_count();

/// Reconfigure a device slot by index (id, gpio, polarity). Resets slot state to OFF.
bool agri_devmap_reconfigure(uint8_t idx, const char* id, uint8_t gpio, bool activeHigh);

#endif // AGRI_DEVMAP_H
