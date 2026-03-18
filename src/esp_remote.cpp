/*******************************************************************************
 * ESP-Agri — Node B: Remote / Control Node   (Phase 3 — Grid UI)
 *
 * Hardware:
 *   - ESP32 DOIT DevKit V1
 *   - BTN_UP   on GPIO16 (INPUT_PULLUP, pressed = LOW)
 *   - BTN_SEL  on GPIO17 (INPUT_PULLUP, pressed = LOW)
 *                short press = SELECT / TOGGLE, long press = BACK
 *   - BTN_DOWN on GPIO18 (INPUT_PULLUP, pressed = LOW)
 *   - SSD1306 OLED 128×64 on I2C (SDA=21, SCL=22, addr 0x3C)
 *
 * Behaviour:
 *   1. Joins the ESP-MESH network
 *   2. 6-tile grid UI: 5 device toggles + 1 Settings tile
 *   3. Short SELECT toggles the highlighted device via agri_send()
 *   4. Waits for ACK with timeout and automatic retry
 *   5. Settings tile opens submenu: Mesh Status, Debug Logs, Node Info
 *   6. Logs every action to the debug ring buffer
 *
 * Application code NEVER calls mesh APIs directly — only the transport
 * abstraction layer (agri_send / agri_broadcast / agri_on_receive).
 ******************************************************************************/
#include <Arduino.h>
#include <ctype.h>
#include <string.h>
#include "agri_config.h"
#include "agri_protocol.h"
#include "agri_transport.h"
#include "agri_mesh_wifi.h"
#include "agri_display.h"
#include "agri_gridui.h"
#include "agri_range.h"
#include "agri_log.h"
#include "agri_nvs.h"
#include "agri_rssi.h"

// ============================================================================
// Globals
// ============================================================================
static AgriMeshWifi g_mesh;
static AgriDisplay  g_display;
static char         g_remoteId[AGRI_DEVICE_ID_LEN] = AGRI_REMOTE_ID;
static const char*  DEVICE_ID = g_remoteId;
static char         g_remoteFarmListCsv[256] = AGRI_REMOTE_FARM_LIST_CSV;

// ============================================================================
// 3-Button State  (UP / SEL / DOWN)
//   UP and DOWN: simple edge detect on press
//   SEL (middle): short press = OK, long press = BACK
// ============================================================================
struct BtnState {
    uint8_t  pin;
    bool     raw;
    bool     stable;
    uint32_t debounceT;
    bool     fired;        // Edge already delivered this press
};

static BtnState g_btnUp   = { AGRI_BTN_UP,   HIGH, HIGH, 0, false };
static BtnState g_btnDown = { AGRI_BTN_DOWN,  HIGH, HIGH, 0, false };
static BtnState g_btnSel  = { AGRI_BTN_SEL,   HIGH, HIGH, 0, false };

// Middle-button long-press tracking
static bool     g_selPressed   = false;   // SEL is currently held down
static uint32_t g_selPressTime = 0;       // millis() when SEL was first pressed
static bool     g_selLongFired = false;   // long-press already delivered

// ============================================================================
// ACK / Retry State Machine
// ============================================================================
enum TxState {
    TX_IDLE,
    TX_WAITING_ACK,
    TX_FAILED
};

static TxState     g_txState       = TX_IDLE;
static uint16_t    g_pendingMsgId  = 0;
static uint32_t    g_ackTimer      = 0;
static uint8_t     g_retryCount    = 0;
static AgriMessage g_pendingMsg;

// Peer tracking
static uint32_t g_relayNodeId      = 0;

// Timing
static uint32_t g_lastDisplayRefresh = 0;
static uint32_t g_statusClearTime    = 0;
static uint32_t g_lastDevlistReq     = 0;
static bool     g_deviceListReady    = false;

// AOD (Always-On Display) — sleep/wake
static bool     g_displaySleeping    = false;
static uint32_t g_lastActivityTime   = 0;
static bool     g_aodPrevEnabled     = true;
static uint8_t  g_aodPrevSec         = AGRI_AOD_DEFAULT_SEC;
static GridScreen g_prevScreenForAod = GRID_MAIN;  // track screen transitions for AOD

// RSSI polling
static uint32_t g_lastRssiPoll       = 0;
static RssiTracker g_rssiTracker;

struct RemoteScheduleUi {
    bool enabled;
    bool running;
    uint32_t leftSec;
    uint32_t delaySec;
    uint32_t runSec;
};

static RemoteScheduleUi g_schedUi[GRID_DEV_TILES] = {};
static uint32_t g_lastSchedUiTick = 0;

// ============================================================================
// Farm Selection  (boot-time picker)
// ============================================================================
static char s_farmStorage[AGRI_TEST_FARM_COUNT][AGRI_FARM_ID_LEN];
static const char* s_farmList[AGRI_TEST_FARM_COUNT];
static uint8_t s_farmCount = 0;
static char g_selectedFarm[AGRI_FARM_ID_LEN] = "";

