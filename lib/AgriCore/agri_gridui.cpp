/*******************************************************************************
 * ESP-Agri Grid UI — Implementation
 *
 * Navigation state machine for the 2×3 tile dashboard.
 * Device tiles toggle via agri_send(); Settings tile opens submenu.
 ******************************************************************************/
#include "agri_gridui.h"
#include "agri_protocol.h"
#include "agri_log.h"

// ============================================================================
// Default Device Configuration (5 tiles)
// ============================================================================
static GridDevice s_devices[GRID_DEV_TILES] = {
    { "SLOT1", "SLOT_1", false, false, false, 0, false, false, 0, 0, 0 },
    { "SLOT2", "SLOT_2", false, false, false, 0, false, false, 0, 0, 0 },
    { "SLOT3", "SLOT_3", false, false, false, 0, false, false, 0, 0, 0 },
    { "SLOT4", "SLOT_4", false, false, false, 0, false, false, 0, 0, 0 },
    { "SLOT5", "SLOT_5", false, false, false, 0, false, false, 0, 0, 0 }
};

// ============================================================================
// Settings Submenu Items
// ============================================================================
#if AGRI_NODE_ROLE == AGRI_ROLE_RELAY
static const char* s_settingsLabels[] = {
    nullptr,            // AOD label — set to s_aodLabel below
    "AOD Time",
    "Schedule",
    "Mesh Status",
    "Debug Logs",
    "Node Info",
    "Back"
};
static const uint8_t s_settingsCount = 7;

static const uint8_t SET_IDX_AOD_TOGGLE = 0;
static const uint8_t SET_IDX_AOD_TIME   = 1;
static const uint8_t SET_IDX_SCHEDULE   = 2;
static const uint8_t SET_IDX_MESH_INFO  = 3;
static const uint8_t SET_IDX_DEBUG_LOGS = 4;
static const uint8_t SET_IDX_NODE_INFO  = 5;
static const uint8_t SET_IDX_BACK       = 6;
#else
static const char* s_settingsLabels[] = {
    "Change Farm",
    nullptr,            // AOD label — set to s_aodLabel below
    "AOD Time",
    "Schedule",
    "Mesh Status",
    "Debug Logs",
    "Node Info",
    "Back"
};
static const uint8_t s_settingsCount = 8;

static const uint8_t SET_IDX_CHANGE_FARM = 0;
static const uint8_t SET_IDX_AOD_TOGGLE  = 1;
static const uint8_t SET_IDX_AOD_TIME    = 2;
static const uint8_t SET_IDX_SCHEDULE    = 3;
static const uint8_t SET_IDX_MESH_INFO   = 4;
static const uint8_t SET_IDX_DEBUG_LOGS  = 5;
static const uint8_t SET_IDX_NODE_INFO   = 6;
static const uint8_t SET_IDX_BACK        = 7;
#endif

// Dynamic AOD label buffer ("AOD: ON" / "AOD: OFF")
static char s_aodLabel[12] = "AOD: ON";

// Always-On Display state
static bool    s_aodEnabled    = true;
static uint8_t s_aodTimeoutSec = AGRI_AOD_DEFAULT_SEC;

// ============================================================================
// State
// ============================================================================
static GridScreen       s_screen       = GRID_MAIN;
static uint8_t          s_gridCursor   = 0;       // 0–5 on main grid
static uint8_t          s_settingsCur  = 0;       // settings submenu cursor
static uint8_t          s_logScroll    = 0;       // debug log scroll offset
static grid_toggle_cb_t s_toggleCb     = nullptr;
static uint32_t         s_lastTouch    = 0;
static char             s_farmId[AGRI_FARM_ID_LEN] = "";
static bool             s_wantsFarmSelect = false;
static uint8_t          s_confirmIdx   = 0;       // device index pending confirm
static uint8_t          s_confirmCmd   = 0;       // command pending confirm
static bool             s_confirmSchedDisable = false;

