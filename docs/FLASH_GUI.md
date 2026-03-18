# ESP Flash GUI Guide

This document describes the desktop flashing/configuration tool:
`tools/esp_flash_gui.py`.

## Purpose

The GUI is a local operator console for:
- building/flashing relay and remote firmware,
- syncing runtime config from ESP over serial,
- writing config back to ESP without reflashing,
- tracking saved node inventory and topology.

## Tabs

## 1) Flash

Main operations:
- Select role (`Relay / Pump` or `Remote / Button`)
- Set node ID
  - Relay format: `FARM_###`
  - Remote format: `REMOTE_###`
- Pick serial port
- Build/flash actions (`Build + Flash`, `Build Only`)
- `Sync from ESP` (reboot + probe)
- `Write Config` (`AGRI_SET_CFG` over serial)
- Port health check (busy/free + holder process hint)

Role-aware build flags:
- Relay: `AGRI_NODE_ROLE=0`, `AGRI_FARM_ID=...`
- Remote: `AGRI_NODE_ROLE=1`, `AGRI_REMOTE_ID=...`, optional `AGRI_REMOTE_FARM_LIST_CSV=...`

Remote farm list editor:
- Add/remove token chips
- Stored as CSV for firmware compatibility

Known relay helper:
- Uses saved relay catalog
- One-click “Assign to Remote” to add relay farm to remote list

## 2) Device List

Shows saved catalog entries from serial sync:
- relay and remote nodes,
- IDs, farm links, port, last update,
- raw detailed JSON in detail pane.

Storage file:
- `tools/.esp_device_catalog.json`

Legacy relay-only helper cache:
- `tools/.esp_relay_catalog.json`

## 3) Topology

Visual relation view inferred from catalog:
- `FROM`: Remote node
- `TO`: Relay farm target
- `LINK`: configured-farm
- `STATUS`:
  - `connected-known` (target farm exists in known relay set)
  - `relay-missing` (remote references a farm not currently known)
  - `no targets` (remote has no configured farm list)

## Config Sync Protocol

Read config:
- `AGRI_GET_CFG`

Write config:
- `AGRI_SET_CFG {json}`

Expected response includes role and version fields:
- `syncVer`
- `protoVer`

GUI currently expects:
- `GUI_SYNC_VER = 2`

The status line in Flash tab shows:
- last sync time,
- sync source (`auto-probe` / `manual-reboot`),
- version warning if mismatch.

## Persistence

GUI profiles (per role):
- `tools/.esp_flash_gui_profiles.json`

Saved fields per role:
- ID
- env override
- remote farm list

Firmware-side runtime config persistence is handled in NVS.

## Typical Workflow

1. Connect ESP and select port.
2. `Sync from ESP` to detect role and current config.
3. Adjust ID/list as needed.
4. Optional: `Write Config` for runtime update without reflashing.
5. `Build + Flash` when firmware rebuild is required.
6. Check Device List and Topology tabs for inventory/link visibility.