static void init_remote_farm_list() {
    s_farmCount = 0;
    char csvBuf[256] = {0};
    strlcpy(csvBuf, g_remoteFarmListCsv, sizeof(csvBuf));

    char* saveptr = nullptr;
    char* token = strtok_r(csvBuf, ",", &saveptr);
    while (token && s_farmCount < AGRI_TEST_FARM_COUNT) {
        while (*token == ' ' || *token == '\t') token++;
        char* end = token + strlen(token);
        while (end > token && (end[-1] == ' ' || end[-1] == '\t')) {
            *--end = '\0';
        }

        if (token[0]) {
            strlcpy(s_farmStorage[s_farmCount], token, sizeof(s_farmStorage[0]));
            s_farmList[s_farmCount] = s_farmStorage[s_farmCount];
            s_farmCount++;
        }
        token = strtok_r(nullptr, ",", &saveptr);
    }

    if (s_farmCount == 0) {
        strlcpy(s_farmStorage[0], AGRI_FARM_ID, sizeof(s_farmStorage[0]));
        s_farmList[0] = s_farmStorage[0];
        s_farmCount = 1;
    }
}

/// Non-blocking farm picker — enters the grid-based farm selection screen
static void enter_farm_picker() {
    grid_enter_farm_select(s_farmList, s_farmCount);
    g_display.markDirty();
}

static bool is_selected_farm(const char* farmId) {
    if (!g_selectedFarm[0]) return false;
    return strcmp(farmId, g_selectedFarm) == 0;
}

static void serial_print_cfg() {
    const char* farm = g_selectedFarm[0] ? g_selectedFarm : AGRI_FARM_ID;
    const char* list = g_remoteFarmListCsv;

    String json = "{\"role\":\"remote\",\"farmId\":\"";
    json += farm;
    json += "\",\"id\":\"";
    json += DEVICE_ID;
    json += "\",\"remoteId\":\"";
    json += DEVICE_ID;
    json += "\",\"remoteFarmList\":\"";
    json += list;
    json += "\",\"list\":\"";
    json += list;
    json += "\",\"syncVer\":2";
    json += ",\"protoVer\":2";
    json += "}";
    Serial.println(json);
}

static bool serial_apply_cfg(const String& jsonText) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, jsonText);
    if (err) return false;

    const char* remoteId = doc["remoteId"] | doc["id"] | "";
    const char* farmId = doc["farmId"] | "";
    const char* remoteFarmList = nullptr;
    if (!doc["remoteFarmList"].isNull()) {
        remoteFarmList = doc["remoteFarmList"] | "";
    } else if (!doc["list"].isNull()) {
        remoteFarmList = doc["list"] | "";
    }

    if (remoteId[0]) {
        strlcpy(g_remoteId, remoteId, sizeof(g_remoteId));
        agri_nvs_save_remote_id(g_remoteId);
    }

    if (remoteFarmList != nullptr) {
        strlcpy(g_remoteFarmListCsv, remoteFarmList, sizeof(g_remoteFarmListCsv));
        agri_nvs_save_remote_farm_list(g_remoteFarmListCsv);
        init_remote_farm_list();
    }

    if (farmId[0]) {
        strlcpy(g_selectedFarm, farmId, sizeof(g_selectedFarm));
        grid_set_farm_id(g_selectedFarm);
        agri_nvs_save_farm(g_selectedFarm);
    }

    String ack = "{\"setCfg\":\"ok\",\"role\":\"remote\",\"syncVer\":2,\"protoVer\":2}";
    Serial.println(ack);
    serial_print_cfg();
    return true;
}

static void handle_serial_cli() {
    if (!Serial.available()) return;

    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (!cmd.length()) return;

    if (cmd == "AGRI_GET_CFG" || cmd == "AGRI_GET_CFG?") {
        serial_print_cfg();
        return;
    }

    if (cmd.startsWith("AGRI_SET_CFG ")) {
        String body = cmd.substring(strlen("AGRI_SET_CFG "));
        if (!serial_apply_cfg(body)) {
            Serial.println("{\"setCfg\":\"error\",\"reason\":\"bad_json\",\"role\":\"remote\",\"syncVer\":2}");
        }
        return;
    }

    if (cmd == "AGRI_HELP") {
        Serial.println("AGRI_GET_CFG");
        Serial.println("AGRI_SET_CFG {json}");
    }
}

