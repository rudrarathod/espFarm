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
static const char*  DEVICE_ID = "REMOTE01";

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

// AOD (Always-On Display) — sleep/wake
static bool     g_displaySleeping    = false;
static uint32_t g_lastActivityTime   = 0;
static bool     g_aodPrevEnabled     = true;
static uint8_t  g_aodPrevSec         = AGRI_AOD_DEFAULT_SEC;
static GridScreen g_prevScreenForAod = GRID_MAIN;  // track screen transitions for AOD

// RSSI polling
static uint32_t g_lastRssiPoll       = 0;
static RssiTracker g_rssiTracker;

// ============================================================================
// Farm Selection  (boot-time picker)
// ============================================================================
static const char* s_farmList[] = {
    "FARM_101", "FARM_102", "FARM_103", "FARM_TST"
};
static const uint8_t s_farmCount = sizeof(s_farmList) / sizeof(s_farmList[0]);
static char g_selectedFarm[AGRI_FARM_ID_LEN] = "";

/// Non-blocking farm picker — enters the grid-based farm selection screen
static void enter_farm_picker() {
    grid_enter_farm_select(s_farmList, s_farmCount);
    g_display.markDirty();
}

// ============================================================================
// Send a command  (farm + device from grid tile, cmd from toggle callback)
// ============================================================================
static void send_command(const char* farmId, const char* deviceId, uint8_t cmd) {
    g_pendingMsg.clear();
    strlcpy(g_pendingMsg.farmId,   farmId,   sizeof(g_pendingMsg.farmId));
    strlcpy(g_pendingMsg.deviceId, deviceId, sizeof(g_pendingMsg.deviceId));
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

// ============================================================================
// Grid Toggle Callback  (fired by grid UI on device tile SELECT)
// ============================================================================
static void on_grid_toggle(const char* farmId, const char* deviceId, uint8_t cmd) {
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
    g_pendingMsg.nonce     = agri_random_nonce();
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
    if (!agri_validate_farm_id(msg.farmId)) {
        Serial.printf("[APP] REJECTED: Farm ID mismatch '%s'\n", msg.farmId);
        return;
    }

    switch (msg.command) {
        case CMD_ACK:
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

                agri_log(msg.farmId, msg.deviceId,
                         agri_command_name(g_pendingMsg.command), "OK");
            } else {
                Serial.printf("[APP] Unexpected ACK #%u (waiting #%u)\n",
                              msg.messageId, g_pendingMsgId);
            }
            break;

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

        case CMD_HEARTBEAT:
            Serial.printf("[APP] Heartbeat from %s  devState=0x%02X\n",
                          msg.deviceId, msg.devState);
            range_on_heartbeat(from);
            g_relayNodeId = from;

            // Decode device state bitmask from relay heartbeat
            if (strcmp(msg.deviceId, "*") == 0 && msg.devState != 0xFF) {
                for (uint8_t i = 0; i < GRID_DEV_TILES && i < 8; i++) {
                    GridDevice* dev = grid_device(i);
                    if (!dev) continue;
                    bool on = (msg.devState >> i) & 1;
                    grid_set_device_state(dev->deviceId, on);
                }
            }

            g_display.markDirty();
            break;

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

    // --- Grid UI system (init before farm picker so grid state exists) ---
    grid_init(on_grid_toggle, nullptr);
    Serial.println("[APP] Grid UI initialised (6-tile dashboard)");

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
}

// ============================================================================
// Loop
// ============================================================================
void loop() {
    // --- Drive mesh radio ---
    agri_update();

    uint32_t now = millis();

    // --- Button handling (3-button: UP / SEL / DOWN) ---
    process_buttons(now);

    // --- Farm re-select (from Settings → Change Farm, or boot farm pick completed) ---
    char pickedFarm[AGRI_FARM_ID_LEN];
    if (grid_farm_selected(pickedFarm, sizeof(pickedFarm))) {
        strlcpy(g_selectedFarm, pickedFarm, sizeof(g_selectedFarm));
        grid_set_farm_id(g_selectedFarm);
        g_display.setFarmId(g_selectedFarm);
        g_display.markDirty();
        agri_nvs_save_farm(g_selectedFarm);
        Serial.printf("[APP] Farm selected: %s (saved to NVS)\n", g_selectedFarm);
    }
    if (grid_wants_farm_select()) {
        enter_farm_picker();
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
