#include <Arduino.h>
#include "agri_config.h"
#include "agri_protocol.h"
#include "agri_transport.h"
#include "agri_mesh_wifi.h"
#include "agri_nvs.h"

static AgriMeshWifi g_mesh;
static uint32_t g_lastReportMs = 0;
static char g_sensorId[AGRI_DEVICE_ID_LEN] = AGRI_SENSOR_ID;
static char g_sensorFarm[AGRI_FARM_ID_LEN] = AGRI_FARM_ID;

static void serial_print_cfg() {
    String json = "{\"role\":\"sensor\",\"farmId\":\"";
    json += g_sensorFarm;
    json += "\",\"id\":\"";
    json += g_sensorId;
    json += "\",\"sensorId\":\"";
    json += g_sensorId;
    json += "\",\"syncVer\":2,\"protoVer\":2}";
    Serial.println(json);
}

static bool serial_apply_cfg(const String& jsonText) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, jsonText);
    if (err) return false;

    const char* sensorId = doc["sensorId"] | doc["id"] | "";
    const char* farmId = doc["farmId"] | "";

    if (sensorId[0]) {
        strlcpy(g_sensorId, sensorId, sizeof(g_sensorId));
        agri_nvs_save_sensor_id(g_sensorId);
    }

    if (farmId[0]) {
        strlcpy(g_sensorFarm, farmId, sizeof(g_sensorFarm));
        agri_set_runtime_farm_id(g_sensorFarm);
        agri_nvs_save_farm(g_sensorFarm);
    }

    Serial.println("{\"setCfg\":\"ok\",\"role\":\"sensor\",\"syncVer\":2,\"protoVer\":2}");
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
            Serial.println("{\"setCfg\":\"error\",\"reason\":\"bad_json\",\"role\":\"sensor\",\"syncVer\":2}");
        }
        return;
    }

    if (cmd == "AGRI_HELP") {
        Serial.println("AGRI_GET_CFG");
        Serial.println("AGRI_SET_CFG {json}");
    }
}

static void on_connection_change(bool connected, uint16_t nodeCount) {
    Serial.printf("[SENSOR] Mesh %s — peers=%u\n", connected ? "connected" : "disconnected", nodeCount);
}

static void publish_sensor_telemetry() {
    int raw = analogRead(AGRI_SENSOR_PIN);
    if (raw < 0) raw = 0;
    if (raw > 4095) raw = 4095;

    uint8_t level = (uint8_t)((raw * 100) / 4095);

    AgriMessage msg;
    msg.clear();
    strlcpy(msg.farmId, g_sensorFarm, sizeof(msg.farmId));
    strlcpy(msg.deviceId, "SENSOR_SOIL", sizeof(msg.deviceId));
    strlcpy(msg.senderId, g_sensorId, sizeof(msg.senderId));
    msg.command = CMD_HEARTBEAT;
    msg.messageId = agri_next_message_id();
    msg.timestamp = millis();
    msg.nonce = agri_random_nonce();
    msg.devState = level;      // 0..100 scaled sensor level
    msg.sourceNodeId = agri_get_node_id();

    bool ok = agri_broadcast(msg);
    Serial.printf(
        "[SENSOR] TX %s farm=%s id=%s raw=%d level=%u%% mid=%u\n",
        ok ? "OK" : "FAIL",
        g_sensorFarm,
        g_sensorId,
        raw,
        level,
        msg.messageId
    );
}

void setup() {
    Serial.begin(115200);
    delay(300);

    Serial.println("\n========================================");
    Serial.println("  ESP-Agri Sensor Node");
    agri_nvs_init();
    agri_set_runtime_farm_id(g_sensorFarm);
    agri_nvs_load_sensor_id(g_sensorId, sizeof(g_sensorId));
    if (agri_nvs_load_farm(g_sensorFarm, sizeof(g_sensorFarm))) {
        agri_set_runtime_farm_id(g_sensorFarm);
    }

    Serial.printf("  Farm: %s\n", g_sensorFarm);
    Serial.printf("  Sensor ID: %s\n", g_sensorId);
    Serial.println("========================================\n");

    analogReadResolution(12);
    pinMode(AGRI_SENSOR_PIN, INPUT);

    agri_transport_init(&g_mesh);
    g_mesh.init();
    agri_on_connection_change(on_connection_change);

    randomSeed(analogRead(AGRI_SENSOR_PIN) ^ millis());

    Serial.printf("[SENSOR] Setup complete — Node ID: %u\n", agri_get_node_id());
    serial_print_cfg();
}

void loop() {
    agri_update();
    handle_serial_cli();

    uint32_t now = millis();
    if ((now - g_lastReportMs) >= AGRI_SENSOR_REPORT_MS) {
        g_lastReportMs = now;
        publish_sensor_telemetry();
    }
}