// ============================================================================
// Send a command  (farm + device from grid tile, cmd from toggle callback)
// ============================================================================
static void send_command(const char* farmId, const char* deviceId, uint8_t cmd) {
    g_pendingMsg.clear();
    strlcpy(g_pendingMsg.farmId,   farmId,   sizeof(g_pendingMsg.farmId));
    strlcpy(g_pendingMsg.deviceId, deviceId, sizeof(g_pendingMsg.deviceId));
    strlcpy(g_pendingMsg.senderId, DEVICE_ID, sizeof(g_pendingMsg.senderId));
    g_pendingMsg.command      = cmd;
    g_pendingMsg.messageId    = agri_next_message_id();
    g_pendingMsg.timestamp    = millis();
    g_pendingMsg.nonce        = agri_random_nonce();
    g_pendingMsg.sourceNodeId = agri_get_node_id();

    g_pendingMsgId = g_pendingMsg.messageId;
    g_retryCount   = 0;

    bool ok;
    if (g_relayNodeId != 0) {
        ok = agri_send(g_relayNodeId, g_pendingMsg);
        if (!ok) {
            Serial.println("[APP] Directed send failed, broadcasting...");
            ok = agri_broadcast(g_pendingMsg);
        }
    } else {
        ok = agri_broadcast(g_pendingMsg);
    }

    const char* cmdName = agri_command_name(cmd);

    if (ok) {
        g_txState  = TX_WAITING_ACK;
        g_ackTimer = millis();
        Serial.printf("[APP] Sent %s → %s/%s #%u\n",
                      cmdName, farmId, deviceId, g_pendingMsgId);
    } else {
        g_txState = TX_FAILED;
        Serial.println("[APP] Send FAILED");
        agri_log(farmId, deviceId, cmdName, "SEND_FAIL");
        grid_clear_ack_pending(deviceId);
    }

    g_display.setLastCommand(cmdName, g_pendingMsgId);
    g_display.setAckStatus("Waiting...");
    g_display.markDirty();
}

static int grid_index_for_device(const char* deviceId) {
    if (!deviceId || !deviceId[0]) return -1;
    for (uint8_t i = 0; i < GRID_DEV_TILES; i++) {
        GridDevice* dev = grid_device(i);
        if (!dev) continue;
        if (strcmp(dev->deviceId, deviceId) == 0) return (int)i;
    }
    return -1;
}

static void send_schedule_command(const char* farmId, const char* deviceId, bool enable, uint8_t delaySec, uint8_t runSec) {
    g_pendingMsg.clear();
    strlcpy(g_pendingMsg.farmId,   farmId,   sizeof(g_pendingMsg.farmId));
    strlcpy(g_pendingMsg.deviceId, deviceId, sizeof(g_pendingMsg.deviceId));
    strlcpy(g_pendingMsg.senderId, DEVICE_ID, sizeof(g_pendingMsg.senderId));
    g_pendingMsg.command      = enable ? CMD_SCHED_SET : CMD_SCHED_CLR;
    g_pendingMsg.messageId    = agri_next_message_id();
    g_pendingMsg.timestamp    = millis();
    g_pendingMsg.nonce        = delaySec;
    g_pendingMsg.devState     = runSec;
    g_pendingMsg.sourceNodeId = agri_get_node_id();

    g_pendingMsgId = g_pendingMsg.messageId;
    g_retryCount   = 0;

    bool ok;
    if (g_relayNodeId != 0) {
        ok = agri_send(g_relayNodeId, g_pendingMsg);
        if (!ok) ok = agri_broadcast(g_pendingMsg);
    } else {
        ok = agri_broadcast(g_pendingMsg);
    }

    if (ok) {
        g_txState  = TX_WAITING_ACK;
        g_ackTimer = millis();
    } else {
        g_txState = TX_FAILED;
    }

    int idx = grid_index_for_device(deviceId);
    if (idx >= 0 && idx < GRID_DEV_TILES) {
        if (enable) {
            g_schedUi[idx].enabled = true;
            g_schedUi[idx].running = false;
            g_schedUi[idx].leftSec = delaySec;
            g_schedUi[idx].delaySec = delaySec;
            g_schedUi[idx].runSec = runSec;
        } else {
            g_schedUi[idx].enabled = false;
            g_schedUi[idx].running = false;
            g_schedUi[idx].leftSec = 0;
        }
        grid_set_device_schedule(deviceId,
                                 g_schedUi[idx].enabled,
                                 g_schedUi[idx].running,
                                 g_schedUi[idx].leftSec,
                                 g_schedUi[idx].delaySec,
                                 g_schedUi[idx].runSec);
    }

    g_display.setLastCommand(agri_command_name(g_pendingMsg.command), g_pendingMsgId);
    g_display.setAckStatus(enable ? "SCH SET" : "SCH OFF");
    g_display.markDirty();
}

