/*******************************************************************************
 * ESP-Agri Radio Layer — ESP-MESH (Wi-Fi) Implementation
 ******************************************************************************/
#include "agri_mesh_wifi.h"
#include <WiFi.h>

// ============================================================================
// Constructor / Destructor
// ============================================================================
AgriMeshWifi::AgriMeshWifi()
    : _receiveCb(nullptr)
    , _connectionCb(nullptr)
    , _nodeCount(0)
{}

AgriMeshWifi::~AgriMeshWifi() {}

// ============================================================================
// Lifecycle
// ============================================================================
bool AgriMeshWifi::init() {
    Serial.println("[MESH-WIFI] Initializing ESP-MESH...");

    _mesh.setDebugMsgTypes(ERROR | STARTUP | CONNECTION);

    _mesh.init(AGRI_MESH_SSID, AGRI_MESH_PASSWORD,
               AGRI_MESH_PORT, WIFI_AP_STA, AGRI_MESH_CHANNEL);

    // --- Bind mesh callbacks via lambdas ---
    _mesh.onReceive([this](uint32_t from, String& msg) {
        _handleReceive(from, msg);
    });

    _mesh.onNewConnection([this](uint32_t nodeId) {
        _handleNewConnection(nodeId);
    });

    _mesh.onDroppedConnection([this](uint32_t nodeId) {
        _handleDroppedConnection(nodeId);
    });

    _mesh.onChangedConnections([this]() {
        _handleChangedConnections();
    });

    Serial.printf("[MESH-WIFI] Node ID : %u\n", _mesh.getNodeId());
    Serial.printf("[MESH-WIFI] SSID    : %s\n", AGRI_MESH_SSID);
    Serial.printf("[MESH-WIFI] Channel : %d\n", AGRI_MESH_CHANNEL);
    Serial.printf("[MESH-WIFI] Port    : %d\n", AGRI_MESH_PORT);
    Serial.println("[MESH-WIFI] Ready — waiting for peers...");
    return true;
}

void AgriMeshWifi::update() {
    _mesh.update();
}

// ============================================================================
// Messaging
// ============================================================================
bool AgriMeshWifi::send(uint32_t destId, const AgriMessage& msg) {
    String payload = agri_serialize(msg);

    if ((int)payload.length() > AGRI_MSG_MAX_SIZE) {
        Serial.printf("[MESH-WIFI] WARN: payload %d B > %d limit\n",
                      payload.length(), AGRI_MSG_MAX_SIZE);
    }

    bool ok = _mesh.sendSingle(destId, payload);
    Serial.printf("[MESH-WIFI] >> Send to %u  cmd=%s  mid=%u  [%s]\n",
                  destId, agri_command_name(msg.command), msg.messageId,
                  ok ? "OK" : "FAIL");
    return ok;
}

bool AgriMeshWifi::broadcast(const AgriMessage& msg) {
    String payload = agri_serialize(msg);

    if ((int)payload.length() > AGRI_MSG_MAX_SIZE) {
        Serial.printf("[MESH-WIFI] WARN: payload %d B > %d limit\n",
                      payload.length(), AGRI_MSG_MAX_SIZE);
    }

    bool ok = _mesh.sendBroadcast(payload);
    Serial.printf("[MESH-WIFI] >> Broadcast  cmd=%s  mid=%u  [%s]\n",
                  agri_command_name(msg.command), msg.messageId,
                  ok ? "OK" : "FAIL");
    return ok;
}

// ============================================================================
// Callbacks
// ============================================================================
void AgriMeshWifi::onReceive(agri_receive_cb_t cb) {
    _receiveCb = cb;
}

void AgriMeshWifi::onConnectionChange(agri_connection_cb_t cb) {
    _connectionCb = cb;
}

// ============================================================================
// Status
// ============================================================================
bool AgriMeshWifi::isConnected() {
    return _nodeCount > 0;
}

uint32_t AgriMeshWifi::getNodeId() {
    return _mesh.getNodeId();
}

uint16_t AgriMeshWifi::getNodeCount() {
    return _nodeCount;
}

int8_t AgriMeshWifi::getRSSI() {
    if (_nodeCount == 0) return 0;   // no peers — no meaningful RSSI
    return (int8_t)WiFi.RSSI();      // STA link RSSI to mesh parent
}

// ============================================================================
// Internal Handlers
// ============================================================================
void AgriMeshWifi::_handleReceive(uint32_t from, String& msg) {
    Serial.printf("[MESH-WIFI] << Recv from %u: %s\n", from, msg.c_str());

    if (_receiveCb) {
        AgriMessage parsed;
        if (agri_deserialize(msg, parsed)) {
            parsed.sourceNodeId = from;
            _receiveCb(from, parsed);
        } else {
            Serial.println("[MESH-WIFI] Failed to parse incoming message");
        }
    }
}

void AgriMeshWifi::_handleNewConnection(uint32_t nodeId) {
    Serial.printf("[MESH-WIFI] + Peer joined : %u\n", nodeId);
    _refreshNodeCount();
}

void AgriMeshWifi::_handleDroppedConnection(uint32_t nodeId) {
    Serial.printf("[MESH-WIFI] - Peer dropped: %u\n", nodeId);
    _refreshNodeCount();
}

void AgriMeshWifi::_handleChangedConnections() {
    Serial.println("[MESH-WIFI] ~ Topology changed");
    _refreshNodeCount();
}

void AgriMeshWifi::_refreshNodeCount() {
    auto nodes = _mesh.getNodeList();
    _nodeCount = (uint16_t)nodes.size();

    Serial.printf("[MESH-WIFI] Mesh nodes: %d (self + %d peers)\n",
                  _nodeCount + 1, _nodeCount);

    if (_connectionCb) {
        _connectionCb(_nodeCount > 0, _nodeCount);
    }
}
