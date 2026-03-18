# ESP-Agri Architecture

## System Overview

ESP-Agri uses a role-based ESP32 design:
- `esp_remote`: field controller with 3-button OLED UI
- `esp_relay`: actuator node with configurable logical device map + dashboard
- `esp_extender`: mesh extension node (routing/coverage only)
- `esp_sensor`: farm telemetry node that publishes sensor data for a target farm relay

Communication is transport-abstracted (`agri_transport`) and currently runs on ESP-MESH (`agri_mesh_wifi`).

## Layering

1. Application layer: `src/esp_remote.cpp`, `src/esp_relay.cpp`
2. Protocol layer: `lib/AgriCore/agri_protocol.*`
3. Transport abstraction: `lib/AgriCore/agri_transport.*`
4. Radio implementation: `lib/AgriCore/agri_mesh_wifi.*`

Application code does not call mesh-library APIs directly.

## Core Runtime Flow

1. Remote syncs logical device bindings from relay (`DEVLIST` snapshot).
2. User selects tile and confirms action on remote.
3. Remote sends command (`DEV_ON`/`DEV_OFF`/`TOGGLE`) with farm + message ID.
4. Relay validates farm ID, duplicate window, and device mapping.
5. Relay applies GPIO action, then replies with `ACK` + resulting state.
6. Remote clears pending state and refreshes UI.
7. Sensor nodes periodically publish farm-tagged telemetry (`CMD_HEARTBEAT`) for relay-side ingestion/logging.

## Dynamic Device List Synchronization

Device tiles on remote are relay-driven (not hardcoded):

- Remote requests bindings via `CMD_DEVLIST_REQ`.
- Relay responds with one `CMD_DEVLIST_RSP` per slot, using `nonce` as slot index.
- Empty slots are reported as `SLOT_n` placeholders.
- Remote updates each grid tile binding from those responses.

Immediate update triggers:
- after dashboard `config` changes on relay,
- on first relay mesh-connect after boot,
- plus remote periodic refresh.

## Message Protocol

Compact JSON fields:
- `f`: farm ID
- `d`: device ID
- `c`: command
- `m`: message ID
- `t`: timestamp
- `n`: nonce (also used as slot index in `DEVLIST_RSP`)
- `s`: device state payload
- `src`: source node ID

## Command Types

| Value | Name         | Direction      | Purpose |
|------:|--------------|----------------|---------|
| 0     | `PUMP_ON`    | Remote → Relay | Legacy compatibility |
| 1     | `PUMP_OFF`   | Remote → Relay | Legacy compatibility |
| 2     | `TOGGLE`     | Remote → Relay | Toggle logical device |
| 3     | `DEV_ON`     | Remote → Relay | Set logical device ON |
| 4     | `DEV_OFF`    | Remote → Relay | Set logical device OFF |
| 10    | `ACK`        | Relay → Remote | Command acknowledgment + state |
| 11    | `NACK`       | Relay → Remote | Negative acknowledgment |
| 12    | `STATUS_REQ` | Remote → Relay | Status polling |
| 13    | `STATUS_RSP` | Relay → Remote | Status response |
| 14    | `DEVLIST_REQ`| Remote → Relay | Request current relay slot bindings |
| 15    | `DEVLIST_RSP`| Relay → Remote | Per-slot binding/state response |
| 20    | `HEARTBEAT`  | Any → Any      | Keepalive / bitmask state |

## Reliability Notes

- Remote ACK retry window (`AGRI_ACK_TIMEOUT_MS`, `AGRI_MAX_RETRIES`)
- Duplicate suppression ring buffer
- Directed send when peer known, fallback broadcast
- Relay fail-safe mapping + runtime reconfiguration via dashboard

## Related Docs

- UI/UX: [DISPLAY_UIUX_GUIDE.md](DISPLAY_UIUX_GUIDE.md)
- Dashboard: [WEB_DASHBOARD.md](WEB_DASHBOARD.md)
- Flash tool: [FLASH_GUI.md](FLASH_GUI.md)
- Migration: [MIGRATION_CHECKLIST.md](MIGRATION_CHECKLIST.md)
- Code reality check: [CODE_ANALYSIS.md](CODE_ANALYSIS.md)
