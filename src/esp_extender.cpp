#include <Arduino.h>
#include "agri_config.h"
#include "agri_protocol.h"
#include "agri_transport.h"
#include "agri_mesh_wifi.h"
#include "agri_nvs.h"

static AgriMeshWifi g_mesh;
static uint32_t g_lastLogMs = 0;
static char g_extenderId[AGRI_DEVICE_ID_LEN] = "EXTENDER_01";

static void serial_print_cfg() {
    String json = "{\"role\":\"extender\",\"id\":\"";
    json += g_extenderId;
    json += "\",\"extenderId\":\"";
    json += g_extenderId;
    json += "\",\"syncVer\":2,\"protoVer\":2}";
    Serial.println(json);
}

static bool serial_apply_cfg(const String& jsonText) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, jsonText);
    if (err) return false;

    const char* extenderId = doc["extenderId"] | doc["id"] | "";
    if (extenderId[0]) {
        strlcpy(g_extenderId, extenderId, sizeof(g_extenderId));
        agri_nvs_save_extender_id(g_extenderId);
    }

    Serial.println("{\"setCfg\":\"ok\",\"role\":\"extender\",\"syncVer\":2,\"protoVer\":2}");
    serial_print_cfg();
    return true;
}

static void handle_serial_cli() {
    if (!Serial.available()) return;

    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (!cmd.length()) return;

    if (cmd == "AGRI_GET_CFG" || cmd == "AGRI_GET_CFG?") {
        serial_print_cfg();
        return;
    }

    if (cmd.startsWith("AGRI_SET_CFG ")) {
        String body = cmd.substring(strlen("AGRI_SET_CFG "));
        if (!serial_apply_cfg(body)) {
            Serial.println("{\"setCfg\":\"error\",\"reason\":\"bad_json\",\"role\":\"extender\",\"syncVer\":2}");
        }
        return;
    }

    if (cmd == "AGRI_HELP") {
        Serial.println("AGRI_GET_CFG");
        Serial.println("AGRI_SET_CFG {json}");
    }
}

static void on_message_received(uint32_t from, const AgriMessage& msg) {
    Serial.printf(
        "[EXT] Msg from %u cmd=%s farm=%s dev=%s mid=%u\n",
        from,
        agri_command_name(msg.command),
        msg.farmId,
        msg.deviceId,
        msg.messageId
    );
}

static void on_connection_change(bool connected, uint16_t nodeCount) {
    Serial.printf("[EXT] Mesh %s — peers=%u\n", connected ? "connected" : "disconnected", nodeCount);
}

void setup() {
    Serial.begin(115200);
    delay(300);

    Serial.println("\n========================================");
    Serial.println("  ESP-Agri Extender Node");
    Serial.println("  Purpose: Mesh coverage extension only");
    Serial.println("========================================\n");

    agri_nvs_init();
    agri_nvs_load_extender_id(g_extenderId, sizeof(g_extenderId));

    agri_transport_init(&g_mesh);
    g_mesh.init();

    agri_on_receive(on_message_received);
    agri_on_connection_change(on_connection_change);

    Serial.printf("[EXT] Setup complete — Node ID: %u\n", agri_get_node_id());
    serial_print_cfg();
}

void loop() {
    agri_update();
    handle_serial_cli();

    uint32_t now = millis();
    if ((now - g_lastLogMs) >= 10000) {
        g_lastLogMs = now;
        Serial.printf(
            "[EXT] Alive node=%u id=%s connected=%d peers=%u rssi=%d\n",
            agri_get_node_id(),
            g_extenderId,
            agri_is_connected() ? 1 : 0,
            agri_get_node_count(),
            agri_get_rssi()
        );
    }
}
