/*******************************************************************************
 * ESP-Agri OLED Display Module — Implementation  (Phase 2)
 ******************************************************************************/
#include "agri_display.h"
#include "agri_devmap.h"
#include "agri_log.h"
#include "agri_gridui.h"
#include "agri_range.h"
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ============================================================================
// Static OLED instance (single display per node)
// ============================================================================
static Adafruit_SSD1306 s_oled(AGRI_OLED_WIDTH, AGRI_OLED_HEIGHT, &Wire, -1);

// ============================================================================
// Menu-list scroll constants
// ============================================================================
static const uint8_t LIST_Y_START   = 14;   // first item y (below 10px title)
static const uint8_t ITEM_HEIGHT    = 9;    // pixels per menu row
static const uint8_t MAX_VISIBLE    = 5;    // (64-14)/9 ≈ 5 full rows
static const uint8_t MAX_VISIBLE_CT = 4;    // DEV_CTRL has header line → 1 fewer

/// Compute first visible index so the cursor stays on-screen.
static uint8_t _scrollTop(uint8_t cursor, uint8_t count, uint8_t maxVis) {
    if (count <= maxVis) return 0;
    // keep cursor roughly centred
    int top = (int)cursor - (int)(maxVis / 2);
    if (top < 0) top = 0;
    if ((uint8_t)top + maxVis > count) top = count - maxVis;
    return (uint8_t)top;
}

/// Draw tiny scroll arrows on the right edge when list overflows
static void _drawScrollIndicators(uint8_t topIdx, uint8_t count, uint8_t maxVis) {
    if (count <= maxVis) return;                  // everything visible
    if (topIdx > 0) {                             // more items above
        s_oled.fillTriangle(124, 16, 120, 20, 128, 20, SSD1306_WHITE);
    }
    if (topIdx + maxVis < count) {                // more items below
        s_oled.fillTriangle(124, 62, 120, 58, 128, 58, SSD1306_WHITE);
    }
}

// ============================================================================
// Constructor
// ============================================================================
AgriDisplay::AgriDisplay()
    : _meshOk(false)
    , _nodeCount(0)
    , _nodeId(0)
    , _devOn(false)
    , _lastSource(0)
    , _rssi(0)
    , _rssiBars(0)
    , _rssiTrend(RSSI_STABLE)
    , _rssiMin(0)
    , _rssiMax(0)
    , _dirty(true)
{
    memset(_role,      0, sizeof(_role));
    memset(_farmId,    0, sizeof(_farmId));
    memset(_lastCmd,   0, sizeof(_lastCmd));
    memset(_ackStatus, 0, sizeof(_ackStatus));
    memset(_rssiQuality, 0, sizeof(_rssiQuality));
    strlcpy(_farmId, AGRI_FARM_ID, sizeof(_farmId));
}

// ============================================================================
// Initialise
// ============================================================================
bool AgriDisplay::init() {
    Wire.begin(AGRI_OLED_SDA, AGRI_OLED_SCL);

    if (!s_oled.begin(SSD1306_SWITCHCAPVCC, AGRI_OLED_ADDR)) {
        Serial.println("[DISPLAY] SSD1306 init FAILED");
        return false;
    }

    s_oled.clearDisplay();
    s_oled.setTextColor(SSD1306_WHITE);
    s_oled.setTextSize(1);
    s_oled.display();

    Serial.println("[DISPLAY] SSD1306 ready");
    _dirty = true;
    return true;
}

// ============================================================================
// State Setters
// ============================================================================
void AgriDisplay::setRole(const char* role) {
    if (strcmp(_role, role) != 0) {
        strlcpy(_role, role, sizeof(_role));
        _dirty = true;
    }
}

void AgriDisplay::setMeshStatus(bool connected, uint16_t nodeCount) {
    if (_meshOk != connected || _nodeCount != nodeCount) {
        _meshOk    = connected;
        _nodeCount = nodeCount;
        _dirty     = true;
    }
}

void AgriDisplay::setNodeId(uint32_t id) {
    if (_nodeId != id) {
        _nodeId = id;
        _dirty  = true;
    }
}

void AgriDisplay::setFarmId(const char* farmId) {
    if (strcmp(_farmId, farmId) != 0) {
        strlcpy(_farmId, farmId, sizeof(_farmId));
        _dirty = true;
    }
}

void AgriDisplay::setDeviceState(bool on) {
    if (_devOn != on) {
        _devOn = on;
        _dirty  = true;
    }
}

void AgriDisplay::setLastCommand(const char* cmd, uint16_t msgId) {
    char buf[20];
    snprintf(buf, sizeof(buf), "%s #%u", cmd, msgId);
    if (strcmp(_lastCmd, buf) != 0) {
        strlcpy(_lastCmd, buf, sizeof(_lastCmd));
        _dirty = true;
    }
}

