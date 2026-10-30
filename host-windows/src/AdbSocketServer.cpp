#include "AdbSocketServer.hpp"

#include <cstdio>
#include <cwchar>
#include <winsock2.h>
#include <ws2tcpip.h>

namespace
{
    bool SetTcpNoDelay(SOCKET client)
    {
        const BOOL enabled = TRUE;
        return ::setsockopt(client, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&enabled), sizeof(enabled)) == 0;
    }

    bool SendAll(SOCKET client, const void* data, uint32_t size)
    {
        const auto* bytes = static_cast<const uint8_t*>(data);
        uint32_t sent = 0;
        while (sent < size)
        {
            const int chunk = ::send(client, reinterpret_cast<const char*>(bytes + sent), static_cast<int>(size - sent), 0);
            if (chunk <= 0)
            {
                return false;
            }
            sent += static_cast<uint32_t>(chunk);
        }
        return true;
    }

    bool ReceiveAll(SOCKET client, void* data, uint32_t size)
    {
        auto* bytes = static_cast<uint8_t*>(data);
        uint32_t received = 0;
        while (received < size)
        {
            const int chunk = ::recv(client, reinterpret_cast<char*>(bytes + received), static_cast<int>(size - received), 0);
            if (chunk <= 0)
            {
                return false;
            }
            received += static_cast<uint32_t>(chunk);
        }
        return true;
    }

    bool ReadHeader(SOCKET client, LtlPacketHeader& header)
    {
        return ReceiveAll(client, &header, sizeof(header));
    }

    bool HeaderIsValid(const LtlPacketHeader& header)
    {
        return header.magic0 == LTL_MAGIC_0
            && header.magic1 == LTL_MAGIC_1
            && header.protocolVersion == LTL_PROTOCOL_VERSION
            && header.payloadSize <= 16u * 1024u * 1024u;
    }

    void DrainPayload(SOCKET client, uint32_t payloadSize)
    {
        uint8_t discard[4096];
        uint32_t remaining = payloadSize;
        while (remaining > 0)
        {
            const uint32_t chunk = remaining > sizeof(discard) ? sizeof(discard) : remaining;
            if (!ReceiveAll(client, discard, chunk))
            {
                return;
            }
            remaining -= chunk;
        }
    }

    SOCKET OpenListener(uint16_t port)
    {
        SOCKET listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener == INVALID_SOCKET)
        {
            return INVALID_SOCKET;
        }

        const BOOL noDelay = TRUE;
        ::setsockopt(listener, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
        address.sin_port = ::htons(port);

        if (::bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0
            || ::listen(listener, 1) != 0)
        {
            ::closesocket(listener);
            return INVALID_SOCKET;
        }
        return listener;
    }
}

AdbSocketServer::~AdbSocketServer()
{
    Stop();
}

bool AdbSocketServer::SpawnAdbForward(uint16_t port)
{
    wchar_t commandLine[128];
    std::swprintf(commandLine, 128, L"adb forward tcp:%u tcp:%u", static_cast<unsigned>(port), static_cast<unsigned>(port));

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const BOOL created = ::CreateProcessW(
        nullptr,
        commandLine,
        nullptr,
        nullptr,
        FALSE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &startup,
        &process);
    if (!created)
    {
        std::fprintf(stderr, "[adb] CreateProcessW(adb) failed err=%lu (adb in PATH?)\n", ::GetLastError());
        return false;
    }

    ::WaitForSingleObject(process.hProcess, 10000);
    DWORD exitCode = 0;
    ::GetExitCodeProcess(process.hProcess, &exitCode);
    ::CloseHandle(process.hProcess);
    ::CloseHandle(process.hThread);
    if (exitCode != 0)
    {
        std::fprintf(stderr, "[adb] forward tcp:%u failed exit=%lu (tablet attached?)\n", static_cast<unsigned>(port), exitCode);
        return false;
    }
    return true;
}

bool AdbSocketServer::Start(uint16_t videoPort, uint16_t inputPort, const StreamConfig& config, const InputHandler& handler)
{
    if (m_running.load())
    {
        return true;
    }

    m_config = config;
    m_onInput = handler;

    m_videoListener = OpenListener(videoPort);
    m_inputListener = OpenListener(inputPort);
    if (m_videoListener == INVALID_SOCKET || m_inputListener == INVALID_SOCKET)
    {
        std::fprintf(stderr, "[tcp] listeners on tcp:%u/tcp:%u failed\n", static_cast<unsigned>(videoPort), static_cast<unsigned>(inputPort));
        if (m_videoListener != INVALID_SOCKET)
        {
            ::closesocket(m_videoListener);
            m_videoListener = INVALID_SOCKET;
        }
        if (m_inputListener != INVALID_SOCKET)
        {
            ::closesocket(m_inputListener);
            m_inputListener = INVALID_SOCKET;
        }
        return false;
    }

    m_running.store(true);

    m_videoThread = std::thread([this]
    {
        ListenLoopVideo();
    });
    m_inputThread = std::thread([this]
    {
        ListenLoopInput();
    });

    std::thread([this, videoPort, inputPort]
    {
        SpawnAdbForward(videoPort);
        SpawnAdbForward(inputPort);
    }).detach();

    return true;
}

