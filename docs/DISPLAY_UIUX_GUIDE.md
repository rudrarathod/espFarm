# ESP-Agri Display UI/UX Guide

> Hardware: SSD1306 128×64 monochrome OLED | Font: 6×8 default | Refresh: 250 ms dirty-flag

---

## 1. Hardware & Constraints

| Parameter       | Value                         |
|-----------------|-------------------------------|
| Display         | SSD1306 OLED, I2C             |
| Resolution      | 128 × 64 pixels              |
| Color           | Monochrome (white on black)   |
| I2C Address     | `0x3C`                        |
| SDA / SCL       | GPIO 21 / GPIO 22             |
| Font            | Built-in 6×8 (1 char = 6w×8h)|
| Max chars/line  | 21 characters (128 ÷ 6)      |
| Max text rows   | 8 rows (64 ÷ 8)              |
| Refresh rate    | 250 ms (`AGRI_DISPLAY_REFRESH_MS`) |

### Design Principles

- **Sunlight readability**: inverted (white-bg) bars for titles and selected items
- **Dirty-flag rendering**: screen only redraws when state changes → minimal flicker
- **Non-blocking**: all screens driven by a state machine, no `delay()` calls
- **3-button only**: UP / SEL / DOWN dictates all navigation patterns

---

## 2. Input System (3-Button Navigation)

| Button | GPIO | Short Press     | Long Press (≥600 ms) |
|--------|------|-----------------|----------------------|
| UP     | 16   | Move cursor up / Scroll up  | —               |
| SEL    | 17   | Select / Confirm / Toggle   | BACK (go up)    |
| DOWN   | 18   | Move cursor down / Scroll down | —            |

All buttons use `INPUT_PULLUP` (active LOW). Debounce: 50 ms.

---

## 3. Node Roles & Screen Routing

The display adapts based on node role:

```
              ┌─────────────┐
              │  AgriDisplay │
              │   refresh()  │
              └──────┬───────┘
                     │
            ┌────────┴────────┐
            │                 │
     role="RELAY"      role="REMOTE"
            │                 │
     ┌──────┴──────┐   ┌─────┴──────┐
     │ _drawHeader │   │ Grid UI    │
     │ _drawRelay  │   │ State Mach.│
     │  Screen()   │   │ (7 screens)│
     └─────────────┘   └────────────┘
```

---

## 4. Relay Node Display

Single fixed screen — no interactive navigation.

### Layout (128×64)

```
┌────────────────────────────┐  y=0
│▓▓ ESP-AGRI  RELAY  ▓▓▓▓▓▓▓│  Title bar (inverted, 10px)
├────────────────────────────┤  y=10
│ Mesh:OK  Nodes:3    ▐▌▌▌▲ │  y=12  Mesh status + RSSI bars + trend
│                            │
│ Farm:FARM_101 ID:1234      │  y=22  Identity
│                            │
│ PUMP_01:ON  VALVE_0:--     │  y=33  Device state table
│ LIGHT_:ON   MOTOR_0:--     │  y=43  (2-col, up to 4 devices)
│                            │
│ Cmd: CMD_SET #42           │  y=55  Last received command
└────────────────────────────┘  y=63
```

### Elements

| Zone          | Y Range | Content                                     |
|---------------|---------|---------------------------------------------|
| Title bar     | 0–9     | Inverted `ESP-AGRI RELAY`                   |
| Mesh status   | 12      | `Mesh:OK/--  Nodes:N` + RSSI bars + trend   |
| Identity      | 22      | `Farm:ID  ID:shortNodeId`                   |
| Device table  | 33–43   | 2-column, device name + ON/`--`             |
| Last command   | 55     | `Cmd: CMD_SET #msgId`                       |

---

## 5. Remote Node — Grid UI State Machine

The Remote node uses an enum-driven state machine with 8 screens:

