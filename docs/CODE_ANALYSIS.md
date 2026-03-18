# ESP-Farm Code Analysis

Last updated: 2026-03-17

## Scope

Analyzed files:
- `src/esp_relay.cpp`
- `src/esp_remote.cpp`
- `lib/AgriCore/*.h`
- `lib/AgriCore/*.cpp`

## Build and Role Model

- `platformio.ini` defines two firmware environments:
  - `esp_relay`: builds `src/esp_relay.cpp`, sets `AGRI_NODE_ROLE=0`
  - `esp_remote`: builds `src/esp_remote.cpp`, sets `AGRI_NODE_ROLE=1`
- Shared code lives in `lib/AgriCore` and is role-switched at compile time where needed.

## Implemented Layering

1. **Application layer**
   - `src/esp_relay.cpp`
   - `src/esp_remote.cpp`
2. **Transport abstraction**
   - `agri_transport.h/.cpp` free-function API (`agri_send`, `agri_broadcast`, callbacks, status)
3. **Radio implementation**
   - `agri_mesh_wifi.h/.cpp` (`AgriTransport` implementation using painlessMesh)

## Module Responsibilities

- `agri_config.h`: constants (pins, timing, IDs, protocol limits)
- `agri_protocol.h/.cpp`: message schema, compact JSON serialize/deserialize, duplicate window helpers
- `agri_transport.h/.cpp`: transport interface + global wrapper
- `agri_mesh_wifi.h/.cpp`: mesh init, send/broadcast, receive callback bridge, node metrics/RSSI
- `agri_range.h/.cpp`: relay reachability state (`OK/CHECKING/LOST`) and per-farm last-seen tracking
- `agri_rssi.h/.cpp`: RSSI smoothing (EMA), hysteresis bars, quality/trend statistics
- `agri_nvs.h/.cpp`: persisted settings in ESP32 `Preferences`
- `agri_devmap.h/.cpp`: logical device ↔ GPIO mapping/state and runtime reconfiguration
- `agri_gridui.h/.cpp`: screen state machine, cursor movement, settings/menu logic
- `agri_display.h/.cpp`: OLED rendering for all screen states
- `agri_log.h/.cpp`: in-memory ring log for actions/results

## Runtime Flow Summary

### Remote (`esp_remote.cpp`)

- Setup: input pins, display, NVS load (farm + AOD), grid/profile init, transport callbacks.
- Loop:
  - updates transport
   - requests and applies relay device-list snapshots (dynamic tile bindings)
  - handles 3-button UI interactions
  - sends command with pending-ACK tracking
  - retries on `AGRI_ACK_TIMEOUT_MS` up to `AGRI_MAX_RETRIES`
  - updates range/RSSI and redraw scheduling

### Relay (`esp_relay.cpp`)

- Setup: devmap + safe outputs, display/UI, NVS load (AOD + dev config), transport callbacks.
- Also starts local dashboard stack (HTTP + WebSocket + mDNS).
- Loop:
  - transport update
  - web server/websocket handling
  - local UI navigation/actions
   - immediate `DEVLIST_RSP` snapshot broadcast on dashboard config change
   - one-time `DEVLIST_RSP` snapshot broadcast on first mesh-connect after boot
  - range/RSSI + periodic heartbeat/status updates

## Protocol Snapshot (Current)

Current compact JSON fields:
- `f`: farm id
- `d`: logical target device id
- `c`: command
- `m`: message id
- `t`: timestamp
- `n`: nonce
- `s`: state payload (device/pump state)
- `src`: source node id

Notes:
- Message ID is monotonic in-process (not globally unique across senders).
- Relay applies duplicate filter over `(deviceId, messageId)` within a time window.
- Remote uses stop-and-wait style single pending command with retries.
- Remote no longer depends on hardcoded logical device IDs; bindings are learned from relay via `DEVLIST_REQ`/`DEVLIST_RSP`.

## Persistence (NVS)

Stored keys in `agri_nvs.cpp`:
- `farm`
- `aod_en`
- `aod_sec`
- `dev_cfg`
- `relay_farm`
- `remote_id`
- `remote_list`

Runtime config sync (serial CLI):
- `AGRI_GET_CFG` returns current role-specific config JSON.
- `AGRI_SET_CFG {json}` applies runtime config updates and persists values to NVS.
- Firmware config payload includes version markers (`syncVer`, `protoVer`).

## Observed Gaps / Risks

1. **Protocol has no auth/crypto**
   - Farm ID remains the main acceptance gate for command traffic.
2. **NACK path is not fully exercised**
   - `CMD_NACK` exists but relay primarily ACKs or drops/logs unknown device cases.
3. **Duplicate key scope is narrow**
   - Duplicate filter key does not include sender node ID or nonce.
4. **Migration coupling on relay**
   - Relay app currently includes Wi-Fi dashboard dependencies, so non-Wi-Fi transport migration is not only a transport class swap.

## Recommended Documentation Baseline

For current implementation docs, keep the following as source-of-truth:
- `docs/ARCHITECTURE.md` for layers/protocol commands and hardware mapping
- `docs/DISPLAY_UIUX_GUIDE.md` for Grid UI states and indicator behavior
- `docs/MIGRATION_CHECKLIST.md` for radio swap strategy and caveats
- this file (`docs/CODE_ANALYSIS.md`) for code-level reality checks