void AdbSocketServer::Stop()
{
    m_running.store(false);
    if (m_videoListener != INVALID_SOCKET)
    {
        ::closesocket(m_videoListener);
        m_videoListener = INVALID_SOCKET;
    }
    if (m_inputListener != INVALID_SOCKET)
    {
        ::closesocket(m_inputListener);
        m_inputListener = INVALID_SOCKET;
    }
    DropVideoClient(INVALID_SOCKET);
    DropInputClient(INVALID_SOCKET);
    if (m_videoThread.joinable())
    {
        m_videoThread.join();
    }
    if (m_inputThread.joinable())
    {
        m_inputThread.join();
    }
}

bool AdbSocketServer::IsVideoConnected() const
{
    return m_videoClient.load() != INVALID_SOCKET;
}

bool AdbSocketServer::ConsumeIdrRequest()
{
    return m_idrPending.exchange(false);
}

void AdbSocketServer::ForceIdr()
{
    m_idrPending.store(true);
}

void AdbSocketServer::ListenLoopVideo()
{
    while (m_running.load())
    {
        SOCKET client = ::accept(m_videoListener, nullptr, nullptr);
        if (client == INVALID_SOCKET)
        {
            break;
        }
        if (m_videoClient.exchange(INVALID_SOCKET) != INVALID_SOCKET)
        {
        }
        HandleVideoClient(client);
    }
}

void AdbSocketServer::ListenLoopInput()
{
    while (m_running.load())
    {
        SOCKET client = ::accept(m_inputListener, nullptr, nullptr);
        if (client == INVALID_SOCKET)
        {
            break;
        }
        HandleInputClient(client);
    }
}

AdbSocketServer::StreamConfig AdbSocketServer::GetConfig()
{
    std::lock_guard<std::mutex> lock(m_configLock);
    return m_config;
}

void AdbSocketServer::UpdateStreamConfig(const StreamConfig& config)
{
    {
        std::lock_guard<std::mutex> lock(m_configLock);
        m_config = config;
    }
    m_resizePending.store(false);
}

bool AdbSocketServer::ConsumeResizeRequest(uint32_t& width, uint32_t& height)
{
    if (!m_resizePending.load()) return false;
    width = m_requestedWidth.load();
    height = m_requestedHeight.load();
    return true;
}

void AdbSocketServer::HandleVideoClient(SOCKET client)
{
    const SOCKET previous = m_videoClient.exchange(INVALID_SOCKET);
    if (previous != INVALID_SOCKET && previous != client)
    {
        ::closesocket(previous);
    }

    SetTcpNoDelay(client);

    DWORD sendTimeoutMs = 500;
    ::setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&sendTimeoutMs), sizeof(sendTimeoutMs));

    LtlPacketHeader header{};
    if (!ReadHeader(client, header) || !HeaderIsValid(header) || header.packetType != LtlPacketHello)
    {
        DropVideoClient(client);
        return;
    }

    uint8_t helloPayload[64];
    if (header.payloadSize > sizeof(helloPayload) || !ReceiveAll(client, helloPayload, header.payloadSize))
    {
        DropVideoClient(client);
        return;
    }

    if (header.payloadSize >= sizeof(LtlHelloPayload))
    {
        LtlHelloPayload hello{};
        std::memcpy(&hello, helloPayload, sizeof(hello));
        const StreamConfig current = GetConfig();
        const bool wantPortrait = hello.maxHeight > hello.maxWidth;
        const bool havePortrait = current.height > current.width;
        if (hello.maxWidth > 0 && hello.maxHeight > 0 && wantPortrait != havePortrait)
        {
            std::fprintf(stderr, "[tcp] tablet rotated -> %s, rebuilding stream\n", wantPortrait ? "portrait" : "landscape");
            m_requestedWidth.store(hello.maxWidth);
            m_requestedHeight.store(hello.maxHeight);
            m_resizePending.store(true);
            for (int waited = 0; m_resizePending.load() && m_running.load() && waited < 15000; waited += 20)
            {
                ::Sleep(20);
            }
            m_resizePending.store(false);
        }
    }

    const StreamConfig config = GetConfig();
    LtlConfigPayload configPayload{};
    configPayload.width = static_cast<uint16_t>(config.width);
    configPayload.height = static_cast<uint16_t>(config.height);
    configPayload.fps = static_cast<uint16_t>(config.fps);
    configPayload.bitrateMbps = static_cast<uint16_t>(config.bitrateMbps);
    configPayload.codec = config.codec;
    if (!SendPacket(client, LtlPacketConfig, &configPayload, sizeof(configPayload)))
    {
        ::closesocket(client);
        return;
    }

    m_videoClient.store(client);
    m_idrPending.store(true);
    std::fprintf(stderr, "[tcp] video client connected (config %ux%u@%u %umbps codec=%u)\n",
        static_cast<unsigned>(config.width),
        static_cast<unsigned>(config.height),
        static_cast<unsigned>(config.fps),
        static_cast<unsigned>(config.bitrateMbps),
        static_cast<unsigned>(config.codec));

    while (m_running.load())
    {
        LtlPacketHeader incoming{};
        if (!ReadHeader(client, incoming) || !HeaderIsValid(incoming))
        {
            break;
        }
        if (incoming.packetType == LtlPacketIdrRequest)
        {
            m_idrPending.store(true);
            continue;
        }
        if (incoming.packetType == LtlPacketBye)
        {
            break;
        }
        DrainPayload(client, incoming.payloadSize);
    }

    DropVideoClient(client);
    std::fprintf(stderr, "[tcp] video client disconnected\n");
}