```
         ┌──────────────────────────────┐
         │         GRID_MAIN            │
         │    2×3 Tile Dashboard        │
         │  [D1][D2] [D3][D4] [D5][SET]│
         └──────┬───────────┬───────────┘
                │ SEL on    │ SEL on
                │ Device    │ SETUP tile
                ▼           ▼
       ┌──────────────┐  ┌──────────────────┐
       │ GRID_CONFIRM │  │  GRID_SETTINGS   │
       │ "Toggle X?"  │  │  Scrollable list  │
       │ SEL=OK       │  │  5 menu items     │
       │ Hold=cancel  │  └──┬──┬──┬──┬──────┘
       └──────────────┘     │  │  │  │
                            │  │  │  └─── "Back" → GRID_MAIN
                            │  │  │
                   ┌────────┘  │  └──────────────┐
                   ▼           ▼                  ▼
          ┌──────────────┐ ┌──────────────┐ ┌──────────────┐
          │GRID_MESH_INFO│ │GRID_DEBUG_LOG│ │ GRID_NODE_INF│
          │ Mesh Status  │ │  Scrollable  │ │  Node Info   │
          │ 4 info lines │ │  log viewer  │ │  4 info lines│
          └──────────────┘ └──────────────┘ └──────────────┘

          "Change Farm" → GRID_FARM_SEL
                          ┌──────────────┐
                          │ GRID_FARM_SEL│
                          │ Farm picker  │
                          │ Scrollable   │
                          └──────────────┘
```

### Screen Enum

| Value | Name              | Description                    |
|-------|-------------------|--------------------------------|
| 0     | `GRID_MAIN`       | 2×3 tile dashboard             |
| 1     | `GRID_SETTINGS`   | Settings submenu list          |
| 2     | `GRID_MESH_INFO`  | Mesh connection status         |
| 3     | `GRID_DEBUG_LOGS` | Scrollable log ring buffer     |
| 4     | `GRID_NODE_INFO`  | Device identity info           |
| 5     | `GRID_FARM_SEL`   | Farm picker (non-blocking)     |
| 6     | `GRID_CONFIRM`    | Device toggle confirmation     |
| 7     | `GRID_AOD_TIME`   | AOD timeout adjustment         |

---

## 6. Screen Layouts (Pixel-Perfect)

### 6.1 GRID_MAIN — 2×3 Tile Dashboard

```
┌────────────────────────────┐  y=0
│▓FARM_101 N:3 -52 ▐▌▌▌▲ ▓▓│  Status bar (inverted, 10px)
├─────────────┬──────────────┤  y=10
│             │              │
│    PUMP     │    VALVE     │  Tile 0 (y:10-27)  Tile 1
│   ON 2m ●   │  OFF 5m ○    │
├─────────────┼──────────────┤  y=28
│             │              │
│    LIGHT    │    MOTOR     │  Tile 2 (y:28-45)  Tile 3
│    ...      │  OFF 15s ○   │
├─────────────┼──────────────┤  y=46
│             │              │
│    AUX      │    SETUP     │  Tile 4 (y:46-63)  Tile 5
│    FAIL     │     >>>      │
└─────────────┴──────────────┘  y=63
```

#### Tile Dimensions

| Parameter     | Value                    |
|---------------|--------------------------|
| Tile width    | 64 px (128 ÷ 2)         |
| Tile height   | 18 px (54 ÷ 3)          |
| Grid origin Y | 10 px (below status bar) |
| Columns       | 2                        |
| Rows          | 3                        |
| Total tiles   | 6 (5 device + 1 SETUP)   |

#### Status Bar (y: 0–9, inverted) — Unified Signal Indicator

```
[FARM_101  N:3 -52 ▐▌▌▌▲      ]
 ↑ x=2    ↑ x=50 ↑x=70 ↑x=97
 Farm ID  Node   dBm   Unified signal
          count        (bars + trend)
```

The status bar now uses a **unified signal indicator** that merges the
previous separate Range icon (`●OK`/`✕NO`) with the RSSI bars:

| Range State      | Unified Display           | Meaning                    |
|------------------|---------------------------|----------------------------|
| `RANGE_OK`       | RSSI bars + trend arrow   | Connected, showing quality |
| `RANGE_CHECKING` | Blinking hollow bars (1Hz) | Waiting for relay response |
| `RANGE_LOST`     | Bold ✕ in bar area        | Relay unreachable          |

> **Note:** The numeric dBm value (e.g. `-52`) is shown between the node count
> and the signal bars. Shows `--` when no RSSI data is available.
> `setTextWrap(false)` prevents overflow on the 128px status bar.

#### RSSI Signal Bars (11×8 px + 5px trend arrow)

A 4-bar signal strength indicator with EMA smoothing and hysteresis:

