/*******************************************************************************
 * ESP-Agri Transport Abstraction — Global API Implementation
 ******************************************************************************/
#include "agri_transport.h"

static AgriTransport* s_transport = nullptr;

void agri_transport_init(AgriTransport* impl) {
    s_transport = impl;
    Serial.println("[TRANSPORT] Implementation registered");
}

bool agri_send(uint32_t dest, const AgriMessage& msg) {
    if (!s_transport) {
        Serial.println("[TRANSPORT] ERROR: No implementation registered");
        return false;
    }
    return s_transport->send(dest, msg);
}

bool agri_broadcast(const AgriMessage& msg) {
    if (!s_transport) {
        Serial.println("[TRANSPORT] ERROR: No implementation registered");
        return false;
    }
    return s_transport->broadcast(msg);
}

void agri_on_receive(agri_receive_cb_t cb) {
    if (s_transport) s_transport->onReceive(cb);
}

void agri_on_connection_change(agri_connection_cb_t cb) {
    if (s_transport) s_transport->onConnectionChange(cb);
}

void agri_update() {
    if (s_transport) s_transport->update();
}

bool agri_is_connected() {
    return s_transport ? s_transport->isConnected() : false;
}

uint32_t agri_get_node_id() {
    return s_transport ? s_transport->getNodeId() : 0;
}

uint16_t agri_get_node_count() {
    return s_transport ? s_transport->getNodeCount() : 0;
}

int8_t agri_get_rssi() {
    return s_transport ? s_transport->getRSSI() : 0;
}