void AgriDisplay::setAckStatus(const char* status) {
    if (strcmp(_ackStatus, status) != 0) {
        strlcpy(_ackStatus, status, sizeof(_ackStatus));
        _dirty = true;
    }
}

void AgriDisplay::setLastSource(uint32_t sourceNodeId) {
    if (_lastSource != sourceNodeId) {
        _lastSource = sourceNodeId;
        _dirty      = true;
    }
}

void AgriDisplay::setRSSI(int8_t rssi, uint8_t bars, RssiTrend trend,
                          int8_t rawMin, int8_t rawMax,
                          const char* qualityLabel) {
    bool changed = (_rssi != rssi || _rssiTrend != trend || _rssiBars != bars);
    _rssi      = rssi;
    _rssiTrend = trend;
    _rssiMin   = rawMin;
    _rssiMax   = rawMax;
    if (bars != 255) _rssiBars = bars;
    if (qualityLabel) strlcpy(_rssiQuality, qualityLabel, sizeof(_rssiQuality));
    if (changed) _dirty = true;
}

void AgriDisplay::markDirty() { _dirty = true; }

// ============================================================================
// Splash Screen
// ============================================================================
void AgriDisplay::showSplash(const char* line1, const char* line2) {
    s_oled.clearDisplay();
    s_oled.setTextSize(1);
    s_oled.setTextColor(SSD1306_WHITE);

    s_oled.setCursor(0, 20);
    s_oled.println(line1);
    if (line2) {
        s_oled.setCursor(0, 36);
        s_oled.println(line2);
    }
    s_oled.display();
}

// ============================================================================
// Farm Selection Screen  (non-blocking via grid state, or legacy blocking)
//
// If farms==nullptr, reads from grid_farm_sel_*() accessors (non-blocking).
// If farms!=nullptr, uses the supplied list (legacy blocking mode).
// ============================================================================
void AgriDisplay::drawFarmSelect(const char* const* farms, uint8_t count,
                                  uint8_t cursor) {
    // Resolve data source
    bool useGrid = (farms == nullptr);
    uint8_t cnt  = useGrid ? grid_farm_sel_count()  : count;
    uint8_t cur  = useGrid ? grid_farm_sel_cursor() : cursor;

    s_oled.clearDisplay();

    // --- Title bar (inverted) ---
    s_oled.fillRect(0, 0, AGRI_OLED_WIDTH, 12, SSD1306_WHITE);
    s_oled.setTextSize(1);
    s_oled.setTextColor(SSD1306_BLACK);
    s_oled.setCursor(2, 2);
    s_oled.print("SELECT FARM");

    // Range indicator on right side of title bar
    RangeState rs = range_state();
    _drawRangeIcon(104, 2, rs, true);   // inverted (white bar)

    // --- Farm list ---
    s_oled.setTextColor(SSD1306_WHITE);
    const uint8_t rowH   = 10;
    const uint8_t yBase  = 14;
    const uint8_t maxVis = 4;    // 4 rows × 10px = 40px (ends at y=54)

    // Keep cursor visible with centred scrolling
    uint8_t top = 0;
    if (cnt > maxVis) {
        int t = (int)cur - (int)(maxVis / 2);
        if (t < 0) t = 0;
        if ((uint8_t)t + maxVis > cnt) t = cnt - maxVis;
        top = (uint8_t)t;
    }

    for (uint8_t i = 0; i < maxVis && (top + i) < cnt; i++) {
        uint8_t idx = top + i;
        uint8_t y   = yBase + i * rowH;

        const char* farmName = useGrid ? grid_farm_sel_item(idx) : farms[idx];

        // Per-farm range state
        RangeState frs = range_farm_state(farmName);
        const char* rangeTag;
        switch (frs) {
            case RANGE_OK:       rangeTag = "OK"; break;
            case RANGE_LOST:     rangeTag = "NO"; break;
            default:             rangeTag = ".."; break;
        }

        if (idx == cur) {
            // Highlight selected row
            s_oled.fillRect(0, y, AGRI_OLED_WIDTH, rowH, SSD1306_WHITE);
            s_oled.setTextColor(SSD1306_BLACK);
            s_oled.setCursor(2, y + 1);
            s_oled.print("> ");
            s_oled.print(farmName);
            // Range icon on right side of selected row
            _drawRangeIcon(104, y + 1, frs, true);  // inverted (white bg)
            s_oled.setTextColor(SSD1306_WHITE);
        } else {
            s_oled.setCursor(2, y + 1);
            s_oled.print("  ");
            s_oled.print(farmName);
            // Range icon on right side of normal row
            _drawRangeIcon(104, y + 1, frs, false); // normal (black bg)
        }
    }

    // --- Scroll arrows (right edge) ---
    if (cnt > maxVis) {
        if (top > 0)
            s_oled.fillTriangle(124, 16, 121, 20, 127, 20, SSD1306_WHITE);
        if (top + maxVis < cnt)
            s_oled.fillTriangle(124, 53, 121, 49, 127, 49, SSD1306_WHITE);
    }

    // --- Footer hint ---
    s_oled.setCursor(2, 56);
    s_oled.setTextSize(1);
    s_oled.print("UP/DN:move  SEL:pick");

    // Only call display() in blocking (legacy) mode.
    // Non-blocking mode is driven by refresh() which calls display() itself.
    if (!useGrid) {
        s_oled.display();
    }
}