| Bars | RSSI Range       | Signal Quality | Hysteresis |
|------|------------------|----------------|------------|
| 4    | ≥ −50 dBm        | Excellent      | Up: ≥ −47, Down: < −53 |
| 3    | −50 to −60 dBm   | Good           | Up: ≥ −57, Down: < −63 |
| 2    | −60 to −70 dBm   | Fair           | Up: ≥ −67, Down: < −73 |
| 1    | −70 to −80 dBm   | Weak           | Up: ≥ −77, Down: < −83 |
| 0    | < −80 dBm        | No signal      | —          |

Bars are 2px wide with 1px gaps, heights 2/4/6/8 px (left to right).
Filled bars = active signal level, hollow outlines = missing bars.

**Smoothing:** Raw RSSI is filtered through an Exponential Moving Average
(α = 0.3) to eliminate jitter. The bar count only changes when the smoothed
value crosses a threshold by ±3 dBm (hysteresis), preventing visual flicker.

#### Trend Arrow (3×5 px, drawn right of bars)

| Trend     | Arrow | Condition                        |
|-----------|-------|----------------------------------|
| Rising    | ▲     | +4 dBm improvement over ~10 s    |
| Falling   | ▼     | −4 dBm degradation over ~10 s    |
| Stable    | (none)| Change within ±4 dBm             |

The trend is computed from a 5-slot ring buffer of smoothed values sampled
every 2 seconds (AGRI_RSSI_POLL_MS), comparing oldest to newest.

#### Tile States

| State           | Line 1 (y+2) | Line 2 (y+10)    | Dot (top-right)     |
|-----------------|---------------|-------------------|----------------------|
| OFF (confirmed) | Device label  | `OFF Xm`          | Hollow circle `○`    |
| ON (confirmed)  | Device label  | `ON Xm`           | Filled circle `●`    |
| OFF (never seen)| Device label  | `OFF`             | Hollow circle `○`    |
| ON (never seen) | Device label  | `ON`              | Filled circle `●`    |
| STALE (>30 s)   | Device label  | `ON Xm!` / `OFF Xs!` | Blinking dot (2 Hz) |
| ACK pending     | Device label  | `...`             | No dot               |
| FAIL flash      | Device label  | `FAIL`            | No dot               |

#### Last-Seen Timestamp Format

When a device state has been confirmed via ACK, the tile shows how long
ago the state was last verified, giving confidence the display is fresh:

| Elapsed          | Format    | Example          |
|------------------|-----------|------------------|
| < 60 seconds     | `Xs`      | `ON 30s`         |
| < 60 minutes     | `Xm`      | `OFF 5m`         |
| < 24 hours       | `Xh`      | `ON 2h`          |
| ≥ 24 hours       | `Xd`      | `OFF 1d`         |

Timestamps refresh every 5 seconds on `GRID_MAIN` via periodic dirty-mark.
Devices that have never received an ACK show only `ON`/`OFF` (no timestamp).

#### Staleness Indicator (`AGRI_STALE_MS = 30 s`)

When a device's `lastSeenMs` exceeds 30 seconds, the tile signals staleness:

- **`!` suffix** appended to the timestamp text: e.g. `ON 45s!`, `OFF 2m!`
- **Blinking dot** — the status dot toggles visibility at 2 Hz (500 ms on/off)

This warns the operator that the displayed state may not reflect reality
(e.g. relay is unreachable). Staleness clears automatically when a fresh
ACK, STATUS_RSP, or heartbeat bitmask is received.

#### Selection Highlight

| Style    | Fill            | Text Color   |
|----------|-----------------|--------------|
| Normal   | Black + 1px border | White text |
| Selected | White fill      | Black text (inverted) |

#### Cursor Navigation (Column-Major Wrapping)

```
Tiles indexed 0–5 in reading order:
  [0] [1]
  [2] [3]
  [4] [5]

UP/DOWN move visually up/down within a column.
At row boundary, cursor wraps to the adjacent column:

  DOWN path: 0→2→4→1→3→5→0
  UP   path: 0→5→3→1→4→2→0
```

### 6.2 GRID_CONFIRM — Toggle Confirmation

```
┌────────────────────────────┐  y=0
│▓▓ CONFIRM ▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓│  Title bar (inverted)
├────────────────────────────┤  y=10
│                            │
│       Toggle PUMP?         │  y=18, centred
│                            │
│        Turn ON             │  y=32, centred
│                            │
│ [SEL] OK      [Hold] No   │  y=50, hint buttons
│                            │
└────────────────────────────┘  y=63
```

