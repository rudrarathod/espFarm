/*******************************************************************************
 * ESP-Agri Grid UI — 6-Tile OLED Dashboard (Remote Node)
 *
 * Replaces the text menu with a 2×3 graphical grid:
 *   [ D1 ] [ D2 ]      D1–D5 = Device toggle tiles
 *   [ D3 ] [ D4 ]      SET   = Settings menu
 *   [ D5 ] [ SET]
 *
 * Navigation (3-button):
 *   UP   → previous tile / scroll up in submenu
 *   DOWN → next tile / scroll down in submenu
 *   SEL short → toggle device / enter settings item
 *   SEL long  → BACK to main grid
 *
 * Non-blocking.  Call grid_check_timeout() from loop().
 ******************************************************************************/
#ifndef AGRI_GRIDUI_H
#define AGRI_GRIDUI_H

#include <Arduino.h>
#include "agri_config.h"

// ============================================================================
// Grid Layout Constants
// ============================================================================
#define GRID_TILES          6       // Total tiles in grid
#define GRID_COLS           2       // Columns
#define GRID_ROWS           3       // Rows
#define GRID_DEV_TILES      5       // First 5 = device tiles
#define GRID_TILE_W         64      // Tile width  (128 / 2)
#define GRID_TILE_H         18      // Tile height (54 / 3)

// ============================================================================
// UI Screens
// ============================================================================
enum GridScreen : uint8_t {
    GRID_MAIN       = 0,    // 6-tile dashboard
    GRID_SETTINGS   = 1,    // Settings submenu list
    GRID_MESH_INFO  = 2,    // Mesh status info page
    GRID_DEBUG_LOGS = 3,    // Scrollable debug log viewer
    GRID_NODE_INFO  = 4,    // Node information page
    GRID_FARM_SEL   = 5,    // Farm re-select requested
    GRID_CONFIRM    = 6,    // Confirm device toggle
    GRID_AOD_TIME   = 7     // AOD timeout adjustment screen
};

// ============================================================================
// Device Tile State
// ============================================================================
struct GridDevice {
    char     label[8];                          // Short display label
    char     deviceId[AGRI_DEVICE_ID_LEN];     // Full device ID for protocol
    bool     state;                             // ON / OFF
    bool     ackPending;                        // Waiting for ACK
    bool     failFlash;                         // ACK timeout flash active
    uint32_t failFlashTime;                     // millis() when flash started
    uint32_t lastSeenMs;                        // millis() when state last confirmed (0=never)
};

// ============================================================================
// Callback — fired when user toggles a device tile
// ============================================================================
typedef void (*grid_toggle_cb_t)(const char* farmId,
                                 const char* deviceId,
                                 uint8_t     cmd);

// ============================================================================
// API
// ============================================================================

/// Initialise grid state and register toggle callback (call once in setup).
/// farmId is stored internally and used in toggle callbacks.
void grid_init(grid_toggle_cb_t onToggle, const char* farmId = nullptr);

/// Currently-selected farm ID (returns AGRI_FARM_ID if none was set)
const char* grid_farm_id();

/// Update the stored farm ID (after re-selection)
void grid_set_farm_id(const char* farmId);

/// Returns true when user chose "Change Farm" (consume it to trigger picker)
bool grid_wants_farm_select();

/// Enter the non-blocking farm selection screen
void grid_enter_farm_select(const char* const* farms, uint8_t count);

/// Farm picker cursor (for display rendering)
uint8_t grid_farm_sel_cursor();

/// Farm picker list accessors
const char* grid_farm_sel_item(uint8_t idx);
uint8_t     grid_farm_sel_count();

/// Returns true once when user confirmed a farm (consumes the flag)
bool grid_farm_selected(char* outFarm, uint8_t outLen);

/// Button events — call from debounced button handlers
void grid_on_up();
void grid_on_down();
void grid_on_ok();          // short press
void grid_on_back();        // long press

/// Current screen for display rendering
GridScreen grid_current_screen();

/// Current cursor position (0–5 on main grid)
uint8_t grid_cursor();

/// Access a device tile by index (0–4).  nullptr if out of range.
GridDevice* grid_device(uint8_t idx);

/// Number of device tiles (always GRID_DEV_TILES)
uint8_t grid_device_count();

/// Update device state from ACK (clears ackPending)
void grid_set_device_state(const char* deviceId, bool state);

/// Clear ackPending flag on NACK / timeout
void grid_clear_ack_pending(const char* deviceId);

/// Mark device as FAIL-flashing (called on ACK timeout)
void grid_set_fail_flash(const char* deviceId);

// ---- Settings submenu ----
uint8_t     grid_settings_cursor();
const char* grid_settings_label(uint8_t idx);
uint8_t     grid_settings_count();

// ---- Debug log scroll position ----
uint8_t grid_log_scroll();

// ---- Confirm screen accessors ----
const char* grid_confirm_device_label();
const char* grid_confirm_action();

// ---- Timeout ----
bool grid_check_timeout();
void grid_touch();

// ---- Always-On Display (AOD) ----
bool     grid_aod_enabled();          // Is AOD currently ON?
void     grid_set_aod(bool enabled);  // Toggle AOD
uint8_t  grid_aod_timeout_sec();      // Current timeout in seconds
void     grid_set_aod_timeout(uint8_t sec); // Set timeout (clamped)

#endif // AGRI_GRIDUI_H
