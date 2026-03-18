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

/// Save relay device configuration blob
bool agri_nvs_save_devcfg(const char* blob);

/// Load relay device configuration blob
bool agri_nvs_load_devcfg(char* outBuf, size_t outLen);

/// Save relay farm ID runtime override
bool agri_nvs_save_relay_farm(const char* farmId);

/// Load relay farm ID runtime override
bool agri_nvs_load_relay_farm(char* outBuf, size_t outLen);

/// Save remote logical ID runtime override
bool agri_nvs_save_remote_id(const char* remoteId);

/// Load remote logical ID runtime override
bool agri_nvs_load_remote_id(char* outBuf, size_t outLen);

/// Save remote farm CSV list runtime override
bool agri_nvs_save_remote_farm_list(const char* csv);

/// Load remote farm CSV list runtime override
bool agri_nvs_load_remote_farm_list(char* outBuf, size_t outLen);

/// Save relay schedule configuration blob
bool agri_nvs_save_schedule(const char* blob);

/// Load relay schedule configuration blob
bool agri_nvs_load_schedule(char* outBuf, size_t outLen);

/// Save sensor logical ID runtime override
bool agri_nvs_save_sensor_id(const char* sensorId);

/// Load sensor logical ID runtime override
bool agri_nvs_load_sensor_id(char* outBuf, size_t outLen);

/// Save extender logical ID runtime override
bool agri_nvs_save_extender_id(const char* extenderId);

/// Load extender logical ID runtime override
bool agri_nvs_load_extender_id(char* outBuf, size_t outLen);

#endif // AGRI_NVS_H
