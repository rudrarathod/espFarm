/*******************************************************************************
 * ESP-Agri — Node A: Relay / Multi-Device Node   (Phase 2)
 *
 * Hardware:
 *   - ESP32 DOIT DevKit V1
 *   - Device GPIOs: PUMP_01→5, VALVE_01→19, LIGHT_01→23, MOTOR_01→4
 *   - D2 LED mirrors PUMP_01 state
 *   - SSD1306 OLED 128×64 on I2C (SDA=21, SCL=22, addr 0x3C)
 *
 * Behaviour:
 *   1. Joins the ESP-MESH network
 *   2. Listens for device commands (DEV_ON / DEV_OFF / TOGGLE + legacy pump)
 *   3. Validates Farm ID before acting
 *   4. Routes command to correct GPIO via device map
 *   5. Sends ACK with device state back to sender
 *   6. Logs every action to the debug ring buffer
 *   7. Displays multi-device state table on OLED
 *
 * Application code NEVER calls mesh APIs directly — only the transport
 * abstraction layer (agri_send / agri_broadcast / agri_on_receive).
 ******************************************************************************/
#include <Arduino.h>
#include "agri_config.h"
#include "agri_protocol.h"
#include "agri_transport.h"
#include "agri_mesh_wifi.h"
#include "agri_display.h"
#include "agri_devmap.h"
#include "agri_log.h"
#include "agri_rssi.h"

// ============================================================================
// Globals
// ============================================================================
static AgriMeshWifi g_mesh;
static AgriDisplay  g_display;
static const char*  DEVICE_ID = "RELAY01";

// Peer tracking (cache the remote node ID for directed ACKs)
static uint32_t g_lastRemoteNode = 0;

// Timing
static uint32_t g_lastDisplayRefresh = 0;
static uint32_t g_lastRssiPoll       = 0;
static uint32_t g_lastHeartbeat      = 0;   // Periodic heartbeat with device states
static RssiTracker g_rssiTracker;

// ============================================================================
// D2 LED — mirrors PUMP_01 state
// ============================================================================
static void update_led() {
    int ps = agri_devmap_state("PUMP_01");
    digitalWrite(AGRI_LED_PIN, (ps == 1) ? HIGH : LOW);
}

// ============================================================================
// Build & Send ACK
// ============================================================================
static void send_ack(uint32_t destNode, uint16_t origMsgId, const char* devId) {
    AgriMessage ack;
    strlcpy(ack.farmId,   AGRI_FARM_ID, sizeof(ack.farmId));
    strlcpy(ack.deviceId, devId,         sizeof(ack.deviceId));
    ack.command      = CMD_ACK;
    ack.messageId    = origMsgId;
    ack.timestamp    = millis();
    ack.nonce        = agri_random_nonce();
    ack.sourceNodeId = agri_get_node_id();

    // Attach the current state of the target device
    int st = agri_devmap_state(devId);
    ack.devState = (st == 1) ? 1 : 0;

    if (destNode != 0) {
        if (!agri_send(destNode, ack)) {
            Serial.println("[APP] Directed ACK failed, broadcasting...");
            agri_broadcast(ack);
        }
    } else {
        agri_broadcast(ack);
    }
}

// ============================================================================
// Handle a device command (ON / OFF / TOGGLE)
// Returns true if the device was found and acted upon.
// ============================================================================
static bool handle_device_cmd(const AgriMessage& msg) {
    const char* devId = msg.deviceId;
    AgriDevice* dev = agri_devmap_find(devId);

    // If the message targets our relay generic ID, default to PUMP_01
    if (!dev && strcmp(devId, DEVICE_ID) == 0) {
        devId = "PUMP_01";
        dev = agri_devmap_find(devId);
    }

    if (!dev) {
        Serial.printf("[APP] Unknown device '%s'\n", msg.deviceId);
        agri_log(msg.farmId, msg.deviceId,
                 agri_command_name(msg.command), "NO_DEV");
        return false;
    }

    switch (msg.command) {
        case CMD_PUMP_ON:
        case CMD_DEV_ON:
            agri_devmap_set(devId, true);
            break;
        case CMD_PUMP_OFF:
        case CMD_DEV_OFF:
            agri_devmap_set(devId, false);
            break;
        case CMD_TOGGLE:
            agri_devmap_toggle(devId);
            break;
        default:
            return false;
    }

    update_led();

    int st = agri_devmap_state(devId);
    Serial.printf("[APP] %s → %s\n", devId, st ? "ON" : "OFF");
    agri_log(msg.farmId, devId,
             agri_command_name(msg.command), st ? "ON" : "OFF");

    return true;
}

