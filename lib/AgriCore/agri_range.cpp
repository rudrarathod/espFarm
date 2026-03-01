/*******************************************************************************
 * ESP-Agri Farm Range Indicator — Implementation
 *
 * Heartbeat-based relay reachability tracker.
 * The remote node periodically pings (STATUS_REQ) and listens for heartbeats.
 * State transitions:
 *   CHECKING → OK    (first heartbeat received)
 *   OK       → LOST  (timeout exceeded with no heartbeat)
 *   LOST     → OK    (heartbeat arrives)
 *   Any      → CHECKING  (ping just sent, waiting)
 ******************************************************************************/
#include "agri_range.h"

// ============================================================================
// State
// ============================================================================
static RangeState s_state     = RANGE_CHECKING;
static uint32_t   s_lastSeen  = 0;        // millis() of last relay contact
static uint32_t   s_relayId   = 0;        // Node ID of last relay
static uint32_t   s_lastPing  = 0;        // millis() of last ping sent
static bool       s_everSeen  = false;    // Have we ever heard from relay?

// ============================================================================
// Init
// ============================================================================
void range_init() {
    s_state    = RANGE_CHECKING;
    s_lastSeen = 0;
    s_relayId  = 0;
    s_lastPing = millis();
    s_everSeen = false;
}

// ============================================================================
// Update  (call from loop — non-blocking)
// Returns true if the state changed (so display can mark dirty).
// ============================================================================
bool range_update() {
    RangeState prev = s_state;
    uint32_t now    = millis();

    if (!s_everSeen) {
        // Never heard from relay — stay CHECKING
        s_state = RANGE_CHECKING;
    } else {
        uint32_t elapsed = now - s_lastSeen;
        if (elapsed < AGRI_RANGE_TIMEOUT_MS) {
            s_state = RANGE_OK;
        } else {
            s_state = RANGE_LOST;
        }
    }

    return (s_state != prev);
}

// ============================================================================
// Heartbeat received  (call from ACK / heartbeat / status_rsp handler)
// ============================================================================
void range_on_heartbeat(uint32_t fromNodeId) {
    s_lastSeen = millis();
    s_relayId  = fromNodeId;
    s_everSeen = true;
    // Immediately promote to OK
    s_state    = RANGE_OK;
}

// ============================================================================
// Accessors
// ============================================================================
RangeState range_state()      { return s_state; }

uint32_t range_last_seen_ago() {
    if (!s_everSeen) return 0;
    return millis() - s_lastSeen;
}

uint32_t range_relay_id()     { return s_relayId; }

// ============================================================================
// Ping timer  (returns true once per AGRI_RANGE_PING_MS interval)
// ============================================================================
bool range_should_ping() {
    uint32_t now = millis();
    if ((now - s_lastPing) >= AGRI_RANGE_PING_MS) {
        s_lastPing = now;
        return true;
    }
    return false;
}

// ============================================================================
// Per-Farm Range Tracking
//
// Maintains a small table of recently-seen farms so the farm picker can show
// a range indicator (OK / CHECKING / LOST) next to each farm name.
// ============================================================================
struct FarmRange {
    char     farmId[AGRI_FARM_ID_LEN];
    uint32_t lastSeen;   // millis() of last contact
    bool     everSeen;   // have we ever heard from this farm?
};

static FarmRange s_farmRange[AGRI_TEST_FARM_COUNT];
static uint8_t   s_farmRangeCount = 0;

/// Find or create a slot for the given farm ID
static FarmRange* _findOrCreateFarm(const char* farmId) {
    // Search existing
    for (uint8_t i = 0; i < s_farmRangeCount; i++) {
        if (strcmp(s_farmRange[i].farmId, farmId) == 0) {
            return &s_farmRange[i];
        }
    }
    // Create new slot if space available
    if (s_farmRangeCount < AGRI_TEST_FARM_COUNT) {
        FarmRange* fr = &s_farmRange[s_farmRangeCount++];
        strlcpy(fr->farmId, farmId, sizeof(fr->farmId));
        fr->lastSeen = 0;
        fr->everSeen = false;
        return fr;
    }
    return nullptr;  // table full
}

void range_on_farm_heartbeat(const char* farmId) {
    if (!farmId || !farmId[0]) return;
    FarmRange* fr = _findOrCreateFarm(farmId);
    if (fr) {
        fr->lastSeen = millis();
        fr->everSeen = true;
    }
}

RangeState range_farm_state(const char* farmId) {
    for (uint8_t i = 0; i < s_farmRangeCount; i++) {
        if (strcmp(s_farmRange[i].farmId, farmId) == 0) {
            if (!s_farmRange[i].everSeen) return RANGE_CHECKING;
            uint32_t elapsed = millis() - s_farmRange[i].lastSeen;
            return (elapsed < AGRI_RANGE_TIMEOUT_MS) ? RANGE_OK : RANGE_LOST;
        }
    }
    return RANGE_CHECKING;  // unknown farm → never seen
}

bool range_farm_update() {
    // Currently a no-op — state is computed lazily in range_farm_state().
    // Reserved for future periodic maintenance.
    return false;
}
