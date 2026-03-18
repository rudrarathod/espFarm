/*******************************************************************************
 * ESP-Agri Protocol Layer — Implementation
 ******************************************************************************/
#include "agri_protocol.h"

// ============================================================================
// Internal State
// ============================================================================
static uint16_t s_msgIdCounter = 0;

struct DupEntry {
    char     deviceId[AGRI_DEVICE_ID_LEN];
    uint16_t messageId;
    uint32_t timestamp;
    bool     valid;
};

static DupEntry  s_dupBuffer[AGRI_DUP_BUFFER_SIZE];
static uint8_t   s_dupIndex = 0;
static char      s_runtimeFarmId[AGRI_FARM_ID_LEN] = AGRI_FARM_ID;

// ============================================================================
// AgriMessage
// ============================================================================
AgriMessage::AgriMessage() {
    clear();
}

void AgriMessage::clear() {
    memset(farmId, 0, sizeof(farmId));
    memset(deviceId, 0, sizeof(deviceId));
    memset(senderId, 0, sizeof(senderId));
    command      = 0;
    messageId    = 0;
    timestamp    = 0;
    nonce        = 0;
    devState     = 0;
    sourceNodeId = 0;
}

// ============================================================================
// Serialization  (compact JSON — stays < 100 bytes)
// ============================================================================
String agri_serialize(const AgriMessage& msg) {
    JsonDocument doc;
    doc["f"]   = msg.farmId;
    doc["d"]   = msg.deviceId;
    doc["sid"] = msg.senderId;
    doc["c"]   = msg.command;
    doc["m"]   = msg.messageId;
    doc["t"]   = msg.timestamp;
    doc["n"]   = msg.nonce;
    doc["s"]   = msg.devState;
    doc["src"] = msg.sourceNodeId;

    String output;
    serializeJson(doc, output);
    return output;
}

bool agri_deserialize(const String& json, AgriMessage& msg) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) {
        Serial.printf("[PROTO] JSON parse error: %s\n", err.c_str());
        return false;
    }

    msg.clear();
    strlcpy(msg.farmId,   doc["f"]   | "", sizeof(msg.farmId));
    strlcpy(msg.deviceId, doc["d"]   | "", sizeof(msg.deviceId));
    strlcpy(msg.senderId, doc["sid"] | "", sizeof(msg.senderId));
    msg.command      = doc["c"]   | (uint8_t)0;
    msg.messageId    = doc["m"]   | (uint16_t)0;
    msg.timestamp    = doc["t"]   | (uint32_t)0;
    msg.nonce        = doc["n"]   | (uint8_t)0;
    msg.devState     = doc["s"]   | (uint8_t)0;
    msg.sourceNodeId = doc["src"] | (uint32_t)0;
    return true;
}

// ============================================================================
// Message ID & Nonce
// ============================================================================
uint16_t agri_next_message_id() {
    return ++s_msgIdCounter;
}

uint8_t agri_random_nonce() {
    return (uint8_t)random(0, 256);
}

// ============================================================================
// Duplicate Detection  (ring buffer with time window)
// ============================================================================
bool agri_is_duplicate(const char* deviceId, uint16_t messageId) {
    uint32_t now = millis();
    for (int i = 0; i < AGRI_DUP_BUFFER_SIZE; i++) {
        if (s_dupBuffer[i].valid &&
            s_dupBuffer[i].messageId == messageId &&
            strcmp(s_dupBuffer[i].deviceId, deviceId) == 0 &&
            (now - s_dupBuffer[i].timestamp) < AGRI_DUP_WINDOW_MS) {
            return true;
        }
    }
    return false;
}

void agri_record_message(const char* deviceId, uint16_t messageId) {
    s_dupBuffer[s_dupIndex].valid = true;
    strlcpy(s_dupBuffer[s_dupIndex].deviceId, deviceId, AGRI_DEVICE_ID_LEN);
    s_dupBuffer[s_dupIndex].messageId  = messageId;
    s_dupBuffer[s_dupIndex].timestamp  = millis();
    s_dupIndex = (s_dupIndex + 1) % AGRI_DUP_BUFFER_SIZE;
}

// ============================================================================
// Utility
// ============================================================================
const char* agri_command_name(uint8_t cmd) {
    switch (cmd) {
        case CMD_PUMP_ON:     return "PUMP_ON";
        case CMD_PUMP_OFF:    return "PUMP_OFF";
        case CMD_TOGGLE:      return "TOGGLE";
        case CMD_DEV_ON:      return "DEV_ON";
        case CMD_DEV_OFF:     return "DEV_OFF";
        case CMD_ACK:         return "ACK";
        case CMD_NACK:        return "NACK";
        case CMD_STATUS_REQ:  return "STATUS_REQ";
        case CMD_STATUS_RSP:  return "STATUS_RSP";
        case CMD_DEVLIST_REQ: return "DEVLIST_REQ";
        case CMD_DEVLIST_RSP: return "DEVLIST_RSP";
        case CMD_HEARTBEAT:   return "HEARTBEAT";
        case CMD_SCHED_SET:   return "SCHED_SET";
        case CMD_SCHED_CLR:   return "SCHED_CLR";
        default:              return "UNKNOWN";
    }
}

bool agri_validate_farm_id(const char* farmId) {
    return (strcmp(farmId, s_runtimeFarmId) == 0);
}

const char* agri_get_runtime_farm_id() {
    return s_runtimeFarmId;
}

void agri_set_runtime_farm_id(const char* farmId) {
    if (!farmId || !farmId[0]) return;
    strlcpy(s_runtimeFarmId, farmId, sizeof(s_runtimeFarmId));
}