// ============================================================================
// Refresh  (redraws only when dirty)
// ============================================================================
void AgriDisplay::refresh() {
    if (!_dirty) return;
    _dirty = false;

    s_oled.clearDisplay();

    if (strcmp(_role, "RELAY") == 0) {
        _drawHeader();
        _drawRelayScreen();
    } else {
        // REMOTE — use grid UI (6-tile dashboard)
        GridScreen scr = grid_current_screen();
        switch (scr) {
            case GRID_MAIN:       _drawGrid();            break;
            case GRID_SETTINGS:   _drawSettingsMenu();    break;
            case GRID_MESH_INFO:  _drawGridMeshInfo();    break;
            case GRID_DEBUG_LOGS: _drawGridDebugLogs();   break;
            case GRID_NODE_INFO:  _drawGridNodeInfo();    break;
            case GRID_FARM_SEL:
                drawFarmSelect(nullptr, 0, 0);  // uses grid farm picker state
                break;
            case GRID_CONFIRM:    _drawConfirm();         break;
            case GRID_AOD_TIME:   _drawAodTimeSetting();  break;
        }
    }

    s_oled.display();
}

// ============================================================================
// Common Header  (y=0..20)
// ============================================================================
void AgriDisplay::_drawHeader() {
    // --- Title bar (inverted) ---
    s_oled.fillRect(0, 0, AGRI_OLED_WIDTH, 10, SSD1306_WHITE);
    s_oled.setTextSize(1);
    s_oled.setTextColor(SSD1306_BLACK);

    char title[22];
    snprintf(title, sizeof(title), " ESP-AGRI  %s", _role);
    s_oled.setCursor(0, 1);
    s_oled.print(title);

    // --- Mesh status line ---
    s_oled.setTextColor(SSD1306_WHITE);
    s_oled.setCursor(0, 12);
    s_oled.printf("Mesh:%s  Nodes:%d",
                  _meshOk ? "OK" : "--", _nodeCount + 1);

    // RSSI signal bars + trend arrow on relay header
    _drawRssiBars(108, 12, _rssi, false, _rssiBars, _rssiTrend);
}

// ============================================================================
// Relay Node Screen — Multi-Device State Table
// ============================================================================
void AgriDisplay::_drawRelayScreen() {
    // Farm + Node ID
    s_oled.setCursor(0, 22);
    s_oled.printf("Farm:%s ID:%u", _farmId, _nodeId % 10000);

    // Device state table (up to 4 devices, 2-column layout)
    const AgriDevice* tbl = agri_devmap_table();
    uint8_t cnt = agri_devmap_count();

    uint8_t y = 33;
    for (uint8_t i = 0; i < cnt && i < 4; i++) {
        if (!tbl[i].valid) continue;
        uint8_t x = (i % 2 == 0) ? 0 : 65;
        if (i == 2) y = 43;
        s_oled.setCursor(x, y);

        // Short device name (first 7 chars) + state
        char shortName[8];
        strlcpy(shortName, tbl[i].id, sizeof(shortName));
        s_oled.printf("%s:%s", shortName, tbl[i].state ? "ON" : "--");
    }

    // Last command received
    s_oled.setCursor(0, 55);
    if (_lastCmd[0]) {
        s_oled.printf("Cmd:%s", _lastCmd);
    } else {
        s_oled.print("Cmd: waiting...");
    }
}

// ============================================================================
// Grid UI — Status Bar  (top 10px — Farm ID + Range Indicator)
// ============================================================================
//  Layout:  [FARM_101         (OK)]   or   [FARM_101         (X)]
//  Inverted bar for high contrast.
// ============================================================================
void AgriDisplay::_drawGridStatusBar() {
    // Inverted background
    s_oled.fillRect(0, 0, AGRI_OLED_WIDTH, 10, SSD1306_WHITE);
    s_oled.setTextSize(1);
    s_oled.setTextColor(SSD1306_BLACK, SSD1306_WHITE);

    // Farm ID on the left
    s_oled.setCursor(2, 1);
    s_oled.print(_farmId);

    // Mesh node count
    s_oled.setCursor(50, 1);
    s_oled.printf("N:%d", _nodeCount + 1);

    // Compact numeric RSSI (e.g. "-52") between node count and signal bars
    s_oled.setTextWrap(false);
    s_oled.setCursor(70, 1);
    if (_rssi != 0) {
        s_oled.printf("%d", _rssi);
    } else {
        s_oled.print("--");
    }

    // Unified signal indicator: combines Range state + RSSI bars + trend
    _drawUnifiedSignal(97, 1, true);   // inverted (on white bar)

    // Restore text color
    s_oled.setTextColor(SSD1306_WHITE, SSD1306_BLACK);
    s_oled.setTextWrap(true);
}

