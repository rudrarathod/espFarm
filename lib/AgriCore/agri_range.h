/*******************************************************************************
 * ESP-Agri Farm Range Indicator
 *
 * Tracks reachability of the relay (farm) node via heartbeats.
 * Provides a tri-state indicator for the OLED top bar:
 *   RANGE_OK       — heartbeat received within timeout
 *   RANGE_CHECKING — waiting for first heartbeat or refresh
 *   RANGE_LOST     — heartbeat missed (timeout exceeded)
 *
 * Non-blocking.  Call range_update() from loop().
 * Radio-agnostic — relies only on the application calling range_on_heartbeat()
 * when a heartbeat or ACK arrives from the relay.
 ******************************************************************************/
#ifndef AGRI_RANGE_H
#define AGRI_RANGE_H

#include <Arduino.h>
#include "agri_config.h"

// ============================================================================
// Range States
// ============================================================================
enum RangeState : uint8_t {
    RANGE_OK       = 0,     // Relay recently seen  — solid circle / checkmark
    RANGE_CHECKING = 1,     // Waiting / refreshing — hollow circle
    RANGE_LOST     = 2      // Timeout exceeded     — X mark
};

// ============================================================================
// API
// ============================================================================

/// Initialise range tracker (call once in setup)
void range_init();

/// Call from loop() — checks timeouts, returns true if state changed
bool range_update();

/// Notify that we heard from the relay (heartbeat, ACK, status_rsp)
void range_on_heartbeat(uint32_t fromNodeId);

/// Current range state
RangeState range_state();

/// Milliseconds since last relay contact (0 if never heard)
uint32_t range_last_seen_ago();

/// Node ID of last-seen relay (0 if never heard)
uint32_t range_relay_id();

/// Should a ping be sent now?  (returns true once per interval)
bool range_should_ping();

// ============================================================================
// Per-Farm Range Tracking  (for farm picker display)
// ============================================================================

/// Notify that we heard from a specific farm (call for any incoming message)
void range_on_farm_heartbeat(const char* farmId);

/// Get range state for a specific farm
RangeState range_farm_state(const char* farmId);

/// Update timeouts for all tracked farms (call from loop)
bool range_farm_update();

#endif // AGRI_RANGE_H
