/*******************************************************************************
 * ESP-Agri OLED Display Module  (Phase 2 — Multi-Farm / Multi-Device)
 *
 * Provides role-aware status display on 128×64 SSD1306 OLED.
 * Relay node: device state table.
 * Remote node: menu rendering + command status.
 * Call refresh() periodically from loop().
 ******************************************************************************/
#ifndef AGRI_DISPLAY_H
#define AGRI_DISPLAY_H

#include <Arduino.h>
#include "agri_config.h"
#include "agri_gridui.h"
#include "agri_range.h"
#include "agri_rssi.h"

class AgriDisplay {
public:
    AgriDisplay();

    /// Initialise I2C and OLED hardware.  Returns false on failure.
    bool init();

    // ------ State setters (update internal state, mark dirty) ------
    void setRole(const char* role);
    void setMeshStatus(bool connected, uint16_t nodeCount);
    void setNodeId(uint32_t id);
    void setFarmId(const char* farmId);
    void setDeviceState(bool on);
    void setLastCommand(const char* cmd, uint16_t msgId);
    void setAckStatus(const char* status);
    void setLastSource(uint32_t sourceNodeId);
    void setRSSI(int8_t rssi, uint8_t bars = 255, RssiTrend trend = RSSI_STABLE,
                  int8_t rawMin = 0, int8_t rawMax = 0,
                  const char* qualityLabel = nullptr);

    /// Mark dirty — force redraw on next refresh()
    void markDirty();

    /// Redraw the OLED if state has changed.  Call from loop().
    void refresh();

    /// Show a two-line splash / info message (blocks briefly).
    void showSplash(const char* line1, const char* line2 = nullptr);

    /// Draw farm-selection screen (called repeatedly from blocking boot loop)
    void drawFarmSelect(const char* const* farms, uint8_t count, uint8_t cursor);

private:
    // Display state
    char     _role[10];
    bool     _meshOk;
    uint16_t _nodeCount;
    uint32_t _nodeId;
    char     _farmId[AGRI_FARM_ID_LEN];
    bool     _devOn;
    char     _lastCmd[20];
    char     _ackStatus[20];
    uint32_t _lastSource;
    int8_t      _rssi;
    uint8_t     _rssiBars;       // Hysteresis-aware bar count (0–4)
    RssiTrend   _rssiTrend;      // Signal trend (RISING/FALLING/STABLE)
    int8_t      _rssiMin;        // Min raw RSSI since boot
    int8_t      _rssiMax;        // Max raw RSSI since boot
    char        _rssiQuality[12]; // Human-readable quality label
    bool     _dirty;

    void _drawHeader();
    void _drawRelayScreen();

    // ---- Grid UI rendering (Remote Node — 6-tile dashboard) ----
    void _drawGridStatusBar();
    void _drawRangeIcon(uint8_t x, uint8_t y, RangeState st, bool inverted);
    void _drawRssiBars(uint8_t x, uint8_t y, int8_t rssi, bool inverted,
                        uint8_t forceBars = 255, RssiTrend trend = RSSI_STABLE);
    void _drawUnifiedSignal(uint8_t x, uint8_t y, bool inverted);
    void _drawTrendArrow(uint8_t x, uint8_t y, RssiTrend trend, uint16_t fg);
    void _drawGrid();
    void _drawGridTile(uint8_t idx, bool selected);
    void _drawSettingsMenu();
    void _drawGridMeshInfo();
    void _drawGridDebugLogs();
    void _drawGridNodeInfo();
    void _drawConfirm();
    void _drawAodTimeSetting();
};

#endif // AGRI_DISPLAY_H
