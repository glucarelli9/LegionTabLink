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
    constexpr DWORD DefaultFps = 30;
    constexpr DWORD DefaultBitrateMbps = 20;

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
    // Make DXGI report physical pixels, not DPI-scaled coordinates
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
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
    DWORD noFrameTicks = 0;
    DWORD framesSinceIdr = 0;
    constexpr DWORD IdrInterval = 30; // Force IDR every ~1 second at 30fps

    // Force DXGI to produce a frame on a static virtual display by nudging
    // a 1-pixel invisible region.  We move the cursor 1px at the top-left
    // corner of the virtual display and back — invisible to the user but
    // enough for DXGI to report a new frame.
    auto forceDirtyFrame = [&monitorRect]()
    {
        const LONG x = monitorRect.left;
        const LONG y = monitorRect.top;
        ::SetCursorPos(x, y);
        ::Sleep(1);
        ::SetCursorPos(x + 1, y);
        ::Sleep(1);
        ::SetCursorPos(x, y);
    };

    for (;;)
    {
        if (!server.IsVideoConnected())
        {
            if (previousConnectionAlive)
            {
                previousConnectionAlive = false;
                noFrameTicks = 0;
                std::fprintf(stderr, "[main] awaiting tablet client\n");
            }
            ::Sleep(2);
            continue;
        }
        if (!previousConnectionAlive)
        {
            previousConnectionAlive = true;
            noFrameTicks = 0;
            std::fprintf(stderr, "[main] client active, capturing frames\n");
            forceDirtyFrame();
        }

        ID3D11Texture2D* frame = nullptr;
        bool acquired = capture.AcquireFrame(&frame, 11);
        if (acquired)
        {
            noFrameTicks = 0;
            ++framesSinceIdr;
            // Got a new frame from DXGI — encode and send it.
            bool wantKeyframe = server.ConsumeIdrRequest();
            if (framesSinceIdr >= IdrInterval)
            {
                wantKeyframe = true;
            }
            bool actuallyIdr = false;
            if (encoder->EncodeFrame(frame, wantKeyframe, bitstream, actuallyIdr) && !bitstream.empty())
            {
                if (actuallyIdr)
                {
                    framesSinceIdr = 0;
                }
                server.SendVideoFrame(bitstream.data(), bitstream.size(), actuallyIdr);
            }
            else if (wantKeyframe)
            {
                server.ForceIdr();
            }
            capture.ReleaseFrame();
        }
        else
        {
            // Static screen — only force a dirty frame when an IDR was
            // requested (decoder recovery) or every ~2s as heartbeat.
            ++noFrameTicks;
            if (server.ConsumeIdrRequest() || noFrameTicks > 2000)
            {
                forceDirtyFrame();
                noFrameTicks = 0;
            }
            ::Sleep(1);
        }

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
