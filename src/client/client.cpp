#include "client.h"

#include "gl/primitive.h"
#include "input/keyboard.h"
#include "world/chunk_mesh_generation.h"
#include <SFML/Window/Mouse.hpp>
#include <common/debug.h>
#include <common/network/net_command.h>
#include <common/network/net_constants.h>
#include <thread>

Client::Client()
    : NetworkHost("Client")
{
}

bool Client::init(float aspect)
{
    // OpenGL stuff
    m_cube = makeCubeVertexArray(1, 2, 1);

    // Basic shader
    m_basicShader.program.create("static", "static");
    m_basicShader.program.bind();
    m_basicShader.modelLocation =
        m_basicShader.program.getUniformLocation("modelMatrix");
    m_basicShader.projectionViewLocation =
        m_basicShader.program.getUniformLocation("projectionViewMatrix");

    // Chunk shader
    m_chunkShader.program.create("chunk", "chunk");
    m_chunkShader.program.bind();
    m_chunkShader.projectionViewLocation =
        m_chunkShader.program.getUniformLocation("projectionViewMatrix");

    // Texture for the player model
    m_texture.create("player");
    m_texture.bind();

    // Texture for grass
    m_grassTexture.create("grass");
    m_grassTexture.bind();

    // Set up the server connection
    auto peer = NetworkHost::createAsClient(LOCAL_HOST);
    if (!peer) {
        return false;
    }
    mp_serverPeer = *peer;

    // Set player stuff
    mp_player = &m_entities[NetworkHost::getPeerId()];
    mp_player->position = {CHUNK_SIZE * 2, CHUNK_SIZE * 2 + 1, CHUNK_SIZE * 2};

    /*
        // Get world from server
        for (int cy = 0; cy < TEMP_WORLD_HEIGHT; cy++) {
            for (int cz = 0; cz < TEMP_WORLD_WIDTH; cz++) {
                for (int cx = 0; cx < TEMP_WORLD_WIDTH; cx++) {
                    ChunkPosition position(cx, cy, cz);
                    sendChunkRequest({cx, cy, cz});
                }
            }
        }
    */
    m_projectionMatrix =
        glm::perspective(3.14f / 2.0f, aspect, 0.01f, 10000.0f);
    return true;
}

void Client::handleInput(const sf::Window &window, const Keyboard &keyboard)
{
    static auto lastMousePosition = sf::Mouse::getPosition(window);

    // Handle mouse input
    if (!m_isMouseLocked && window.hasFocus() &&
        sf::Mouse::getPosition(window).y >= 0) {
        auto change = sf::Mouse::getPosition(window) - lastMousePosition;
        mp_player->rotation.x += static_cast<float>(change.y / 8.0f);
        mp_player->rotation.y += static_cast<float>(change.x / 8.0f);
        sf::Mouse::setPosition(
            {(int)window.getSize().x / 2, (int)window.getSize().y / 2}, window);
        lastMousePosition = sf::Mouse::getPosition(window);
    }

    // Handle keyboard input
    float PLAYER_SPEED = 0.05f;
    if (keyboard.isKeyDown(sf::Keyboard::LControl)) {
        PLAYER_SPEED *= 10;
    }

    auto &rotation = mp_player->rotation;
    auto &position = mp_player->position;
    if (keyboard.isKeyDown(sf::Keyboard::W)) {
        position += forwardsVector(rotation) * PLAYER_SPEED;
    }
    else if (keyboard.isKeyDown(sf::Keyboard::S)) {
        position += backwardsVector(rotation) * PLAYER_SPEED;
    }
    if (keyboard.isKeyDown(sf::Keyboard::A)) {
        position += leftVector(rotation) * PLAYER_SPEED;
    }
    else if (keyboard.isKeyDown(sf::Keyboard::D)) {
        position += rightVector(rotation) * PLAYER_SPEED;
    }

    if (keyboard.isKeyDown(sf::Keyboard::Space)) {
        position.y += PLAYER_SPEED * 2;
    }
    else if (keyboard.isKeyDown(sf::Keyboard::LShift)) {
        position.y -= PLAYER_SPEED * 2;
    }

    /*
        if (rotation.x < -80.0f) {
            rotation.x = -79.0f;
        }
        else if (rotation.x > 85.0f) {
            rotation.x = 84.0f;
        }
    */
}

void Client::onKeyRelease(sf::Keyboard::Key key)
{
    if (key == sf::Keyboard::L) {
        m_isMouseLocked = !m_isMouseLocked;
    }
}

