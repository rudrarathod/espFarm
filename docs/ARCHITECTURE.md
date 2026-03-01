# ESP-Agri Architecture

## System Overview

Radio-agnostic smart irrigation control with two ESP32 nodes communicating
over a self-forming mesh.

```
  ┌──────────────────────┐              ┌──────────────────────┐
  │    Node B: REMOTE    │   ESP-MESH   │    Node A: RELAY     │
  │  ┌────────────────┐  │  (Wi-Fi /    │  ┌────────────────┐  │
  │  │  Push Button   │  │   multi-hop) │  │  Relay Module  │  │
  │  │  GPIO18        │──┼──────────────┼──│  GPIO5 → Pump  │  │
  │  └────────────────┘  │              │  └────────────────┘  │
  │  ┌────────────────┐  │  TOGGLE ──►  │  ┌────────────────┐  │
  │  │  OLED 128×64   │  │  ◄── ACK     │  │  OLED 128×64   │  │
  │  │  I2C 0x3C      │  │              │  │  I2C 0x3C      │  │
  │  └────────────────┘  │              │  └────────────────┘  │
  └──────────────────────┘              └──────────────────────┘
```

## Three-Layer Architecture

```
  ╔═══════════════════════════════════════════════════════════╗
  ║               APPLICATION LAYER                           ║
  ║  esp_relay.cpp  │  esp_remote.cpp                        ║
  ║  Pump logic, button handling, Farm ID, OLED UI           ║
  ╠═══════════════════════════════════════════════════════════╣
  ║              TRANSPORT LAYER (agri_transport.h)           ║
  ║  agri_send()  agri_broadcast()  agri_on_receive()        ║
  ║  agri_update()  agri_is_connected()  agri_get_node_id()  ║
  ╠═══════════════════════════════════════════════════════════╣
  ║              RADIO LAYER (swappable)                      ║
  ║  ┌─────────────────────┐  ┌─────────────────────────┐    ║
  ║  │  AgriMeshWifi       │  │  AgriMeshtastic         │    ║
  ║  │  (painlessMesh)     │  │  (future LoRa)          │    ║
  ║  │  ✅ Phase 1 POC     │  │  📋 Phase 2             │    ║
  ║  └─────────────────────┘  └─────────────────────────┘    ║
  ╚═══════════════════════════════════════════════════════════╝
```

**Key rule:** Application code (`esp_relay.cpp`, `esp_remote.cpp`) NEVER
calls painlessMesh or any radio-specific API directly.  All messaging goes
through the `agri_*()` free-function transport API.

## Message Flow

```
  REMOTE                          RELAY
    │                               │
    │  [Button Press]               │
    │──── TOGGLE (broadcast) ──────►│
    │                               │── validate Farm ID
    │                               │── check duplicates
    │                               │── toggle relay
    │◄──── ACK + pump state ────────│
    │                               │
    │  [Update OLED]                │  [Update OLED]
```

### Retry Flow
```
  REMOTE                          RELAY
    │── TOGGLE ────────────────────►│  (lost)
    │   ... 3s timeout ...          │
    │── TOGGLE (retry 1/3) ────────►│
    │◄──── ACK ─────────────────────│
```

## Message Protocol

Compact JSON (<100 bytes), Meshtastic-ready:

```json
{"f":"FARM01","d":"REMOTE01","c":2,"m":1234,"t":567890,"n":42,"s":0,"src":0}
```

| Key   | Field        | Type   | Description                        |
|-------|-------------|--------|------------------------------------|
| `f`   | Farm ID     | string | Farm identity for multi-farm       |
| `d`   | Device ID   | string | Sender device identifier           |
| `c`   | Command     | uint8  | 0=ON, 1=OFF, 2=TOGGLE, 10=ACK ... |
| `m`   | Message ID  | uint16 | Monotonic counter (per device)     |
| `t`   | Timestamp   | uint32 | millis() at creation               |
| `n`   | Nonce       | uint8  | Random byte for uniqueness         |
| `s`   | Pump State  | uint8  | 0=OFF, 1=ON (in ACK/STATUS)       |
| `src` | Source Node | uint32 | Mesh node ID of sender             |

### Command Types

| Value | Name       | Direction        | Description                  |
|-------|-----------|------------------|------------------------------|
| 0     | PUMP_ON   | Remote → Relay   | Turn pump on                 |
| 1     | PUMP_OFF  | Remote → Relay   | Turn pump off                |
| 2     | TOGGLE    | Remote → Relay   | Toggle pump state            |
| 10    | ACK       | Relay → Remote   | Acknowledge + pump state     |
| 11    | NACK      | Relay → Remote   | Negative acknowledge         |
| 12    | STATUS_REQ| Remote → Relay   | Request current status       |
| 13    | STATUS_RSP| Relay → Remote   | Status response              |
| 20    | HEARTBEAT | Any → Any        | Keepalive (no ACK required)  |

## Project Structure

```
espFarm/
├── platformio.ini              # Multi-env config (esp_relay + esp_remote)
├── lib/
│   └── AgriCore/               # Shared library
│       ├── agri_config.h       # All constants & pin defs
│       ├── agri_protocol.h     # Message struct, serialize, dup detection
│       ├── agri_protocol.cpp
│       ├── agri_transport.h    # Abstract transport + free-function API
│       ├── agri_transport.cpp
│       ├── agri_mesh_wifi.h    # painlessMesh radio layer
│       ├── agri_mesh_wifi.cpp
│       ├── agri_display.h      # OLED display abstraction
│       └── agri_display.cpp
├── src/
│   ├── esp_relay.cpp           # Node A — Pump/Relay firmware
│   └── esp_remote.cpp          # Node B — Button/Remote firmware
└── docs/
    ├── ARCHITECTURE.md         # This file
    └── MIGRATION_CHECKLIST.md  # Meshtastic migration guide
```

## Reliability Features

| Feature                | Implementation                              |
|------------------------|---------------------------------------------|
| Button debounce        | Software debounce (50 ms)                   |
| ACK with retry         | 3 retries, 3 s timeout each                 |
| Duplicate detection    | Ring buffer of (deviceId, messageId) pairs   |
| Relay fail-safe        | Pump OFF on boot and on unrecoverable error |
| Mesh self-healing      | painlessMesh handles reconnection           |
| Watchdog friendly      | Non-blocking loop, no delay() in main loop  |
| Directed + broadcast   | Caches peer node ID for directed sends      |

## Serial Debug Output

Both nodes emit tagged debug logs at 115200 baud:

```
[MESH-WIFI] Initializing ESP-MESH...
[MESH-WIFI] Node ID : 2987340156
[MESH-WIFI] Ready — waiting for peers...
[MESH-WIFI] + Peer joined : 4089556218
[APP] Mesh connected — 1 peer(s)
[APP] *BUTTON PRESSED* — sending TOGGLE
[MESH-WIFI] >> Broadcast  cmd=TOGGLE  mid=1  [OK]
[APP] Sent TOGGLE #1 — waiting ACK...
[MESH-WIFI] << Recv from 2987340156: {"f":"FARM01",...}
[APP] ACK received for #1 — pump=ON
```