// ============================================================================
// Monochrome Range Icon Renderer
//
// Draws a compact 18×8 icon at (x,y):
//   OK       →  filled circle + "OK"         ●OK
//   CHECKING →  hollow circle + "CHK"        ○CHK
//   LOST     →  X mark + "NO"                ✕NO
//
// inverted=true when drawing on a white background (bar).
// ============================================================================
void AgriDisplay::_drawRangeIcon(uint8_t x, uint8_t y, RangeState st, bool inverted) {
    uint16_t fg = inverted ? SSD1306_BLACK : SSD1306_WHITE;
    uint16_t bg = inverted ? SSD1306_WHITE : SSD1306_BLACK;
    s_oled.setTextColor(fg, bg);
    s_oled.setTextSize(1);
    s_oled.setTextWrap(false);   // prevent overflow wrapping to next line

    uint8_t cx = x + 3;   // circle centre x
    uint8_t cy = y + 3;   // circle centre y

    switch (st) {
        case RANGE_OK:
            // Filled circle (solid dot) + "OK"
            s_oled.fillCircle(cx, cy, 3, fg);
            s_oled.setCursor(x + 9, y);
            s_oled.print("OK");
            break;

        case RANGE_CHECKING:
            // Hollow circle + "CHK"
            s_oled.drawCircle(cx, cy, 3, fg);
            s_oled.setCursor(x + 9, y);
            s_oled.print("..");
            break;

        case RANGE_LOST:
            // X mark (two diagonal lines) + "NO"
            s_oled.drawLine(cx - 2, cy - 2, cx + 2, cy + 2, fg);
            s_oled.drawLine(cx - 2, cy + 2, cx + 2, cy - 2, fg);
            s_oled.setCursor(x + 9, y);
            s_oled.print("NO");
            break;
    }

    // Restore
    s_oled.setTextColor(SSD1306_WHITE, SSD1306_BLACK);
}

// ============================================================================
// RSSI Signal Strength Bars
//
// Draws a compact 4-bar signal icon at (x,y):
//   Bar heights: 2, 4, 6, 8 px  (left to right)
//   Bar width: 2px,  gap: 1px   →  total 11×8 px  (+5px for trend arrow)
//   Filled bars = signal level,  hollow outlines = missing bars
//
// forceBars: if != 255, use this bar count instead of computing from rssi.
// trend: draws a small trend arrow after the bars.
// ============================================================================
void AgriDisplay::_drawRssiBars(uint8_t x, uint8_t y, int8_t rssi, bool inverted,
                                uint8_t forceBars, RssiTrend trend) {
    uint16_t fg = inverted ? SSD1306_BLACK : SSD1306_WHITE;

    // Determine how many bars to fill (0–4)
    uint8_t bars;
    if (forceBars != 255) {
        bars = forceBars;
    } else {
        bars = 0;
        if (rssi == 0) {
            bars = 0;                        // no data
        } else if (rssi >= AGRI_RSSI_EXCELLENT) {
            bars = 4;
        } else if (rssi >= AGRI_RSSI_GOOD) {
            bars = 3;
        } else if (rssi >= AGRI_RSSI_FAIR) {
            bars = 2;
        } else if (rssi >= AGRI_RSSI_WEAK) {
            bars = 1;
        }
    }

    // Draw 4 bars (increasing height left to right)
    for (uint8_t i = 0; i < 4; i++) {
        uint8_t barH = (i + 1) * 2;     // heights: 2, 4, 6, 8
        uint8_t bx   = x + i * 3;       // bar x (2px wide + 1px gap)
        uint8_t by   = y + (8 - barH);  // align bars to bottom

        if (i < bars) {
            s_oled.fillRect(bx, by, 2, barH, fg);   // filled
        } else {
            s_oled.drawRect(bx, by, 2, barH, fg);   // outline only
        }
    }

    // Trend arrow (drawn to the right of bars at x+12)
    if (trend != RSSI_STABLE) {
        _drawTrendArrow(x + 13, y, trend, fg);
    }
}