void Client::onMouseRelease(sf::Mouse::Button button, [[maybe_unused]] int x,
                            [[maybe_unused]] int y)
{
    // Handle block removal/ block placing events
    Ray ray(mp_player->position, mp_player->rotation);
    for (; ray.getLength() < 8; ray.step()) {
        auto blockPosition = toBlockPosition(ray.getEndpoint());
        if (m_chunks.manager.getBlock(blockPosition) == 1) {

            BlockUpdate blockUpdate;
            blockUpdate.block = button == sf::Mouse::Left ? 0 : 1;
            blockUpdate.position = button == sf::Mouse::Left
                                       ? blockPosition
                                       : toBlockPosition(ray.getLastPoint());
            m_chunks.blockUpdates.push_back(blockUpdate);
            break;
        }
    }
}

sf::Clock test;
int radius = 0;

void Client::update()
{
    if (test.getElapsedTime().asSeconds() > 2) {
    }

    NetworkHost::tick();
    sendPlayerPosition(mp_player->position);

    // Update blocks
    for (auto &blockUpdate : m_chunks.blockUpdates) {
        auto chunkPosition = toChunkPosition(blockUpdate.position);
        m_chunks.manager.ensureNeighbours(chunkPosition);
        m_chunks.manager.setBlock(blockUpdate.position, blockUpdate.block);
        m_chunks.updates.emplace(chunkPosition);
    }
    m_chunks.blockUpdates.clear();

    // Update chunk meshes
    for (auto itr = m_chunks.updates.begin(); itr != m_chunks.updates.end();) {
        auto pos = *itr;
        if (m_chunks.manager.hasNeighbours(pos)) {
            auto &chunk = m_chunks.manager.getChunk(pos);
            auto buffer = makeChunkMesh(chunk);
            m_chunks.bufferables.push_back(buffer);
            deleteChunkRenderable(pos);
            itr = m_chunks.updates.erase(itr);
        }
        else {
            itr++;
        }
    }
}

void Client::render()
{
    // Setup matrices
    glm::mat4 viewMatrix{1.0f};
    glm::mat4 projectionViewMatrix{1.0f};

    m_basicShader.program.bind();
    rotateMatrix(viewMatrix, mp_player->rotation);
    translateMatrix(viewMatrix, -mp_player->position);

    projectionViewMatrix = m_projectionMatrix * viewMatrix;
    gl::loadUniform(m_basicShader.projectionViewLocation, projectionViewMatrix);

    // Render all the entities
    auto drawable = m_cube.getDrawable();
    drawable.bind();
    m_texture.bind();
    for (auto &ent : m_entities) {
        if (ent.active && &ent != mp_player) {
            glm::mat4 modelMatrix{1.0f};
            translateMatrix(modelMatrix,
                            {ent.position.x, ent.position.y, ent.position.z});
            gl::loadUniform(m_basicShader.modelLocation, modelMatrix);
            drawable.draw();
        }
    }

    // Render chunks
    m_chunkShader.program.bind();
    m_grassTexture.bind();
    gl::loadUniform(m_chunkShader.projectionViewLocation, projectionViewMatrix);

    // Buffer chunks
    for (auto &chunk : m_chunks.bufferables) {
        m_chunks.drawables.push_back(chunk.createBuffer());
        m_chunks.positions.push_back(chunk.position);
        // std::cout << "Buffered me a new one" << std::endl;
    }
    m_chunks.bufferables.clear();

    // Render them
    for (auto &chunk : m_chunks.drawables) {
        chunk.getDrawable().bindAndDraw();
    }
}

void Client::endGame()
{
    m_cube.destroy();
    m_texture.destroy();
    m_basicShader.program.destroy();
    m_chunkShader.program.destroy();

    for (auto &chunk : m_chunks.drawables) {
        chunk.destroy();
    }
    NetworkHost::disconnectFromPeer(mp_serverPeer);
}

EngineStatus Client::currentStatus() const
{
    return m_status;
}

int Client::findChunkDrawableIndex(const ChunkPosition &position)
{
    for (int i = 0; i < static_cast<int>(m_chunks.positions.size()); i++) {
        if (m_chunks.positions[i] == position) {
            return i;
        }
    }
    return -1;
}

void Client::deleteChunkRenderable(const ChunkPosition &position)
{
    auto index = findChunkDrawableIndex(position);
    if (index > -1) {
        m_chunks.drawables[index].destroy();

        // As the chunk renders need not be a sorted array, "swap and pop"
        // can be used
        // More efficent (and maybe safer) than normal deletion
        std::iter_swap(m_chunks.drawables.begin() + index,
                       m_chunks.drawables.end() - 1);
        std::iter_swap(m_chunks.positions.begin() + index,
                       m_chunks.positions.end() - 1);
        m_chunks.drawables.pop_back();
        m_chunks.positions.pop_back();
    }
}