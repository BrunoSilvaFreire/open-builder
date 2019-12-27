#include "server.h"

#include "../server_config.h"
#include <SFML/System/Clock.hpp>
#include <algorithm>
#include <common/debug.h>
#include <thread>

#include "../world/terrain_generation.h"

Server::Server()
    : NetworkHost("Server")
{
    // Create "spawn"
    m_spawn = &m_chunkManager.addChunk({0, 0, 0});
    makeFlatTerrain(m_spawn);
}

void Server::sendChunk(peer_id_t peerId, const Chunk &chunk)
{
    if (!m_connectedClients[peerId].connected) {
        return;
    }
    // Create the chunk-data packet
    sf::Packet packet;
    packet << ClientCommand::ChunkData << chunk.getPosition().x
           << chunk.getPosition().y << chunk.getPosition().z;

    for (auto &block : chunk.blocks) {
        packet << block;
    }
    packet.append(chunk.blocks.data(),
                  chunk.blocks.size() * sizeof(chunk.blocks[0]));

    // Send chunk data to client
    sendToPeer(m_connectedClients[peerId].peer, packet, 1,
               ENET_PACKET_FLAG_RELIABLE);
}

void Server::onPeerConnect(ENetPeer *peer)
{
    int slot = findEmptySlot();
    if (slot >= 0) {
        peer_id_t id = static_cast<peer_id_t>(slot);

        // Send client back their id
        sf::Packet packet;
        packet << ClientCommand::PeerId << id;
        NetworkHost::sendToPeer(peer, packet, 0, ENET_PACKET_FLAG_RELIABLE);

        // Broadcast the connection event
        sf::Packet announcement;
        announcement << ClientCommand::PlayerJoin << id;
        broadcastToPeers(announcement, 0, ENET_PACKET_FLAG_RELIABLE);

        addPeer(peer, id);

        // Send the inital world to the client
        for (int cy = 0; cy < TEMP_WORLD_HEIGHT; cy++) {
            for (int cz = 0; cz < TEMP_WORLD_WIDTH; cz++) {
                for (int cx = 0; cx < TEMP_WORLD_WIDTH; cx++) {
                    Chunk &chunk = m_chunkManager.addChunk({cx, cy, cz});
                    makeFlatTerrain(&chunk);

                    // Create the chunk-data packet
                    sendChunk(id, chunk);
                }
            }
        }
    }
}

void Server::onPeerDisconnect(ENetPeer *peer)
{
    removePeer(peer->connectID);
}

void Server::onPeerTimeout(ENetPeer *peer)
{
    removePeer(peer->connectID);
}

void Server::onCommandRecieve([[maybe_unused]] ENetPeer *peer,
                              sf::Packet &packet, command_t command)
{
    switch (static_cast<ServerCommand>(command)) {
        case ServerCommand::PlayerPosition:
            handleCommandPlayerPosition(packet);
            break;
    }
}

void Server::handleCommandPlayerPosition(sf::Packet &packet)
{
    peer_id_t id;
    packet >> id;
    packet >> m_entities[id].x >> m_entities[id].y >> m_entities[id].z;
}

void Server::sendPackets()
{
    // Player positions
    {
        sf::Packet packet;
        u16 count = static_cast<u16>(NetworkHost::getConnectedPeerCount());
        packet << ClientCommand::Snapshot << count;
        for (int i = 0; i < NetworkHost::getMaxConnections(); i++) {
            if (m_connectedClients[i].connected) {
                packet << static_cast<peer_id_t>(i) << m_entities[i].x
                       << m_entities[i].y << m_entities[i].z;
            }
        }
        broadcastToPeers(packet, 0, 0);
    }
}

int Server::findEmptySlot() const
{
    for (int i = 0; i < NetworkHost::getMaxConnections(); i++) {
        if (!m_connectedClients[i].connected) {
            return i;
        }
    }
    return -1;
}

void Server::addPeer(ENetPeer *peer, peer_id_t id)
{
    LOGVAR("Server", "New Peer, Peer Id:", (int)id);
    m_connectedClients[id].peer = peer;
    m_connectedClients[id].connected = true;
    m_connectedClients[id].entityId = id;
}

void Server::removePeer(u32 connectionId)
{
    auto itr = std::find_if(
        m_connectedClients.begin(), m_connectedClients.end(),
        [this, &connectionId](auto &conn) {
            return conn.peer && conn.peer->connectID == connectionId;
        });

    assert(itr != m_connectedClients.end());
    if (itr != m_connectedClients.end()) {
        LOGVAR("Server", "Client disconnected, Peer Id:", (int)itr->entityId);
        m_entities[itr->entityId].active = false;
        itr->connected = false;
        itr->peer = nullptr;

        // Broadcast the disconnection event
        sf::Packet announcement;
        announcement << ClientCommand::PlayerLeave << itr->entityId;
        broadcastToPeers(announcement, 0, ENET_PACKET_FLAG_RELIABLE);

        itr->entityId = 0;
    }
}