- **SEL press** → fires the toggle command, returns to GRID_MAIN
- **Hold SEL** → cancels, returns to GRID_MAIN (no action)

### 6.3 GRID_SETTINGS — Scrollable Menu

```
┌────────────────────────────┐  y=0
│▓▓ SETTINGS ▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓│  Title bar (inverted)
├────────────────────────────┤  y=10
│                            │  y=12
│ ▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓│  Item 0 (selected, inverted)
│ ▓> Change Farm            ▓│
│ ▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓│
│  AOD: ON                   │  Item 1
│  AOD Time                  │  Item 2
│  Mesh Status               │  Item 3
│                            │ ▲▼ scroll arrows (right edge)
│ [Hold] back                │  y=56, footer
└────────────────────────────┘  y=63
```

#### Menu Items

| Index | Label         | Action                          |
|-------|---------------|---------------------------------|
| 0     | Change Farm   | → `GRID_FARM_SEL`              |
| 1     | AOD: ON/OFF   | Toggles always-on display       |
| 2     | AOD Time      | → `GRID_AOD_TIME`              |
| 3     | Mesh Status   | → `GRID_MESH_INFO`             |
| 4     | Debug Logs    | → `GRID_DEBUG_LOGS`            |
| 5     | Node Info     | → `GRID_NODE_INFO`             |
| 6     | Back          | → `GRID_MAIN`                  |

#### Scroll Behavior

| Parameter      | Value                              |
|----------------|------------------------------------|
| Max visible    | 4 items                            |
| Row height     | 10 px                              |
| List Y range   | 12–52 px                           |
| Scroll style   | Cursor-centred (`_scrollTop`)      |
| Overflow arrows | ▲ at y=16–20, ▼ at y=58–62 (right edge) |

### 6.4 GRID_MESH_INFO — Mesh Status

```
┌────────────────────────────┐  y=0
│▓▓ MESH STATUS ▓▓▓▓▓▓▓▓▓▓▓▓│  Title bar (inverted)
├────────────────────────────┤  y=10
│ Mesh: OK                   │  y=12
│ Nodes: 3                   │  y=20
│ My ID: 1234                │  y=28
│ Range: ●OK                 │  y=36
│ RSSI:-52dBm ▐▌▌▌▲          │  y=44  RSSI + bars + trend
│ Good -68/-45               │  y=52  Quality label + min/max
│ [Hold] back                │  y=56, footer
└────────────────────────────┘  y=63
```

The RSSI line now shows:
- **Smoothed dBm value** with bars and trend arrow inline
- **Quality label** (`Excellent`/`Good`/`Fair`/`Weak`/`None`) on the next line
- **Min/Max RSSI** since boot (format: `lo/hi`), giving insight into signal range

### 6.5 GRID_NODE_INFO — Node Information

```
┌────────────────────────────┐  y=0
│▓▓ NODE INFO ▓▓▓▓▓▓▓▓▓▓▓▓▓▓│  Title bar (inverted)
├────────────────────────────┤  y=10
│                            │
│ Role: REMOTE               │  y=13
│ Farm: FARM_101             │  y=23
│ ID: 3456789012             │  y=33
│ Uptime: 3600s              │  y=43
│                            │
│ [Hold] back                │  y=56, footer
└────────────────────────────┘  y=63
```

### 6.6 GRID_AOD_TIME — AOD Timeout Adjustment

```
┌────────────────────────────┐  y=0
│▓▓ AOD TIMEOUT ▓▓▓▓▓▓▓▓▓▓▓▓│  Title bar (inverted)
├────────────────────────────┤  y=10
│                            │
│                            │
│        ↑  30 sec  ↓        │  y=28  Current value (centred)
│                            │
│                            │
│ [UP/DN]Adj    [Hold]Back   │  y=56, footer
└────────────────────────────┘  y=63
```

- **UP** increases timeout by 5 sec (max 120)
- **DOWN** decreases timeout by 5 sec (min 5)
- **[Hold]** returns to `GRID_SETTINGS`
- Value persisted to NVS on change

### 6.7 GRID_DEBUG_LOGS — Scrollable Log Viewer

