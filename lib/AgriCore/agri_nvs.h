/*******************************************************************************
 * ESP-Agri NVS Persistence — Save / Restore user selections
 *
 * Uses ESP32 Preferences (NVS) to persist:
 *   - Selected farm ID  (survives reboot)
 *
 * Non-blocking.  Safe to call from setup() and loop().
 ******************************************************************************/
#ifndef AGRI_NVS_H
#define AGRI_NVS_H

#include <Arduino.h>
#include "agri_config.h"

/// Initialise NVS storage (call once in setup before use)
void agri_nvs_init();

/// Save the selected farm ID to NVS.  Returns true on success.
bool agri_nvs_save_farm(const char* farmId);

/// Load the saved farm ID from NVS into outBuf.
/// Returns true if a value was found, false if empty/missing.
bool agri_nvs_load_farm(char* outBuf, uint8_t outLen);

/// Check if a farm ID has been saved previously
bool agri_nvs_has_farm();

/// Save AOD settings (enabled flag + timeout seconds)
bool agri_nvs_save_aod(bool enabled, uint8_t timeoutSec);

/// Load AOD settings.  Returns true if values were found.
bool agri_nvs_load_aod(bool* outEnabled, uint8_t* outTimeoutSec);

#endif // AGRI_NVS_H
