#!/usr/bin/env python3
from __future__ import annotations

import json
import os
import queue
import re
import shlex
import shutil
import subprocess
import threading
import time
from datetime import datetime
from pathlib import Path
import tkinter as tk
from tkinter import ttk

try:
    import serial  # type: ignore[import-not-found]
except ImportError:
    serial = None


ROLE_TO_ENV = {
    "Relay / Pump": "esp_relay",
    "Remote / Button": "esp_remote",
    "Extender / Mesh": "esp_extender",
    "Sensor / Farm": "esp_sensor",
}

ROLE_TO_VALUE = {
    "Relay / Pump": 0,
    "Remote / Button": 1,
    "Extender / Mesh": 2,
    "Sensor / Farm": 3,
}

GUI_SYNC_VER = 2
MAX_ID_LEN = 11


class EspFlashGui(tk.Tk):
    def __init__(self) -> None:
        super().__init__()
        self.title("ESP Farm Flasher")
        self.geometry("860x560")

        self.project_root = Path(__file__).resolve().parents[1]
        self.log_queue: queue.Queue[str] = queue.Queue()
        self.flash_thread: threading.Thread | None = None
        self.probe_thread: threading.Thread | None = None
        self.pio_exe = self._find_platformio_executable()
        self.profile_path = self.project_root / "tools" / ".esp_flash_gui_profiles.json"
        self.relay_catalog_path = self.project_root / "tools" / ".esp_relay_catalog.json"
        self.device_catalog_path = self.project_root / "tools" / ".esp_device_catalog.json"
        self.profiles: dict[str, dict[str, str]] = {}
        self.relay_catalog: dict[str, dict[str, object]] = {}
        self.device_catalog: dict[str, dict[str, object]] = {}
        self.device_tree: ttk.Treeview | None = None
        self.device_details_text: tk.Text | None = None
        self.topology_tree: ttk.Treeview | None = None
        self.topology_details_text: tk.Text | None = None
        self.role_locked = False
        self.last_sync_source = "-"
        self.last_sync_ver = "-"
        self.last_sync_time = "-"

        self.role_var = tk.StringVar(value="Relay / Pump")
        self.id_var = tk.StringVar(value="")
        self.remote_farm_list_var = tk.StringVar(value="")
        self.new_remote_farm_var = tk.StringVar(value="")
        self.known_relay_var = tk.StringVar(value="")
        self.relay_devices_preview_var = tk.StringVar(value="No relay catalog yet")
        self.port_var = tk.StringVar(value="")
        self.custom_env_var = tk.StringVar(value="")
        self.status_var = tk.StringVar(value="Idle")
        self.sync_status_var = tk.StringVar(value="Sync: never")

        self.remote_farm_tokens: list[str] = []

        self._build_ui()
        self._load_profiles()
        self._load_relay_catalog()
        self._load_device_catalog()
        self._load_profile_for_role(self.role_var.get())
        self._sync_remote_tokens_from_csv()
        self._refresh_relay_catalog_ui()
        self._populate_device_tree()
        self._populate_topology_tree()
        self._refresh_ports()
        self.after(80, self._drain_log_queue)

    def _find_platformio_executable(self) -> str | None:
        found = shutil.which("platformio")
        if found:
            return found

        candidates = [
            str(Path.home() / ".platformio" / "penv" / "bin" / "platformio"),
            str(self.project_root / ".venv" / "bin" / "platformio"),
        ]

        for candidate in candidates:
            if Path(candidate).exists():
                return candidate
        return None

    def _build_ui(self) -> None:
        notebook = ttk.Notebook(self)
        notebook.pack(fill=tk.BOTH, expand=True)

        frame = ttk.Frame(notebook, padding=12)
        notebook.add(frame, text="Flash")

        devices_tab = ttk.Frame(notebook, padding=12)
        notebook.add(devices_tab, text="Device List")

        topology_tab = ttk.Frame(notebook, padding=12)
        notebook.add(topology_tab, text="Topology")

        controls = ttk.LabelFrame(frame, text="Flash Settings", padding=10)
        controls.pack(fill=tk.X)

        ttk.Label(controls, text="Role").grid(row=0, column=0, sticky="w", padx=(0, 8), pady=6)
        role_combo = ttk.Combobox(
            controls,
            textvariable=self.role_var,
            state="readonly",
            values=list(ROLE_TO_ENV.keys()),
            width=22,
        )
        self.role_combo = role_combo
        role_combo.grid(row=0, column=1, sticky="w", pady=6)
        role_combo.bind("<<ComboboxSelected>>", self._on_role_change)

        ttk.Label(controls, text="ID").grid(row=0, column=2, sticky="w", padx=(20, 8), pady=6)
        ttk.Entry(controls, textvariable=self.id_var, width=20).grid(row=0, column=3, sticky="w", pady=6)
        ttk.Button(controls, text="Auto Name", command=self._auto_name_farm_id).grid(
            row=0, column=4, sticky="w", padx=(8, 0), pady=6
        )
        self.lock_btn = ttk.Button(controls, text="Unlock Role/Env", command=self._toggle_role_lock)
        self.lock_btn.grid(row=0, column=5, sticky="w", padx=(8, 0), pady=6)

        ttk.Label(controls, text="Serial Port").grid(row=1, column=0, sticky="w", padx=(0, 8), pady=6)
        self.port_combo = ttk.Combobox(controls, textvariable=self.port_var, width=22)
        self.port_combo.grid(row=1, column=1, sticky="w", pady=6)
        self.port_combo.bind("<<ComboboxSelected>>", self._on_port_selected)

        ttk.Button(controls, text="Refresh Ports", command=self._refresh_ports).grid(
            row=1, column=2, sticky="w", padx=(20, 8), pady=6
        )

        ttk.Button(controls, text="Check Port", command=self._check_port_health).grid(
            row=1, column=3, sticky="w", padx=(8, 8), pady=6
        )

        ttk.Button(controls, text="Sync from ESP", command=self._sync_from_esp).grid(
            row=1, column=4, sticky="w", padx=(8, 8), pady=6
        )

        ttk.Button(controls, text="Retry Sync", command=self._sync_from_esp).grid(
            row=1, column=5, sticky="w", padx=(8, 8), pady=6
        )

        ttk.Button(controls, text="Write Config", command=self._write_config_to_esp).grid(
            row=1, column=6, sticky="w", padx=(8, 8), pady=6
        )
        ttk.Label(controls, text="Env (optional)").grid(row=2, column=0, sticky="w", padx=(0, 8), pady=6)
        self.env_entry = ttk.Entry(controls, textvariable=self.custom_env_var, width=20)
        self.env_entry.grid(row=2, column=1, sticky="w", pady=6)

        ttk.Label(controls, text="Add Farm").grid(row=2, column=2, sticky="w", padx=(20, 8), pady=6)
        self.remote_add_entry = ttk.Entry(controls, textvariable=self.new_remote_farm_var, width=18)
        self.remote_add_entry.grid(row=2, column=3, sticky="w", pady=6)
        ttk.Button(controls, text="Add", command=self._add_remote_farm_token).grid(
            row=2, column=4, sticky="w", padx=(8, 8), pady=6
        )
        ttk.Button(controls, text="Clear Farms", command=self._clear_remote_farm_tokens).grid(
            row=2, column=5, sticky="w", padx=(8, 8), pady=6
        )

        ttk.Label(controls, text="Remote Farm List").grid(row=3, column=0, sticky="nw", padx=(0, 8), pady=6)
        self.remote_chip_frame = ttk.Frame(controls)
        self.remote_chip_frame.grid(row=3, column=1, columnspan=6, sticky="w", pady=6)

        ttk.Label(controls, text="Known Relay").grid(row=4, column=0, sticky="w", padx=(0, 8), pady=6)
        self.known_relay_combo = ttk.Combobox(controls, textvariable=self.known_relay_var, width=22, state="readonly")
        self.known_relay_combo.grid(row=4, column=1, sticky="w", pady=6)
        self.known_relay_combo.bind("<<ComboboxSelected>>", self._on_known_relay_selected)
        ttk.Button(controls, text="Assign to Remote", command=self._assign_known_relay_to_remote).grid(
            row=4, column=2, sticky="w", padx=(8, 8), pady=6
        )

        ttk.Label(controls, text="Relay Devices").grid(row=5, column=0, sticky="nw", padx=(0, 8), pady=6)
        ttk.Label(controls, textvariable=self.relay_devices_preview_var, justify="left").grid(
            row=5, column=1, columnspan=6, sticky="w", pady=6
        )

        actions = ttk.Frame(frame)
        actions.pack(fill=tk.X, pady=(10, 8))

        self.flash_btn = ttk.Button(actions, text="Build + Flash", command=self._start_flash)
        self.flash_btn.pack(side=tk.LEFT)

        ttk.Button(actions, text="Build Only", command=lambda: self._start_flash(upload=False)).pack(
            side=tk.LEFT, padx=(8, 0)
        )

        ttk.Label(actions, textvariable=self.status_var).pack(side=tk.RIGHT)

        log_frame = ttk.LabelFrame(frame, text="Output", padding=8)
        log_frame.pack(fill=tk.BOTH, expand=True)

        self.log_text = tk.Text(log_frame, wrap="word", height=20)
        self.log_text.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)

        scrollbar = ttk.Scrollbar(log_frame, orient="vertical", command=self.log_text.yview)
        scrollbar.pack(side=tk.RIGHT, fill=tk.Y)
        self.log_text.configure(yscrollcommand=scrollbar.set)

        info = (
            "Role selects firmware env (esp_relay / esp_remote / esp_extender / esp_sensor). "
            "ID writes role-specific compile flags at build time. "
            "Farm List is used by Remote and Sensor (Sensor uses first farm as target). "
            "Mesh Node ID is assigned by painlessMesh at runtime."
        )
        ttk.Label(frame, text=info).pack(anchor="w", pady=(8, 0))
        ttk.Label(frame, textvariable=self.sync_status_var).pack(anchor="w", pady=(4, 0))
        self._auto_name_farm_id()
        self._update_lock_ui()
        self._on_role_change()

        self._build_device_list_tab(devices_tab)
        self._build_topology_tab(topology_tab)

    def _build_device_list_tab(self, parent: ttk.Frame) -> None:
        ttk.Label(parent, text="Saved ESP Device Catalog").pack(anchor="w")

        tree_frame = ttk.Frame(parent)
        tree_frame.pack(fill=tk.BOTH, expand=True, pady=(8, 0))

        cols = ("key", "role", "id", "farm", "port", "updated")
        tree = ttk.Treeview(tree_frame, columns=cols, show="headings", height=14)
        for col, width in (
            ("key", 220),
            ("role", 80),
            ("id", 140),
            ("farm", 140),
            ("port", 150),
            ("updated", 180),
        ):
            tree.heading(col, text=col.upper())
            tree.column(col, width=width, anchor="w")

        y_scroll = ttk.Scrollbar(tree_frame, orient="vertical", command=tree.yview)
        tree.configure(yscrollcommand=y_scroll.set)
        tree.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        y_scroll.pack(side=tk.RIGHT, fill=tk.Y)

        details = tk.Text(parent, height=9, wrap="word")
        details.pack(fill=tk.BOTH, expand=False, pady=(8, 0))

        actions = ttk.Frame(parent)
        actions.pack(fill=tk.X, pady=(8, 0))
        ttk.Button(actions, text="Refresh", command=self._populate_device_tree).pack(side=tk.LEFT)
        ttk.Button(actions, text="Delete Selected", command=self._delete_selected_catalog_item_tab).pack(
            side=tk.LEFT, padx=(8, 0)
        )

        self.device_tree = tree
        self.device_details_text = details
        self.device_tree.bind("<<TreeviewSelect>>", self._on_device_tree_select)
        self._populate_device_tree()

    def _build_topology_tab(self, parent: ttk.Frame) -> None:
        ttk.Label(parent, text="Mesh Topology (Remote -> Relay Farm)").pack(anchor="w")

        tree_frame = ttk.Frame(parent)
        tree_frame.pack(fill=tk.BOTH, expand=True, pady=(8, 0))

        cols = ("from", "to", "link", "status")
        tree = ttk.Treeview(tree_frame, columns=cols, show="headings", height=14)
        for col, width in (
            ("from", 220),
            ("to", 220),
            ("link", 180),
            ("status", 180),
        ):
            tree.heading(col, text=col.upper())
            tree.column(col, width=width, anchor="w")

        y_scroll = ttk.Scrollbar(tree_frame, orient="vertical", command=tree.yview)
        tree.configure(yscrollcommand=y_scroll.set)
        tree.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        y_scroll.pack(side=tk.RIGHT, fill=tk.Y)

        details = tk.Text(parent, height=9, wrap="word")
        details.pack(fill=tk.BOTH, expand=False, pady=(8, 0))

        actions = ttk.Frame(parent)
        actions.pack(fill=tk.X, pady=(8, 0))
        ttk.Button(actions, text="Refresh", command=self._populate_topology_tree).pack(side=tk.LEFT)

        self.topology_tree = tree
        self.topology_details_text = details
        self.topology_tree.bind("<<TreeviewSelect>>", self._on_topology_tree_select)
        self._populate_topology_tree()

    def _set_widget_enabled(self, widget: tk.Widget, enabled: bool) -> None:
        widget.configure(state=tk.NORMAL if enabled else tk.DISABLED)

    def _update_lock_ui(self) -> None:
        if self.role_locked:
            self.role_combo.configure(state=tk.DISABLED)
            self.env_entry.configure(state=tk.DISABLED)
            self.lock_btn.configure(text="Unlock Role/Env")
        else:
            self.role_combo.configure(state="readonly")
            self.env_entry.configure(state=tk.NORMAL)
            self.lock_btn.configure(text="Lock Role/Env")

    def _toggle_role_lock(self) -> None:
        self.role_locked = not self.role_locked
        if self.role_locked:
            role = self.role_var.get().strip()
            self.custom_env_var.set(ROLE_TO_ENV.get(role, "esp_relay"))
        self._update_lock_ui()

    def _profiles_default(self) -> dict[str, dict[str, str]]:
        return {
            "Relay / Pump": {"id": "FARM_101", "env": "", "remote_farm_list": ""},
            "Remote / Button": {"id": "REMOTE_101", "env": "", "remote_farm_list": ""},
            "Extender / Mesh": {"id": "EXTENDER_101", "env": "", "remote_farm_list": ""},
            "Sensor / Farm": {"id": "SENSOR_101", "env": "", "remote_farm_list": ""},
        }

    def _load_profiles(self) -> None:
        self.profiles = self._profiles_default()
        try:
            if self.profile_path.exists():
                data = json.loads(self.profile_path.read_text(encoding="utf-8"))
                if isinstance(data, dict):
                    for role in self.profiles:
                        src = data.get(role, {})
                        if isinstance(src, dict):
                            self.profiles[role].update(
                                {
                                    "id": str(src.get("id", self.profiles[role]["id"])),
                                    "env": str(src.get("env", "")),
                                    "remote_farm_list": str(src.get("remote_farm_list", "")),
                                }
                            )
        except Exception as err:
            self.log_queue.put(f"[WARN] Could not load profiles: {err}")

    def _save_profiles(self) -> None:
        role = self.role_var.get().strip()
        if role not in self.profiles:
            self.profiles[role] = {"id": "", "env": "", "remote_farm_list": ""}
        self.profiles[role]["id"] = self.id_var.get().strip()
        self.profiles[role]["env"] = self.custom_env_var.get().strip()
        self.profiles[role]["remote_farm_list"] = self.remote_farm_list_var.get().strip()
        try:
            self.profile_path.write_text(json.dumps(self.profiles, indent=2), encoding="utf-8")
        except Exception as err:
            self.log_queue.put(f"[WARN] Could not save profiles: {err}")

    def _load_profile_for_role(self, role: str) -> None:
        profile = self.profiles.get(role)
        if not profile:
            return
        if profile.get("id"):
            self.id_var.set(profile["id"])
        self.custom_env_var.set(profile.get("env", ""))
        self.remote_farm_list_var.set(profile.get("remote_farm_list", ""))
        self._sync_remote_tokens_from_csv()

    def _load_relay_catalog(self) -> None:
        self.relay_catalog = {}
        try:
            if self.relay_catalog_path.exists():
                data = json.loads(self.relay_catalog_path.read_text(encoding="utf-8"))
                if isinstance(data, dict):
                    self.relay_catalog = data
        except Exception as err:
            self.log_queue.put(f"[WARN] Could not load relay catalog: {err}")

    def _save_relay_catalog(self) -> None:
        try:
            self.relay_catalog_path.write_text(json.dumps(self.relay_catalog, indent=2), encoding="utf-8")
        except Exception as err:
            self.log_queue.put(f"[WARN] Could not save relay catalog: {err}")

    def _load_device_catalog(self) -> None:
        self.device_catalog = {}
        try:
            if self.device_catalog_path.exists():
                data = json.loads(self.device_catalog_path.read_text(encoding="utf-8"))
                if isinstance(data, dict):
                    self.device_catalog = data
                    return

            if self.relay_catalog:
                for farm_id, entry in self.relay_catalog.items():
                    if not isinstance(entry, dict):
                        continue
                    key = f"relay:{farm_id}"
                    self.device_catalog[key] = {
                        "role": "relay",
                        "id": farm_id,
                        "farmId": farm_id,
                        "port": entry.get("port", ""),
                        "relayDevices": entry.get("relayDevices", ""),
                        "devices": entry.get("devices", []),
                        "updatedAt": entry.get("updatedAt", datetime.now().isoformat(timespec="seconds")),
                    }
                self._save_device_catalog()
        except Exception as err:
            self.log_queue.put(f"[WARN] Could not load device catalog: {err}")

    def _save_device_catalog(self) -> None:
        try:
            self.device_catalog_path.write_text(json.dumps(self.device_catalog, indent=2), encoding="utf-8")
        except Exception as err:
            self.log_queue.put(f"[WARN] Could not save device catalog: {err}")

    def _upsert_device_catalog_from_probe(self, role: str, port: str, payload: dict[str, object]) -> None:
        now_iso = datetime.now().isoformat(timespec="seconds")

        if role == "relay":
            farm_id = str(payload.get("farmId", payload.get("id", ""))).strip()
            relay_devices_csv = str(payload.get("relayDevices", payload.get("list", ""))).strip()
            key = f"relay:{farm_id or port}"
            self.device_catalog[key] = {
                "role": "relay",
                "id": farm_id,
                "farmId": farm_id,
                "port": port,
                "syncVer": payload.get("syncVer", payload.get("protoVer", "")),
                "relayDevices": relay_devices_csv,
                "devices": self._parse_relay_devices_csv(relay_devices_csv),
                "updatedAt": now_iso,
            }
            self._populate_device_tree()
            self._populate_topology_tree()
            return

        if role == "remote":
            remote_id = str(payload.get("remoteId", payload.get("id", ""))).strip()
            farm_id = str(payload.get("farmId", "")).strip()
            remote_farms = str(payload.get("remoteFarmList", payload.get("list", ""))).strip()
            key = f"remote:{remote_id or port}"
            self.device_catalog[key] = {
                "role": "remote",
                "id": remote_id,
                "remoteId": remote_id,
                "farmId": farm_id,
                "port": port,
                "syncVer": payload.get("syncVer", payload.get("protoVer", "")),
                "remoteFarmList": remote_farms,
                "remoteFarms": [x.strip() for x in remote_farms.split(",") if x.strip()],
                "updatedAt": now_iso,
            }

            self._populate_device_tree()
            self._populate_topology_tree()
            return

        if role == "sensor":
            sensor_id = str(payload.get("sensorId", payload.get("id", ""))).strip()
            farm_id = str(payload.get("farmId", "")).strip()
            key = f"sensor:{sensor_id or port}"
            self.device_catalog[key] = {
                "role": "sensor",
                "id": sensor_id,
                "sensorId": sensor_id,
                "farmId": farm_id,
                "port": port,
                "syncVer": payload.get("syncVer", payload.get("protoVer", "")),
                "updatedAt": now_iso,
            }
            self._populate_device_tree()
            self._populate_topology_tree()
            return

        if role == "extender":
            extender_id = str(payload.get("extenderId", payload.get("id", ""))).strip()
            key = f"extender:{extender_id or port}"
            self.device_catalog[key] = {
                "role": "extender",
                "id": extender_id,
                "extenderId": extender_id,
                "farmId": "",
                "port": port,
                "syncVer": payload.get("syncVer", payload.get("protoVer", "")),
                "updatedAt": now_iso,
            }
            self._populate_device_tree()
            self._populate_topology_tree()

    def _collect_topology_edges(self) -> tuple[list[dict[str, str]], int, int, int, int]:
        relay_farms: set[str] = set()
        remote_nodes: list[dict[str, object]] = []
        sensor_nodes: list[dict[str, object]] = []
        extender_nodes: list[dict[str, object]] = []

        for entry in self.device_catalog.values():
            if not isinstance(entry, dict):
                continue
            role = str(entry.get("role", "")).strip().lower()
            if role == "relay":
                farm = str(entry.get("farmId", entry.get("id", ""))).strip()
                if farm:
                    relay_farms.add(farm)
            elif role == "remote":
                remote_nodes.append(entry)
            elif role == "sensor":
                sensor_nodes.append(entry)
            elif role == "extender":
                extender_nodes.append(entry)

        edges: list[dict[str, str]] = []
        for entry in remote_nodes:
            remote_id = str(entry.get("remoteId", entry.get("id", "UNKNOWN_REMOTE"))).strip() or "UNKNOWN_REMOTE"
            farms = entry.get("remoteFarms")
            farm_tokens: list[str] = []
            if isinstance(farms, list):
                farm_tokens = [str(x).strip() for x in farms if str(x).strip()]
            if not farm_tokens:
                csv = str(entry.get("remoteFarmList", "")).strip()
                if csv:
                    farm_tokens = [x.strip() for x in csv.split(",") if x.strip()]

            if not farm_tokens:
                edges.append({
                    "from": remote_id,
                    "to": "(none)",
                    "link": "configured-farm",
                    "status": "no targets",
                })
                continue

            for farm in farm_tokens:
                status = "connected-known" if farm in relay_farms else "relay-missing"
                edges.append(
                    {
                        "from": remote_id,
                        "to": farm,
                        "link": "configured-farm",
                        "status": status,
                    }
                )

        for entry in sensor_nodes:
            sensor_id = str(entry.get("sensorId", entry.get("id", "UNKNOWN_SENSOR"))).strip() or "UNKNOWN_SENSOR"
            farm = str(entry.get("farmId", "")).strip()
            if not farm:
                edges.append({
                    "from": sensor_id,
                    "to": "(none)",
                    "link": "sensor-farm",
                    "status": "no-farm",
                })
                continue

            status = "connected-known" if farm in relay_farms else "relay-missing"
            edges.append(
                {
                    "from": sensor_id,
                    "to": farm,
                    "link": "sensor-farm",
                    "status": status,
                }
            )

        for entry in extender_nodes:
            extender_id = str(entry.get("extenderId", entry.get("id", "UNKNOWN_EXTENDER"))).strip() or "UNKNOWN_EXTENDER"
            edges.append(
                {
                    "from": extender_id,
                    "to": "mesh-backbone",
                    "link": "mesh-extension",
                    "status": "routing",
                }
            )

        return edges, len(relay_farms), len(remote_nodes), len(sensor_nodes), len(extender_nodes)

    def _populate_topology_tree(self) -> None:
        if self.topology_tree is None:
            return
        for item in self.topology_tree.get_children():
            self.topology_tree.delete(item)

        edges, relay_count, remote_count, sensor_count, extender_count = self._collect_topology_edges()
        for edge in edges:
            self.topology_tree.insert(
                "",
                tk.END,
                values=(edge["from"], edge["to"], edge["link"], edge["status"]),
            )

        if self.topology_details_text is not None:
            self.topology_details_text.delete("1.0", tk.END)
            self.topology_details_text.insert(
                tk.END,
                (
                    f"Relays: {relay_count}\n"
                    f"Remotes: {remote_count}\n"
                    f"Sensors: {sensor_count}\n"
                    f"Extenders: {extender_count}\n"
                    f"Links: {len(edges)}\n"
                ),
            )

    def _on_topology_tree_select(self, _event: object | None = None) -> None:
        if self.topology_tree is None or self.topology_details_text is None:
            return
        selected = self.topology_tree.selection()
        if not selected:
            return
        row = self.topology_tree.item(selected[0])
        values = row.get("values", [])
        if len(values) < 4:
            return
        self.topology_details_text.delete("1.0", tk.END)
        self.topology_details_text.insert(
            tk.END,
            f"From: {values[0]}\nTo: {values[1]}\nLink: {values[2]}\nStatus: {values[3]}\n",
        )

    def _populate_device_tree(self) -> None:
        if self.device_tree is None:
            return
        for item in self.device_tree.get_children():
            self.device_tree.delete(item)
        for key in sorted(self.device_catalog.keys()):
            entry = self.device_catalog.get(key, {})
            role = str(entry.get("role", ""))
            node_id = str(entry.get("id", entry.get("remoteId", entry.get("farmId", ""))))
            farm = str(entry.get("farmId", ""))
            port = str(entry.get("port", ""))
            updated = str(entry.get("updatedAt", ""))
            self.device_tree.insert("", tk.END, values=(key, role, node_id, farm, port, updated))

    def _on_device_tree_select(self, _event: object | None = None) -> None:
        if self.device_tree is None or self.device_details_text is None:
            return
        selected = self.device_tree.selection()
        if not selected:
            return
        row = self.device_tree.item(selected[0])
        values = row.get("values", [])
        if not values:
            return
        key = str(values[0])
        entry = self.device_catalog.get(key, {})
        self.device_details_text.delete("1.0", tk.END)
        self.device_details_text.insert(tk.END, json.dumps(entry, indent=2))

    def _delete_selected_catalog_item_tab(self) -> None:
        if self.device_tree is None or self.device_details_text is None:
            return
        selected = self.device_tree.selection()
        if not selected:
            return
        row = self.device_tree.item(selected[0])
        values = row.get("values", [])
        if not values:
            return
        key = str(values[0])
        self.device_catalog.pop(key, None)
        self._save_device_catalog()
        self._populate_device_tree()
        self._populate_topology_tree()
        self.device_details_text.delete("1.0", tk.END)

    def _parse_relay_devices_csv(self, csv_text: str) -> list[dict[str, str]]:
        devices: list[dict[str, str]] = []
        if not csv_text.strip():
            return devices
        for chunk in csv_text.split(";"):
            part = chunk.strip()
            if not part:
                continue
            cols = [c.strip() for c in part.split(",")]
            if len(cols) >= 4:
                devices.append({"idx": cols[0], "id": cols[1], "gpio": cols[2], "ah": cols[3]})
            elif len(cols) == 3:
                devices.append({"idx": "-", "id": cols[0], "gpio": cols[1], "ah": cols[2]})
        return devices

    def _refresh_relay_catalog_ui(self) -> None:
        farms = sorted(self.relay_catalog.keys())
        self.known_relay_combo["values"] = farms
        if farms and self.known_relay_var.get() not in farms:
            self.known_relay_var.set(farms[0])
        self._on_known_relay_selected()

    def _on_known_relay_selected(self, _event: object | None = None) -> None:
        farm = self.known_relay_var.get().strip()
        entry = self.relay_catalog.get(farm)
        if not farm or not isinstance(entry, dict):
            self.relay_devices_preview_var.set("No relay catalog yet")
            return

        device_items = entry.get("devices", [])
        if not isinstance(device_items, list) or not device_items:
            self.relay_devices_preview_var.set(f"{farm}: no device details")
            return

        lines = [f"{farm}:"]
        for item in device_items[:8]:
            if not isinstance(item, dict):
                continue
            lines.append(
                f"- {item.get('id', '?')}  gpio={item.get('gpio', '?')}  ah={item.get('ah', '?')}"
            )
        self.relay_devices_preview_var.set("\n".join(lines))

    def _assign_known_relay_to_remote(self) -> None:
        farm = self.known_relay_var.get().strip()
        if not farm:
            self.log_queue.put("[WARN] Select a known relay farm first")
            return
        if farm not in self.remote_farm_tokens:
            self.remote_farm_tokens.append(farm)
            self._sync_remote_csv_from_tokens()
            self._render_remote_farm_tokens()
            self._save_profiles()
        self.log_queue.put(f"[OK] Added {farm} to Remote Farm List")

    def _render_remote_farm_tokens(self) -> None:
        for child in self.remote_chip_frame.winfo_children():
            child.destroy()

        if not self.remote_farm_tokens:
            ttk.Label(self.remote_chip_frame, text="(empty)").grid(row=0, column=0, sticky="w")
            return

        col = 0
        for idx, token in enumerate(self.remote_farm_tokens):
            btn = ttk.Button(
                self.remote_chip_frame,
                text=f"{token} ✕",
                command=lambda i=idx: self._remove_remote_farm_token(i),
                width=max(10, min(18, len(token) + 3)),
            )
            btn.grid(row=0, column=col, sticky="w", padx=(0, 6), pady=2)
            col += 1

    def _sync_remote_csv_from_tokens(self) -> None:
        self.remote_farm_list_var.set(",".join(self.remote_farm_tokens))

    def _sync_remote_tokens_from_csv(self) -> None:
        csv = self.remote_farm_list_var.get().strip()
        tokens: list[str] = []
        if csv:
            for item in csv.split(","):
                value = item.strip()
                if value and value not in tokens:
                    tokens.append(value)
        self.remote_farm_tokens = tokens
        self._render_remote_farm_tokens()

    def _add_remote_farm_token(self) -> None:
        token = self.new_remote_farm_var.get().strip()
        if not token:
            return
        if token not in self.remote_farm_tokens:
            self.remote_farm_tokens.append(token)
            self._sync_remote_csv_from_tokens()
            self._render_remote_farm_tokens()
            self._save_profiles()
        self.new_remote_farm_var.set("")

    def _remove_remote_farm_token(self, index: int) -> None:
        if 0 <= index < len(self.remote_farm_tokens):
            self.remote_farm_tokens.pop(index)
            self._sync_remote_csv_from_tokens()
            self._render_remote_farm_tokens()
            self._save_profiles()

    def _clear_remote_farm_tokens(self) -> None:
        self.remote_farm_tokens = []
        self._sync_remote_csv_from_tokens()
        self._render_remote_farm_tokens()
        self._save_profiles()

    def _update_sync_status(self, source: str, sync_ver: str | int | None) -> None:
        self.last_sync_source = source
        self.last_sync_ver = "-" if sync_ver is None else str(sync_ver)
        self.last_sync_time = datetime.now().strftime("%H:%M:%S")
        text = f"Sync: {self.last_sync_time}  source={self.last_sync_source}  syncVer={self.last_sync_ver}"
        if str(self.last_sync_ver) != str(GUI_SYNC_VER):
            text += f"  [WARN expected {GUI_SYNC_VER}]"
        self.sync_status_var.set(text)

    def _on_role_change(self, _event: object | None = None) -> None:
        self._save_profiles()
        role = self.role_var.get().strip()
        uses_farm_list = role in {"Remote / Button", "Sensor / Farm"}
        self._set_widget_enabled(self.remote_add_entry, uses_farm_list)
        if _event is not None:
            self._load_profile_for_role(role)
            if not self.id_var.get().strip():
                self._auto_name_farm_id()
        self._render_remote_farm_tokens()
        if self.role_locked:
            self.custom_env_var.set(ROLE_TO_ENV.get(role, "esp_relay"))

    def _extract_id_num(self, value: str, prefix: str) -> int | None:
        if prefix == "FARM_":
            pattern = r"FARM_(\d{1,3})"
        elif prefix == "REMOTE_":
            pattern = r"REMOTE_?(\d{1,3})"
        elif prefix == "EXTENDER_":
            pattern = r"EXTENDER_?(\d{1,3})"
        else:
            pattern = r"SENSOR_?(\d{1,3})"

        match = re.fullmatch(pattern, value.strip())
        if not match:
            return None
        return int(match.group(1))

    def _auto_name_farm_id(self) -> None:
        role = self.role_var.get().strip()
        if role == "Relay / Pump":
            prefix = "FARM_"
        elif role == "Remote / Button":
            prefix = "REMOTE_"
        elif role == "Extender / Mesh":
            prefix = "EXTENDER_"
        else:
            prefix = "SENSOR_"
        used_nums: set[int] = set()

        current_num = self._extract_id_num(self.id_var.get(), prefix)
        if current_num is not None:
            used_nums.add(current_num)

        if prefix == "FARM_":
            remote_farms = self.remote_farm_list_var.get().strip()
            if remote_farms:
                for token in remote_farms.split(","):
                    farm_num = self._extract_id_num(token, "FARM_")
                    if farm_num is not None:
                        used_nums.add(farm_num)

        next_num = 101
        if used_nums:
            max_num = max(used_nums)
            if max_num < 999:
                next_num = max_num + 1
            else:
                for candidate in range(100, 1000):
                    if candidate not in used_nums:
                        next_num = candidate
                        break

        self.id_var.set(f"{prefix}{next_num:03d}")

    def _append_log(self, line: str) -> None:
        self.log_text.insert(tk.END, line)
        if not line.endswith("\n"):
            self.log_text.insert(tk.END, "\n")
        self.log_text.see(tk.END)

    def _drain_log_queue(self) -> None:
        while True:
            try:
                msg = self.log_queue.get_nowait()
            except queue.Empty:
                break
            self._append_log(msg)
        self.after(80, self._drain_log_queue)

    def _run_capture(self, cmd: list[str]) -> tuple[int, str]:
        try:
            proc = subprocess.run(
                cmd,
                cwd=self.project_root,
                capture_output=True,
                text=True,
                check=False,
            )
            return proc.returncode, proc.stdout
        except FileNotFoundError as err:
            return 127, str(err)

    def _check_port_health(self) -> None:
        port = self.port_var.get().strip()
        if not port:
            self.status_var.set("Select a port")
            self.log_queue.put("[WARN] Select a serial port to check")
            return

        try:
            proc = subprocess.run(["lsof", "-t", port], capture_output=True, text=True, check=False)
            pids = [p.strip() for p in proc.stdout.splitlines() if p.strip().isdigit()]
            if not pids:
                self.log_queue.put(f"[OK] Port {port} appears free")
                self.status_var.set("Port free")
                return

            self.status_var.set("Port busy")
            self.log_queue.put(f"[WARN] Port {port} is busy")
            for pid in pids[:5]:
                ps = subprocess.run(["ps", "-p", pid, "-o", "pid=,comm="], capture_output=True, text=True, check=False)
                line = ps.stdout.strip() or pid
                self.log_queue.put(f"[WARN] holder: {line}")
            self.log_queue.put("[INFO] Close serial monitors and retry sync")
        except Exception as err:
            self.log_queue.put(f"[WARN] Port health check failed: {err}")

    def _validate_inputs(self, role: str, node_id: str, remote_csv: str) -> list[str]:
        errors: list[str] = []
        if not node_id:
            errors.append("ID is required")
            return errors

        if len(node_id) > MAX_ID_LEN:
            errors.append(f"ID too long (max {MAX_ID_LEN})")

        if role == "Relay / Pump":
            if not re.fullmatch(r"FARM_\d{3}", node_id):
                errors.append("Relay ID must match FARM_###")
        elif role == "Remote / Button":
            if not re.fullmatch(r"REMOTE_\d{3}", node_id):
                errors.append("Remote ID must match REMOTE_###")
        elif role == "Extender / Mesh":
            if not re.fullmatch(r"EXTENDER_\d{3}", node_id):
                errors.append("Extender ID must match EXTENDER_###")
        elif role == "Sensor / Farm":
            if not re.fullmatch(r"SENSOR_\d{3}", node_id):
                errors.append("Sensor ID must match SENSOR_###")

        if role == "Remote / Button" and remote_csv.strip():
            for token in [x.strip() for x in remote_csv.split(",") if x.strip()]:
                if not re.fullmatch(r"FARM_\d{3}", token):
                    errors.append(f"Invalid remote farm token: {token}")
                if len(token) > MAX_ID_LEN:
                    errors.append(f"Remote farm token too long: {token}")

        if role == "Sensor / Farm":
            tokens = [x.strip() for x in remote_csv.split(",") if x.strip()]
            if not tokens:
                errors.append("Sensor requires at least one target farm token")
            else:
                farm = tokens[0]
                if not re.fullmatch(r"FARM_\d{3}", farm):
                    errors.append(f"Invalid sensor farm token: {farm}")

        return errors

    def _write_config_to_esp(self) -> None:
        if self.probe_thread and self.probe_thread.is_alive():
            self.log_queue.put("[WARN] Probe/write already running")
            return

        port = self.port_var.get().strip()
        if not port:
            self.status_var.set("Select a port")
            self.log_queue.put("[WARN] Select a serial port before write")
            return

        role = self.role_var.get().strip()
        node_id = self.id_var.get().strip()
        remote_csv = self.remote_farm_list_var.get().strip()
        farm_tokens = [x.strip() for x in remote_csv.split(",") if x.strip()]
        sensor_target_farm = farm_tokens[0] if farm_tokens else ""
        errors = self._validate_inputs(role, node_id, remote_csv)
        if errors:
            for err in errors:
                self.log_queue.put(f"[ERR] {err}")
            self.status_var.set("Validation failed")
            return

        payload: dict[str, object] = {"syncVer": GUI_SYNC_VER, "protoVer": GUI_SYNC_VER}
        if role == "Relay / Pump":
            payload.update({"role": "relay", "farmId": node_id, "id": node_id})
        elif role == "Remote / Button":
            payload.update(
                {
                    "role": "remote",
                    "remoteId": node_id,
                    "id": node_id,
                    "remoteFarmList": remote_csv,
                    "list": remote_csv,
                }
            )
        elif role == "Extender / Mesh":
            payload.update({"role": "extender", "extenderId": node_id, "id": node_id})
        elif role == "Sensor / Farm":
            payload.update({"role": "sensor", "sensorId": node_id, "id": node_id, "farmId": sensor_target_farm})

        self.log_queue.put(f"[INFO] Writing config to {port}...")
        self.probe_thread = threading.Thread(target=self._write_cfg_worker, args=(port, payload), daemon=True)
        self.probe_thread.start()

    def _write_cfg_worker(self, port: str, payload: dict[str, object]) -> None:
        if serial is None:
            self.log_queue.put("[WARN] pyserial not installed; cannot write config")
            return

        try:
            with serial.Serial(port, 115200, timeout=0.6, write_timeout=1) as ser:  # type: ignore[arg-type]
                time.sleep(0.5)
                body = json.dumps(payload, separators=(",", ":"))
                cmd = f"AGRI_SET_CFG {body}\n".encode("utf-8")
                ser.reset_input_buffer()
                ser.reset_output_buffer()
                ser.write(cmd)
                ser.flush()

                ack: dict[str, object] | None = None
                deadline = time.time() + 3.2
                while time.time() < deadline:
                    raw = ser.readline()
                    if not raw:
                        continue
                    line = raw.decode(errors="ignore").strip()
                    if not line:
                        continue
                    try:
                        msg = json.loads(line)
                    except json.JSONDecodeError:
                        continue
                    if isinstance(msg, dict) and msg.get("setCfg"):
                        ack = msg
                        break

                if ack and str(ack.get("setCfg")) == "ok":
                    self.log_queue.put("[OK] Config write acknowledged")
                    self.after(0, lambda: self.status_var.set("Config written"))
                    self.after(200, lambda: self._sync_from_esp())
                else:
                    self.log_queue.put("[ERR] No AGRI_SET_CFG ack")
                    self.after(0, lambda: self.status_var.set("Config write failed"))
        except Exception as err:
            self.log_queue.put(f"[ERR] Config write failed: {err}")

    def _is_likely_esp_port(self, port: str) -> bool:
        p = port.lower()
        return ("ttyusb" in p) or ("ttyacm" in p) or ("usbserial" in p) or ("usbmodem" in p)

    def _refresh_ports(self) -> None:
        if not self.pio_exe:
            self.status_var.set("PlatformIO not found")
            self.log_queue.put("[ERR] PlatformIO executable not found.")
            self.log_queue.put("[ERR] Install PlatformIO or add it to PATH.")
            return

        cmd = [self.pio_exe, "device", "list", "--json-output"]
        code, out = self._run_capture(cmd)
        if code != 0:
            self.status_var.set("Port scan failed")
            self.log_queue.put("[ERR] Could not query serial ports via PlatformIO")
            if out:
                self.log_queue.put(out.strip())
            return

        try:
            devices = json.loads(out)
        except json.JSONDecodeError:
            self.status_var.set("Port parse failed")
            self.log_queue.put("[ERR] Invalid JSON from platformio device list")
            return

        ports = []
        for dev in devices:
            port = dev.get("port")
            if port:
                ports.append(port)

        ports.sort(key=lambda p: (0 if self._is_likely_esp_port(p) else 1, p))

        self.port_combo["values"] = ports
        if ports and self.port_var.get() not in ports:
            preferred = next((p for p in ports if self._is_likely_esp_port(p)), ports[0])
            self.port_var.set(preferred)
            self._probe_selected_port()
        self.status_var.set(f"Ports: {len(ports)}")

    def _on_port_selected(self, _event: object | None = None) -> None:
        self._probe_selected_port()

    def _sync_from_esp(self) -> None:
        port = self.port_var.get().strip()
        if not port:
            self.status_var.set("Select a port")
            self.log_queue.put("[WARN] Select a serial port before syncing")
            return
        self.log_queue.put(f"[INFO] Syncing config from {port} (probe, then reboot fallback)...")
        self._probe_selected_port(reboot_first=False)

    def _probe_selected_port(self, reboot_first: bool = False) -> None:
        if self.probe_thread and self.probe_thread.is_alive():
            self.log_queue.put("[INFO] Probe already running; wait for completion")
            return

        if self.flash_thread and self.flash_thread.is_alive():
            self.log_queue.put("[INFO] Flash in progress; probe deferred")
            return

        port = self.port_var.get().strip()
        if not port:
            return

        self.probe_thread = threading.Thread(target=self._probe_worker, args=(port, reboot_first), daemon=True)
        self.probe_thread.start()

    def _probe_worker(self, port: str, reboot_first: bool = False) -> None:
        if serial is None:
            self.log_queue.put("[WARN] pyserial not installed; skipping device probe")
            return

        if not self._is_likely_esp_port(port):
            self.log_queue.put(f"[INFO] Skipping non-USB serial port {port}")
            return

        try:
            with serial.Serial(port, 115200, timeout=0.45, write_timeout=1) as ser:  # type: ignore[arg-type]
                try:
                    ser.dtr = False
                    ser.rts = False
                except Exception:
                    pass

                time.sleep(0.9)

                if reboot_first:
                    self.log_queue.put(f"[INFO] Rebooting ESP on {port} for sync...")
                    try:
                        ser.setDTR(False)
                        ser.setRTS(True)
                        time.sleep(0.12)
                        ser.setRTS(False)
                        ser.setDTR(False)
                    except Exception:
                        pass
                    time.sleep(1.4)

                payload: dict[str, object] | None = None

                commands = (b"AGRI_GET_CFG\\n", b"AGRI_GET_CFG?\\n", b"AGRI_GET_CFG\\n")
                for idx, cmd in enumerate(commands, start=1):
                    self.log_queue.put(f"[INFO] Probe attempt {idx}/{len(commands)} on {port}")
                    ser.reset_input_buffer()
                    ser.reset_output_buffer()
                    ser.write(cmd)
                    ser.flush()

                    deadline = time.time() + (4.5 if reboot_first else 2.6)
                    saw_line = False
                    while time.time() < deadline:
                        raw = ser.readline()
                        if not raw:
                            continue
                        saw_line = True
                        line = raw.decode(errors="ignore").strip()
                        if not line:
                            continue

                        try:
                            payload = json.loads(line)
                        except json.JSONDecodeError:
                            start = line.find("{")
                            end = line.rfind("}")
                            if start != -1 and end != -1 and end > start:
                                try:
                                    payload = json.loads(line[start : end + 1])
                                except json.JSONDecodeError:
                                    payload = None

                        if isinstance(payload, dict) and payload.get("role") in {"relay", "remote", "extender", "sensor"}:
                            break

                    if not saw_line:
                        self.log_queue.put(f"[INFO] No serial response on attempt {idx}")

                    if isinstance(payload, dict) and payload.get("role") in {"relay", "remote", "extender", "sensor"}:
                        break

                if not payload:
                    if not reboot_first:
                        self.log_queue.put(f"[INFO] Probe failed on {port}; retrying with reboot...")
                        self.after(0, lambda: self._probe_selected_port(reboot_first=True))
                        return
                    self.log_queue.put(f"[INFO] Probe unsupported or no config reply from {port}")
                    return

                probe_source = "manual-reboot" if reboot_first else "auto-probe"
                self.after(0, lambda: self._apply_probe_payload(port, payload, probe_source))
        except Exception as err:
            self.log_queue.put(f"[INFO] Probe failed on {port}: {err}")

    def _apply_probe_payload(self, port: str, payload: dict[str, object], source: str) -> None:
        role = str(payload.get("role", "")).strip().lower()
        sync_ver = payload.get("syncVer", payload.get("protoVer"))
        self._update_sync_status(source, sync_ver)
        self._upsert_device_catalog_from_probe(role, port, payload)
        self._save_device_catalog()

        if role == "relay":
            farm_id = str(payload.get("farmId", payload.get("id", ""))).strip()
            relay_devices_csv = str(payload.get("relayDevices", payload.get("list", ""))).strip()
            self.id_var.set(farm_id)
            self.role_var.set("Relay / Pump")
            self.remote_farm_list_var.set("")
            self.remote_farm_tokens = []
            self._on_role_change()
            if farm_id:
                self.relay_catalog[farm_id] = {
                    "farmId": farm_id,
                    "port": port,
                    "relayDevices": relay_devices_csv,
                    "devices": self._parse_relay_devices_csv(relay_devices_csv),
                    "updatedAt": datetime.now().isoformat(timespec="seconds"),
                }
                self._save_relay_catalog()
                self._refresh_relay_catalog_ui()
            self.role_locked = True
            self.custom_env_var.set("esp_relay")
            self._update_lock_ui()
            self.status_var.set(f"Probed relay on {port}")
            self.log_queue.put(f"[OK] Probed relay config from {port}")
        elif role == "remote":
            remote_id = str(payload.get("remoteId", payload.get("id", ""))).strip()
            self.id_var.set(remote_id)
            self.role_var.set("Remote / Button")
            remote_farms = str(payload.get("remoteFarmList", payload.get("list", ""))).strip()
            self.remote_farm_list_var.set(remote_farms)
            self._sync_remote_tokens_from_csv()
            self._on_role_change()
            self.role_locked = True
            self.custom_env_var.set("esp_remote")
            self._update_lock_ui()
            self.status_var.set(f"Probed remote on {port}")
            self.log_queue.put(f"[OK] Probed remote config from {port}")
        elif role == "extender":
            extender_id = str(payload.get("extenderId", payload.get("id", ""))).strip()
            if extender_id:
                self.id_var.set(extender_id)
            self.role_var.set("Extender / Mesh")
            self.role_locked = True
            self.custom_env_var.set("esp_extender")
            self._update_lock_ui()
            self.status_var.set(f"Probed extender on {port}")
            self.log_queue.put(f"[OK] Probed extender config from {port}")
        elif role == "sensor":
            sensor_id = str(payload.get("sensorId", payload.get("id", ""))).strip()
            farm_id = str(payload.get("farmId", "")).strip()
            if sensor_id:
                self.id_var.set(sensor_id)
            self.role_var.set("Sensor / Farm")
            self.remote_farm_list_var.set(farm_id)
            self._sync_remote_tokens_from_csv()
            self.role_locked = True
            self.custom_env_var.set("esp_sensor")
            self._update_lock_ui()
            self.status_var.set(f"Probed sensor on {port}")
            self.log_queue.put(f"[OK] Probed sensor config from {port}")

        self._save_profiles()

    def _start_flash(self, upload: bool = True) -> None:
        if self.flash_thread and self.flash_thread.is_alive():
            self.log_queue.put("[WARN] Flash/build already running")
            return

        if not self.pio_exe:
            self.status_var.set("PlatformIO not found")
            self.log_queue.put("[ERR] PlatformIO executable not found.")
            return

        role = self.role_var.get().strip()
        env = self.custom_env_var.get().strip() or ROLE_TO_ENV.get(role, "esp_relay")
        role_value = ROLE_TO_VALUE.get(role, 0)
        farm_id = self.id_var.get().strip()
        remote_farm_list = self.remote_farm_list_var.get().strip()
        farm_tokens = [x.strip() for x in remote_farm_list.split(",") if x.strip()]
        sensor_target_farm = farm_tokens[0] if farm_tokens else ""
        port = self.port_var.get().strip()

        errors = self._validate_inputs(role, farm_id, remote_farm_list)
        if errors:
            self.status_var.set("Validation failed")
            for err in errors:
                self.log_queue.put(f"[ERR] {err}")
            return

        self._save_profiles()

        if not farm_id:
            self.status_var.set("ID required")
            self.log_queue.put("[ERR] Please enter ID (Farm ID)")
            return

        cmd = [self.pio_exe, "run", "-e", env]
        if upload:
            cmd.extend(["-t", "upload"])

        cmd.extend(["--project-dir", str(self.project_root)])

        if upload and port:
            cmd.extend(["--upload-port", port])

        escaped_farm_id = farm_id.replace("\\", "\\\\").replace('"', '\\"')
        extra_build_flags = f"-DAGRI_NODE_ROLE={role_value}"
        if role == "Remote / Button":
            extra_build_flags += f" -DAGRI_REMOTE_ID=\\\"{escaped_farm_id}\\\""
        elif role == "Relay / Pump":
            extra_build_flags += f" -DAGRI_FARM_ID=\\\"{escaped_farm_id}\\\""
        elif role == "Sensor / Farm":
            extra_build_flags += f" -DAGRI_SENSOR_ID=\\\"{escaped_farm_id}\\\""
            if sensor_target_farm:
                escaped_sensor_farm = sensor_target_farm.replace("\\", "\\\\").replace('"', '\\"')
                extra_build_flags += f" -DAGRI_FARM_ID=\\\"{escaped_sensor_farm}\\\""
        if role == "Remote / Button" and remote_farm_list:
            escaped_remote_farm_list = remote_farm_list.replace("\\", "\\\\").replace('"', '\\"')
            extra_build_flags += f" -DAGRI_REMOTE_FARM_LIST_CSV=\\\"{escaped_remote_farm_list}\\\""

        run_env = dict(os.environ)
        existing_build_flags = run_env.get("PLATFORMIO_BUILD_FLAGS", "").strip()
        run_env["PLATFORMIO_BUILD_FLAGS"] = (
            f"{existing_build_flags} {extra_build_flags}".strip()
        )

        self.flash_btn.configure(state=tk.DISABLED)
        self.status_var.set("Running...")
        self.log_queue.put("\n=== START ===")
        self.log_queue.put(f"$ export PLATFORMIO_BUILD_FLAGS={run_env['PLATFORMIO_BUILD_FLAGS']}")
        self.log_queue.put("$ " + " ".join(shlex.quote(x) for x in cmd))

        self.flash_thread = threading.Thread(target=self._flash_worker, args=(cmd, run_env), daemon=True)
        self.flash_thread.start()

    def _flash_worker(self, cmd: list[str], run_env: dict[str, str]) -> None:
        proc = subprocess.Popen(
            cmd,
            cwd=self.project_root,
            env=run_env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
        )

        assert proc.stdout is not None
        for line in proc.stdout:
            self.log_queue.put(line.rstrip("\n"))

        rc = proc.wait()
        if rc == 0:
            self.log_queue.put("=== DONE (success) ===")
            self.after(0, lambda: self.status_var.set("Success"))
            self.after(1300, self._probe_selected_port)
        else:
            self.log_queue.put(f"=== DONE (failed: exit {rc}) ===")
            self.after(0, lambda: self.status_var.set(f"Failed ({rc})"))

        self.after(0, lambda: self.flash_btn.configure(state=tk.NORMAL))


def main() -> None:
    app = EspFlashGui()
    try:
        app.mainloop()
    except KeyboardInterrupt:
        try:
            app.destroy()
        except tk.TclError:
            pass


if __name__ == "__main__":
    main()