```
┌────────────────────────────┐  y=0
│▓▓ DEBUG LOGS ▓▓▓▓▓▓▓▓▓▓▓▓▓│  Title bar (inverted)
├────────────────────────────┤  y=10
│                            │
│ PUMP_ SET ACK              │  y=13  Log entry 0
│ VALVE SET NACK             │  y=23  Log entry 1
│ LIGHT SET ACK              │  y=33  Log entry 2
│ MOTOR SET NACK             │  y=43  Log entry 3
│                         ▲▼ │  Scroll arrows (right)
│ [Hold] back                │  y=56, footer
└────────────────────────────┘  y=63
```

| Parameter      | Value                              |
|----------------|------------------------------------|
| Max visible    | 4 rows                             |
| Row height     | 10 px                              |
| Format         | `DeviceShort CMD Result`           |
| Ring buffer    | 20 entries (`AGRI_LOG_ENTRIES`)    |
| Scroll         | UP/DOWN buttons shift window       |

### 6.8 GRID_FARM_SEL — Farm Picker

```
┌────────────────────────────┐  y=0
│▓▓ SELECT FARM ▓▓▓▓▓ ●OK ▓▓│  Title bar (inverted + global range icon)
├────────────────────────────┤  y=10
│                            │
│   FARM_101          ●OK    │  y=14  (per-farm range icon)
│ ▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓│
│ ▓> FARM_102         ○..   ▓│  y=24, selected (inverted row)
│ ▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓│
│   FARM_103          ✕NO    │  y=34
│   FARM_104          ○..    │  y=44
│                         ▲▼ │  Scroll arrows
│ UP/DN:move  SEL:pick       │  y=56, footer
└────────────────────────────┘  y=63
```

| Parameter      | Value                              |
|----------------|------------------------------------|
| Max visible    | 4 farms                            |
| Row height     | 10 px                              |
| List Y range   | 14–54 px                           |
| Selection      | `> FarmName` on inverted row       |
| Range icon     | Per-farm `●OK`/`○..`/`✕NO` at x=104 |
| Persistence    | Saved to NVS, survives reboot      |

#### Per-Farm Range Indicator

Each farm row shows a range icon on the right side (x=104), indicating
whether that farm's relay has been heard recently:

| State          | Icon   | Meaning                              |
|----------------|--------|--------------------------------------|
| `RANGE_OK`     | `●OK`  | Heartbeat received within 24 s       |
| `RANGE_CHECKING`| `○..` | Never heard / waiting                |
| `RANGE_LOST`   | `✕NO`  | No heartbeat for >24 s               |

Range is tracked per-farm via `range_on_farm_heartbeat()`, called
before the farm-ID filter so heartbeats from all farms are recorded.

---

## 7. Splash Screen

Shown at boot or for status messages. Blocks briefly.

```
┌────────────────────────────┐
│                            │
│                            │
│ ESP-Agri v2.0              │  y=20
│                            │
│ Initializing mesh...       │  y=36
│                            │
│                            │
└────────────────────────────┘
```

---

## 8. Common UI Patterns

### 8.1 Screen Zone Template

All submenus follow this consistent layout:

```
┌────────────────────────────┐
│▓▓ SCREEN TITLE ▓▓▓▓▓▓▓▓▓▓▓│  Zone 1: Title bar     (y: 0–9,   inverted)
├────────────────────────────┤
│                            │  Zone 2: Content area   (y: 12–53)
│  Content / list / info     │
│                            │
│                            │
│ [Hold] back                │  Zone 3: Footer hint    (y: 56–63)
└────────────────────────────┘
```

| Zone    | Y Range | Height | Purpose                      |
|---------|---------|--------|------------------------------|
| Title   | 0–9     | 10 px  | Inverted bar, screen name    |
| Content | 12–53   | 42 px  | Scrollable list or info text |
| Footer  | 56–63   |  8 px  | Navigation hint text         |

> **Footer width rule:** The footer zone is a single 8px text row starting at
> x=2. At 6px per character, the **maximum usable width is 126px (21 chars)**
> from x=2 to x=128. For dual-hint footers (left + right label), ensure
> `left_x + left_chars×6 ≤ right_x` and `right_x + right_chars×6 ≤ 128`.
> Omit spaces inside brackets (e.g. `[Hold]Back` not `[Hold] Back`) when
> space is tight. Always verify total pixel width before committing.

### 8.2 Inverted Selection Pattern

For high contrast under bright conditions:
- **Title bars**: always white-fill, black text
- **Selected list item**: full-width white-fill row, black text with `> ` prefix
- **Selected tile**: entire tile filled white, text in black
- **Normal items**: white text on black background

