/*******************************************************************************
 * ESP-Agri NVS Persistence — Implementation
 ******************************************************************************/
#include "agri_nvs.h"
#include <Preferences.h>

static Preferences s_prefs;
static const char* NVS_NAMESPACE = "agri";
static const char* KEY_FARM_ID   = "farm";
static const char* KEY_AOD_EN    = "aod_en";
static const char* KEY_AOD_SEC   = "aod_sec";

void agri_nvs_init() {
    // Open in read-write mode (false = RW)
    s_prefs.begin(NVS_NAMESPACE, false);
    Serial.println("[NVS] Preferences initialised");
}

bool agri_nvs_save_farm(const char* farmId) {
    if (!farmId || !farmId[0]) return false;
    size_t written = s_prefs.putString(KEY_FARM_ID, farmId);
    if (written > 0) {
        Serial.printf("[NVS] Farm saved: %s\n", farmId);
        return true;
    }
    Serial.println("[NVS] Farm save FAILED");
    return false;
}

bool agri_nvs_load_farm(char* outBuf, uint8_t outLen) {
    String val = s_prefs.getString(KEY_FARM_ID, "");
    if (val.length() == 0) {
        Serial.println("[NVS] No saved farm");
        return false;
    }
    strlcpy(outBuf, val.c_str(), outLen);
    Serial.printf("[NVS] Farm loaded: %s\n", outBuf);
    return true;
}

bool agri_nvs_has_farm() {
    return s_prefs.isKey(KEY_FARM_ID);
}

bool agri_nvs_save_aod(bool enabled, uint8_t timeoutSec) {
    s_prefs.putBool(KEY_AOD_EN, enabled);
    s_prefs.putUChar(KEY_AOD_SEC, timeoutSec);
    Serial.printf("[NVS] AOD saved: en=%d  sec=%u\n", enabled, timeoutSec);
    return true;
}

bool agri_nvs_load_aod(bool* outEnabled, uint8_t* outTimeoutSec) {
    if (!s_prefs.isKey(KEY_AOD_EN)) return false;
    *outEnabled    = s_prefs.getBool(KEY_AOD_EN, true);
    *outTimeoutSec = s_prefs.getUChar(KEY_AOD_SEC, AGRI_AOD_DEFAULT_SEC);
    Serial.printf("[NVS] AOD loaded: en=%d  sec=%u\n", *outEnabled, *outTimeoutSec);
    return true;
}