// ============================================================================
// Trend Arrow — tiny 3×5 triangle right of the signal bars
// ============================================================================
void AgriDisplay::_drawTrendArrow(uint8_t x, uint8_t y, RssiTrend trend, uint16_t fg) {
    if (trend == RSSI_RISING) {
        // Upward triangle: apex at top
        s_oled.fillTriangle(x + 1, y,       // top centre
                            x,     y + 4,   // bottom left
                            x + 2, y + 4,   // bottom right
                            fg);
    } else if (trend == RSSI_FALLING) {
        // Downward triangle: apex at bottom
        s_oled.fillTriangle(x + 1, y + 7,   // bottom centre
                            x,     y + 3,   // top left
                            x + 2, y + 3,   // top right
                            fg);
    }
}

// ============================================================================
// Unified Signal Indicator for GRID_MAIN status bar
//
// Combines the Range state with RSSI bars into a single widget:
//   RANGE_OK       → normal RSSI bars + trend arrow
//   RANGE_CHECKING → all bars as outlines (hollow), blinking at 1 Hz
//   RANGE_LOST     → small ✕ drawn in place of bars
//
// Width: 11–16 px (bars + optional trend arrow)
// ============================================================================
void AgriDisplay::_drawUnifiedSignal(uint8_t x, uint8_t y, bool inverted) {
    uint16_t fg = inverted ? SSD1306_BLACK : SSD1306_WHITE;
    uint16_t bg = inverted ? SSD1306_WHITE : SSD1306_BLACK;
    RangeState rs = range_state();

    switch (rs) {
        case RANGE_OK:
            // Normal RSSI bars with trend
            _drawRssiBars(x, y, _rssi, inverted, _rssiBars, _rssiTrend);
            break;

        case RANGE_CHECKING: {
            // Blinking hollow bars (visible for 500ms, hidden for 500ms)
            bool visible = ((millis() / 500) % 2) == 0;
            if (visible) {
                // All 4 bars as outlines (hollow)
                for (uint8_t i = 0; i < 4; i++) {
                    uint8_t barH = (i + 1) * 2;
                    uint8_t bx   = x + i * 3;
                    uint8_t by   = y + (8 - barH);
                    s_oled.drawRect(bx, by, 2, barH, fg);
                }
            }
            // When not visible: nothing drawn → blank area (blink off phase)
            break;
        }

        case RANGE_LOST: {
            // Draw a bold ✕ in the centre of the bar area (11×8 px)
            uint8_t cx = x + 5;   // centre x of 11px span
            uint8_t cy = y + 3;   // centre y of 8px span
            // Two diagonal lines forming X
            s_oled.drawLine(cx - 3, cy - 3, cx + 3, cy + 3, fg);
            s_oled.drawLine(cx - 3, cy + 3, cx + 3, cy - 3, fg);
            // Second pass offset by 1px for boldness
            s_oled.drawLine(cx - 2, cy - 3, cx + 3, cy + 2, fg);
            s_oled.drawLine(cx - 2, cy + 3, cx + 3, cy - 2, fg);
            break;
        }
    }
}

// ============================================================================
// Grid UI — Main 6-Tile Dashboard  (10px status bar + 54px grid)
// ============================================================================
void AgriDisplay::_drawGrid() {
    // Status bar at top (0–9)
    _drawGridStatusBar();

    // Draw 6 tiles below the status bar
    uint8_t cur = grid_cursor();
    for (uint8_t i = 0; i < GRID_TILES; i++) {
        _drawGridTile(i, i == cur);
    }
}

// ============================================================================
// Compact elapsed-time formatter for tile display
// Formats:  <1m → "Xs"  |  <60m → "Xm"  |  <24h → "Xh"  |  else → "Xd"
// Returns pointer to static buffer.
// ============================================================================
static const char* _fmtElapsed(uint32_t elapsedMs) {
    static char buf[6];
    uint32_t sec = elapsedMs / 1000;
    if (sec < 60)           snprintf(buf, sizeof(buf), "%us",  sec);
    else if (sec < 3600)    snprintf(buf, sizeof(buf), "%um",  sec / 60);
    else if (sec < 86400)   snprintf(buf, sizeof(buf), "%uh",  sec / 3600);
    else                    snprintf(buf, sizeof(buf), "%ud",  sec / 86400);
    return buf;
}

// ============================================================================
// Grid UI — Single Tile Renderer  (accounts for 10px status bar offset)
//
// Tile size: 64×18  (128/2 cols, 54/3 rows)
// Y-offset: 10px (status bar)
//
// Monochrome design:
//   Selected  → fully inverted (white fill, black text)
//   Normal    → 1px outline, white text on black
//   ON state  → filled dot indicator
//   OFF state → hollow dot indicator
//   Pending   → "..." text, no dot
// ============================================================================
static const uint8_t GRID_Y_OFFSET = 10;   // status bar height
static const uint8_t TILE_W        = 64;   // 128 / 2
static const uint8_t TILE_H        = 18;   // 54 / 3