static void request_device_list() {
    if (!g_selectedFarm[0]) {
        g_lastDevlistReq = millis();
        return;
    }

    AgriMessage req;
    req.clear();
    strlcpy(req.farmId,   g_selectedFarm, sizeof(req.farmId));
    strlcpy(req.deviceId, "*",           sizeof(req.deviceId));
    strlcpy(req.senderId, DEVICE_ID,      sizeof(req.senderId));
    req.command      = CMD_DEVLIST_REQ;
    req.messageId    = agri_next_message_id();
    req.timestamp    = millis();
    req.nonce        = agri_random_nonce();
    req.sourceNodeId = agri_get_node_id();

    if (g_relayNodeId != 0) {
        agri_send(g_relayNodeId, req);
    } else {
        agri_broadcast(req);
    }

    g_lastDevlistReq = millis();
}

// ============================================================================
// Grid Toggle Callback  (fired by grid UI on device tile SELECT)
// ============================================================================
static void on_grid_toggle(const char* farmId, const char* deviceId, uint8_t cmd) {
    if (!g_deviceListReady) {
        Serial.println("[APP] Device list not synced yet");
        grid_clear_ack_pending(deviceId);
        g_display.setAckStatus("SYNC...");
        g_display.markDirty();
        return;
    }

    if (g_txState == TX_IDLE || g_txState == TX_FAILED) {
        send_command(farmId, deviceId, cmd);
    } else {
        Serial.println("[APP] Busy waiting for ACK — command ignored");
        grid_clear_ack_pending(deviceId);
    }
}

// ============================================================================
// Retry Logic
// ============================================================================
static void retry_send() {
    g_retryCount++;
    if (g_pendingMsg.command != CMD_SCHED_SET) {
        g_pendingMsg.nonce = agri_random_nonce();
    }
    g_pendingMsg.timestamp = millis();

    Serial.printf("[APP] Retry %d/%d for #%u\n",
                  g_retryCount, AGRI_MAX_RETRIES, g_pendingMsgId);

    bool ok;
    if (g_relayNodeId != 0) {
        ok = agri_send(g_relayNodeId, g_pendingMsg);
    } else {
        ok = agri_broadcast(g_pendingMsg);
    }

    if (ok) {
        g_txState  = TX_WAITING_ACK;
        g_ackTimer = millis();
    } else {
        g_txState = TX_FAILED;
    }

    char buf[16];
    snprintf(buf, sizeof(buf), "Retry %d/%d", g_retryCount, AGRI_MAX_RETRIES);
    g_display.setAckStatus(buf);
    g_display.markDirty();
}

