# Meshtastic Migration Checklist

## Overview

The ESP-Agri system uses a three-layer architecture specifically designed for
radio-layer swapping.  This document describes exactly what changes when
migrating from **ESP-MESH (Wi-Fi)** to **Meshtastic (LoRa)**.

---

## What STAYS THE SAME (No Changes Required)

| Component              | File(s)                 | Notes                          |
|------------------------|------------------------|--------------------------------|
| Application logic      | `esp_relay.cpp`        | Pump control, Farm ID check    |
| Application logic      | `esp_remote.cpp`       | Button handling, ACK/retry     |
| Message protocol       | `agri_protocol.h/cpp`  | Already <100 bytes             |
| Transport API          | `agri_transport.h/cpp` | Free-function API unchanged    |
| Display module         | `agri_display.h/cpp`   | Role-aware OLED UI             |
| Configuration (mostly) | `agri_config.h`        | Farm ID, pins, timing          |
| Duplicate detection    | `agri_protocol.cpp`    | deviceId + messageId based     |
| ACK / retry logic      | `esp_remote.cpp`       | Already handles high latency   |

---

## What CHANGES

### 1. New Radio Layer Implementation

Create `agri_meshtastic.h` and `agri_meshtastic.cpp` that implement the
`AgriTransport` interface:

```cpp
class AgriMeshtastic : public AgriTransport {
public:
    bool     init() override;
    void     update() override;
    bool     send(uint32_t destId, const AgriMessage& msg) override;
    bool     broadcast(const AgriMessage& msg) override;
    void     onReceive(agri_receive_cb_t cb) override;
    void     onConnectionChange(agri_connection_cb_t cb) override;
    bool     isConnected() override;
    uint32_t getNodeId() override;
    uint16_t getNodeCount() override;
};
```

### 2. Swap Radio in setup()

In both `esp_relay.cpp` and `esp_remote.cpp`, change ONE line:

```cpp
// Before (Wi-Fi POC):
#include "agri_mesh_wifi.h"
static AgriMeshWifi g_mesh;

// After (Meshtastic LoRa):
#include "agri_meshtastic.h"
static AgriMeshtastic g_mesh;
```

Everything else remains identical because the application only uses:
- `agri_transport_init(&g_mesh)`
- `agri_send()` / `agri_broadcast()`
- `agri_on_receive()`
- `agri_update()`

### 3. Update `platformio.ini`

```ini
lib_deps =
    ; Remove: painlessmesh/painlessMesh
    ; Add:    meshtastic/Meshtastic-arduino (or serial bridge library)
    adafruit/Adafruit SSD1306 @ ^2.5.7
```

### 4. Adjust Timing Constants

In `agri_config.h`, increase timeouts for LoRa latency:

```cpp
#define AGRI_ACK_TIMEOUT_MS     10000   // Was 3000 (LoRa is slower)
#define AGRI_MAX_RETRIES        5       // Was 3 (more retries over LoRa)
```

### 5. Optional: Binary Encoding

For maximum LoRa efficiency, replace JSON serialization with binary packing
in `agri_protocol.cpp`.  The `AgriMessage` struct is already fixed-size, so
`agri_serialize()` / `agri_deserialize()` can pack into ~20 bytes:

```cpp
// Binary layout (20 bytes):
// [0-5]  farmId (6 bytes, padded)
// [6-7]  messageId (uint16, little-endian)
// [8]    command (uint8)
// [9]    nonce (uint8)
// [10]   pumpState (uint8)
// [11-14] timestamp (uint32, little-endian)
// [15-18] sourceNodeId (uint32, little-endian)
// [19]   checksum (XOR of bytes 0-18)
```

---

## Migration Considerations

### Node Identity
- ESP-MESH uses MAC-derived 32-bit node IDs (auto-assigned)
- Meshtastic uses persistent node numbers
- The `AgriTransport::getNodeId()` abstraction hides this difference

### Routing
- ESP-MESH: tree-based routing, self-healing
- Meshtastic: flood-based routing (simpler, higher redundancy)
- Both handle multi-hop transparently — application layer unaffected

### Bandwidth
- ESP-MESH (Wi-Fi): high bandwidth, low latency
- Meshtastic (LoRa): ~200 bytes/message, seconds of latency
- Current protocol already fits within LoRa constraints (<100 bytes)

### Power
- ESP-MESH: always-on Wi-Fi (higher power)
- Meshtastic: can duty-cycle LoRa radio (lower power)
- Consider adding sleep modes in the Meshtastic radio layer

### Multi-Farm Scaling
- Farm ID validation already implemented
- Multiple farms can share the same LoRa mesh
- Each relay only responds to its own Farm ID

---

## Step-by-Step Migration Procedure

1. [ ] Create `lib/AgriCore/agri_meshtastic.h` implementing `AgriTransport`
2. [ ] Create `lib/AgriCore/agri_meshtastic.cpp` with Meshtastic serial/SPI bridge
3. [ ] Update `platformio.ini` lib_deps (remove painlessMesh, add Meshtastic)
4. [ ] Change `#include` and static instance in `esp_relay.cpp`
5. [ ] Change `#include` and static instance in `esp_remote.cpp`
6. [ ] Increase `AGRI_ACK_TIMEOUT_MS` in `agri_config.h`
7. [ ] (Optional) Switch to binary encoding in `agri_protocol.cpp`
8. [ ] Test with two LoRa nodes
9. [ ] Test multi-hop with three or more nodes
10. [ ] Remove `agri_mesh_wifi.h/cpp` if Wi-Fi POC no longer needed

**Total application code changes: 2 lines per node (include + instance type)**