// ============================================================================
// Message Handler  (called by transport layer)
// ============================================================================
static void on_message_received(uint32_t from, const AgriMessage& msg) {
    Serial.printf("[APP] Msg from %u  cmd=%s  farm=%s  dev=%s  mid=%u\n",
                  from, agri_command_name(msg.command), msg.farmId,
                  msg.deviceId, msg.messageId);

    // --- Farm ID validation ---
    if (!agri_validate_farm_id(msg.farmId)) {
        Serial.printf("[APP] REJECTED: Farm ID mismatch (got '%s')\n", msg.farmId);
        agri_log(msg.farmId, msg.deviceId,
                 agri_command_name(msg.command), "BAD_FARM");
        return;
    }

    // --- Duplicate detection ---
    if (agri_is_duplicate(msg.deviceId, msg.messageId)) {
        Serial.printf("[APP] DUPLICATE: %s #%u — ignored\n",
                      msg.deviceId, msg.messageId);
        send_ack(from, msg.messageId, msg.deviceId);
        return;
    }
    agri_record_message(msg.deviceId, msg.messageId);

    // --- Cache peer node ID ---
    g_lastRemoteNode = from;

    // --- Process command ---
    switch (msg.command) {
        case CMD_PUMP_ON:
        case CMD_PUMP_OFF:
        case CMD_DEV_ON:
        case CMD_DEV_OFF:
        case CMD_TOGGLE:
            if (handle_device_cmd(msg)) {
                send_ack(from, msg.messageId, msg.deviceId);
                g_display.setLastCommand(agri_command_name(msg.command), msg.messageId);
                g_display.setLastSource(from);
                g_display.markDirty();
            }
            break;

        case CMD_STATUS_REQ: {
            if (strcmp(msg.deviceId, "*") == 0) {
                // Wildcard: reply once per registered device
                const AgriDevice* tbl = agri_devmap_table();
                uint8_t cnt = agri_devmap_count();
                for (uint8_t i = 0; i < cnt; i++) {
                    if (!tbl[i].valid) continue;
                    AgriMessage rsp;
                    strlcpy(rsp.farmId,   AGRI_FARM_ID, sizeof(rsp.farmId));
                    strlcpy(rsp.deviceId, tbl[i].id,    sizeof(rsp.deviceId));
                    rsp.command      = CMD_STATUS_RSP;
                    rsp.messageId    = msg.messageId;
                    rsp.timestamp    = millis();
                    rsp.nonce        = agri_random_nonce();
                    rsp.sourceNodeId = agri_get_node_id();
                    rsp.devState     = tbl[i].state ? 1 : 0;
                    agri_send(from, rsp);
                }
            } else {
                // Single-device status request
                AgriMessage rsp;
                strlcpy(rsp.farmId,   AGRI_FARM_ID, sizeof(rsp.farmId));
                strlcpy(rsp.deviceId, msg.deviceId,  sizeof(rsp.deviceId));
                rsp.command      = CMD_STATUS_RSP;
                rsp.messageId    = msg.messageId;
                rsp.timestamp    = millis();
                rsp.nonce        = agri_random_nonce();
                rsp.sourceNodeId = agri_get_node_id();

                int st = agri_devmap_state(msg.deviceId);
                rsp.devState = (st == 1) ? 1 : 0;
                agri_send(from, rsp);
            }
            return;
        }

        case CMD_HEARTBEAT:
            Serial.printf("[APP] Heartbeat from %s\n", msg.deviceId);
            return;

        case CMD_ACK:
        case CMD_NACK:
        case CMD_STATUS_RSP:
            return;

        default:
            Serial.printf("[APP] Unknown command: %d\n", msg.command);
            return;
    }
}

