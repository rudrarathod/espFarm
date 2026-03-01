

 Here are feature ideas that fit the existing hardware (SSD1306 128×64, 3-button, ESP-MESH) and architecture:

### High-Value / Low-Effort

1. **Batch Toggle (All ON / All OFF)** — Add a long-press on the SETUP tile to show a "Toggle All?" confirm screen. Farmers often need to shut everything down at once.

2. **Device Auto-Off Timer** — After confirming a toggle-ON, optionally set a countdown (5/15/30/60 min). Tile shows remaining time instead of "ON". Prevents forgetting a pump running overnight.

3. **OLED Contrast / Brightness** — Add a settings item to adjust `ssd1306_command(SSD1306_SETCONTRAST, val)`. Useful for day vs night field use. One byte NVS save.

4. **Connection RSSI Indicator** — Replace the simple `●OK`/`✕NO` range icon with a 3-bar signal strength icon using `painlessMesh` RSSI data. More informative at a glance.

### Medium Effort

5. **Alert Badge on GRID_MAIN** — When a device enters FAIL state or mesh drops, flash a small `!` icon on the status bar that persists until the user views the relevant screen. Prevents missed failures.

6. **Last-Seen Timestamps on Tiles** ✅ — Show how long ago each device's state was confirmed (e.g., `2m` ago). Gives confidence the displayed state is fresh vs stale. *Enhanced: wildcard STATUS_REQ, heartbeat bitmask, 30s staleness visual (blinking dot + `!` suffix).*

7. **Quick-Peek on Hold** — Long-press on a device tile (instead of going to GRID_CONFIRM) shows a mini info popup: last toggle time, ACK count, fail count — then release returns to grid. No screen transition needed.

### Higher Effort / High Value

8. **Schedule Screen (GRID_SCHEDULE)** — Per-device daily on/off time. Stored in NVS. Remote node sends the command at the scheduled time autonomously. Critical for irrigation automation.

9. **Multi-Farm Dashboard** — On GRID_MAIN, a quick-swipe (double-tap DOWN from tile 5) switches to the next farm's tile view without going through Settings → Change Farm. Status bar shows active farm.

10. **OTA Update Screen** — A settings menu item that puts the node into OTA mode via WiFi AP, with a progress bar on screen. Avoids needing USB access in the field.

---

Want me to implement any of these? I'd recommend starting with **#2 (Auto-Off Timer)** and **#5 (Alert Badge)** — they're the most impactful for agricultural use and fit cleanly into the existing state machine.