// Schedule setup state (relay OLED)
static uint8_t          s_schedDevIdx = 0;
static uint8_t          s_schedField  = 0;  // 0=device 1=delay 2=run 3=apply
static uint32_t         s_schedDelaySec = 30;
static uint32_t         s_schedRunSec   = 60;
static bool             s_schedApplyReq = false;
static char             s_schedReqDevId[AGRI_DEVICE_ID_LEN] = "";
static uint32_t         s_schedReqDelay = 0;
static uint32_t         s_schedReqRun   = 0;
static bool             s_schedDisableReq = false;
static char             s_schedDisableDevId[AGRI_DEVICE_ID_LEN] = "";

// Farm picker state
static const char* const* s_farmList   = nullptr;
static uint8_t            s_farmCount  = 0;
static uint8_t            s_farmCursor = 0;
static bool               s_farmPicked = false;
static char               s_pickedFarm[AGRI_FARM_ID_LEN] = "";

// ============================================================================
// Init
// ============================================================================
void grid_init(grid_toggle_cb_t onToggle, const char* farmId) {
    s_toggleCb    = onToggle;
    s_screen      = GRID_MAIN;
    s_gridCursor  = 0;
    s_settingsCur = 0;
    s_logScroll   = 0;
    s_lastTouch   = millis();
    s_confirmSchedDisable = false;
    s_schedField = 0;
    s_schedDevIdx = 0;
    // Point settings label to dynamic AOD buffer
    s_settingsLabels[SET_IDX_AOD_TOGGLE] = s_aodLabel;
    if (farmId && farmId[0]) {
        strlcpy(s_farmId, farmId, sizeof(s_farmId));
    } else {
        strlcpy(s_farmId, AGRI_FARM_ID, sizeof(s_farmId));
    }
}

// ============================================================================
// UP — move cursor up / scroll up  (wraps on main grid)
// ============================================================================
void grid_on_up() {
    grid_touch();

    switch (s_screen) {
        case GRID_MAIN:
            if (s_gridCursor >= GRID_COLS) {
                s_gridCursor -= GRID_COLS;     // move visually up
            } else {
                // top row → wrap to bottom of previous column
                uint8_t col = (s_gridCursor + GRID_COLS - 1) % GRID_COLS;
                s_gridCursor = (GRID_ROWS - 1) * GRID_COLS + col;
            }
            break;

        case GRID_SETTINGS:
            s_settingsCur = (s_settingsCur == 0)
                            ? (s_settingsCount - 1)
                            : (s_settingsCur - 1);
            break;

        case GRID_DEBUG_LOGS:
            if (s_logScroll > 0) s_logScroll--;
            break;

        case GRID_MESH_INFO:
        case GRID_NODE_INFO:
            // Info screens — no scroll action
            break;

        case GRID_FARM_SEL:
            if (s_farmCount > 0) {
                s_farmCursor = (s_farmCursor == 0)
                               ? (s_farmCount - 1)
                               : (s_farmCursor - 1);
            }
            break;

        case GRID_CONFIRM:
            break;

        case GRID_AOD_TIME:
            // UP increases timeout
            if (s_aodTimeoutSec + AGRI_AOD_STEP_SEC <= AGRI_AOD_MAX_SEC)
                s_aodTimeoutSec += AGRI_AOD_STEP_SEC;
            break;

        case GRID_SCHED_SETUP:
            if (s_schedField == 0) {
                s_schedDevIdx = (s_schedDevIdx == 0) ? (GRID_DEV_TILES - 1) : (s_schedDevIdx - 1);
                s_schedDelaySec = s_devices[s_schedDevIdx].schedDelaySec ? s_devices[s_schedDevIdx].schedDelaySec : 30;
                s_schedRunSec = s_devices[s_schedDevIdx].schedRunSec ? s_devices[s_schedDevIdx].schedRunSec : 60;
            } else if (s_schedField == 1) {
                if (s_schedDelaySec < 86400UL) s_schedDelaySec += 5;
            } else if (s_schedField == 2) {
                if (s_schedRunSec < 86400UL) s_schedRunSec += 5;
            } else {
                s_schedField = 0;
            }
            break;
    }
}