void AgriDisplay::_drawGridTile(uint8_t idx, bool selected) {
    uint8_t col = idx % GRID_COLS;
    uint8_t row = idx / GRID_COLS;
    uint8_t x   = col * TILE_W;
    uint8_t y   = GRID_Y_OFFSET + row * TILE_H;

    if (selected) {
        // Inverted tile — high contrast for sunlight readability
        s_oled.fillRect(x, y, TILE_W, TILE_H, SSD1306_WHITE);
        s_oled.setTextColor(SSD1306_BLACK, SSD1306_WHITE);
    } else {
        // Normal tile — 1px border for clear separation
        s_oled.drawRect(x, y, TILE_W, TILE_H, SSD1306_WHITE);
        s_oled.setTextColor(SSD1306_WHITE, SSD1306_BLACK);
    }

    s_oled.setTextSize(1);    // 6×8 font

    if (idx < GRID_DEV_TILES) {
        // ---- Device tile ----
        GridDevice* dev = grid_device(idx);
        if (!dev) return;

        // Line 1: device label (centred horizontally)
        uint8_t labelLen = strlen(dev->label);
        uint8_t tx = x + (TILE_W - labelLen * 6) / 2;
        s_oled.setCursor(tx, y + 2);
        s_oled.print(dev->label);

        // Line 2: state text + elapsed time (centred)
        //   Normal: "ON 2m" / "OFF 15s"   Pending: "..."   Fail: "FAIL"
        //   Stale:  "ON 45s!" / "OFF 2m!" (elapsed > AGRI_STALE_MS)
        char stateBuf[11];     // max "OFF 999d!\0" = 10 chars
        bool stale = false;
        if (dev->failFlash) {
            strlcpy(stateBuf, "FAIL", sizeof(stateBuf));
        } else if (dev->ackPending) {
            strlcpy(stateBuf, "...", sizeof(stateBuf));
        } else if (dev->lastSeenMs != 0) {
            uint32_t elapsed = millis() - dev->lastSeenMs;
            stale = (elapsed >= AGRI_STALE_MS);
            snprintf(stateBuf, sizeof(stateBuf), "%s %s%s",
                     dev->state ? "ON" : "OFF",
                     _fmtElapsed(elapsed),
                     stale ? "!" : "");
        } else {
            strlcpy(stateBuf, dev->state ? "ON" : "OFF", sizeof(stateBuf));
        }
        uint8_t sLen = strlen(stateBuf);
        uint8_t sx = x + (TILE_W - sLen * 6) / 2;
        s_oled.setCursor(sx, y + 10);
        s_oled.print(stateBuf);

        // Status dot  (top-right corner of tile)
        // Blinks at 2 Hz when timestamp is stale (> AGRI_STALE_MS)
        if (!dev->ackPending && !dev->failFlash) {
            bool dotVisible = !stale || ((millis() / 500) % 2 == 0);
            if (dotVisible) {
                uint8_t dotX = x + TILE_W - 7;
                uint8_t dotY = y + 3;
                uint16_t fg = selected ? SSD1306_BLACK : SSD1306_WHITE;
                if (dev->state) {
                    // ON → filled circle (solid dot)
                    s_oled.fillCircle(dotX, dotY, 2, fg);
                } else {
                    // OFF → hollow circle
                    s_oled.drawCircle(dotX, dotY, 2, fg);
                }
            }
        }
    } else {
        // ---- Settings tile ----
        const char* lbl = "SETUP";
        uint8_t lLen = strlen(lbl);
        uint8_t tx = x + (TILE_W - lLen * 6) / 2;
        s_oled.setCursor(tx, y + 2);
        s_oled.print(lbl);

        // Gear-like hint: three dots >>>
        const char* hint = ">>>";
        uint8_t hLen = strlen(hint);
        uint8_t hx = x + (TILE_W - hLen * 6) / 2;
        s_oled.setCursor(hx, y + 10);
        s_oled.print(hint);
    }

    // Restore default text color
    s_oled.setTextColor(SSD1306_WHITE);
}

