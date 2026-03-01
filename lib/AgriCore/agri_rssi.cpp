/*******************************************************************************
 * ESP-Agri RSSI Tracker — Implementation
 *
 * EMA smoothing with hysteresis for stable bar display, plus trend detection
 * and min/max tracking.
 ******************************************************************************/
#include "agri_rssi.h"

// ============================================================================
// Constructor
// ============================================================================
RssiTracker::RssiTracker()
    : _emaX10(0)
    , _smoothed(0)
    , _initialised(false)
    , _bars(0)
    , _trendIdx(0)
    , _trendCount(0)
    , _trend(RSSI_STABLE)
    , _rawMin(0)
    , _rawMax(0)
    , _minMaxInit(false)
{
    memset(_trendRing, 0, sizeof(_trendRing));
}

// ============================================================================
// Feed — call every AGRI_RSSI_POLL_MS with a fresh WiFi.RSSI() sample
// ============================================================================
void RssiTracker::feed(int8_t raw) {
    // raw == 0 means "no data" — skip smoothing to avoid pulling EMA to 0
    if (raw == 0) {
        // Still push into trend ring so trend reflects lack of data
        _trendRing[_trendIdx] = _smoothed;  // hold previous value
        _trendIdx = (_trendIdx + 1) % AGRI_RSSI_TREND_WINDOW;
        if (_trendCount < AGRI_RSSI_TREND_WINDOW) _trendCount++;
        _updateTrend();
        return;
    }

    // --- Min / Max tracking ---
    if (!_minMaxInit) {
        _rawMin = raw;
        _rawMax = raw;
        _minMaxInit = true;
    } else {
        if (raw < _rawMin) _rawMin = raw;
        if (raw > _rawMax) _rawMax = raw;
    }

    // --- EMA filter (fixed-point ×10) ---
    //   ema = alpha * raw + (1 - alpha) * ema
    //   Using integer: ema_x10 = (ALPHA_NUM * raw * 10 + (DEN - ALPHA_NUM) * ema_x10) / DEN
    //   With DEN=10: ema_x10 = ALPHA_NUM * raw_x10 + (10 - ALPHA_NUM) * ema_x10) / 10
    if (!_initialised) {
        _emaX10      = (int16_t)raw * 10;
        _smoothed    = raw;
        _initialised = true;
    } else {
        int16_t rawX10 = (int16_t)raw * 10;
        _emaX10 = (int16_t)(
            ((int32_t)AGRI_RSSI_EMA_ALPHA_NUM * rawX10 +
             (int32_t)(AGRI_RSSI_EMA_ALPHA_DEN - AGRI_RSSI_EMA_ALPHA_NUM) * _emaX10)
            / AGRI_RSSI_EMA_ALPHA_DEN
        );
        // Round to nearest integer
        _smoothed = (int8_t)((_emaX10 + (_emaX10 >= 0 ? 5 : -5)) / 10);
    }

    // --- Update bars with hysteresis ---
    _updateBars();

    // --- Trend ring buffer ---
    _trendRing[_trendIdx] = _smoothed;
    _trendIdx = (_trendIdx + 1) % AGRI_RSSI_TREND_WINDOW;
    if (_trendCount < AGRI_RSSI_TREND_WINDOW) _trendCount++;
    _updateTrend();
}

// ============================================================================
// Bar calculation with hysteresis
//
// Maps smoothed RSSI → 0–4 bars using the standard thresholds,
// but only changes bar count when the value crosses a threshold
// by at least AGRI_RSSI_HYST_DB beyond it.
// ============================================================================
void RssiTracker::_updateBars() {
    // Thresholds in ascending order (worst to best):
    //   <-80 → 0 bars
    //   -80..-61 → 1 bar
    //   -60..-51 → 2 bars
    //   -50..-41 → 3 bars    (wait, original thresholds differ slightly)
    //
    // Original mapping:
    //   rssi >= -50 → 4 bars (EXCELLENT)
    //   rssi >= -60 → 3 bars (GOOD)
    //   rssi >= -70 → 2 bars (FAIR)
    //   rssi >= -80 → 1 bar  (WEAK)
    //   else        → 0 bars
    //
    // With hysteresis HYST_DB=3:
    //   To go UP from N bars, smoothed must exceed threshold by +HYST
    //   To go DOWN from N bars, smoothed must drop below threshold by -HYST

    int8_t s = _smoothed;
    uint8_t newBars = _bars;  // start from current

    // Define the four thresholds (bar N requires rssi >= thresh[N-1])
    static const int8_t thresh[4] = {
        AGRI_RSSI_WEAK,       // -80: 0→1
        AGRI_RSSI_FAIR,       // -70: 1→2
        AGRI_RSSI_GOOD,       // -60: 2→3
        AGRI_RSSI_EXCELLENT   // -50: 3→4
    };

    // Try to increase bars
    while (newBars < 4) {
        int8_t upThresh = thresh[newBars] + AGRI_RSSI_HYST_DB;
        if (s >= upThresh) {
            newBars++;
        } else {
            break;
        }
    }

    // Try to decrease bars
    while (newBars > 0) {
        int8_t downThresh = thresh[newBars - 1] - AGRI_RSSI_HYST_DB;
        if (s < downThresh) {
            newBars--;
        } else {
            break;
        }
    }

    _bars = newBars;
}

// ============================================================================
// Trend detection
//
// Compares the oldest sample in the ring to the newest.
// If the difference exceeds AGRI_RSSI_TREND_DELTA → RISING or FALLING.
// ============================================================================
void RssiTracker::_updateTrend() {
    if (_trendCount < 2) {
        _trend = RSSI_STABLE;
        return;
    }

    // Oldest sample is at _trendIdx (it was just overwritten, so the one
    // *before* the write pointer is newest). Actually:
    //   newest = ring[(_trendIdx - 1 + WINDOW) % WINDOW]
    //   oldest = ring[_trendIdx % WINDOW]  if count == WINDOW
    //          = ring[0]                   if count < WINDOW
    uint8_t oldestIdx;
    if (_trendCount >= AGRI_RSSI_TREND_WINDOW) {
        oldestIdx = _trendIdx;  // ring is full, write pointer wraps to oldest
    } else {
        oldestIdx = 0;
    }
    uint8_t newestIdx = (_trendIdx + AGRI_RSSI_TREND_WINDOW - 1) % AGRI_RSSI_TREND_WINDOW;

    int8_t oldest = _trendRing[oldestIdx];
    int8_t newest = _trendRing[newestIdx];
    int8_t delta  = newest - oldest;

    if (delta >= AGRI_RSSI_TREND_DELTA) {
        _trend = RSSI_RISING;
    } else if (delta <= -AGRI_RSSI_TREND_DELTA) {
        _trend = RSSI_FALLING;
    } else {
        _trend = RSSI_STABLE;
    }
}

// ============================================================================
// Quality label (human-readable name for current bar level)
// ============================================================================
const char* RssiTracker::qualityLabel() const {
    switch (_bars) {
        case 4:  return "Excellent";
        case 3:  return "Good";
        case 2:  return "Fair";
        case 1:  return "Weak";
        default: return "None";
    }
}