// ============================================================================
// Connection Change Handler
// ============================================================================
static void on_connection_change(bool connected, uint16_t nodeCount) {
    g_display.setMeshStatus(connected, nodeCount);
    Serial.printf("[APP] Mesh %s — %d peer(s)\n",
                  connected ? "connected" : "no peers", nodeCount);
}

// ============================================================================
// Setup
// ============================================================================
void setup() {
    Serial.begin(115200);
    delay(500);

    Serial.println("\n========================================");
    Serial.println("  ESP-Agri Relay Node (Multi-Device)");
    Serial.println("  Farm: " AGRI_FARM_ID);
    Serial.println("========================================\n");

    // --- Device map (fail-safe: all OFF at boot) ---
    agri_devmap_init();
    pinMode(AGRI_LED_PIN, OUTPUT);
    digitalWrite(AGRI_LED_PIN, LOW);
    Serial.println("[APP] Device map initialised");

    // --- OLED ---
    g_display.init();
    g_display.showSplash("ESP-Agri RELAY", "Multi-Device v2");
    delay(1000);

    // --- Transport (radio-agnostic API) ---
    agri_transport_init(&g_mesh);
    g_mesh.init();

    agri_on_receive(on_message_received);
    agri_on_connection_change(on_connection_change);

    // --- Seed random for nonces ---
    randomSeed(analogRead(0) ^ millis());

    // --- Display initial state ---
    g_display.setRole("RELAY");
    g_display.setNodeId(agri_get_node_id());
    g_display.setFarmId(AGRI_FARM_ID);
    g_display.setDeviceState(false);
    g_display.setMeshStatus(false, 0);

    Serial.printf("[APP] Setup complete — Node ID: %u\n", agri_get_node_id());
}

// ============================================================================
// Loop
// ============================================================================
void loop() {
    // --- Drive mesh radio ---
    agri_update();

    // --- Periodic display refresh ---
    uint32_t now = millis();
    if (now - g_lastDisplayRefresh >= AGRI_DISPLAY_REFRESH_MS) {
        g_lastDisplayRefresh = now;
        g_display.refresh();
    }

    // --- RSSI polling ---
    if ((now - g_lastRssiPoll) >= AGRI_RSSI_POLL_MS) {
        g_lastRssiPoll = now;
        g_rssiTracker.feed(agri_get_rssi());
        g_display.setRSSI(g_rssiTracker.smoothed(),
                          g_rssiTracker.bars(),
                          g_rssiTracker.trend(),
                          g_rssiTracker.rawMin(),
                          g_rssiTracker.rawMax(),
                          g_rssiTracker.qualityLabel());
    }

    // --- Periodic heartbeat broadcast with device state bitmask ---
    if ((now - g_lastHeartbeat) >= AGRI_RANGE_PING_MS) {
        g_lastHeartbeat = now;

        // Build a bitmask of device states (bit i = device i ON)
        const AgriDevice* tbl = agri_devmap_table();
        uint8_t cnt = agri_devmap_count();
        uint8_t bitmask = 0;
        for (uint8_t i = 0; i < cnt && i < 8; i++) {
            if (tbl[i].valid && tbl[i].state)
                bitmask |= (1 << i);
        }

        AgriMessage hb;
        hb.clear();
        strlcpy(hb.farmId,   AGRI_FARM_ID, sizeof(hb.farmId));
        strlcpy(hb.deviceId, "*",           sizeof(hb.deviceId));
        hb.command      = CMD_HEARTBEAT;
        hb.messageId    = agri_next_message_id();
        hb.timestamp    = millis();
        hb.nonce        = agri_random_nonce();
        hb.sourceNodeId = agri_get_node_id();
        hb.devState     = bitmask;

        agri_broadcast(hb);
    }
}