// ============================================================================
// Grid UI — Settings Submenu  (scrollable list with cursor)
// ============================================================================
void AgriDisplay::_drawSettingsMenu() {
    // Title bar (inverted)  — y: 0–9
    s_oled.fillRect(0, 0, AGRI_OLED_WIDTH, 10, SSD1306_WHITE);
    s_oled.setTextSize(1);
    s_oled.setTextColor(SSD1306_BLACK);
    s_oled.setCursor(2, 1);
    s_oled.print("SETTINGS");
    s_oled.setTextColor(SSD1306_WHITE);

    // Footer hint  — y: 56–63 (reserve bottom 8px)
    s_oled.setCursor(2, 56);
    s_oled.print("[Hold] back");

    // List area: y 12..54  →  max 4 rows × 10px each
    const uint8_t listY   = 12;
    const uint8_t rowH    = 10;
    const uint8_t maxVis  = 4;

    uint8_t cur = grid_settings_cursor();
    uint8_t cnt = grid_settings_count();
    uint8_t top = _scrollTop(cur, cnt, maxVis);

    uint8_t y = listY;
    for (uint8_t vi = 0; vi < maxVis && (top + vi) < cnt; vi++) {
        uint8_t i = top + vi;
        if (i == cur) {
            s_oled.fillRect(0, y, AGRI_OLED_WIDTH, rowH, SSD1306_WHITE);
            s_oled.setTextColor(SSD1306_BLACK, SSD1306_WHITE);
            s_oled.setCursor(2, y + 1);
            s_oled.printf("> %s", grid_settings_label(i));
            s_oled.setTextColor(SSD1306_WHITE, SSD1306_BLACK);
        } else {
            s_oled.setCursor(2, y + 1);
            s_oled.printf("  %s", grid_settings_label(i));
        }
        y += rowH;
    }

    // Scroll arrows when list overflows
    _drawScrollIndicators(top, cnt, maxVis);
}

// ============================================================================
// Grid UI — Mesh Info Screen  (with range indicator)
// ============================================================================
void AgriDisplay::_drawGridMeshInfo() {
    // Title bar
    s_oled.fillRect(0, 0, AGRI_OLED_WIDTH, 10, SSD1306_WHITE);
    s_oled.setTextSize(1);
    s_oled.setTextColor(SSD1306_BLACK);
    s_oled.setCursor(2, 1);
    s_oled.print("MESH STATUS");
    s_oled.setTextColor(SSD1306_WHITE);

    // Footer (reserved y: 56-63)
    s_oled.setCursor(2, 56);
    s_oled.print("[Hold] back");

    // Content area: y 12..54 (6 lines × 7px spacing, tighter for extra info)
    uint8_t y = 12;
    s_oled.setCursor(2, y);
    s_oled.printf("Mesh: %s", _meshOk ? "OK" : "DOWN");
    y += 8;
    s_oled.setCursor(2, y);
    s_oled.printf("Nodes: %d", _nodeCount + 1);
    y += 8;
    s_oled.setCursor(2, y);
    s_oled.printf("My ID: %u", _nodeId % 10000);
    y += 8;

    // Range status with icon
    s_oled.setCursor(2, y);
    s_oled.print("Range: ");
    RangeState rs = range_state();
    _drawRangeIcon(44, y, rs, false);
    y += 8;

    // RSSI signal strength with bars + trend + min/max
    s_oled.setCursor(2, y);
    if (_rssi != 0) {
        s_oled.printf("RSSI:%ddBm", _rssi);
        _drawRssiBars(78, y, _rssi, false, _rssiBars, _rssiTrend);
    } else {
        s_oled.print("RSSI: N/A");
    }
    y += 8;

    // Extra line: quality label + min/max range
    s_oled.setCursor(2, y);
    if (_rssi != 0 && _rssiQuality[0]) {
        if (_rssiMin != 0 && _rssiMax != 0) {
            s_oled.printf("%s %d/%d", _rssiQuality, _rssiMin, _rssiMax);
        } else {
            s_oled.print(_rssiQuality);
        }
    }
}

// ============================================================================
// Grid UI — Debug Log Viewer  (scrollable)
// ============================================================================
void AgriDisplay::_drawGridDebugLogs() {
    // Title bar
    s_oled.fillRect(0, 0, AGRI_OLED_WIDTH, 10, SSD1306_WHITE);
    s_oled.setTextSize(1);
    s_oled.setTextColor(SSD1306_BLACK);
    s_oled.setCursor(2, 1);
    s_oled.print("DEBUG LOGS");
    s_oled.setTextColor(SSD1306_WHITE);

    uint8_t cnt = agri_log_count();
    if (cnt == 0) {
        s_oled.setCursor(2, 28);
        s_oled.print("No logs yet");
        s_oled.setCursor(2, 56);
        s_oled.print("[Hold] back");
        return;
    }

    uint8_t scroll  = grid_log_scroll();
    uint8_t maxVis  = 4;            // visible rows (4 × 10px = 40px, leaves room for footer)
    uint8_t y       = 13;

    for (uint8_t vi = 0; vi < maxVis; vi++) {
        uint8_t logIdx = scroll + vi;
        if (logIdx >= cnt) break;

        const AgriLogEntry* e = agri_log_get(logIdx);
        if (!e || !e->valid) continue;

        s_oled.setCursor(0, y);
        // Compact format: device cmd result
        char shortDev[6];
        strlcpy(shortDev, e->device, sizeof(shortDev));
        s_oled.printf("%s %s %s", shortDev, e->cmd, e->result);
        y += 10;
    }

    // Scroll indicators (arrows on right edge)
    if (scroll > 0) {
        s_oled.fillTriangle(124, 14, 120, 18, 128, 18, SSD1306_WHITE);
    }
    uint8_t maxScroll = (cnt > maxVis) ? (cnt - maxVis) : 0;
    if (scroll < maxScroll) {
        s_oled.fillTriangle(124, 62, 120, 58, 128, 58, SSD1306_WHITE);
    }

    // Footer hint — consistent with all submenu screens
    s_oled.setCursor(2, 56);
    s_oled.print("[Hold] back");
}