### 8.3 Scrollable List Pattern

Used by: Settings Menu, Farm Picker, Debug Logs

```
Algorithm: _scrollTop(cursor, count, maxVisible)
  if count ≤ maxVisible → top = 0 (no scroll)
  else → top = cursor - maxVisible/2, clamped to [0, count-maxVisible]

Indicators: _drawScrollIndicators(top, count, maxVisible)
  ▲ triangle at (x=120-128, y=16-20) when top > 0
  ▼ triangle at (x=120-128, y=58-62) when top + maxVisible < count
```

### 8.4 Text Centering

Used on GRID_CONFIRM and tile labels:

```
x = (128 - strlen(text) × 6) / 2
```

---

## 9. Visual Feedback Patterns

### 9.1 ACK Pending → "..."

When a command is sent to the relay, the device tile shows `...` until:
- ACK received → updates state to ON/OFF
- Timeout (3 s) → shows FAIL flash

### 9.2 FAIL Flash (1.5 s)

On ACK timeout, the tile shows `FAIL` for 1500 ms, then auto-clears.

```
Timeline:
  [SEL press] → [...] → (3s timeout) → [FAIL] → (1.5s) → [OFF/ON restored]
```

### 9.3 Confirmation Gate

Device toggles require a confirmation step:
```
SEL on device tile → GRID_CONFIRM → SEL confirms → command sent → GRID_MAIN
                                   → Hold cancels → GRID_MAIN (no action)
```

### 9.4 Range Indicator

Continuously shown in status bar. Updated via periodic pings (every 8 s).

| State     | Visual  | Meaning                        |
|-----------|---------|--------------------------------|
| OK        | `●OK`  | Relay responded within 24 s    |
| CHECKING  | `○..`  | Initial state / waiting        |
| LOST      | `✕NO`  | No response for 24 s (3× ping)|

### 9.5 Always-On Display (AOD)

When **AOD is OFF**, the display sleeps after the configured timeout (default 30 s)
of **continuous** inactivity on `GRID_MAIN`. The idle timer only counts time
spent on `GRID_MAIN` — navigating into a submenu does **not** accumulate
idle time. When the screen returns to `GRID_MAIN` (via user action or
menu timeout auto-return), the idle timer restarts from zero.

| Behavior         | Detail                                             |
|------------------|----------------------------------------------------|
| Sleep trigger     | Continuous inactivity timeout on `GRID_MAIN` only  |
| Timer reset       | Any button press **or** screen transition to `GRID_MAIN` |
| Screen blank      | `clearDisplay()` + `display()` (all pixels off)    |
| Wake trigger      | Any button press (UP / DOWN / SEL)                 |
| Wake safety       | First press only wakes — **no navigation action**  |
| SEL on wake       | `g_selLongFired = true` suppresses short & long    |
| NVS persistence   | AOD enabled + timeout saved on change              |

```
Timeline (AOD OFF, 30s timeout):
  [Last button on GRID_MAIN] → (30s continuous idle) → [Screen OFF]
                                                          │
                                                     [Any button]
                                                          │
                                                     [Screen ON — no action]
                                                          │
                                                     [Next button → normal nav]

  Submenu scenario:
  [GRID_MAIN] → [Enter Settings] → (60s in submenu) → [Back to GRID_MAIN]
                                                          │
                                                  idle timer resets to 0
                                                          │
                                                     (30s idle) → [Screen OFF]
```

---

## 10. Timing Constants

| Constant              | Value  | Used For                           |
|-----------------------|--------|------------------------------------|
| `AGRI_DISPLAY_REFRESH_MS` | 250 ms  | Dirty-flag redraw interval      |
| `AGRI_ACK_TIMEOUT_MS`     | 3000 ms | Command ACK wait time           |
| `AGRI_LONG_PRESS_MS`      | 600 ms  | Hold threshold for BACK action  |
| `AGRI_DEBOUNCE_MS`        | 50 ms   | Button debounce window          |
| `AGRI_MENU_TIMEOUT_MS`    | 15000 ms| Return to home on inactivity    |
| `AGRI_RANGE_PING_MS`      | 8000 ms | Range check interval            |
| `AGRI_RANGE_TIMEOUT_MS`   | 24000 ms| Range LOST threshold            |
| `AGRI_RSSI_POLL_MS`       | 2000 ms | RSSI sampling interval          |
| `AGRI_AOD_DEFAULT_SEC`    | 30 s    | Default AOD sleep timeout       |
| `AGRI_AOD_MIN_SEC`        | 5 s     | Minimum AOD timeout             |
| `AGRI_AOD_MAX_SEC`        | 120 s   | Maximum AOD timeout             |
| `AGRI_AOD_STEP_SEC`       | 5 s     | AOD timeout adjustment step     |
| Fail flash duration        | 1500 ms | FAIL text display on tile       |
| Tile timestamp refresh     | 5000 ms | Last-seen timestamp update rate |
| `AGRI_STALE_MS`            | 30000 ms| Staleness warning threshold     |