// ============================================================================
// Message Handler  (called by transport layer)
// ============================================================================
static void on_message_received(uint32_t from, const AgriMessage& msg) {
    Serial.printf("[APP] Msg from %u  cmd=%s  farm=%s  dev=%s  mid=%u  st=%d\n",
                  from, agri_command_name(msg.command), msg.farmId,
                  msg.deviceId, msg.messageId, msg.devState);

    // --- Per-farm range tracking (before farm filter so all farms are seen) ---
    if (msg.command == CMD_HEARTBEAT || msg.command == CMD_STATUS_RSP ||
        msg.command == CMD_ACK) {
        range_on_farm_heartbeat(msg.farmId);
    }

    // --- Farm ID validation ---
    if (!is_selected_farm(msg.farmId)) {
        Serial.printf("[APP] REJECTED: Farm mismatch '%s' (selected '%s')\n",
                      msg.farmId, g_selectedFarm);
        return;
    }

    switch (msg.command) {
        case CMD_ACK: {
            if (g_txState == TX_WAITING_ACK && msg.messageId == g_pendingMsgId) {
                Serial.printf("[APP] ACK #%u — dev=%s  state=%s\n",
                              msg.messageId, msg.deviceId,
                              msg.devState ? "ON" : "OFF");

                g_txState      = TX_IDLE;
                g_relayNodeId  = from;

                // Range: relay responded — mark reachable
                range_on_heartbeat(from);

                // Update per-device state in grid UI
                bool devOn = (msg.devState != 0);
                grid_set_device_state(msg.deviceId, devOn);

                g_display.setAckStatus("OK");
                g_display.setDeviceState(devOn);
                g_display.setLastSource(from);
                g_display.markDirty();
                g_statusClearTime = millis() + AGRI_STATUS_CLEAR_MS;

                if (g_pendingMsg.command == CMD_SCHED_SET || g_pendingMsg.command == CMD_SCHED_CLR) {
                    int idx = grid_index_for_device(g_pendingMsg.deviceId);
                    if (idx >= 0 && idx < GRID_DEV_TILES) {
                        bool en = (g_pendingMsg.command == CMD_SCHED_SET);
                        if (!en) {
                            g_schedUi[idx].enabled = false;
                            g_schedUi[idx].running = false;
                            g_schedUi[idx].leftSec = 0;
                        }
                        grid_set_device_schedule(g_pendingMsg.deviceId,
                                                 g_schedUi[idx].enabled,
                                                 g_schedUi[idx].running,
                                                 g_schedUi[idx].leftSec,
                                                 g_schedUi[idx].delaySec,
                                                 g_schedUi[idx].runSec);
                    }
                    request_device_list();
                }

                agri_log(msg.farmId, msg.deviceId,
                         agri_command_name(g_pendingMsg.command), "OK");
            } else {
                Serial.printf("[APP] Unexpected ACK #%u (waiting #%u)\n",
                              msg.messageId, g_pendingMsgId);
            }
            break;
        }

        case CMD_NACK:
            if (g_txState == TX_WAITING_ACK && msg.messageId == g_pendingMsgId) {
                Serial.printf("[APP] NACK for #%u\n", msg.messageId);
                g_txState = TX_IDLE;

                // Clear pending flag — state unchanged
                grid_clear_ack_pending(msg.deviceId);

                g_display.setAckStatus("NACK!");
                g_display.markDirty();
                g_statusClearTime = millis() + AGRI_STATUS_CLEAR_MS;

                agri_log(msg.farmId, msg.deviceId,
                         agri_command_name(g_pendingMsg.command), "NACK");
            }
            break;

        case CMD_STATUS_RSP:
            Serial.printf("[APP] Status: dev=%s  state=%s\n",
                          msg.deviceId, msg.devState ? "ON" : "OFF");
            g_relayNodeId = from;
            range_on_heartbeat(from);
            grid_set_device_state(msg.deviceId, msg.devState != 0);
            g_display.setLastSource(from);
            g_display.markDirty();
            break;

        case CMD_DEVLIST_RSP: {
            g_relayNodeId = from;
            range_on_heartbeat(from);

            uint8_t idx = msg.nonce;
            if (idx < GRID_DEV_TILES) {
                const char* devId = msg.deviceId;
                char slotName[AGRI_DEVICE_ID_LEN] = {0};
                if (!devId[0]) {
                    snprintf(slotName, sizeof(slotName), "SLOT_%u", idx + 1);
                    devId = slotName;
                }
                bool devOn = (msg.devState & 0x01) != 0;
                bool schedRun = (msg.devState & 0x40) != 0;
                bool schedEn = (msg.devState & 0x80) != 0;
                grid_set_device_binding(idx, devId, devOn);
                g_schedUi[idx].enabled = schedEn;
                g_schedUi[idx].running = schedRun;
                if (!schedEn) {
                    g_schedUi[idx].running = false;
                    g_schedUi[idx].leftSec = 0;
                }
                grid_set_device_schedule(devId,
                                         g_schedUi[idx].enabled,
                                         g_schedUi[idx].running,
                                         g_schedUi[idx].leftSec,
                                         g_schedUi[idx].delaySec,
                                         g_schedUi[idx].runSec);
                g_deviceListReady = true;
                g_display.markDirty();
            }
            break;
        }

        case CMD_HEARTBEAT: {
            Serial.printf("[APP] Heartbeat from %s  devState=0x%02X\n",
                          msg.deviceId, msg.devState);
            range_on_heartbeat(from);
            g_relayNodeId = from;

            bool scheduleOn = (msg.devState & 0x80) != 0;
            uint8_t stateMask = (uint8_t)(msg.devState & 0x7F);

            // Decode device state bitmask from relay heartbeat
            if (strcmp(msg.deviceId, "*") == 0 && msg.devState != 0xFF) {
                for (uint8_t i = 0; i < GRID_DEV_TILES && i < 8; i++) {
                    GridDevice* dev = grid_device(i);
                    if (!dev) continue;
                    bool on = (stateMask >> i) & 1;
                    grid_set_device_state(dev->deviceId, on);
                }
            }

            g_display.setAckStatus(scheduleOn ? "SCH ON" : "SCH OFF");

            g_display.markDirty();
            break;
        }

        default:
            break;
    }
}

// ============================================================================
// Connection Change Handler
// ============================================================================
static void on_connection_change(bool connected, uint16_t nodeCount) {
    g_display.setMeshStatus(connected, nodeCount);
    g_display.markDirty();
    Serial.printf("[APP] Mesh %s — %d peer(s)\n",
                  connected ? "connected" : "no peers", nodeCount);
}

// ============================================================================
// Debounce a single button — returns true on falling edge (press detected)
// ============================================================================
static bool debounce(BtnState& b) {
    bool reading = digitalRead(b.pin);

    if (reading != b.raw) {
        b.debounceT = millis();
    }
    b.raw = reading;

    if ((millis() - b.debounceT) < AGRI_DEBOUNCE_MS) return false;

    if (reading != b.stable) {
        b.stable = reading;
        if (b.stable == LOW) {
            // Pressed
            b.fired = false;
        }
    }

    // Return single edge per press
    if (b.stable == LOW && !b.fired) {
        b.fired = true;
        return true;
    }
    return false;
}