// ============================================================================
// Grid UI — Node Info Screen
// ============================================================================
void AgriDisplay::_drawGridNodeInfo() {
    // Title bar
    s_oled.fillRect(0, 0, AGRI_OLED_WIDTH, 10, SSD1306_WHITE);
    s_oled.setTextSize(1);
    s_oled.setTextColor(SSD1306_BLACK);
    s_oled.setCursor(2, 1);
    s_oled.print("NODE INFO");
    s_oled.setTextColor(SSD1306_WHITE);

    // Footer (reserved y: 56-63)
    s_oled.setCursor(2, 56);
    s_oled.print("[Hold] back");

    // Content area: y 13..53
    uint8_t y = 13;
    s_oled.setCursor(2, y);
    s_oled.printf("Role: %s", _role);
    y += 10;
    s_oled.setCursor(2, y);
    s_oled.printf("Farm: %s", _farmId);
    y += 10;
    s_oled.setCursor(2, y);
    s_oled.printf("ID: %u", _nodeId);
    y += 10;
    s_oled.setCursor(2, y);
    s_oled.printf("Uptime: %lus", millis() / 1000);
}

// ============================================================================
// Grid UI — Confirm Toggle Screen
//
// Centred prompt:  "Toggle PUMP?"  "Turn ON"
//                  [SEL] confirm   [Hold] cancel
// ============================================================================
void AgriDisplay::_drawConfirm() {
    // Title bar
    s_oled.fillRect(0, 0, AGRI_OLED_WIDTH, 10, SSD1306_WHITE);
    s_oled.setTextSize(1);
    s_oled.setTextColor(SSD1306_BLACK);
    s_oled.setCursor(2, 1);
    s_oled.print("CONFIRM");
    s_oled.setTextColor(SSD1306_WHITE);

    // Device name (centred)
    const char* devLabel  = grid_confirm_device_label();
    const char* actionStr = grid_confirm_action();

    char line1[24];
    snprintf(line1, sizeof(line1), "Toggle %s?", devLabel);
    uint8_t l1Len = strlen(line1);
    uint8_t l1x   = (AGRI_OLED_WIDTH - l1Len * 6) / 2;
    s_oled.setCursor(l1x, 18);
    s_oled.print(line1);

    // Action line
    char line2[16];
    snprintf(line2, sizeof(line2), "Turn %s", actionStr);
    uint8_t l2Len = strlen(line2);
    uint8_t l2x   = (AGRI_OLED_WIDTH - l2Len * 6) / 2;
    s_oled.setCursor(l2x, 32);
    s_oled.print(line2);

    // Hint buttons
    s_oled.setCursor(2, 50);
    s_oled.print("[SEL] OK");
    s_oled.setCursor(68, 50);
    s_oled.print("[Hold] No");
}

// ============================================================================
// AOD Time Setting Screen
// ============================================================================
void AgriDisplay::_drawAodTimeSetting() {
    // Title bar (inverted)
    s_oled.fillRect(0, 0, AGRI_OLED_WIDTH, 12, SSD1306_WHITE);
    s_oled.setTextColor(SSD1306_BLACK);
    s_oled.setCursor(2, 2);
    s_oled.print("AOD TIMEOUT");
    s_oled.setTextColor(SSD1306_WHITE);

    // Current value — large centred
    uint8_t sec = grid_aod_timeout_sec();
    char buf[12];
    snprintf(buf, sizeof(buf), "%u sec", sec);
    uint8_t len = strlen(buf);
    uint8_t x   = (AGRI_OLED_WIDTH - len * 6) / 2;
    s_oled.setCursor(x, 28);
    s_oled.print(buf);

    // UP / DOWN arrows
    if (sec < AGRI_AOD_MAX_SEC) {
        s_oled.setCursor(x - 18, 28);
        s_oled.print("\x18");   // up arrow
    }
    if (sec > AGRI_AOD_MIN_SEC) {
        uint8_t endX = x + len * 6 + 6;
        s_oled.setCursor(endX, 28);
        s_oled.print("\x19");   // down arrow
    }

    // Hint buttons
    s_oled.setCursor(2, 56);
    s_oled.print("[UP/DN]Adj");
    s_oled.setCursor(68, 56);
    s_oled.print("[Hold]Back");
}