// ============================================================================
// DOWN — move cursor down / scroll down  (wraps on main grid)
// ============================================================================
void grid_on_down() {
    grid_touch();

    switch (s_screen) {
        case GRID_MAIN: {
            uint8_t next = s_gridCursor + GRID_COLS;
            if (next < GRID_TILES) {
                s_gridCursor = next;           // move visually down
            } else {
                // bottom row → wrap to top of next column
                s_gridCursor = (s_gridCursor % GRID_COLS + 1) % GRID_COLS;
            }
            break;
        }

        case GRID_SETTINGS:
            s_settingsCur = (s_settingsCur + 1) % s_settingsCount;
            break;

        case GRID_DEBUG_LOGS: {
            uint8_t cnt      = agri_log_count();
            uint8_t maxVis   = 4;   // visible rows on log screen (4 rows + footer)
            uint8_t maxScroll = (cnt > maxVis) ? (cnt - maxVis) : 0;
            if (s_logScroll < maxScroll) s_logScroll++;
            break;
        }

        case GRID_MESH_INFO:
        case GRID_NODE_INFO:
            break;

        case GRID_FARM_SEL:
            if (s_farmCount > 0) {
                s_farmCursor = (s_farmCursor + 1) % s_farmCount;
            }
            break;

        case GRID_CONFIRM:
            break;

        case GRID_AOD_TIME:
            // DOWN decreases timeout
            if (s_aodTimeoutSec > AGRI_AOD_MIN_SEC + AGRI_AOD_STEP_SEC)
                s_aodTimeoutSec -= AGRI_AOD_STEP_SEC;
            else
                s_aodTimeoutSec = AGRI_AOD_MIN_SEC;
            break;

        case GRID_SCHED_SETUP:
            if (s_schedField == 0) {
                s_schedDevIdx = (s_schedDevIdx + 1) % GRID_DEV_TILES;
                s_schedDelaySec = s_devices[s_schedDevIdx].schedDelaySec ? s_devices[s_schedDevIdx].schedDelaySec : 30;
                s_schedRunSec = s_devices[s_schedDevIdx].schedRunSec ? s_devices[s_schedDevIdx].schedRunSec : 60;
            } else if (s_schedField == 1) {
                if (s_schedDelaySec >= 5) s_schedDelaySec -= 5;
                else s_schedDelaySec = 0;
            } else if (s_schedField == 2) {
                if (s_schedRunSec > 5) s_schedRunSec -= 5;
                else s_schedRunSec = 1;
            } else {
                s_schedField = 3;
            }
            break;
    }
}

