#include "AdbSocketServer.hpp"
#include "DxgiCapture.hpp"
#include "EncoderFactory.hpp"
#include "PenTouchInjector.hpp"
#include "packet_spec.h"

#include <cstdio>
#include <cstring>
#include <vector>
#include <windows.h>

namespace
{
    constexpr DWORD DefaultWidth = 2000;
    constexpr DWORD DefaultHeight = 1200;
    constexpr DWORD DefaultFps = 60;
    constexpr DWORD DefaultBitrateMbps = 25;

    void ShowUsage()
    {
        std::fputs(
            "LegionTabLink host-streamer\n"
            "  --width N      virtual display width  (default 2000)\n"
            "  --height N     virtual display height (default 1200)\n"
            "  --fps N        encoder frame rate     (default 60)\n"
            "  --bitrate N    target bitrate in Mbps (default 25)\n",
            stderr);
    }

    bool ParseArguments(int argc, char** argv, DWORD& width, DWORD& height, DWORD& fps, DWORD& bitrateMbps)
    {
        for (int i = 1; i < argc; ++i)
        {
            if (::lstrcmpiA(argv[i], "--width") == 0 && i + 1 < argc)
            {
                width = static_cast<DWORD>(::atoi(argv[++i]));
            }
            else if (::lstrcmpiA(argv[i], "--height") == 0 && i + 1 < argc)
            {
                height = static_cast<DWORD>(::atoi(argv[++i]));
            }
            else if (::lstrcmpiA(argv[i], "--fps") == 0 && i + 1 < argc)
            {
                fps = static_cast<DWORD>(::atoi(argv[++i]));
            }
            else if (::lstrcmpiA(argv[i], "--bitrate") == 0 && i + 1 < argc)
            {
                bitrateMbps = static_cast<DWORD>(::atoi(argv[++i]));
            }
            else if (::lstrcmpiA(argv[i], "--help") == 0)
            {
                ShowUsage();
                return false;
            }
        }
        if (width == 0 || height == 0 || fps == 0 || bitrateMbps == 0)
        {
            ShowUsage();
            return false;
        }
        return true;
    }

    BOOL WINAPI CtrlHandler(DWORD type)
    {
        if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT)
        {
            return FALSE;
        }
        return FALSE;
    }
}

int main(int argc, char** argv)
{
    DWORD width = DefaultWidth;
    DWORD height = DefaultHeight;
    DWORD fps = DefaultFps;
    DWORD bitrateMbps = DefaultBitrateMbps;
    if (!ParseArguments(argc, argv, width, height, fps, bitrateMbps))
    {
        return 1;
    }

    WSADATA wsaData{};
    if (::WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
    {
        std::fprintf(stderr, "[main] WSAStartup failed\n");
        return 1;
    }
    ::SetConsoleCtrlHandler(CtrlHandler, TRUE);

    DxgiCapture capture;
    if (!capture.Start(width, height))
    {
        std::fprintf(stderr, "[main] virtual display %ux%u not found\n", static_cast<unsigned>(width), static_cast<unsigned>(height));
        ::WSACleanup();
        return 1;
    }

    auto encoder = CreateEncoderForAdapterVendor(capture.VendorId(), capture.Device(), capture.Width(), capture.Height(), fps, bitrateMbps * 1000u * 1000u);
    if (encoder == nullptr)
    {
        capture.Stop();
        ::WSACleanup();
        return 1;
    }

    RECT monitorRect{};
    capture.GetMonitorRect(monitorRect);

    PenTouchInjector injector;
    if (!injector.Start(monitorRect))
    {
        encoder->Shutdown();
        capture.Stop();
        ::WSACleanup();
        return 1;
    }

    AdbSocketServer server;
    AdbSocketServer::StreamConfig config{};
    config.width = capture.Width();
    config.height = capture.Height();
    config.fps = fps;
    config.bitrateMbps = bitrateMbps;
    config.codec = LtlVideoCodecHevc;

    if (!server.Start(LTL_VIDEO_TCP_PORT, LTL_INPUT_TCP_PORT, config, [&injector](const LtlInputEventPayload& event)
    {
        injector.Inject(event);
    }))
    {
        std::fprintf(stderr, "[main] transport start failed\n");
        injector.Stop();
        encoder->Shutdown();
        capture.Stop();
        ::WSACleanup();
        return 1;
    }

    std::fprintf(stderr, "[main] streaming on tcp:%u (video) / tcp:%u (input)\n",
        static_cast<unsigned>(LTL_VIDEO_TCP_PORT),
        static_cast<unsigned>(LTL_INPUT_TCP_PORT));

    std::vector<uint8_t> bitstream;
    bool previousConnectionAlive = false;

    for (;;)
    {
        if (!server.IsVideoConnected())
        {
            if (previousConnectionAlive)
            {
                previousConnectionAlive = false;
                std::fprintf(stderr, "[main] awaiting tablet client\n");
            }
            ::Sleep(2);
            continue;
        }
        previousConnectionAlive = true;

        ID3D11Texture2D* frame = nullptr;
        if (!capture.AcquireFrame(&frame, 33))
        {
            ::Sleep(0);
            continue;
        }

        const bool keyframe = server.ConsumeIdrRequest();
        if (encoder->EncodeFrame(frame, keyframe, bitstream) && !bitstream.empty())
        {
            server.SendVideoFrame(bitstream.data(), bitstream.size(), keyframe);
        }
        capture.ReleaseFrame();

        if (::GetAsyncKeyState(VK_ESCAPE) & 0x8000)
        {
            break;
        }
    }

    std::fprintf(stderr, "[main] shutting down\n");
    server.Stop();
    injector.Stop();
    encoder->Shutdown();
    capture.Stop();
    ::WSACleanup();
    return 0;
}
