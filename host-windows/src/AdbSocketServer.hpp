#pragma once

#include "packet_spec.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>

#include <winsock2.h>
#include <ws2tcpip.h>

class AdbSocketServer
{
public:
    struct StreamConfig
    {
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t fps = 0;
        uint32_t bitrateMbps = 0;
        uint8_t codec = LtlVideoCodecHevc;
    };

    using InputHandler = std::function<void(const LtlInputEventPayload&)>;

    AdbSocketServer() = default;
    ~AdbSocketServer();

    AdbSocketServer(const AdbSocketServer&) = delete;
    AdbSocketServer& operator=(const AdbSocketServer&) = delete;

    bool Start(uint16_t videoPort, uint16_t inputPort, const StreamConfig& config, const InputHandler& handler);
    void Stop();

    bool IsVideoConnected() const;
    bool ConsumeIdrRequest();
    void ForceIdr();
    bool SendVideoFrame(const uint8_t* payload, size_t payloadSize, bool keyframe);

    // A client whose Hello asks for the other orientation is held until the
    // pipeline is rebuilt and UpdateStreamConfig() publishes the new size.
    bool ConsumeResizeRequest(uint32_t& width, uint32_t& height);
    void UpdateStreamConfig(const StreamConfig& config);

private:
    static bool SpawnAdbForward(uint16_t port);
    void ListenLoopVideo();
    void ListenLoopInput();
    void HandleVideoClient(SOCKET client);
    void HandleInputClient(SOCKET client);
    void DropVideoClient(SOCKET stale);
    void DropInputClient(SOCKET stale);
    bool SendPacket(SOCKET client, LtlPacketType packetType, const void* payload, uint32_t payloadSize);

    SOCKET m_videoListener = INVALID_SOCKET;
    SOCKET m_inputListener = INVALID_SOCKET;
    std::atomic<SOCKET> m_videoClient{ INVALID_SOCKET };
    std::atomic<SOCKET> m_inputClient{ INVALID_SOCKET };
    std::atomic<bool> m_running{ false };
    std::atomic<bool> m_idrPending{ false };
    StreamConfig GetConfig();

    std::mutex m_configLock;
    StreamConfig m_config{};
    std::atomic<bool> m_resizePending{ false };
    std::atomic<uint32_t> m_requestedWidth{ 0 };
    std::atomic<uint32_t> m_requestedHeight{ 0 };
    InputHandler m_onInput;
    std::thread m_videoThread;
    std::thread m_inputThread;
};