// ============================================================================
// OK — select / confirm  (short press)
// ============================================================================
void grid_on_ok() {
    grid_touch();

    switch (s_screen) {
        // ---- Main Grid ----
        case GRID_MAIN:
            if (s_gridCursor < GRID_DEV_TILES) {
                // Device tile → show confirmation screen
                GridDevice& dev = s_devices[s_gridCursor];
                if (!dev.ackPending) {
                    s_confirmIdx = s_gridCursor;
                    s_confirmSchedDisable = dev.schedEnabled;
                    s_confirmCmd = s_confirmSchedDisable ? CMD_TOGGLE : (dev.state ? CMD_DEV_OFF : CMD_DEV_ON);
                    s_screen = GRID_CONFIRM;
                }
            } else {
                // Settings tile → open settings submenu
                s_screen      = GRID_SETTINGS;
                s_settingsCur = 0;
            }
            break;

        // ---- Confirm Screen ----
        case GRID_CONFIRM:
            // OK on confirm → fire the toggle command
            {
                GridDevice& dev = s_devices[s_confirmIdx];
                if (s_confirmSchedDisable) {
                    strlcpy(s_schedDisableDevId, dev.deviceId, sizeof(s_schedDisableDevId));
                    s_schedDisableReq = true;
                } else if (s_toggleCb) {
                    dev.ackPending = true;
                    s_toggleCb(s_farmId, dev.deviceId, s_confirmCmd);
                }
            }
            s_screen = GRID_MAIN;
            break;

        // ---- Settings Submenu ----
        case GRID_SETTINGS:
            switch (s_settingsCur) {
    #if AGRI_NODE_ROLE == AGRI_ROLE_RELAY
            case SET_IDX_AOD_TOGGLE:
                s_aodEnabled = !s_aodEnabled;
                strlcpy(s_aodLabel, s_aodEnabled ? "AOD: ON" : "AOD: OFF",
                    sizeof(s_aodLabel));
                break;
            case SET_IDX_AOD_TIME:  s_screen = GRID_AOD_TIME;   break;
            case SET_IDX_SCHEDULE:
                s_schedField = 0;
                s_schedDevIdx = 0;
                s_schedDelaySec = s_devices[s_schedDevIdx].schedDelaySec ? s_devices[s_schedDevIdx].schedDelaySec : 30;
                s_schedRunSec = s_devices[s_schedDevIdx].schedRunSec ? s_devices[s_schedDevIdx].schedRunSec : 60;
                s_screen = GRID_SCHED_SETUP;
                break;
            case SET_IDX_MESH_INFO: s_screen = GRID_MESH_INFO;  break;
            case SET_IDX_DEBUG_LOGS:
                s_screen = GRID_DEBUG_LOGS;
                s_logScroll = 0;
                break;
            case SET_IDX_NODE_INFO: s_screen = GRID_NODE_INFO;  break;
            case SET_IDX_BACK:      s_screen = GRID_MAIN;       break;
    #else
            case SET_IDX_CHANGE_FARM:
                s_screen = GRID_FARM_SEL;
                s_wantsFarmSelect = true;
                break;
            case SET_IDX_AOD_TOGGLE:
                s_aodEnabled = !s_aodEnabled;
                strlcpy(s_aodLabel, s_aodEnabled ? "AOD: ON" : "AOD: OFF",
                    sizeof(s_aodLabel));
                break;
            case SET_IDX_AOD_TIME:  s_screen = GRID_AOD_TIME;   break;
            case SET_IDX_SCHEDULE:
                s_schedField = 0;
                s_schedDevIdx = 0;
                s_schedDelaySec = s_devices[s_schedDevIdx].schedDelaySec ? s_devices[s_schedDevIdx].schedDelaySec : 30;
                s_schedRunSec = s_devices[s_schedDevIdx].schedRunSec ? s_devices[s_schedDevIdx].schedRunSec : 60;
                s_screen = GRID_SCHED_SETUP;
                break;
            case SET_IDX_MESH_INFO: s_screen = GRID_MESH_INFO;  break;
            case SET_IDX_DEBUG_LOGS:
                s_screen = GRID_DEBUG_LOGS;
                s_logScroll = 0;
                break;
            case SET_IDX_NODE_INFO: s_screen = GRID_NODE_INFO;  break;
            case SET_IDX_BACK:      s_screen = GRID_MAIN;       break;
    #endif
            }
            break;

        // ---- Info / Log screens — OK does nothing ----
        case GRID_MESH_INFO:
        case GRID_DEBUG_LOGS:
        case GRID_NODE_INFO:
        case GRID_AOD_TIME:
            break;

        case GRID_SCHED_SETUP:
            if (s_schedField < 3) {
                s_schedField++;
                if (s_schedField == 1) {
                    s_schedDelaySec = s_devices[s_schedDevIdx].schedDelaySec ? s_devices[s_schedDevIdx].schedDelaySec : 30;
                } else if (s_schedField == 2) {
                    s_schedRunSec = s_devices[s_schedDevIdx].schedRunSec ? s_devices[s_schedDevIdx].schedRunSec : 60;
                }
            } else {
                GridDevice& dev = s_devices[s_schedDevIdx];
                strlcpy(s_schedReqDevId, dev.deviceId, sizeof(s_schedReqDevId));
                s_schedReqDelay = s_schedDelaySec;
                s_schedReqRun = s_schedRunSec;
                s_schedApplyReq = true;
                s_screen = GRID_MAIN;
            }
            break;

        // ---- Farm Selection ----
        case GRID_FARM_SEL:
            if (s_farmList && s_farmCount > 0) {
                strlcpy(s_pickedFarm, s_farmList[s_farmCursor], sizeof(s_pickedFarm));
                strlcpy(s_farmId, s_pickedFarm, sizeof(s_farmId));
                s_farmPicked = true;
                s_wantsFarmSelect = false;
                s_screen = GRID_MAIN;
            }
            break;
    }
}

