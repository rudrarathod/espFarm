/*******************************************************************************
 * ESP-Agri Radio Layer — ESP-MESH (Wi-Fi) via painlessMesh
 *
 * This is the POC radio implementation.  It wraps painlessMesh and implements
 * the AgriTransport interface.  To switch to Meshtastic (LoRa), create a new
 * class that implements AgriTransport and swap it in setup().
 ******************************************************************************/
#ifndef AGRI_MESH_WIFI_H
#define AGRI_MESH_WIFI_H

#include "agri_transport.h"
#include <painlessMesh.h>

class AgriMeshWifi : public AgriTransport {
public:
    AgriMeshWifi();
    ~AgriMeshWifi() override;

    // AgriTransport interface
    bool     init() override;
    void     update() override;
    bool     send(uint32_t destId, const AgriMessage& msg) override;
    bool     broadcast(const AgriMessage& msg) override;
    void     onReceive(agri_receive_cb_t cb) override;
    void     onConnectionChange(agri_connection_cb_t cb) override;
    bool     isConnected() override;
    uint32_t getNodeId() override;
    uint16_t getNodeCount() override;
    int8_t   getRSSI() override;

private:
    painlessMesh         _mesh;
    agri_receive_cb_t    _receiveCb;
    agri_connection_cb_t _connectionCb;
    uint16_t             _nodeCount;

    void _handleReceive(uint32_t from, String& msg);
    void _handleNewConnection(uint32_t nodeId);
    void _handleDroppedConnection(uint32_t nodeId);
    void _handleChangedConnections();
    void _refreshNodeCount();
};

#endif // AGRI_MESH_WIFI_H
