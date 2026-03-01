/*******************************************************************************
 * ESP-Agri Mesh Pump Control System
 * Configuration Header
 *
 * All system-wide constants and pin definitions.
 * Edit this file to match your hardware setup.
 ******************************************************************************/
#ifndef AGRI_CONFIG_H
#define AGRI_CONFIG_H

// ============================================================================
// Farm Identity  (this relay's own farm — set per-device at compile time)
// ============================================================================
#define AGRI_FARM_ID            "FARM_101"

// ============================================================================
// Node Roles
// ============================================================================
#define AGRI_ROLE_RELAY         0
#define AGRI_ROLE_REMOTE        1

// ============================================================================
// OLED Display (I2C) — Both Nodes
// ============================================================================
#define AGRI_OLED_SDA           21
#define AGRI_OLED_SCL           22
#define AGRI_OLED_ADDR          0x3C
#define AGRI_OLED_WIDTH         128
#define AGRI_OLED_HEIGHT        64

// ============================================================================
// Push Buttons — Remote Node (3-button navigation)
// ============================================================================
#define AGRI_BTN_UP             16      // INPUT_PULLUP — move cursor up
#define AGRI_BTN_SEL            17      // INPUT_PULLUP — short=select, long=back
#define AGRI_BTN_DOWN           18      // INPUT_PULLUP — move cursor down
#define AGRI_LONG_PRESS_MS      600     // Hold threshold for BACK action

// ============================================================================
// Relay — Pump Node (default device GPIO)
// ============================================================================
#define AGRI_RELAY_PIN          5
#define AGRI_RELAY_ACTIVE_HIGH  true    // true = GPIO HIGH activates relay
#define AGRI_LED_PIN            2       // D2 — status LED on relay node

// ============================================================================
// Multi-Device GPIO Map  (Relay Node — test mode)
// ============================================================================
#define AGRI_DEV_PUMP_PIN       5       // PUMP_01  → GPIO5
#define AGRI_DEV_VALVE_PIN      19      // VALVE_01 → GPIO19
#define AGRI_DEV_LIGHT_PIN      23      // LIGHT_01 → GPIO23
#define AGRI_DEV_MOTOR_PIN      4       // MOTOR_01 → GPIO4
#define AGRI_DEV_AUX_PIN        15      // AUX_01   → GPIO15
#define AGRI_MAX_DEVICES        5       // Max logical devices per relay

// ============================================================================
// ESP-MESH (Wi-Fi POC)
// ============================================================================
#define AGRI_MESH_SSID          "espAgriMesh"
#define AGRI_MESH_PASSWORD      "espAgri2026!"
#define AGRI_MESH_PORT          5555
#define AGRI_MESH_CHANNEL       6

// ============================================================================
// Timing Constants (milliseconds)
// ============================================================================
#define AGRI_DEBOUNCE_MS        50
#define AGRI_ACK_TIMEOUT_MS     3000
#define AGRI_MAX_RETRIES        3
#define AGRI_DISPLAY_REFRESH_MS 250
#define AGRI_HEARTBEAT_MS       30000
#define AGRI_STATUS_CLEAR_MS    8000

// ============================================================================
// Range Indicator Timing
// ============================================================================
#define AGRI_RANGE_PING_MS      8000    // Send STATUS_REQ every 8 s
#define AGRI_RANGE_TIMEOUT_MS   24000   // 3× ping interval → declare LOST

// ============================================================================
// Staleness Detection
// ============================================================================
#define AGRI_STALE_MS           30000   // 30 s — mark tile timestamp as stale

// ============================================================================
// RSSI Signal Strength Indicator
// ============================================================================
#define AGRI_RSSI_POLL_MS       2000    // Poll RSSI every 2 seconds
#define AGRI_RSSI_EXCELLENT     (-50)   // ≥ -50 dBm → 4 bars
#define AGRI_RSSI_GOOD          (-60)   // ≥ -60 dBm → 3 bars
#define AGRI_RSSI_FAIR          (-70)   // ≥ -70 dBm → 2 bars
#define AGRI_RSSI_WEAK          (-80)   // ≥ -80 dBm → 1 bar
                                        // <  -80 dBm → 0 bars

// RSSI Smoothing (EMA fixed-point:  alpha = NUM / DEN = 0.3)
#define AGRI_RSSI_EMA_ALPHA_NUM  3      // Numerator  (higher = more responsive)
#define AGRI_RSSI_EMA_ALPHA_DEN  10     // Denominator
#define AGRI_RSSI_HYST_DB        3      // Hysteresis band in dBm (prevents bar flicker)
#define AGRI_RSSI_TREND_WINDOW   5      // Ring-buffer slots for trend detection
#define AGRI_RSSI_TREND_DELTA    4      // dBm change over window → RISING / FALLING

// ============================================================================
// Menu — Remote Node
// ============================================================================
#define AGRI_MENU_TIMEOUT_MS    15000   // Return to home after inactivity

// ============================================================================
// Always-On Display (AOD)
// ============================================================================
#define AGRI_AOD_DEFAULT_SEC    30      // Default display-off timeout (seconds)
#define AGRI_AOD_MIN_SEC        5       // Minimum AOD timeout
#define AGRI_AOD_MAX_SEC        120     // Maximum AOD timeout
#define AGRI_AOD_STEP_SEC       5       // UP/DOWN increment

// ============================================================================
// Protocol Limits
// ============================================================================
#define AGRI_MSG_MAX_SIZE       100     // Max serialized message bytes
#define AGRI_DUP_BUFFER_SIZE    32      // Duplicate detection ring buffer
#define AGRI_DEVICE_ID_LEN      16      // Max device ID string length
#define AGRI_FARM_ID_LEN        12      // Max farm ID string length
#define AGRI_DUP_WINDOW_MS      30000   // Duplicate detection time window

// ============================================================================
// Test Farms & Devices  (compile-time lists for simulation)
// ============================================================================
#define AGRI_TEST_FARM_COUNT    4
#define AGRI_TEST_DEVICE_COUNT  4

// ============================================================================
// Debug Log
// ============================================================================
#define AGRI_LOG_ENTRIES        20      // Ring buffer size

#endif // AGRI_CONFIG_H