---

## 11. Navigation Flow Chart

```
Boot
 │
 └──→ GRID_FARM_SEL ──pick──→ GRID_MAIN ──(AOD timeout)──→ Display Sleep
      (always shown on boot,     │                          (any btn wakes,
       NVS farm pre-loaded       │                           1st press consumed)
       if available)       ┌──────┴──────┐
                      SEL on D0-D4   SEL on D5
                           │              │
                      GRID_CONFIRM    GRID_SETTINGS (7 items)
                       │      │          │
                  SEL=OK   Hold=No   ┌───┼────┬────┬────┬────┬────┐
                    │        │       0    1    2    3    4    5    6
                    │   GRID_MAIN   │    │    │    │    │    │    │
                    │           Change  AOD  AOD  MESH DEBUG NODE BACK
                 Send CMD        Farm  Togl Time INFO LOGS INFO  │
                    │              │    │    │    │    │    │  GRID_MAIN
                 GRID_MAIN        SEL  inline  │  └────┴────┘
                               picker toggle  │  [Hold] → GRID_MAIN
                                       │  GRID_AOD_TIME
                                       │  UP/DN adj
                                       │  [Hold] → GRID_SETTINGS
                                       │
                                   [Hold] → GRID_MAIN (from settings)
```

---

## 12. File Reference

| File | Purpose |
|------|---------|
| `lib/AgriCore/agri_display.h`   | Display class declaration, all screen methods |
| `lib/AgriCore/agri_display.cpp` | Pixel-level rendering (718 lines)             |
| `lib/AgriCore/agri_gridui.h`    | Grid state machine API + structs              |
| `lib/AgriCore/agri_gridui.cpp`  | Navigation logic, cursor management           |
| `lib/AgriCore/agri_config.h`    | Pin defs, timing constants, dimensions        |
| `lib/AgriCore/agri_range.h`     | Range indicator state enum + per-farm API |
| `lib/AgriCore/agri_rssi.h`      | RSSI tracker: EMA smoothing, hysteresis, trend |
| `lib/AgriCore/agri_rssi.cpp`    | RSSI tracker implementation                   |
| `lib/AgriCore/agri_transport.h`  | Transport abstraction (incl. `getRSSI()`)  |
| `lib/AgriCore/agri_mesh_wifi.h`  | Mesh WiFi layer (RSSI via `WiFi.RSSI()`)   |
| `lib/AgriCore/agri_nvs.h`       | NVS persistence for farm + AOD settings       |
| `lib/AgriCore/agri_nvs.cpp`     | NVS implementation (farm, AOD enabled/timeout) |

---

## 13. Design Guidelines for New Screens

When adding a new screen to the Grid UI:

1. **Add enum value** to `GridScreen` in `agri_gridui.h`
2. **Add draw method** `_drawNewScreen()` to `AgriDisplay`
3. **Follow the zone template**: title (0–9), content (12–53), footer (56–63)
4. **Use inverted title bar** (white fill, black text) for consistency
5. **Use `_scrollTop()` + `_drawScrollIndicators()`** if content may overflow
6. **Reserve y=56–63** for `[Hold] back` footer — never let content overlap
7. **Add navigation case** in `grid_on_up/down/ok/back` in `agri_gridui.cpp`
8. **Add rendering case** in `AgriDisplay::refresh()` switch block
9. **Max content rows**: 4 rows × 10px in scrollable list zone
10. **Max info lines**: 4 lines at y=13, 23, 33, 43 (10px spacing)
11. **Footer text must fit 128px width** — single-hint: max 21 chars from x=2;
    dual-hint: verify `left_x + len×6 ≤ right_x` and `right_x + len×6 ≤ 128`.
    Drop inner spaces (e.g. `[Hold]Back`) when needed to stay within bounds.
