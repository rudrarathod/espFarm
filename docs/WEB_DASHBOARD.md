# ESP-Agri Relay Web Dashboard

This page documents the embedded relay dashboard served by `esp_relay`.

## Access

- HTTP UI: `http://<relay-ip>/`
- mDNS UI: `http://esp-agri-relay.local/`
- WebSocket: `ws://<relay-ip>:81/`

Ports:
- HTTP: `80`
- WebSocket: `81`

## UI Areas

1. Header + connection indicator
2. System banner (success/error state)
3. Quick toggle (master ON/OFF with confirmation mode)
4. Device grid (configured + empty slots)
5. Advanced configuration (per-slot ID/GPIO/polarity)

## Device Cards

- `ON`: active state card
- `OFF`: inactive state card
- `No device`: unconfigured slot placeholder
- `Pending`: temporary state while action is in-flight

## Validation Rules

Frontend validates before sending; backend remains authoritative.

- `id`: required, regex `^[A-Za-z0-9_-]{1,15}$`
- `gpio`: integer `0..39`
- no duplicate GPIO across active slots
- reserved GPIOs rejected:
  - `16`, `17`, `18` (buttons)
  - `21`, `22` (OLED I2C)
  - `2` (LED)

## WebSocket Protocol

### Client → Relay

```json
{"type":"get_status"}
{"type":"toggle","device":"PUMP_01"}
{"type":"set","device":"PUMP_01","state":true}
{"type":"set_all","state":false}
{"type":"config","idx":0,"id":"PUMP_01","gpio":13,"ah":true}
```

### Relay → Client

```json
{"type":"status","data":{...}}
{"type":"ok"}
{"type":"error","err":"reserved gpio"}
```

Status payload includes `devices[]` entries shaped like:

```json
{"id":"PUMP_01","state":true,"idx":0,"gpio":13,"ah":true,"valid":true}
```

## Persistence and Sync

- `config` updates reconfigure `agri_devmap` immediately.
- updated slot map is saved in NVS (`dev_cfg`).
- relay OLED grid is resynced from new mapping.
- dashboard clients receive refreshed `status` broadcast.

Mesh-side sync to remotes:
- relay now broadcasts a `DEVLIST_RSP` snapshot immediately after successful dashboard config,
- and also on first mesh-connect after boot.

This ensures remotes refresh tile bindings without waiting for periodic polling.

## Troubleshooting

- No live updates: verify WebSocket on port `81`.
- `reserved gpio` / `invalid args` / `conflict/invalid config`: correct ID/GPIO and retry.
- mDNS issue: use direct IP URL.