// ============================================================================
// Debounce SEL — tracks stable state without auto-firing edge
// ============================================================================
static void debounce_sel() {
    bool reading = digitalRead(g_btnSel.pin);

    if (reading != g_btnSel.raw) {
        g_btnSel.debounceT = millis();
    }
    g_btnSel.raw = reading;

    if ((millis() - g_btnSel.debounceT) < AGRI_DEBOUNCE_MS) return;

    if (reading != g_btnSel.stable) {
        g_btnSel.stable = reading;
    }
}

// ============================================================================
// Process all 3 buttons
//   UP / DOWN: navigate grid tiles / scroll submenus
//   SEL: short press (<600ms) → toggle / select, long press → back
// ============================================================================
static void process_buttons(uint32_t now) {
    bool anyPress = false;

    // --- Detect raw presses (debounce but don't act yet) ---
    bool upEdge  = debounce(g_btnUp);
    bool dnEdge  = debounce(g_btnDown);
    debounce_sel();
    bool selDown = (g_btnSel.stable == LOW);
    bool selJustPressed = (selDown && !g_selPressed);

    if (upEdge || dnEdge || selJustPressed) anyPress = true;

    // --- If display is sleeping, first press only wakes — no action ---
    if (anyPress && g_displaySleeping) {
        g_displaySleeping = false;
        g_lastActivityTime = now;
        g_display.markDirty();
        // Consume SEL press state so release doesn't fire OK
        if (selJustPressed) {
            g_selPressed   = true;
            g_selPressTime = now;
            g_selLongFired = true;   // suppress both short & long action
        }
        Serial.println("[AOD] Display wake (input consumed)");
        return;                      // skip all navigation this cycle
    }

    // --- UP ---
    if (upEdge) {
        Serial.println("[BTN] UP");
        grid_on_up();
        g_display.markDirty();
    }

    // --- DOWN ---
    if (dnEdge) {
        Serial.println("[BTN] DOWN");
        grid_on_down();
        g_display.markDirty();
    }

    // --- SEL (middle — short / long press) ---
    if (selJustPressed) {
        g_selPressed   = true;
        g_selPressTime = now;
        g_selLongFired = false;
    }

    if (selDown && g_selPressed && !g_selLongFired) {
        // Still held — check for long press threshold
        if ((now - g_selPressTime) >= AGRI_LONG_PRESS_MS) {
            Serial.println("[BTN] SEL long → BACK");
            grid_on_back();
            g_display.markDirty();
            g_selLongFired = true;
        }
    }

    if (!selDown && g_selPressed) {
        // Released
        if (!g_selLongFired) {
            // Short press → OK / toggle
            Serial.println("[BTN] SEL short → OK");
            grid_on_ok();
            g_display.markDirty();
        }
        g_selPressed = false;
    }

    if (anyPress) {
        g_lastActivityTime = now;
    }
}