// ============================================================================
// BACK — navigate to parent  (long press)
// ============================================================================
void grid_on_back() {
    grid_touch();

    switch (s_screen) {
        case GRID_MAIN:
            // Already at root — no action
            break;

        case GRID_SETTINGS:
        case GRID_MESH_INFO:
        case GRID_DEBUG_LOGS:
        case GRID_NODE_INFO:
        case GRID_FARM_SEL:
            s_screen = GRID_MAIN;
            s_wantsFarmSelect = false;
            break;

        case GRID_AOD_TIME:
            s_screen = GRID_SETTINGS;
            break;

        case GRID_SCHED_SETUP:
            s_screen = GRID_SETTINGS;
            break;

        case GRID_CONFIRM:
            // Cancel confirmation → return to grid
            s_screen = GRID_MAIN;
            break;
    }
}

// ============================================================================
// Accessors
// ============================================================================
GridScreen  grid_current_screen()  { return s_screen; }
uint8_t     grid_cursor()          { return s_gridCursor; }
GridDevice* grid_device(uint8_t i) { return (i < GRID_DEV_TILES) ? &s_devices[i] : nullptr; }
uint8_t     grid_device_count()    { return GRID_DEV_TILES; }
const char* grid_farm_id()         { return s_farmId; }

void grid_set_farm_id(const char* farmId) {
    if (farmId && farmId[0]) {
        strlcpy(s_farmId, farmId, sizeof(s_farmId));
    }
}

bool grid_wants_farm_select() {
    if (s_wantsFarmSelect) {
        s_wantsFarmSelect = false;
        return true;
    }
    return false;
}

// ============================================================================
// Device State Updates  (called from ACK / NACK handlers)
// ============================================================================
bool grid_set_device_binding(uint8_t idx, const char* deviceId, bool state) {
    if (idx >= GRID_DEV_TILES || !deviceId || !deviceId[0]) return false;

    GridDevice& dev = s_devices[idx];
    strlcpy(dev.deviceId, deviceId, sizeof(dev.deviceId));

    uint8_t labelPos = 0;
    for (uint8_t i = 0; deviceId[i] && labelPos < sizeof(dev.label) - 1; i++) {
        if (deviceId[i] == '_') break;
        dev.label[labelPos++] = deviceId[i];
    }
    dev.label[labelPos] = '\0';

    dev.state      = state;
    dev.ackPending = false;
    dev.failFlash  = false;
    dev.schedEnabled = false;
    dev.schedRunning = false;
    dev.schedLeftSec = 0;
    dev.schedDelaySec = 0;
    dev.schedRunSec = 0;
    return true;
}

void grid_set_device_state(const char* deviceId, bool state) {
    for (uint8_t i = 0; i < GRID_DEV_TILES; i++) {
        if (strcmp(s_devices[i].deviceId, deviceId) == 0) {
            s_devices[i].state      = state;
            s_devices[i].ackPending = false;
            return;
        }
    }
}

void grid_clear_ack_pending(const char* deviceId) {
    for (uint8_t i = 0; i < GRID_DEV_TILES; i++) {
        if (strcmp(s_devices[i].deviceId, deviceId) == 0) {
            s_devices[i].ackPending = false;
            return;
        }
    }
}

void grid_set_fail_flash(const char* deviceId) {
    for (uint8_t i = 0; i < GRID_DEV_TILES; i++) {
        if (strcmp(s_devices[i].deviceId, deviceId) == 0) {
            s_devices[i].ackPending   = false;
            s_devices[i].failFlash    = true;
            s_devices[i].failFlashTime = millis();
            return;
        }
    }
}

void grid_set_device_schedule(const char* deviceId,
                              bool enabled,
                              bool running,
                              uint32_t leftSec,
                              uint32_t delaySec,
                              uint32_t runSec) {
    for (uint8_t i = 0; i < GRID_DEV_TILES; i++) {
        if (strcmp(s_devices[i].deviceId, deviceId) == 0) {
            s_devices[i].schedEnabled = enabled;
            s_devices[i].schedRunning = running;
            s_devices[i].schedLeftSec = leftSec;
            s_devices[i].schedDelaySec = delaySec;
            s_devices[i].schedRunSec = runSec;
            return;
        }
    }
}

