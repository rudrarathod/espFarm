/*******************************************************************************
 * ESP-Agri Transport Abstraction Layer
 *
 * Provides a radio-agnostic API for sending and receiving AgriMessages.
 * Application code must use ONLY the global free functions below.
 * The concrete radio implementation (ESP-MESH, Meshtastic, etc.) is injected
 * at startup via agri_transport_init().
 *
 * ┌─────────────────────────────────────────┐
 * │         Application Layer               │  ← uses agri_send / agri_broadcast
 * ├─────────────────────────────────────────┤
 * │     Transport Abstraction (this file)   │
 * ├─────────────────────────────────────────┤
 * │  Radio Layer: ESP-MESH │ Meshtastic     │  ← implements AgriTransport
 * └─────────────────────────────────────────┘
 ******************************************************************************/
#ifndef AGRI_TRANSPORT_H
#define AGRI_TRANSPORT_H

#include <Arduino.h>
#include "agri_protocol.h"

// ============================================================================
// Callback Types
// ============================================================================
typedef void (*agri_receive_cb_t)(uint32_t from, const AgriMessage& msg);
typedef void (*agri_connection_cb_t)(bool connected, uint16_t nodeCount);

// ============================================================================
// Abstract Transport Interface  (implemented by each radio layer)
// ============================================================================
class AgriTransport {
public:
    virtual ~AgriTransport() {}

    // Lifecycle
    virtual bool     init()   = 0;
    virtual void     update() = 0;

    // Messaging
    virtual bool     send(uint32_t destId, const AgriMessage& msg)  = 0;
    virtual bool     broadcast(const AgriMessage& msg)              = 0;

    // Callbacks
    virtual void     onReceive(agri_receive_cb_t cb)                = 0;
    virtual void     onConnectionChange(agri_connection_cb_t cb)    = 0;

    // Status
    virtual bool     isConnected()  = 0;
    virtual uint32_t getNodeId()    = 0;
    virtual uint16_t getNodeCount() = 0;
    virtual int8_t   getRSSI()      = 0;   // Station RSSI in dBm (0 if N/A)
};

// ============================================================================
// Global Transport API — Application code uses ONLY these
// ============================================================================

/// Register the concrete transport implementation (call once in setup)
void     agri_transport_init(AgriTransport* impl);

/// Send a message to a specific node
bool     agri_send(uint32_t dest, const AgriMessage& msg);

/// Broadcast a message to all mesh nodes
bool     agri_broadcast(const AgriMessage& msg);

/// Register a callback for incoming messages
void     agri_on_receive(agri_receive_cb_t cb);

/// Register a callback for connection state changes
void     agri_on_connection_change(agri_connection_cb_t cb);

/// Must be called in loop() — drives the radio layer
void     agri_update();

/// Check if at least one peer is connected
bool     agri_is_connected();

/// Get this node's unique mesh ID
uint32_t agri_get_node_id();

/// Get the number of peer nodes currently in the mesh
uint16_t agri_get_node_count();

/// Get the station RSSI in dBm (0 if not available)
int8_t   agri_get_rssi();

#endif // AGRI_TRANSPORT_H