void AdbSocketServer::HandleInputClient(SOCKET client)
{
    const SOCKET previous = m_inputClient.exchange(client);
    if (previous != INVALID_SOCKET && previous != client)
    {
        ::closesocket(previous);
    }

    SetTcpNoDelay(client);

    while (m_running.load())
    {
        LtlPacketHeader incoming{};
        if (!ReadHeader(client, incoming) || !HeaderIsValid(incoming))
        {
            break;
        }

        if (incoming.packetType == LtlPacketIdrRequest)
        {
            m_idrPending.store(true);
            continue;
        }
        if (incoming.packetType == LtlPacketBye)
        {
            break;
        }
        if (incoming.packetType == LtlPacketInput && incoming.payloadSize == sizeof(LtlInputEventPayload))
        {
            LtlInputEventPayload event{};
            if (!ReceiveAll(client, &event, sizeof(event)))
            {
                break;
            }
            if (event.pointerCount > LTL_MAX_INPUT_POINTERS)
            {
                event.pointerCount = LTL_MAX_INPUT_POINTERS;
            }
            if (m_onInput)
            {
                m_onInput(event);
            }
            continue;
        }
        DrainPayload(client, incoming.payloadSize);
    }

    const SOCKET current = m_inputClient.exchange(INVALID_SOCKET);
    if (current == client)
    {
        ::closesocket(client);
    }
    else if (current == INVALID_SOCKET)
    {
        ::closesocket(client);
    }
    else if (current != client)
    {
        ::closesocket(client);
    }
}

void AdbSocketServer::DropVideoClient(SOCKET stale)
{
    for (;;)
    {
        SOCKET current = m_videoClient.load();
        if (current == INVALID_SOCKET && stale == INVALID_SOCKET)
        {
            return;
        }
        if (current == INVALID_SOCKET)
        {
            ::closesocket(stale);
            return;
        }
        if (stale == INVALID_SOCKET || current == stale)
        {
            if (m_videoClient.compare_exchange_weak(current, INVALID_SOCKET))
            {
                ::closesocket(current);
                return;
            }
            continue;
        }
        ::closesocket(stale);
        return;
    }
}

void AdbSocketServer::DropInputClient(SOCKET stale)
{
    for (;;)
    {
        SOCKET current = m_inputClient.load();
        if (current == INVALID_SOCKET && stale == INVALID_SOCKET)
        {
            return;
        }
        if (current == INVALID_SOCKET)
        {
            ::closesocket(stale);
            return;
        }
        if (stale == INVALID_SOCKET || current == stale)
        {
            if (m_inputClient.compare_exchange_weak(current, INVALID_SOCKET))
            {
                ::closesocket(current);
                return;
            }
            continue;
        }
        ::closesocket(stale);
        return;
    }
}

bool AdbSocketServer::SendPacket(SOCKET client, LtlPacketType packetType, const void* payload, uint32_t payloadSize)
{
    LtlPacketHeader header{};
    header.magic0 = LTL_MAGIC_0;
    header.magic1 = LTL_MAGIC_1;
    header.packetType = static_cast<uint8_t>(packetType);
    header.protocolVersion = static_cast<uint8_t>(LTL_PROTOCOL_VERSION);
    header.payloadSize = payloadSize;
    if (!SendAll(client, &header, sizeof(header)))
    {
        return false;
    }
    if (payload != nullptr && payloadSize > 0)
    {
        return SendAll(client, payload, payloadSize);
    }
    return true;
}

bool AdbSocketServer::SendVideoFrame(const uint8_t* payload, size_t payloadSize, bool keyframe)
{
    const SOCKET client = m_videoClient.load();
    if (client == INVALID_SOCKET || payload == nullptr || payloadSize == 0)
    {
        return false;
    }

    LtlVideoFrameHeader frameHeader{};
    frameHeader.keyframe = keyframe ? 1 : 0;

    LtlPacketHeader header{};
    header.magic0 = LTL_MAGIC_0;
    header.magic1 = LTL_MAGIC_1;
    header.packetType = LtlPacketVideo;
    header.protocolVersion = static_cast<uint8_t>(LTL_PROTOCOL_VERSION);
    header.payloadSize = static_cast<uint32_t>(sizeof(frameHeader) + payloadSize);

    if (!SendAll(client, &header, sizeof(header))
        || !SendAll(client, &frameHeader, sizeof(frameHeader))
        || !SendAll(client, payload, static_cast<uint32_t>(payloadSize)))
    {
        DropVideoClient(client);
        m_idrPending.store(true);
        return false;
    }
    return true;
}
