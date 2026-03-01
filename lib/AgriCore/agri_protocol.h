/*******************************************************************************
 * ESP-Agri Protocol Layer
 *
 * Defines the message format, command types, serialization, and duplicate
 * detection. Designed to stay under 100 bytes when serialized for future
 * Meshtastic (LoRa) compatibility.
 *
 * JSON wire format (compact keys):
 *   {"f":"FARM_101","d":"PUMP_01","c":2,"m":1234,"t":567890,"n":42,"s":0,"src":0}
 ******************************************************************************/
#ifndef AGRI_PROTOCOL_H
#define AGRI_PROTOCOL_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include "agri_config.h"

// ============================================================================
// Command Types
// ============================================================================
enum AgriCommand : uint8_t {
    CMD_PUMP_ON     = 0,
    CMD_PUMP_OFF    = 1,
    CMD_TOGGLE      = 2,
    CMD_DEV_ON      = 3,      // Generic device ON
    CMD_DEV_OFF     = 4,      // Generic device OFF
    CMD_ACK         = 10,
    CMD_NACK        = 11,
    CMD_STATUS_REQ  = 12,
    CMD_STATUS_RSP  = 13,
    CMD_HEARTBEAT   = 20
};

// ============================================================================
// Message Structure
// ============================================================================
struct AgriMessage {
    char     farmId[AGRI_FARM_ID_LEN];      // Target farm
    char     deviceId[AGRI_DEVICE_ID_LEN];  // Target device (e.g. PUMP_01)
    uint8_t  command;                        // AgriCommand enum
    uint16_t messageId;                      // Monotonic per-device counter
    uint32_t timestamp;                      // millis() at creation
    uint8_t  nonce;                          // Random, aids duplicate detection
    uint8_t  devState;                       // 0=OFF, 1=ON (used in ACK/STATUS)
    uint32_t sourceNodeId;                   // Mesh node ID (filled on receive)

    AgriMessage();
    void clear();
};

// ============================================================================
// Serialization / Deserialization
// ============================================================================
String  agri_serialize(const AgriMessage& msg);
bool    agri_deserialize(const String& json, AgriMessage& msg);

// ============================================================================
// Message ID & Nonce Generation
// ============================================================================
uint16_t agri_next_message_id();
uint8_t  agri_random_nonce();

// ============================================================================
// Duplicate Detection
// ============================================================================
bool    agri_is_duplicate(const char* deviceId, uint16_t messageId);
void    agri_record_message(const char* deviceId, uint16_t messageId);

// ============================================================================
// Utility
// ============================================================================
const char* agri_command_name(uint8_t cmd);
bool        agri_validate_farm_id(const char* farmId);

#endif // AGRI_PROTOCOL_H
