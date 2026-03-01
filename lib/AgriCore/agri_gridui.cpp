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
    { "PUMP",  "PUMP_01",  false, false, false, 0, 0 },
    { "VALVE", "VALVE_01", false, false, false, 0, 0 },
    { "LIGHT", "LIGHT_01", false, false, false, 0, 0 },
    { "MOTOR", "MOTOR_01", false, false, false, 0, 0 },
    { "AUX",   "AUX_01",   false, false, false, 0, 0 }
};

// ============================================================================
// Settings Submenu Items
// ============================================================================
static const char* s_settingsLabels[] = {
    "Change Farm",
    nullptr,            // AOD label — set to s_aodLabel below
    "AOD Time",
    "Mesh Status",
    "Debug Logs",
    "Node Info",
    "Back"
};
static const uint8_t s_settingsCount = 7;

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
    // Point settings label to dynamic AOD buffer
    s_settingsLabels[1] = s_aodLabel;
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
                    s_confirmCmd = dev.state ? CMD_DEV_OFF : CMD_DEV_ON;
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
            if (s_toggleCb) {
                GridDevice& dev = s_devices[s_confirmIdx];
                dev.ackPending = true;
                s_toggleCb(s_farmId, dev.deviceId, s_confirmCmd);
            }
            s_screen = GRID_MAIN;
            break;

        // ---- Settings Submenu ----
        case GRID_SETTINGS:
            switch (s_settingsCur) {
                case 0: s_screen = GRID_FARM_SEL;
                        s_wantsFarmSelect = true;     break;  // Change Farm
                case 1:                                        // AOD toggle
                        s_aodEnabled = !s_aodEnabled;
                        strlcpy(s_aodLabel, s_aodEnabled ? "AOD: ON" : "AOD: OFF",
                                sizeof(s_aodLabel));
                        break;
                case 2: s_screen = GRID_AOD_TIME;     break;  // AOD Time
                case 3: s_screen = GRID_MESH_INFO;    break;
                case 4: s_screen = GRID_DEBUG_LOGS;
                        s_logScroll = 0;               break;
                case 5: s_screen = GRID_NODE_INFO;     break;
                case 6: s_screen = GRID_MAIN;          break;  // Back
            }
            break;

        // ---- Info / Log screens — OK does nothing ----
        case GRID_MESH_INFO:
        case GRID_DEBUG_LOGS:
        case GRID_NODE_INFO:
        case GRID_AOD_TIME:
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
void grid_set_device_state(const char* deviceId, bool state) {
    for (uint8_t i = 0; i < GRID_DEV_TILES; i++) {
        if (strcmp(s_devices[i].deviceId, deviceId) == 0) {
            s_devices[i].state      = state;
            s_devices[i].ackPending = false;
            s_devices[i].lastSeenMs = millis();
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
    return (s_confirmCmd == CMD_DEV_ON) ? "ON" : "OFF";
}

// ============================================================================
// Timeout — return to main grid after inactivity
// ============================================================================
void grid_touch() {
    s_lastTouch = millis();
}

// Periodic timestamp refresh interval (5 seconds — faster for stale detection)
static const uint32_t TIMESTAMP_REFRESH_MS = 5000;
static uint32_t s_lastTimestampRefresh = 0;

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

    // Periodic refresh on GRID_MAIN so last-seen timestamps update
    if (s_screen == GRID_MAIN &&
        (now - s_lastTimestampRefresh) >= TIMESTAMP_REFRESH_MS)
    {
        s_lastTimestampRefresh = now;
        // Only mark changed if at least one device has a valid timestamp
        for (uint8_t i = 0; i < GRID_DEV_TILES; i++) {
            if (s_devices[i].lastSeenMs != 0) {
                changed = true;
                break;
            }
        }
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
