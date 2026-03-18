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
static const char* KEY_DEV_CFG   = "dev_cfg";
static const char* KEY_RELAY_FARM = "relay_farm";
static const char* KEY_REMOTE_ID  = "remote_id";
static const char* KEY_REMOTE_LIST = "remote_list";
static const char* KEY_SCHEDULE = "schedule";
static const char* KEY_SENSOR_ID = "sensor_id";
static const char* KEY_EXTENDER_ID = "extender_id";

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

bool agri_nvs_save_devcfg(const char* blob) {
    if (!blob) return false;
    size_t written = s_prefs.putString(KEY_DEV_CFG, blob);
    if (written > 0) {
        Serial.println("[NVS] Device config saved");
        return true;
    }
    Serial.println("[NVS] Device config save FAILED");
    return false;
}

bool agri_nvs_load_devcfg(char* outBuf, size_t outLen) {
    if (!outBuf || outLen == 0) return false;
    String val = s_prefs.getString(KEY_DEV_CFG, "");
    if (val.length() == 0) return false;
    strlcpy(outBuf, val.c_str(), outLen);
    Serial.println("[NVS] Device config loaded");
    return true;
}

bool agri_nvs_save_relay_farm(const char* farmId) {
    if (!farmId || !farmId[0]) return false;
    size_t written = s_prefs.putString(KEY_RELAY_FARM, farmId);
    return written > 0;
}

bool agri_nvs_load_relay_farm(char* outBuf, size_t outLen) {
    if (!outBuf || outLen == 0) return false;
    String val = s_prefs.getString(KEY_RELAY_FARM, "");
    if (val.length() == 0) return false;
    strlcpy(outBuf, val.c_str(), outLen);
    return true;
}

bool agri_nvs_save_remote_id(const char* remoteId) {
    if (!remoteId || !remoteId[0]) return false;
    size_t written = s_prefs.putString(KEY_REMOTE_ID, remoteId);
    return written > 0;
}

bool agri_nvs_load_remote_id(char* outBuf, size_t outLen) {
    if (!outBuf || outLen == 0) return false;
    String val = s_prefs.getString(KEY_REMOTE_ID, "");
    if (val.length() == 0) return false;
    strlcpy(outBuf, val.c_str(), outLen);
    return true;
}

bool agri_nvs_save_remote_farm_list(const char* csv) {
    if (!csv) return false;
    size_t written = s_prefs.putString(KEY_REMOTE_LIST, csv);
    return written > 0 || csv[0] == '\0';
}

bool agri_nvs_load_remote_farm_list(char* outBuf, size_t outLen) {
    if (!outBuf || outLen == 0) return false;
    String val = s_prefs.getString(KEY_REMOTE_LIST, "");
    if (val.length() == 0) return false;
    strlcpy(outBuf, val.c_str(), outLen);
    return true;
}

bool agri_nvs_save_schedule(const char* blob) {
    if (!blob) return false;
    size_t written = s_prefs.putString(KEY_SCHEDULE, blob);
    return written > 0 || blob[0] == '\0';
}

bool agri_nvs_load_schedule(char* outBuf, size_t outLen) {
    if (!outBuf || outLen == 0) return false;
    String val = s_prefs.getString(KEY_SCHEDULE, "");
    if (val.length() == 0) return false;
    strlcpy(outBuf, val.c_str(), outLen);
    return true;
}

bool agri_nvs_save_sensor_id(const char* sensorId) {
    if (!sensorId || !sensorId[0]) return false;
    size_t written = s_prefs.putString(KEY_SENSOR_ID, sensorId);
    return written > 0;
}

bool agri_nvs_load_sensor_id(char* outBuf, size_t outLen) {
    if (!outBuf || outLen == 0) return false;
    String val = s_prefs.getString(KEY_SENSOR_ID, "");
    if (val.length() == 0) return false;
    strlcpy(outBuf, val.c_str(), outLen);
    return true;
}

bool agri_nvs_save_extender_id(const char* extenderId) {
    if (!extenderId || !extenderId[0]) return false;
    size_t written = s_prefs.putString(KEY_EXTENDER_ID, extenderId);
    return written > 0;
}

bool agri_nvs_load_extender_id(char* outBuf, size_t outLen) {
    if (!outBuf || outLen == 0) return false;
    String val = s_prefs.getString(KEY_EXTENDER_ID, "");
    if (val.length() == 0) return false;
    strlcpy(outBuf, val.c_str(), outLen);
    return true;
}
