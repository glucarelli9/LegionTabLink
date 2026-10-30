#include "AdbSocketServer.hpp"
#include "DxgiCapture.hpp"
#include "EncoderFactory.hpp"
#include "PenTouchInjector.hpp"
#include "packet_spec.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <windows.h>
#include <shellapi.h>

namespace
{
    constexpr DWORD DefaultWidth = 2000;
    constexpr DWORD DefaultHeight = 1200;
    constexpr DWORD DefaultFps = 30;
    constexpr DWORD DefaultBitrateMbps = 20;

    // ── ADB management ──────────────────────────────────────────────

    std::string RunCommand(const std::string& cmd)
    {
        std::string result;
        FILE* pipe = ::_popen(cmd.c_str(), "r");
        if (!pipe) return result;
        char buf[256];
        while (std::fgets(buf, sizeof(buf), pipe))
            result += buf;
        ::_pclose(pipe);
        // trim trailing whitespace
        while (!result.empty() && (result.back() == '\n' || result.back() == '\r' || result.back() == ' '))
            result.pop_back();
        return result;
    }

    bool FileExists(const std::string& path)
    {
        return ::GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES;
    }

    std::string FindAdbPath()
    {
        // 1. On PATH
        auto found = RunCommand("where adb.exe 2>nul");
        if (!found.empty())
        {
            // 'where' may return multiple lines; take the first
            auto nl = found.find('\n');
            if (nl != std::string::npos) found.resize(nl);
            while (!found.empty() && (found.back() == '\r' || found.back() == ' ')) found.pop_back();
            if (FileExists(found)) return found;
        }
        // 2. Known location
        const char* knownPaths[] = {
            "C:\\Users\\Gian\\platform-tools\\platform-tools\\adb.exe",
            nullptr // LOCALAPPDATA placeholder filled below
        };
        for (auto p = knownPaths; *p; ++p)
            if (FileExists(*p)) return *p;

        // 3. %LOCALAPPDATA%\Android\Sdk\platform-tools
        char localAppData[MAX_PATH]{};
        if (::GetEnvironmentVariableA("LOCALAPPDATA", localAppData, MAX_PATH) > 0)
        {
            std::string sdkAdb = std::string(localAppData) + "\\Android\\Sdk\\platform-tools\\adb.exe";
            if (FileExists(sdkAdb)) return sdkAdb;
        }
        // 4. Same directory as this exe
        char exePath[MAX_PATH]{};
        if (::GetModuleFileNameA(nullptr, exePath, MAX_PATH) > 0)
        {
            std::string dir(exePath);
            auto slash = dir.find_last_of("\\/");
            if (slash != std::string::npos) dir.resize(slash + 1);
            std::string beside = dir + "adb.exe";
            if (FileExists(beside)) return beside;
        }
        return {};
    }

    bool RunAdb(const std::string& adbPath, const std::string& args)
    {
        std::string cmd = "\"" + adbPath + "\" " + args + " 2>nul";
        int rc = ::system(cmd.c_str());
        return rc == 0;
    }

    std::string RunAdbOutput(const std::string& adbPath, const std::string& args)
    {
        std::string cmd = "\"" + adbPath + "\" " + args + " 2>nul";
        return RunCommand(cmd);
    }

    bool SetupAdbTunnels(const std::string& adbPath)
    {
        std::fprintf(stderr, "[server] Starting ADB server...\n");
        RunAdb(adbPath, "kill-server");
        ::Sleep(1500);
        RunAdb(adbPath, "start-server");
        ::Sleep(1000);

        std::fprintf(stderr, "[server] Looking for connected tablets...\n");
        auto devices = RunAdbOutput(adbPath, "devices");
        // Parse: skip first line ("List of devices attached"), look for a line with "device"
        bool found = false;
        auto pos = devices.find('\n');
        if (pos != std::string::npos)
        {
            auto rest = devices.substr(pos + 1);
            if (rest.find("device") != std::string::npos)
                found = true;
        }
        if (!found)
        {
            std::fprintf(stderr, "[server] No tablet detected. Check USB cable and USB Debugging.\n");
            return false;
        }
        std::fprintf(stderr, "[server] Tablet found! Setting up tunnels...\n");
        if (!RunAdb(adbPath, "reverse tcp:27183 tcp:27183")) return false;
        if (!RunAdb(adbPath, "reverse tcp:27184 tcp:27184")) return false;
        return true;
    }