// ============================================================================
// Setup
// ============================================================================
void setup() {
    Serial.begin(115200);
    delay(500);

    Serial.println("\n========================================");
    Serial.println("  ESP-Agri Remote Node (Grid UI)");
    Serial.println("========================================\n");

    // --- 3 Buttons ---
    pinMode(AGRI_BTN_UP,   INPUT_PULLUP);
    pinMode(AGRI_BTN_DOWN, INPUT_PULLUP);
    pinMode(AGRI_BTN_SEL,  INPUT_PULLUP);
    Serial.printf("[APP] BTN_UP=%d  BTN_SEL=%d  BTN_DOWN=%d\n",
                  AGRI_BTN_UP, AGRI_BTN_SEL, AGRI_BTN_DOWN);

    // --- OLED (init early so farm picker can draw) ---
    g_display.init();

    // --- NVS persistence ---
    agri_nvs_init();

    {
        char savedRemoteId[AGRI_DEVICE_ID_LEN] = {0};
        if (agri_nvs_load_remote_id(savedRemoteId, sizeof(savedRemoteId))) {
            strlcpy(g_remoteId, savedRemoteId, sizeof(g_remoteId));
        }
        char savedRemoteList[256] = {0};
        if (agri_nvs_load_remote_farm_list(savedRemoteList, sizeof(savedRemoteList))) {
            strlcpy(g_remoteFarmListCsv, savedRemoteList, sizeof(g_remoteFarmListCsv));
        }
    }

    // --- Grid UI system (init before farm picker so grid state exists) ---
    grid_init(on_grid_toggle, nullptr);
    Serial.println("[APP] Grid UI initialised (6-tile dashboard)");

    init_remote_farm_list();

    // --- Load AOD settings from NVS ---
    {
        bool aodEn = true;
        uint8_t aodSec = AGRI_AOD_DEFAULT_SEC;
        if (agri_nvs_load_aod(&aodEn, &aodSec)) {
            grid_set_aod(aodEn);
            grid_set_aod_timeout(aodSec);
        }
        g_aodPrevEnabled = grid_aod_enabled();
        g_aodPrevSec     = grid_aod_timeout_sec();
    }
    g_lastActivityTime = millis();

    // --- Farm selection: always show picker on boot ---
    if (agri_nvs_load_farm(g_selectedFarm, sizeof(g_selectedFarm))) {
        // Pre-set the farm ID so status bar shows it while picker is open
        grid_set_farm_id(g_selectedFarm);
        Serial.printf("[APP] NVS has farm: %s — opening picker\n", g_selectedFarm);
    } else {
        Serial.println("[APP] No saved farm — opening picker");
    }
    enter_farm_picker();
    Serial.println("[APP] Farm picker active — waiting for selection");

    // --- Range indicator ---
    range_init();
    Serial.println("[APP] Range indicator initialised");

    // --- Transport (radio-agnostic API) ---
    agri_transport_init(&g_mesh);
    g_mesh.init();

    agri_on_receive(on_message_received);
    agri_on_connection_change(on_connection_change);

    // --- Seed random for nonces ---
    randomSeed(analogRead(0) ^ millis());

    // --- Display initial state ---
    g_display.setRole("REMOTE");
    g_display.setNodeId(agri_get_node_id());
    g_display.setFarmId(g_selectedFarm);
    g_display.setDeviceState(false);
    g_display.setMeshStatus(false, 0);

    Serial.printf("[APP] Setup complete — Node ID: %u  Farm: %s\n",
                  agri_get_node_id(), g_selectedFarm);
    Serial.println("[APP] UP=navigate  DOWN=navigate  SEL=short:toggle/long:back");
    Serial.println("[APP] Grid: PUMP | VALVE | LIGHT | MOTOR | AUX | SETUP");
    serial_print_cfg();

    // Prime a relay-side device list sync after boot.
    request_device_list();
}