bool grid_take_schedule_apply_request(char* outDeviceId,
                                      uint8_t outLen,
                                      uint32_t* outDelaySec,
                                      uint32_t* outRunSec) {
    if (!s_schedApplyReq) return false;
    s_schedApplyReq = false;
    if (outDeviceId && outLen) strlcpy(outDeviceId, s_schedReqDevId, outLen);
    if (outDelaySec) *outDelaySec = s_schedReqDelay;
    if (outRunSec) *outRunSec = s_schedReqRun;
    return true;
}

bool grid_take_schedule_disable_request(char* outDeviceId, uint8_t outLen) {
    if (!s_schedDisableReq) return false;
    s_schedDisableReq = false;
    if (outDeviceId && outLen) strlcpy(outDeviceId, s_schedDisableDevId, outLen);
    return true;
}

uint8_t grid_schedule_field() { return s_schedField; }
uint8_t grid_schedule_device_index() { return s_schedDevIdx; }
uint32_t grid_schedule_delay_sec() { return s_schedDelaySec; }
uint32_t grid_schedule_run_sec() { return s_schedRunSec; }

// ============================================================================
// Settings Submenu Accessors
// ============================================================================
uint8_t     grid_settings_cursor()          { return s_settingsCur; }
const char* grid_settings_label(uint8_t i)  { return (i < s_settingsCount) ? s_settingsLabels[i] : ""; }
uint8_t     grid_settings_count()           { return s_settingsCount; }

// ============================================================================
// Debug Log Scroll
// ============================================================================
uint8_t grid_log_scroll() { return s_logScroll; }

// ============================================================================
// Confirm Screen Accessors
// ============================================================================
const char* grid_confirm_device_label() {
    return (s_confirmIdx < GRID_DEV_TILES) ? s_devices[s_confirmIdx].label : "";
}

const char* grid_confirm_action() {
    if (s_confirmSchedDisable) return "SCH OFF";
    return (s_confirmCmd == CMD_DEV_ON) ? "ON" : "OFF";
}

// ============================================================================
// Timeout — return to main grid after inactivity
// ============================================================================
void grid_touch() {
    s_lastTouch = millis();
}

bool grid_check_timeout() {
    uint32_t now = millis();
    bool changed = false;

    // Auto-clear fail flash after 1.5 seconds
    for (uint8_t i = 0; i < GRID_DEV_TILES; i++) {
        if (s_devices[i].failFlash &&
            (now - s_devices[i].failFlashTime) > 1500)
        {
            s_devices[i].failFlash = false;
            changed = true;
        }
    }

    if (s_screen != GRID_MAIN &&
        (now - s_lastTouch) > AGRI_MENU_TIMEOUT_MS)
    {
        s_screen     = GRID_MAIN;
        s_gridCursor = 0;
        changed = true;
    }

    return changed;
}

// ============================================================================
// Farm Picker — Non-blocking
// ============================================================================
void grid_enter_farm_select(const char* const* farms, uint8_t count) {
    s_farmList   = farms;
    s_farmCount  = count;
    s_farmCursor = 0;
    s_farmPicked = false;
    s_wantsFarmSelect = false;
    s_screen     = GRID_FARM_SEL;
}

uint8_t     grid_farm_sel_cursor()          { return s_farmCursor; }
const char* grid_farm_sel_item(uint8_t i)   { return (s_farmList && i < s_farmCount) ? s_farmList[i] : ""; }
uint8_t     grid_farm_sel_count()           { return s_farmCount; }

bool grid_farm_selected(char* outFarm, uint8_t outLen) {
    if (s_farmPicked) {
        s_farmPicked = false;
        strlcpy(outFarm, s_pickedFarm, outLen);
        return true;
    }
    return false;
}

// ============================================================================
// AOD (Always-On Display) Accessors
// ============================================================================
bool     grid_aod_enabled()                { return s_aodEnabled; }
void     grid_set_aod(bool en) {
    s_aodEnabled = en;
    // Keep the dynamic label in sync
    strlcpy(s_aodLabel, en ? "AOD: ON" : "AOD: OFF", sizeof(s_aodLabel));
}
uint8_t  grid_aod_timeout_sec()            { return s_aodTimeoutSec; }
void     grid_set_aod_timeout(uint8_t s)   { s_aodTimeoutSec = s; }