    void LaunchTabletApp(const std::string& adbPath)
    {
        RunAdb(adbPath, "shell am force-stop com.legion.tablink");
        ::Sleep(500);
        RunAdb(adbPath, "shell am start -n com.legion.tablink/.MainActivity");
    }

    // ── CLI ──────────────────────────────────────────────────────────

    void ShowUsage()
    {
        std::fputs(
            "GL ExtraScreen Server v1.0\n"
            "  --width N      virtual display width  (default 2000)\n"
            "  --height N     virtual display height (default 1200)\n"
            "  --fps N        encoder frame rate     (default 30)\n"
            "  --bitrate N    target bitrate in Mbps (default 20)\n",
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

    std::fprintf(stderr, "============================================\n");
    std::fprintf(stderr, "  GL ExtraScreen Server v1.0\n");
    std::fprintf(stderr, "============================================\n\n");

    DWORD width = DefaultWidth;
    DWORD height = DefaultHeight;
    DWORD fps = DefaultFps;
    DWORD bitrateMbps = DefaultBitrateMbps;
    if (!ParseArguments(argc, argv, width, height, fps, bitrateMbps))
    {
        return 1;
    }

    // ── ADB setup (find adb, connect tablet, set tunnels) ──
    auto adbPath = FindAdbPath();
    if (adbPath.empty())
    {
        std::fprintf(stderr, "[server] ERROR: adb.exe not found!\n");
        std::fprintf(stderr, "[server] Download Android Platform Tools from:\n");
        std::fprintf(stderr, "[server]   https://developer.android.com/tools/releases/platform-tools\n");
        std::fprintf(stderr, "[server] Place adb.exe next to this .exe or add it to PATH.\n");
        std::fprintf(stderr, "\nPress any key to exit...\n");
        ::system("pause >nul");
        return 1;
    }
    std::fprintf(stderr, "[server] Found adb: %s\n", adbPath.c_str());

    if (!SetupAdbTunnels(adbPath))
    {
        std::fprintf(stderr, "[server] ERROR: Could not connect to tablet.\n");
        std::fprintf(stderr, "[server] Make sure:\n");
        std::fprintf(stderr, "[server]   1. Tablet is connected via USB\n");
        std::fprintf(stderr, "[server]   2. USB Debugging is enabled (Developer Options)\n");
        std::fprintf(stderr, "[server]   3. You accepted the 'Allow USB debugging' popup\n");
        std::fprintf(stderr, "\nPress any key to exit...\n");
        ::system("pause >nul");
        return 1;
    }
    std::fprintf(stderr, "[server] ADB tunnels ready\n");

    LaunchTabletApp(adbPath);
    std::fprintf(stderr, "[server] Tablet app launched\n\n");

    // ── Ensure virtual display is available ──
    {
        auto ensureVDD = []() {
            const char* vddPaths[] = {
                "C:\\Program Files\\ParsecVDisplay\\ParsecVDisplay.exe",
                "C:\\Program Files (x86)\\ParsecVDisplay\\ParsecVDisplay.exe",
                nullptr
            };
            for (auto p = vddPaths; *p; ++p)
            {
                if (FileExists(*p))
                {
                    auto running = RunCommand("tasklist /FI \"IMAGENAME eq ParsecVDisplay.exe\" /NH 2>nul");
                    if (running.find("ParsecVDisplay") == std::string::npos)
                    {
                        std::fprintf(stderr, "[server] Launching ParsecVDisplay...\n");
                        ::ShellExecuteA(nullptr, "open", *p, nullptr, nullptr, SW_SHOWMINNOACTIVE);
                        ::Sleep(2000);
                    }
                    return true;
                }
            }
            return false;
        };
        ensureVDD();
    }

    // ── Video pipeline ──
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
        std::fprintf(stderr, "\n[server] No virtual display found!\n");
        std::fprintf(stderr, "[server] Open ParsecVDisplay from the system tray and add a display.\n");
        std::fprintf(stderr, "[server] Recommended: 1920x1080 @ 60Hz\n");
        std::fprintf(stderr, "\nWaiting for virtual display... (press Ctrl+C to cancel)\n");
        for (int attempt = 0; attempt < 120; ++attempt)
        {
            ::Sleep(2000);
            if (capture.Start(width, height))
            {
                std::fprintf(stderr, "[server] Virtual display detected!\n\n");
                goto pipelineReady;
            }
        }
        std::fprintf(stderr, "[server] Timed out waiting for virtual display.\n");
        ::WSACleanup();
        return 1;
    }
    pipelineReady:

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
