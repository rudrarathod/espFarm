Here are feature ideas for the current hardware (SSD1306 128×64, 3-button, ESP-MESH) and architecture.

Status legend: ✅ implemented | 🧪 partial/prototype | 📌 backlog

### High-Value / Low-Effort

1. 📌 **Batch Toggle (All ON / All OFF)** — Add a long-press on the SETUP tile to show a "Toggle All?" confirm screen. Farmers often need to shut everything down at once.

2. 📌 **Device Auto-Off Timer** — After confirming a toggle-ON, optionally set a countdown (5/15/30/60 min). Tile shows remaining time instead of "ON". Prevents forgetting a pump running overnight.

3. 📌 **OLED Contrast / Brightness** — Add a settings item to adjust `ssd1306_command(SSD1306_SETCONTRAST, val)`. Useful for day vs night field use. One byte NVS save.

4. ✅ **Connection RSSI Indicator** — Implemented as a unified status-bar signal widget (RSSI bars + trend + range-loss fallback).

5. ✅ **Relay-Driven Device List Sync** — Remote device tiles now sync from relay dashboard configuration via mesh device-list snapshots (`DEVLIST_REQ/RSP`), including immediate push after relay config changes.

### Medium Effort

6. 📌 **Alert Badge on GRID_MAIN** — When a device enters FAIL state or mesh drops, flash a small `!` icon on the status bar that persists until the user views the relevant screen. Prevents missed failures.

7. 📌 **Last-Seen Timestamps on Tiles** — Not currently implemented on tiles; could show how long ago each device state was confirmed (e.g., `2m`).

8. 📌 **Quick-Peek on Hold** — Long-press on a device tile (instead of going to GRID_CONFIRM) shows a mini info popup: last toggle time, ACK count, fail count — then release returns to grid. No screen transition needed.

### Higher Effort / High Value

9. 📌 **Schedule Screen (GRID_SCHEDULE)** — Per-device daily on/off time. Stored in NVS. Remote node sends the command at the scheduled time autonomously. Critical for irrigation automation.

10. 🧪 **Multi-Farm Dashboard** — Farm selection exists in settings (`Change Farm`); quick-swipe farm cycling on `GRID_MAIN` is still pending.

11. 📌 **OTA Update Screen** — A settings menu item that puts the node into OTA mode via WiFi AP, with a progress bar on screen. Avoids needing USB access in the field.

---

Suggested next implementation pair: **#2 (Auto-Off Timer)** and **#5 (Alert Badge)** for high operational impact with moderate UI changes.