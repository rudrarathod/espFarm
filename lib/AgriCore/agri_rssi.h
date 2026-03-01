/*******************************************************************************
 * ESP-Agri RSSI Tracker
 *
 * Provides EMA smoothing, hysteresis-based bar mapping, trend detection,
 * and min/max tracking for the WiFi RSSI signal indicator.
 *
 * Usage:
 *   static RssiTracker g_rssi;
 *   // In loop(), every AGRI_RSSI_POLL_MS:
 *   g_rssi.feed(agri_get_rssi());
 *   display.setRSSI(g_rssi.smoothed(), g_rssi.bars(), g_rssi.trend());
 ******************************************************************************/
#ifndef AGRI_RSSI_H
#define AGRI_RSSI_H

#include <Arduino.h>
#include "agri_config.h"

// ============================================================================
// Trend Enum
// ============================================================================
enum RssiTrend : uint8_t {
    RSSI_STABLE  = 0,
    RSSI_RISING  = 1,
    RSSI_FALLING = 2
};

// ============================================================================
// RSSI Tracker Class
// ============================================================================
class RssiTracker {
public:
    RssiTracker();

    /// Feed a new raw RSSI sample (call every AGRI_RSSI_POLL_MS)
    void feed(int8_t raw);

    /// EMA-smoothed RSSI value in dBm (0 = no data)
    int8_t smoothed() const { return _smoothed; }

    /// Hysteresis-aware bar count (0–4)
    uint8_t bars() const { return _bars; }

    /// Signal trend over the last ~10 seconds
    RssiTrend trend() const { return _trend; }

    /// Minimum raw RSSI seen since boot (0 if never fed)
    int8_t rawMin() const { return _rawMin; }

    /// Maximum raw RSSI seen since boot (0 if never fed)
    int8_t rawMax() const { return _rawMax; }

    /// Human-readable quality label for the current bar level
    const char* qualityLabel() const;

private:
    // EMA state
    int16_t  _emaX10;      // EMA scaled ×10 for fixed-point precision
    int8_t   _smoothed;    // EMA rounded to int8_t
    bool     _initialised; // First sample flag
    uint8_t  _bars;        // Current hysteresis-aware bar count

    // Trend detection ring buffer (stores smoothed values at each feed)
    int8_t   _trendRing[AGRI_RSSI_TREND_WINDOW];
    uint8_t  _trendIdx;
    uint8_t  _trendCount;  // How many samples collected (up to WINDOW)
    RssiTrend _trend;

    // Min/Max tracking
    int8_t   _rawMin;
    int8_t   _rawMax;
    bool     _minMaxInit;

    /// Recalculate bars with hysteresis
    void _updateBars();

    /// Recalculate trend from ring buffer
    void _updateTrend();
};

#endif // AGRI_RSSI_H