// ============================================================================
// Loop
// ============================================================================
void loop() {
    // --- Drive mesh radio ---
    agri_update();
    handle_serial_cli();

    uint32_t now = millis();

    // --- Button handling (3-button: UP / SEL / DOWN) ---
    process_buttons(now);

    // --- Farm re-select (from Settings → Change Farm, or boot farm pick completed) ---
    char pickedFarm[AGRI_FARM_ID_LEN];
    if (grid_farm_selected(pickedFarm, sizeof(pickedFarm))) {
        strlcpy(g_selectedFarm, pickedFarm, sizeof(g_selectedFarm));
        grid_set_farm_id(g_selectedFarm);
        g_deviceListReady = false;
        g_display.setFarmId(g_selectedFarm);
        g_display.markDirty();
        agri_nvs_save_farm(g_selectedFarm);
        Serial.printf("[APP] Farm selected: %s (saved to NVS)\n", g_selectedFarm);

        request_device_list();
    }
    if (grid_wants_farm_select()) {
        enter_farm_picker();
    }

    // --- Schedule actions from remote OLED setup/device confirm ---
    {
        char reqDev[AGRI_DEVICE_ID_LEN] = {0};
        uint32_t reqDelay = 0;
        uint32_t reqRun = 0;
        if (grid_take_schedule_apply_request(reqDev, sizeof(reqDev), &reqDelay, &reqRun)) {
            uint8_t delaySec = (uint8_t)((reqDelay > 255) ? 255 : reqDelay);
            uint8_t runSec = (uint8_t)((reqRun > 255) ? 255 : (reqRun < 1 ? 1 : reqRun));
            send_schedule_command(g_selectedFarm, reqDev, true, delaySec, runSec);
        }

        char disableDev[AGRI_DEVICE_ID_LEN] = {0};
        if (grid_take_schedule_disable_request(disableDev, sizeof(disableDev))) {
            send_schedule_command(g_selectedFarm, disableDev, false, 0, 0);
        }
    }

    // --- Local schedule badge countdown model (mirrors remote-issued timers) ---
    if ((now - g_lastSchedUiTick) >= 1000UL) {
        g_lastSchedUiTick = now;
        bool changed = false;
        for (uint8_t i = 0; i < GRID_DEV_TILES; i++) {
            GridDevice* dev = grid_device(i);
            if (!dev || !g_schedUi[i].enabled) continue;
            if (g_schedUi[i].leftSec > 0) g_schedUi[i].leftSec--;
            if (!g_schedUi[i].running && g_schedUi[i].leftSec == 0) {
                g_schedUi[i].running = true;
                g_schedUi[i].leftSec = g_schedUi[i].runSec;
            } else if (g_schedUi[i].running && g_schedUi[i].leftSec == 0) {
                g_schedUi[i].enabled = false;
                g_schedUi[i].running = false;
            }
            grid_set_device_schedule(dev->deviceId,
                                     g_schedUi[i].enabled,
                                     g_schedUi[i].running,
                                     g_schedUi[i].leftSec,
                                     g_schedUi[i].delaySec,
                                     g_schedUi[i].runSec);
            changed = true;
        }
        if (changed) g_display.markDirty();
    }

    // --- Grid UI timeout (return to main grid after inactivity) ---
    if (grid_check_timeout()) {
        g_display.markDirty();
    }

    // --- AOD: reset idle timer when screen transitions TO GRID_MAIN ---
    // Ensures sleep timeout counts only continuous idle time on GRID_MAIN,
    // not time spent in submenus (including auto-return via menu timeout).
    {
        GridScreen cur = grid_current_screen();
        if (cur == GRID_MAIN && g_prevScreenForAod != GRID_MAIN) {
            g_lastActivityTime = now;
        }
        g_prevScreenForAod = cur;
    }

    // --- ACK timeout & retry ---
    if (g_txState == TX_WAITING_ACK) {
        if ((now - g_ackTimer) >= AGRI_ACK_TIMEOUT_MS) {
            if (g_retryCount < AGRI_MAX_RETRIES) {
                retry_send();
            } else {
                Serial.printf("[APP] TIMEOUT: No ACK for #%u after %d retries\n",
                              g_pendingMsgId, AGRI_MAX_RETRIES);
                g_txState = TX_FAILED;

                // Clear grid ack pending flag + trigger fail flash
                grid_set_fail_flash(g_pendingMsg.deviceId);

                g_display.setAckStatus("TIMEOUT!");
                g_display.markDirty();
                g_statusClearTime = now + AGRI_STATUS_CLEAR_MS;

                agri_log(g_pendingMsg.farmId, g_pendingMsg.deviceId,
                         agri_command_name(g_pendingMsg.command), "TIMEOUT");
            }
        }
    }

    // --- Clear stale ACK status ---
    if (g_statusClearTime && now >= g_statusClearTime) {
        g_statusClearTime = 0;
        if (g_txState == TX_IDLE || g_txState == TX_FAILED) {
            g_txState = TX_IDLE;
        }
    }

    // --- Range indicator: update state (check timeout) ---
    if (range_update()) {
        g_display.markDirty();     // state changed → redraw status bar
    }

    // --- RSSI polling (every AGRI_RSSI_POLL_MS) ---
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

    // --- Periodic range ping (STATUS_REQ broadcast) ---
    if (range_should_ping() && g_txState == TX_IDLE) {
        AgriMessage ping;
        ping.clear();
        strlcpy(ping.farmId,   g_selectedFarm, sizeof(ping.farmId));
        strlcpy(ping.deviceId, "*",           sizeof(ping.deviceId));
        strlcpy(ping.senderId, DEVICE_ID,      sizeof(ping.senderId));
        ping.command      = CMD_STATUS_REQ;
        ping.messageId    = agri_next_message_id();
        ping.timestamp    = millis();
        ping.nonce        = agri_random_nonce();
        ping.sourceNodeId = agri_get_node_id();

        if (g_relayNodeId != 0) {
            agri_send(g_relayNodeId, ping);
        } else {
            agri_broadcast(ping);
        }
    }

    // --- Periodic device list sync from relay dashboard-configured map ---
    if ((now - g_lastDevlistReq) >= AGRI_RANGE_PING_MS) {
        request_device_list();
    }

    // --- Periodic display refresh ---
    if (now - g_lastDisplayRefresh >= AGRI_DISPLAY_REFRESH_MS) {
        g_lastDisplayRefresh = now;
        if (!g_displaySleeping) {
            g_display.refresh();
        }
    }

    // --- AOD: auto-sleep display when inactive (only on main grid) ---
    if (!grid_aod_enabled() && !g_displaySleeping
        && grid_current_screen() == GRID_MAIN)
    {
        uint32_t timeout = (uint32_t)grid_aod_timeout_sec() * 1000UL;
        if ((now - g_lastActivityTime) >= timeout) {
            g_displaySleeping = true;
            // Clear screen to turn off pixels
            g_display.showSplash("", "");
            Serial.printf("[AOD] Display sleep after %us\n", grid_aod_timeout_sec());
        }
    }

    // --- AOD: persist settings when changed ---
    {
        bool  curEn  = grid_aod_enabled();
        uint8_t curS = grid_aod_timeout_sec();
        if (curEn != g_aodPrevEnabled || curS != g_aodPrevSec) {
            agri_nvs_save_aod(curEn, curS);
            g_aodPrevEnabled = curEn;
            g_aodPrevSec     = curS;
            // Reset activity timer when settings change
            g_lastActivityTime = now;
        }
    }
}
