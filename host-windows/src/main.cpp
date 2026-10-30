// ═══════════════════════════════════════════════════════════════════
//  GL ExtraScreen Server — Windows Desktop Application
//  by Gianfranco Lucarelli (glucarelli9@gmail.com)
//  https://github.com/glucarelli9/LegionTabLink
// ═══════════════════════════════════════════════════════════════════

#include "AdbSocketServer.hpp"
#include "DxgiCapture.hpp"
#include "EncoderFactory.hpp"
#include "PenTouchInjector.hpp"
#include "packet_spec.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <io.h>
#include <fcntl.h>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <windows.h>
#include <shellapi.h>
#include <setupapi.h>
#include <initguid.h>

#pragma comment(lib, "setupapi.lib")

#include <objidl.h>
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")

#include <dwmapi.h>
#include <wrl.h>
#include "WebView2.h"
using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

// ── Resource & Message IDs ──────────────────────────────────
#define IDI_APPICON      101
#define IDC_BTN_START   1001
#define IDC_LBL_STATUS  1002
#define IDC_LOG_EDIT    1003
#define IDC_BTN_LANG    1004
#define IDM_TRAY_SHOW   2001
#define IDM_TRAY_EXIT   2002
#define WM_TRAYICON     (WM_USER + 100)
#define WM_SERVERLOG    (WM_USER + 101)
#define WM_STATUSUPDATE (WM_USER + 102)
#define WM_STATUSTEXT   (WM_USER + 103)
#define IDR_UI_HTML      201

namespace
{
    constexpr DWORD DefaultWidth       = 2000;
    constexpr DWORD DefaultHeight      = 1200;
    constexpr DWORD DefaultFps         = 60;
    constexpr DWORD DefaultBitrateMbps = 30;
}

// ═══════════════════════════════════════════════════════════════════
//  Theme Colors
// ═══════════════════════════════════════════════════════════════════
namespace Theme
{
    constexpr COLORREF BgMain     = RGB(15, 15, 22);
    constexpr COLORREF BgHeader   = RGB(20, 20, 30);
    constexpr COLORREF BgCard     = RGB(24, 24, 36);
    constexpr COLORREF BgLog      = RGB(12, 12, 18);
    constexpr COLORREF BgButton   = RGB(99, 102, 241);
    constexpr COLORREF BgBtnHover = RGB(120, 122, 255);
    constexpr COLORREF BgBtnDown  = RGB(79, 82, 210);
    constexpr COLORREF TextMain   = RGB(237, 237, 245);
    constexpr COLORREF TextSub    = RGB(120, 120, 150);
    constexpr COLORREF TextBtn    = RGB(255, 255, 255);
    constexpr COLORREF TextLog    = RGB(140, 145, 170);
    constexpr COLORREF TextAccent = RGB(129, 132, 255);
    constexpr COLORREF DotIdle    = RGB(70, 70, 95);
    constexpr COLORREF DotOk      = RGB(52, 211, 153);
    constexpr COLORREF DotWarn    = RGB(251, 191, 36);
    constexpr COLORREF Separator  = RGB(38, 38, 55);
    constexpr COLORREF CardBorder = RGB(40, 40, 60);
    constexpr COLORREF BgBtnLang  = RGB(30, 30, 45);
    constexpr COLORREF BgBtnLangH = RGB(40, 40, 60);
}

// ═══════════════════════════════════════════════════════════════════
//  Language Strings (EN / ES)
// ═══════════════════════════════════════════════════════════════════
struct UiStrings
{
    const wchar_t* howToConnect;
    const wchar_t* stepsText;
    const wchar_t* btnStart;
    const wchar_t* btnStop;
    const wchar_t* ready;
    const wchar_t* findingAdb;
    const wchar_t* connectingTablet;
    const wchar_t* creatingDisplay;
    const wchar_t* waitingDisplay;
    const wchar_t* configuringDisplay;
    const wchar_t* startingCapture;
    const wchar_t* streamingWait;
    const wchar_t* connected;
    const wchar_t* stopping;
    const wchar_t* logLabel;
    const wchar_t* errNoAdb;
    const wchar_t* errNoTablet;
    const wchar_t* errNoVdd;
};

static const UiStrings g_strEN =
{
    L"Setup",
    L"1   Enable Developer Options\r\n"
    L"     Settings \x2192 About tablet \x2192 tap Build number 7\x00D7\r\n\r\n"
    L"2   Enable USB Debugging\r\n"
    L"     Settings \x2192 Developer Options \x2192 toggle ON\r\n\r\n"
    L"3   Connect the tablet to this PC via USB\r\n\r\n"
    L"4   Tap \x201C" L"Allow\x201D when prompted on the tablet",
    L"Start Server",
    L"Stop Server",
    L"Ready",
    L"Finding ADB\x2026",
    L"Connecting to tablet\x2026",
    L"Creating virtual display\x2026",
    L"Waiting for display\x2026",
    L"Configuring display\x2026",
    L"Starting capture\x2026",
    L"Streaming \x2014 waiting for tablet\x2026",
    L"Connected \x2014 streaming to tablet",
    L"Stopping\x2026",
    L"Log",
    L"adb.exe not found!\n\nMake sure ADB is in the same folder "
    L"as the server or added to PATH.",
    L"Could not connect to tablet.\n\n"
    L"1. Tablet connected via USB?\n"
    L"2. USB Debugging enabled?\n"
    L"3. 'Allow USB debugging' accepted?",
    L"Virtual Display Driver not found!\n\n"
    L"Reinstall GL ExtraScreen to fix this.",
};

static const UiStrings g_strES =
{
    L"Configuración",
    L"1   Activar Opciones de desarrollador\r\n"
    L"     Ajustes \x2192 Info de tablet \x2192 tocar N° compilación 7\x00D7\r\n\r\n"
    L"2   Activar Depuración USB\r\n"
    L"     Ajustes \x2192 Opciones de desarrollador \x2192 activar\r\n\r\n"
    L"3   Conectar la tablet al PC con cable USB\r\n\r\n"
    L"4   Tocar \x201CPermitir\x201D en el mensaje de la tablet",
    L"Iniciar servidor",
    L"Detener servidor",
    L"Listo",
    L"Buscando ADB\x2026",
    L"Conectando a la tablet\x2026",
    L"Creando pantalla virtual\x2026",
    L"Esperando pantalla\x2026",
    L"Configurando pantalla\x2026",
    L"Iniciando captura\x2026",
    L"Transmitiendo \x2014 esperando tablet\x2026",
    L"Conectado \x2014 transmitiendo a tablet",
    L"Deteniendo\x2026",
    L"Registro",
    L"¡adb.exe no encontrado!\n\nAsegurate de que ADB esté en la "
    L"misma carpeta que el servidor o en el PATH.",
    L"No se pudo conectar a la tablet.\n\n"
    L"1. ¿Tablet conectada por USB?\n"
    L"2. ¿Depuración USB activada?\n"
    L"3. ¿\x201CPermitir depuración USB\x201D aceptado?",
    L"¡Driver de pantalla virtual no encontrado!\n\n"
    L"Reinstalar GL ExtraScreen para solucionarlo.",
};

// ═══════════════════════════════════════════════════════════════════
//  ParsecVDD Driver Communication
//  Reference: https://github.com/nomi-san/parsec-vdd (BSD-3-Clause)
// ═══════════════════════════════════════════════════════════════════
namespace VddDriver
{
    // {00b41627-04c4-429e-a26e-0265cf50c8fa}
    DEFINE_GUID(ADAPTER_GUID,
        0x00b41627, 0x04c4, 0x429e,
        0xa2, 0x6e, 0x02, 0x65, 0xcf, 0x50, 0xc8, 0xfa);

    enum IoctlCode : DWORD
    {
        IOCTL_ADD    = 0x0022e004,
        IOCTL_REMOVE = 0x0022a008,
        IOCTL_UPDATE = 0x0022a00c,
    };

    static HANDLE OpenAdapter()
    {
        HANDLE handle = INVALID_HANDLE_VALUE;
        HDEVINFO devInfo = ::SetupDiGetClassDevsW(
            &ADAPTER_GUID, nullptr, nullptr,
            DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (devInfo == INVALID_HANDLE_VALUE) return handle;

        SP_DEVICE_INTERFACE_DATA iface{};
        iface.cbSize = sizeof(iface);

        for (DWORD i = 0; ::SetupDiEnumDeviceInterfaces(
                devInfo, nullptr, &ADAPTER_GUID, i, &iface); ++i)
        {
            DWORD size = 0;
            ::SetupDiGetDeviceInterfaceDetailW(
                devInfo, &iface, nullptr, 0, &size, nullptr);

            auto* detail = static_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(
                calloc(1, size));
            if (!detail) continue;
            detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

            if (::SetupDiGetDeviceInterfaceDetailW(
                    devInfo, &iface, detail, size, &size, nullptr))
            {
                handle = ::CreateFileW(detail->DevicePath,
                    GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                    nullptr, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_NO_BUFFERING |
                    FILE_FLAG_OVERLAPPED  | FILE_FLAG_WRITE_THROUGH,
                    nullptr);
            }
            free(detail);
            if (handle != INVALID_HANDLE_VALUE) break;
        }
        ::SetupDiDestroyDeviceInfoList(devInfo);
        return handle;
    }

    static DWORD IoControl(HANDLE dev, IoctlCode code,
                           const void* data, size_t dataSize)
    {
        if (dev == nullptr || dev == INVALID_HANDLE_VALUE)
            return static_cast<DWORD>(-1);

        BYTE inBuf[32]{};
        if (data && dataSize > 0)
            std::memcpy(inBuf, data, (dataSize < 32) ? dataSize : 32);

        OVERLAPPED ov{};
        ov.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);

        DWORD outBuf = 0, transferred = 0;
        ::DeviceIoControl(dev, static_cast<DWORD>(code),
            inBuf, sizeof(inBuf), &outBuf, sizeof(DWORD), nullptr, &ov);

        BOOL ok = ::GetOverlappedResultEx(dev, &ov, &transferred, 5000, FALSE);
        ::CloseHandle(ov.hEvent);
        return ok ? outBuf : static_cast<DWORD>(-1);
    }

    static void  Update(HANDLE dev)       { IoControl(dev, IOCTL_UPDATE, nullptr, 0); }
    static int   AddDisplay(HANDLE dev)   { int i = static_cast<int>(IoControl(dev, IOCTL_ADD, nullptr, 0)); Update(dev); return i; }
    static void  RemoveDisplay(HANDLE dev, int index)
    {
        UINT16 be = static_cast<UINT16>(((index & 0xFF) << 8) | ((index >> 8) & 0xFF));
        IoControl(dev, IOCTL_REMOVE, &be, sizeof(be));
        Update(dev);
    }

    // Register a custom display mode in ParsecVDD's registry so the
    // driver advertises it in its EDID.  Up to 5 custom modes (keys
    // "0" through "4") are supported under HKLM\SOFTWARE\Parsec\vdd.
    // We write slot 0 with the tablet's native resolution; any
    // previous value is overwritten.  The display must be added AFTER
    // this call so the driver picks up the new mode.
    static bool RegisterCustomMode(int slot, DWORD w, DWORD h, DWORD hz)
    {
        HKEY hKey = nullptr;
        char keyPath[64];
        std::snprintf(keyPath, sizeof(keyPath), "SOFTWARE\\Parsec\\vdd\\%d", slot);
        LONG rc = ::RegCreateKeyExA(HKEY_LOCAL_MACHINE,
            keyPath,
            0, nullptr, REG_OPTION_NON_VOLATILE,
            KEY_SET_VALUE | KEY_WOW64_64KEY,
            nullptr, &hKey, nullptr);
        if (rc != ERROR_SUCCESS)
        {
            std::fprintf(stderr, "[vdd] Cannot open registry key "
                "HKLM\\SOFTWARE\\Parsec\\vdd\\0  (error %ld) "
                "— run as admin?\n", rc);
            return false;
        }
        ::RegSetValueExA(hKey, "width",  0, REG_DWORD,
            reinterpret_cast<const BYTE*>(&w), sizeof(DWORD));
        ::RegSetValueExA(hKey, "height", 0, REG_DWORD,
            reinterpret_cast<const BYTE*>(&h), sizeof(DWORD));
        ::RegSetValueExA(hKey, "hz",     0, REG_DWORD,
            reinterpret_cast<const BYTE*>(&hz), sizeof(DWORD));
        ::RegCloseKey(hKey);
        std::fprintf(stderr, "[vdd] Registered custom mode %lux%lu @%luHz "
            "in registry slot %d\n", w, h, hz, slot);
        return true;
    }
}

// ═══════════════════════════════════════════════════════════════════
//  ADB Helpers
// ═══════════════════════════════════════════════════════════════════
namespace
{
    std::string RunCommand(const std::string& cmd)
    {
        std::string result;
        FILE* pipe = ::_popen(cmd.c_str(), "r");
        if (!pipe) return result;
        char buf[256];
        while (std::fgets(buf, sizeof(buf), pipe)) result += buf;
        ::_pclose(pipe);
        while (!result.empty() && (result.back() == '\n' ||
               result.back() == '\r' || result.back() == ' '))
            result.pop_back();
        return result;
    }

    bool PathExists(const std::string& path)
    {
        return ::GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES;
    }

    std::string FindAdbPath()
    {
        auto found = RunCommand("where adb.exe 2>nul");
        if (!found.empty())
        {
            auto nl = found.find('\n');
            if (nl != std::string::npos) found.resize(nl);
            while (!found.empty() && (found.back() == '\r' || found.back() == ' '))
                found.pop_back();
            if (PathExists(found)) return found;
        }
        char localApp[MAX_PATH]{};
        if (::GetEnvironmentVariableA("LOCALAPPDATA", localApp, MAX_PATH) > 0)
        {
            std::string p = std::string(localApp) + "\\Android\\Sdk\\platform-tools\\adb.exe";
            if (PathExists(p)) return p;
        }
        char exePath[MAX_PATH]{};
        if (::GetModuleFileNameA(nullptr, exePath, MAX_PATH) > 0)
        {
            std::string dir(exePath);
            auto slash = dir.find_last_of("\\/");
            if (slash != std::string::npos) dir.resize(slash + 1);
            std::string beside = dir + "adb.exe";
            if (PathExists(beside)) return beside;
        }
        return {};
    }

    bool RunAdb(const std::string& adbPath, const std::string& args)
    {
        std::string cmd = "\"" + adbPath + "\" " + args + " 2>nul";
        return ::system(cmd.c_str()) == 0;
    }

    std::string RunAdbOutput(const std::string& adbPath, const std::string& args)
    {
        return RunCommand("\"" + adbPath + "\" " + args + " 2>nul");
    }

    bool QueryTabletResolution(const std::string& adbPath, DWORD& outW, DWORD& outH)
    {
        // Ask the tablet for its physical screen size via wm size
        auto raw = RunAdbOutput(adbPath, "shell wm size");
        // Output looks like: "Physical size: 2000x1200"
        // If overridden:     "Physical size: 2000x1200\nOverride size: ..."
        // We want the Physical size line
        auto pos = raw.find("Physical size:");
        if (pos == std::string::npos)
        {
            // Fallback: try parsing any "NNNNxNNNN" pattern
            pos = 0;
        }
        else
        {
            pos += 14; // skip "Physical size:"
        }
        // Find digits x digits
        unsigned tw = 0, th = 0;
        while (pos < raw.size() && !std::isdigit(static_cast<unsigned char>(raw[pos]))) ++pos;
        while (pos < raw.size() && std::isdigit(static_cast<unsigned char>(raw[pos])))
        {
            tw = tw * 10 + (raw[pos] - '0');
            ++pos;
        }
        if (pos < raw.size() && raw[pos] == 'x') ++pos;
        while (pos < raw.size() && std::isdigit(static_cast<unsigned char>(raw[pos])))
        {
            th = th * 10 + (raw[pos] - '0');
            ++pos;
        }
        if (tw < 100 || th < 100) return false;

        // Ensure landscape: wider dimension is width
        outW = static_cast<DWORD>((tw >= th) ? tw : th);
        outH = static_cast<DWORD>((tw >= th) ? th : tw);
        std::fprintf(stderr, "[adb] Tablet resolution: %lux%lu\n", outW, outH);
        return true;
    }

    bool TabletAttached(const std::string& adbPath)
    {
        return RunAdbOutput(adbPath, "devices").find("\tdevice") != std::string::npos;
    }

    bool SetupAdbTunnels(const std::string& adbPath)
    {
        std::fprintf(stderr, "[adb] Starting ADB server...\n");
        RunAdb(adbPath, "kill-server");
        ::Sleep(1500);
        RunAdb(adbPath, "start-server");
        ::Sleep(1000);

        std::fprintf(stderr, "[adb] Looking for tablets...\n");
        if (!TabletAttached(adbPath))
        {
            std::fprintf(stderr, "[adb] No tablet detected.\n");
            return false;
        }
        std::fprintf(stderr, "[adb] Tablet found! Setting up tunnels...\n");
        RunAdb(adbPath, "reverse --remove-all");
        if (!RunAdb(adbPath, "reverse tcp:27183 tcp:27183")) return false;
        if (!RunAdb(adbPath, "reverse tcp:27184 tcp:27184")) return false;
        std::fprintf(stderr, "[adb] Tunnels ready\n");
        return true;
    }

    // singleTask: brings a running instance to the front instead of restarting it
    void LaunchTabletApp(const std::string& adbPath)
    {
        RunAdb(adbPath, "shell am start -n com.legion.tablink/.MainActivity");
    }

    bool TunnelsActive(const std::string& adbPath)
    {
        auto list = RunAdbOutput(adbPath, "reverse --list");
        return list.find("tcp:27183") != std::string::npos && list.find("tcp:27184") != std::string::npos;
    }
}

// ═══════════════════════════════════════════════════════════════════
//  Global State
// ═══════════════════════════════════════════════════════════════════
static HINSTANCE g_hInst        = nullptr;
static HWND     g_mainWnd      = nullptr;
static HWND     g_btnStart     = nullptr;
static HWND     g_lblStatus    = nullptr;
static HWND     g_logEdit      = nullptr;
static HFONT    g_fontTitle    = nullptr;
static HFONT    g_fontSub      = nullptr;
static HFONT    g_fontBody     = nullptr;
static HFONT    g_fontMono     = nullptr;
static HFONT    g_fontBold     = nullptr;
static HBRUSH   g_bgBrush     = nullptr;
static HBRUSH   g_headerBrush = nullptr;
static HBRUSH   g_cardBrush   = nullptr;
static HBRUSH   g_logBrush    = nullptr;
static NOTIFYICONDATAW g_nid   = {};
static bool     g_trayActive   = false;

static const UiStrings* g_lang = &g_strEN;
static bool     g_isSpanish   = false;
static HWND     g_btnLang     = nullptr;
static HWND     g_lblHowTo    = nullptr;
static HWND     g_lblSteps    = nullptr;
static HWND     g_lblLog      = nullptr;
static COLORREF g_dotColor    = 0;
static bool     g_btnHover    = false;
static bool     g_langHover   = false;
static ULONG_PTR g_gdipToken  = 0;

static std::atomic<bool> g_serverRunning{false};
static std::atomic<bool> g_stopRequested{false};
static std::thread       g_serverThread;
static std::thread       g_vddPingThread;
static std::thread       g_logReader;

static HANDLE g_vddDevice     = INVALID_HANDLE_VALUE;
static int    g_vddDisplayIdx = -1;
static int    g_pipeRd        = -1;
static int    g_pipeWr        = -1;

static DWORD g_width   = DefaultWidth;
static DWORD g_height  = DefaultHeight;
static DWORD g_fps     = DefaultFps;
static DWORD g_bitrate = DefaultBitrateMbps;

// Quality presets: Smooth / Balanced / Sharp
struct QualityPreset { DWORD fps; DWORD bitrateMbps; };
static constexpr QualityPreset kQualityPresets[] = { {90, 35}, {60, 30}, {60, 50} };
static DWORD g_quality = 1;
static bool  g_restartPending = false;
static constexpr const wchar_t* kSettingsKey = L"Software\\GLExtraScreen";

static void ApplyQuality(DWORD q)
{
    g_quality = q < 3 ? q : 1;
    g_fps     = kQualityPresets[g_quality].fps;
    g_bitrate = kQualityPresets[g_quality].bitrateMbps;
}

static void LoadQuality()
{
    DWORD value = 1, size = sizeof(value);
    if (::RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"Quality", RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS)
        value = 1;
    ApplyQuality(value);
}

static void SaveQuality()
{
    ::RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, L"Quality", REG_DWORD, &g_quality, sizeof(g_quality));
}

// ═══════════════════════════════════════════════════════════════════
//  Stderr → GUI Log Redirect
// ═══════════════════════════════════════════════════════════════════
static void SetupStderrRedirect()
{
    // Allocate a hidden console so _popen / system / fprintf work
    ::AllocConsole();
    HWND hCon = ::GetConsoleWindow();
    if (hCon) ::ShowWindow(hCon, SW_HIDE);

    int fds[2];
    if (_pipe(fds, 8192, _O_BINARY) == 0)
    {
        g_pipeRd = fds[0];
        g_pipeWr = fds[1];
        _dup2(fds[1], _fileno(stderr));
        setvbuf(stderr, nullptr, _IONBF, 0);
    }
}

static void LogReaderLoop()
{
    char raw[2048];
    std::string leftover;
    while (true)
    {
        int n = _read(g_pipeRd, raw, sizeof(raw) - 1);
        if (n <= 0) break;
        raw[n] = '\0';
        leftover += raw;
        size_t start = 0;
        for (size_t i = 0; i < leftover.size(); ++i)
        {
            if (leftover[i] == '\n')
            {
                std::string line = leftover.substr(start, i - start);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (!line.empty() && g_mainWnd && ::IsWindow(g_mainWnd))
                {
                    char* copy = _strdup(line.c_str());
                    ::PostMessageA(g_mainWnd, WM_SERVERLOG, 0,
                        reinterpret_cast<LPARAM>(copy));
                }
                start = i + 1;
            }
        }
        leftover = leftover.substr(start);
    }
}

enum StatusKind : WPARAM { KindIdle, KindWarn, KindOk, KindLive };
static const wchar_t* const kKindNames[] = {L"idle", L"warn", L"ok", L"live"};
static StatusKind   g_statusKind = KindIdle;
static std::wstring g_statusText;

static void SetStatus(const wchar_t* text, COLORREF dot = 0)
{
    if (dot != 0) g_dotColor = dot;
    if (g_lblStatus && ::IsWindow(g_lblStatus))
        ::SetWindowTextW(g_lblStatus, text);
    if (g_mainWnd && ::IsWindow(g_mainWnd))
    {
        RECT dotRect = {20, 329, 44, 353};
        ::InvalidateRect(g_mainWnd, &dotRect, FALSE);

        StatusKind kind = text == g_lang->connected   ? KindLive
                        : g_dotColor == Theme::DotOk   ? KindOk
                        : g_dotColor == Theme::DotWarn ? KindWarn
                        : KindIdle;
        ::PostMessageW(g_mainWnd, WM_STATUSTEXT, kind, reinterpret_cast<LPARAM>(_wcsdup(text)));
    }
}

// ═══════════════════════════════════════════════════════════════════
//  WebView2 UI bridge (falls back to the GDI controls if unavailable)
// ═══════════════════════════════════════════════════════════════════
static ComPtr<ICoreWebView2Controller> g_wvController;
static ComPtr<ICoreWebView2>           g_webview;
static bool                            g_webReady = false;
static std::vector<std::wstring>       g_logHistory;

static std::wstring JsonString(const std::wstring& s)
{
    std::wstring out = L"\"";
    for (wchar_t c : s)
    {
        switch (c)
        {
        case L'"':  out += L"\\\""; break;
        case L'\\': out += L"\\\\"; break;
        case L'\n': out += L"\\n";  break;
        case L'\r': break;
        case L'\t': out += L"\\t";  break;
        default:
            if (c < 0x20) { wchar_t b[8]; swprintf(b, 8, L"\\u%04x", c); out += b; }
            else out += c;
        }
    }
    return out + L"\"";
}

static std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty()) return {};
    int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

static void WebPost(const std::wstring& json)
{
    if (g_webview && g_webReady) g_webview->PostWebMessageAsJson(json.c_str());
}

static void WebSendState()
{
    wchar_t buf[256];
    swprintf(buf, 256, L"{\"t\":\"state\",\"lang\":\"%s\",\"running\":%s,\"w\":%lu,\"h\":%lu,\"fps\":%lu,\"br\":%lu,\"q\":%lu}",
        g_isSpanish ? L"es" : L"en", g_serverRunning.load() ? L"true" : L"false",
        g_width, g_height, g_fps, g_bitrate, g_quality);
    WebPost(buf);
}

static void WebSendStatus()
{
    WebPost(L"{\"t\":\"status\",\"kind\":\"" + std::wstring(kKindNames[g_statusKind]) +
            L"\",\"text\":" + JsonString(g_statusText) + L"}");
}

static void WebSendLog(const std::wstring& line)
{
    WebPost(L"{\"t\":\"log\",\"line\":" + JsonString(line) + L"}");
}

static std::wstring LoadUiHtml()
{
    HRSRC res = ::FindResourceW(g_hInst, MAKEINTRESOURCEW(IDR_UI_HTML), MAKEINTRESOURCEW(10)); // RT_RCDATA
    if (!res) return {};
    HGLOBAL data = ::LoadResource(g_hInst, res);
    if (!data) return {};
    const char* bytes = static_cast<const char*>(::LockResource(data));
    return Utf8ToWide(std::string(bytes, ::SizeofResource(g_hInst, res)));
}

static void WebResize()
{
    if (!g_wvController) return;
    RECT rc; ::GetClientRect(g_mainWnd, &rc);
    g_wvController->put_Bounds(rc);
}

static BOOL CALLBACK HideLegacyControl(HWND child, LPARAM)
{
    wchar_t cls[32]{};
    ::GetClassNameW(child, cls, 32);
    if (!lstrcmpiW(cls, L"STATIC") || !lstrcmpiW(cls, L"BUTTON") || !lstrcmpiW(cls, L"EDIT"))
        ::ShowWindow(child, SW_HIDE);
    return TRUE;
}

static void HandleWebMessage(const std::wstring& msg)
{
    if (msg == L"ready")
    {
        g_webReady = true;
        WebSendState();
        for (const auto& line : g_logHistory) WebSendLog(line);
        if (!g_statusText.empty()) WebSendStatus();
    }
    else if (msg == L"toggle")
    {
        ::SendMessageW(g_mainWnd, WM_COMMAND, MAKEWPARAM(IDC_BTN_START, BN_CLICKED), 0);
    }
    else if (msg == L"lang:es" || msg == L"lang:en")
    {
        if ((msg == L"lang:es") != g_isSpanish)
            ::SendMessageW(g_mainWnd, WM_COMMAND, MAKEWPARAM(IDC_BTN_LANG, BN_CLICKED), 0);
    }
    else if (msg.size() == 9 && msg.rfind(L"quality:", 0) == 0)
    {
        DWORD q = static_cast<DWORD>(msg[8] - L'0');
        if (q > 2 || q == g_quality) return;
        ApplyQuality(q);
        SaveQuality();
        std::fprintf(stderr, "[ui] Quality -> %lu FPS / %lu Mbps\n", g_fps, g_bitrate);
        if (g_serverRunning.load() && !g_stopRequested.load())
        {
            g_restartPending = true;
            ::SendMessageW(g_mainWnd, WM_COMMAND, MAKEWPARAM(IDC_BTN_START, BN_CLICKED), 0);
        }
        WebSendState();
    }
    else if (msg == L"open:github")
    {
        ::ShellExecuteW(nullptr, L"open", L"https://github.com/glucarelli9/LegionTabLink", nullptr, nullptr, SW_SHOWNORMAL);
    }
    else if (msg.rfind(L"copy:", 0) == 0)
    {
        std::wstring text = msg.substr(5);
        if (::OpenClipboard(g_mainWnd))
        {
            ::EmptyClipboard();
            size_t bytes = (text.size() + 1) * sizeof(wchar_t);
            if (HGLOBAL mem = ::GlobalAlloc(GMEM_MOVEABLE, bytes))
            {
                std::memcpy(::GlobalLock(mem), text.c_str(), bytes);
                ::GlobalUnlock(mem);
                ::SetClipboardData(CF_UNICODETEXT, mem);
            }
            ::CloseClipboard();
        }
    }
}

static void InitWebUi(HWND hwnd)
{
    wchar_t dataDir[MAX_PATH]{};
    ::ExpandEnvironmentStringsW(L"%LOCALAPPDATA%\\GLExtraScreen\\WebView2", dataDir, MAX_PATH);

    ::CreateCoreWebView2EnvironmentWithOptions(nullptr, dataDir, nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
        [hwnd](HRESULT hr, ICoreWebView2Environment* env) -> HRESULT
    {
        if (FAILED(hr) || !env)
        {
            std::fprintf(stderr, "[ui] WebView2 unavailable (0x%08lx), using classic UI\n", hr);
            return S_OK;
        }
        return env->CreateCoreWebView2Controller(hwnd,
            Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
            [hwnd](HRESULT hr2, ICoreWebView2Controller* controller) -> HRESULT
        {
            if (FAILED(hr2) || !controller)
            {
                std::fprintf(stderr, "[ui] WebView2 controller failed (0x%08lx)\n", hr2);
                return S_OK;
            }
            g_wvController = controller;
            g_wvController->get_CoreWebView2(&g_webview);

            ComPtr<ICoreWebView2Controller2> c2;
            if (SUCCEEDED(g_wvController.As(&c2)))
                c2->put_DefaultBackgroundColor(COREWEBVIEW2_COLOR{255, 9, 9, 15});

            ComPtr<ICoreWebView2Settings> settings;
            g_webview->get_Settings(&settings);
            settings->put_AreDevToolsEnabled(FALSE);
            settings->put_IsStatusBarEnabled(FALSE);
            settings->put_IsZoomControlEnabled(FALSE);
            ComPtr<ICoreWebView2Settings3> s3;
            if (SUCCEEDED(settings.As(&s3))) s3->put_AreBrowserAcceleratorKeysEnabled(FALSE);

            g_webview->add_WebMessageReceived(
                Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT
            {
                LPWSTR raw = nullptr;
                if (SUCCEEDED(args->TryGetWebMessageAsString(&raw)) && raw)
                {
                    HandleWebMessage(raw);
                    ::CoTaskMemFree(raw);
                }
                return S_OK;
            }).Get(), nullptr);

            // Keep any navigation inside the embedded page
            g_webview->add_NewWindowRequested(
                Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                [](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT
            {
                args->put_Handled(TRUE);
                return S_OK;
            }).Get(), nullptr);

            WebResize();
            ::EnumChildWindows(hwnd, HideLegacyControl, 0);
            g_webview->NavigateToString(LoadUiHtml().c_str());
            return S_OK;
        }).Get());
    }).Get());
}

// ═══════════════════════════════════════════════════════════════════
//  VDD Ping Thread (keeps virtual display alive)
// ═══════════════════════════════════════════════════════════════════
static void VddPingLoop()
{
    while (!g_stopRequested.load())
    {
        if (g_vddDevice != INVALID_HANDLE_VALUE)
            VddDriver::Update(g_vddDevice);
        ::Sleep(100);
    }
}

// ═══════════════════════════════════════════════════════════════════
//  Display Configuration
// ═══════════════════════════════════════════════════════════════════
static bool ConfigureVddPosition(const std::wstring& vddName,
                                  DWORD w, DWORD h, DWORD hz)
{
    LONG rightEdge = 0;
    DISPLAY_DEVICEW dd{};
    dd.cb = sizeof(dd);
    for (DWORD j = 0; ::EnumDisplayDevicesW(nullptr, j, &dd, 0); ++j)
    {
        if (std::wstring(dd.DeviceName) == vddName) { dd.cb = sizeof(dd); continue; }
        DEVMODEW mode{}; mode.dmSize = sizeof(mode);
        if (::EnumDisplaySettingsW(dd.DeviceName, ENUM_CURRENT_SETTINGS, &mode))
        {
            LONG r = mode.dmPosition.x + static_cast<LONG>(mode.dmPelsWidth);
            if (r > rightEdge) rightEdge = r;
        }
        dd.cb = sizeof(dd);
    }
    if (rightEdge == 0) rightEdge = 1920;

    std::fprintf(stderr, "[vdd] Extending at X=%ld\n", rightEdge);

    DEVMODEW dm{};
    dm.dmSize             = sizeof(dm);
    dm.dmPelsWidth        = w;
    dm.dmPelsHeight       = h;
    dm.dmBitsPerPel       = 32;
    dm.dmDisplayFrequency = hz;
    dm.dmPosition.x       = rightEdge;
    dm.dmPosition.y       = 0;
    dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL |
                  DM_DISPLAYFREQUENCY | DM_POSITION;

    LONG res = ::ChangeDisplaySettingsExW(vddName.c_str(), &dm,
        nullptr, CDS_UPDATEREGISTRY | CDS_NORESET, nullptr);
    if (res == DISP_CHANGE_SUCCESSFUL)
    {
        ::ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr);
        std::fprintf(stderr, "[vdd] Configured %lux%lu @%luHz at X=%ld\n", w, h, hz, rightEdge);
        return true;
    }
    std::fprintf(stderr, "[vdd] ChangeDisplaySettingsExW error %ld\n", res);
    return false;
}

// Check whether a specific resolution is already available on a display
static bool DisplayModeExists(const std::wstring& deviceName, DWORD w, DWORD h, DWORD hz)
{
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    for (DWORD i = 0; ::EnumDisplaySettingsW(deviceName.c_str(), i, &dm); ++i)
    {
        if (dm.dmPelsWidth == w && dm.dmPelsHeight == h && dm.dmDisplayFrequency == hz)
            return true;
        dm.dmSize = sizeof(dm);
    }
    return false;
}

// ═══════════════════════════════════════════════════════════════════
//  Server Thread
// ═══════════════════════════════════════════════════════════════════
static void ServerMain()
{
    g_serverRunning = true;
    if (g_mainWnd && ::IsWindow(g_mainWnd))
        ::PostMessageW(g_mainWnd, WM_STATUSUPDATE, 0, 0);

    DWORD w  = g_width, h = g_height;
    DWORD fps = g_fps, br = g_bitrate;

    auto cleanupVdd = [&]()
    {
        g_stopRequested = true; // ensure ping thread stops
        if (g_vddPingThread.joinable()) g_vddPingThread.join();
        if (g_vddDisplayIdx >= 0 && g_vddDevice != INVALID_HANDLE_VALUE)
        {
            VddDriver::RemoveDisplay(g_vddDevice, g_vddDisplayIdx);
            std::fprintf(stderr, "[vdd] Display removed\n");
            g_vddDisplayIdx = -1;
        }
        if (g_vddDevice != INVALID_HANDLE_VALUE)
        {
            ::CloseHandle(g_vddDevice);
            g_vddDevice = INVALID_HANDLE_VALUE;
        }
    };

    auto finish = [&]()
    {
        cleanupVdd();
        g_serverRunning = false;
        g_stopRequested = false;
        if (g_mainWnd && ::IsWindow(g_mainWnd))
            ::PostMessageW(g_mainWnd, WM_STATUSUPDATE, 0, 0);
    };

    // ── ADB ──
    SetStatus(g_lang->findingAdb, Theme::DotWarn);
    auto adbPath = FindAdbPath();
    if (adbPath.empty())
    {
        std::fprintf(stderr, "[server] ERROR: adb.exe not found!\n");
        ::MessageBoxW(g_mainWnd, g_lang->errNoAdb,
            L"GL ExtraScreen", MB_OK | MB_ICONERROR);
        finish(); return;
    }
    std::fprintf(stderr, "[adb] Found: %s\n", adbPath.c_str());

    SetStatus(g_lang->connectingTablet, Theme::DotWarn);
    if (!SetupAdbTunnels(adbPath))
    {
        ::MessageBoxW(g_mainWnd, g_lang->errNoTablet,
            L"GL ExtraScreen", MB_OK | MB_ICONWARNING);
        finish(); return;
    }

    // Query tablet's native resolution and use it
    {
        DWORD tabW = 0, tabH = 0;
        if (QueryTabletResolution(adbPath, tabW, tabH))
        {
            w  = tabW;
            h  = tabH;
            g_width  = tabW;
            g_height = tabH;
            std::fprintf(stderr, "[server] Using tablet resolution: %lux%lu\n", w, h);
        }
        else
        {
            std::fprintf(stderr, "[server] Could not query tablet resolution, using default %lux%lu\n", w, h);
        }
    }

    // ── VDD Driver ──
    SetStatus(g_lang->creatingDisplay, Theme::DotWarn);
    g_vddDevice = VddDriver::OpenAdapter();
    if (g_vddDevice == INVALID_HANDLE_VALUE)
    {
        std::fprintf(stderr, "[vdd] ParsecVDD driver not found!\n");
        ::MessageBoxW(g_mainWnd, g_lang->errNoVdd,
            L"GL ExtraScreen", MB_OK | MB_ICONERROR);
        finish(); return;
    }
    std::fprintf(stderr, "[vdd] Driver opened\n");

    g_vddDisplayIdx = VddDriver::AddDisplay(g_vddDevice);
    if (g_vddDisplayIdx < 0)
    {
        std::fprintf(stderr, "[vdd] Failed to add display!\n");
        finish(); return;
    }
    std::fprintf(stderr, "[vdd] Display created (index %d)\n", g_vddDisplayIdx);

    // Start keep-alive ping
    g_vddPingThread = std::thread(VddPingLoop);

    // Wait for the display to appear in Windows
    SetStatus(g_lang->waitingDisplay, Theme::DotWarn);
    std::wstring vddName;
    for (int attempt = 0; attempt < 30 && !g_stopRequested; ++attempt)
    {
        ::Sleep(500);
        DISPLAY_DEVICEW dd{}; dd.cb = sizeof(dd);
        for (DWORD i = 0; ::EnumDisplayDevicesW(nullptr, i, &dd, 0); ++i)
        {
            std::wstring desc(dd.DeviceString);
            for (auto& c : desc) c = ::towlower(c);
            if (desc.find(L"parsec") != std::wstring::npos ||
                desc.find(L"virtual") != std::wstring::npos ||
                desc.find(L"vdd")     != std::wstring::npos)
            {
                vddName = dd.DeviceName;
                break;
            }
            dd.cb = sizeof(dd);
        }
        if (!vddName.empty()) break;
    }
    if (g_stopRequested) { finish(); return; }
    if (vddName.empty())
    {
        std::fprintf(stderr, "[vdd] Display did not appear in time.\n");
        finish(); return;
    }
    std::fprintf(stderr, "[vdd] Appeared as %ls\n", vddName.c_str());

    // Both orientations must exist so the tablet can rotate without re-creating the display
    if (!DisplayModeExists(vddName, w, h, fps) || !DisplayModeExists(vddName, h, w, fps))
    {
        std::fprintf(stderr, "[vdd] Modes %lux%lu / %lux%lu @%lu not available "
            "— registering custom modes...\n", w, h, h, w, fps);

        // Remove the display, register custom modes, re-add
        VddDriver::RemoveDisplay(g_vddDevice, g_vddDisplayIdx);
        g_vddDisplayIdx = -1;
        ::Sleep(500);

        VddDriver::RegisterCustomMode(0, w, h, fps);
        VddDriver::RegisterCustomMode(1, h, w, fps);

        g_vddDisplayIdx = VddDriver::AddDisplay(g_vddDevice);
        if (g_vddDisplayIdx < 0)
        {
            std::fprintf(stderr, "[vdd] Failed to re-add display!\n");
            finish(); return;
        }
        std::fprintf(stderr, "[vdd] Display re-created (index %d)\n",
            g_vddDisplayIdx);

        // Wait for the display to re-appear
        vddName.clear();
        for (int attempt = 0; attempt < 30 && !g_stopRequested; ++attempt)
        {
            ::Sleep(500);
            DISPLAY_DEVICEW dd{}; dd.cb = sizeof(dd);
            for (DWORD i = 0; ::EnumDisplayDevicesW(nullptr, i, &dd, 0); ++i)
            {
                std::wstring desc(dd.DeviceString);
                for (auto& c : desc) c = ::towlower(c);
                if (desc.find(L"parsec") != std::wstring::npos ||
                    desc.find(L"virtual") != std::wstring::npos ||
                    desc.find(L"vdd")     != std::wstring::npos)
                {
                    vddName = dd.DeviceName;
                    break;
                }
                dd.cb = sizeof(dd);
            }
            if (!vddName.empty()) break;
        }
        if (g_stopRequested) { finish(); return; }
        if (vddName.empty())
        {
            std::fprintf(stderr, "[vdd] Display did not re-appear.\n");
            finish(); return;
        }
        std::fprintf(stderr, "[vdd] Re-appeared as %ls\n", vddName.c_str());
    }
    else
    {
        std::fprintf(stderr, "[vdd] Resolution %lux%lu already "
            "available — no custom mode needed\n", w, h);
    }

    // Configure resolution + extend to the right
    SetStatus(g_lang->configuringDisplay, Theme::DotWarn);
    ConfigureVddPosition(vddName, w, h, fps);
    ::Sleep(2000);
    if (g_stopRequested) { finish(); return; }

    // ── DXGI Capture ──
    SetStatus(g_lang->startingCapture, Theme::DotWarn);
    WSADATA wsa{};
    if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        std::fprintf(stderr, "[server] WSAStartup failed\n");
        finish(); return;
    }

    DxgiCapture capture;
    const wchar_t* preferDev = vddName.empty() ? nullptr : vddName.c_str();
    bool started = capture.Start(w, h, preferDev);
    if (!started)
    {
        std::fprintf(stderr, "[dxgi] Waiting for virtual display in DXGI...\n");
        for (int a = 0; a < 60 && !g_stopRequested; ++a)
        {
            ::Sleep(2000);
            if (capture.Start(w, h, preferDev)) { started = true; break; }
        }
    }
    if (!started || g_stopRequested)
    {
        ::WSACleanup();
        finish(); return;
    }
    std::fprintf(stderr, "[dxgi] Capture active %ux%u\n", capture.Width(), capture.Height());

    auto encoder = CreateEncoderForAdapterVendor(
        capture.VendorId(), capture.Device(),
        capture.Width(), capture.Height(), fps, br * 1000u * 1000u);
    if (!encoder)
    {
        capture.Stop(); ::WSACleanup(); finish(); return;
    }

    RECT monRect{};
    capture.GetMonitorRect(monRect);
    std::fprintf(stderr, "[inject] monRect = {left=%ld, top=%ld, right=%ld, bottom=%ld}\n",
        monRect.left, monRect.top, monRect.right, monRect.bottom);

    PenTouchInjector injector;
    if (!injector.Start(monRect))
    {
        encoder->Shutdown(); capture.Stop(); ::WSACleanup(); finish(); return;
    }

    AdbSocketServer server;
    AdbSocketServer::StreamConfig cfg{};
    cfg.width = capture.Width();
    cfg.height = capture.Height();
    cfg.fps = fps;
    cfg.bitrateMbps = br;
    cfg.codec = LtlVideoCodecHevc;

    if (!server.Start(LTL_VIDEO_TCP_PORT, LTL_INPUT_TCP_PORT, cfg,
            [&injector](const LtlInputEventPayload& ev) { injector.Inject(ev); }))
    {
        injector.Stop(); encoder->Shutdown(); capture.Stop();
        ::WSACleanup(); finish(); return;
    }

    LaunchTabletApp(adbPath);
    std::fprintf(stderr, "[server] Tablet app launched\n");
    SetStatus(g_lang->streamingWait, Theme::DotOk);
    std::fprintf(stderr, "[server] Streaming on tcp:%u / tcp:%u\n",
        static_cast<unsigned>(LTL_VIDEO_TCP_PORT),
        static_cast<unsigned>(LTL_INPUT_TCP_PORT));

    // ── Frame Loop ──
    std::vector<uint8_t> bits;
    bool prevConn = false;
    DWORD noFrame = 0, sinceIdr = 0;
    const DWORD IdrInt = fps * 2;
    DWORD disconnTicks = 0;
    DWORD monitorRefreshTicks = 0;
    constexpr DWORD ReconnectIntervalMs = 2000;
    constexpr DWORD MonitorRefreshMs    = 3000;

    auto nudge = [&monRect]() {
        LONG x = monRect.left, y = monRect.top;
        ::SetCursorPos(x, y); ::Sleep(1);
        ::SetCursorPos(x + 1, y); ::Sleep(1);
        ::SetCursorPos(x, y);
    };

    // A USB replug drops the reverse tunnels; the tablet app retries on its own once they exist.
    auto tryReconnect = [&adbPath]() -> bool {
        if (!TabletAttached(adbPath) || TunnelsActive(adbPath)) return false;
        std::fprintf(stderr, "[reconnect] Tablet detected, re-establishing tunnels...\n");
        RunAdb(adbPath, "reverse --remove-all");
        if (!RunAdb(adbPath, "reverse tcp:27183 tcp:27183")) return false;
        if (!RunAdb(adbPath, "reverse tcp:27184 tcp:27184")) return false;
        std::fprintf(stderr, "[reconnect] Tunnels restored, bringing app to front\n");
        LaunchTabletApp(adbPath);
        return true;
    };

    // Switch the virtual display between landscape/portrait in place and rebuild capture + encoder.
    auto rotateTo = [&](uint32_t reqW, uint32_t reqH) -> bool {
        const DWORD longSide = w > h ? w : h, shortSide = w > h ? h : w;
        const bool portrait = reqH > reqW;
        const DWORD nw = portrait ? shortSide : longSide;
        const DWORD nh = portrait ? longSide : shortSide;
        std::fprintf(stderr, "[rotate] Switching virtual display to %lux%lu\n", nw, nh);

        encoder->Shutdown();
        encoder.reset();
        capture.Stop();
        ConfigureVddPosition(vddName, nw, nh, fps);

        bool ok = false;
        for (int attempt = 0; attempt < 24 && !g_stopRequested; ++attempt)
        {
            ::Sleep(250);
            if (capture.Start(nw, nh, preferDev) && capture.Width() == nw && capture.Height() == nh) { ok = true; break; }
            capture.Stop();
        }
        if (!ok && !capture.Start(nw, nh, preferDev))
        {
            std::fprintf(stderr, "[rotate] Capture could not restart\n");
            return false;
        }
        encoder = CreateEncoderForAdapterVendor(capture.VendorId(), capture.Device(),
            capture.Width(), capture.Height(), fps, br * 1000u * 1000u);
        if (!encoder)
        {
            std::fprintf(stderr, "[rotate] Encoder could not restart\n");
            return false;
        }
        capture.GetMonitorRect(monRect);
        injector.UpdateBounds(monRect);
        cfg.width = capture.Width();
        cfg.height = capture.Height();
        server.UpdateStreamConfig(cfg);
        sinceIdr = 0;
        std::fprintf(stderr, "[rotate] Done: %ux%u\n", capture.Width(), capture.Height());
        return true;
    };

    while (!g_stopRequested.load())
    {
        uint32_t reqW = 0, reqH = 0;
        if (server.ConsumeResizeRequest(reqW, reqH) && !rotateTo(reqW, reqH))
        {
            g_stopRequested = true;
            break;
        }

        if (!server.IsVideoConnected())
        {
            if (prevConn) {
                prevConn = false; noFrame = 0; disconnTicks = 0;
                SetStatus(g_lang->streamingWait, Theme::DotWarn);
                std::fprintf(stderr, "[server] Tablet disconnected\n");
            }
            disconnTicks += 50;
            if (disconnTicks >= ReconnectIntervalMs)
            {
                disconnTicks = 0;
                tryReconnect();
            }
            ::Sleep(50); continue;
        }
        disconnTicks = 0;

        if (!prevConn) {
            prevConn = true; noFrame = 0; monitorRefreshTicks = 0;
            SetStatus(g_lang->connected, Theme::DotOk);
            std::fprintf(stderr, "[server] Tablet connected!\n");
            nudge();
        }

        monitorRefreshTicks += 11;
        if (monitorRefreshTicks >= MonitorRefreshMs)
        {
            monitorRefreshTicks = 0;
            if (capture.RefreshMonitorRect())
            {
                capture.GetMonitorRect(monRect);
                injector.UpdateBounds(monRect);
                std::fprintf(stderr, "[inject] Bounds refreshed: {%ld,%ld,%ld,%ld}\n",
                    monRect.left, monRect.top, monRect.right, monRect.bottom);
            }
        }

        ID3D11Texture2D* frame = nullptr;
        if (capture.AcquireFrame(&frame, 11))
        {
            noFrame = 0; ++sinceIdr;
            bool wantKey = server.ConsumeIdrRequest();
            if (sinceIdr >= IdrInt) wantKey = true;
            bool isIdr = false;
            if (encoder->EncodeFrame(frame, wantKey, bits, isIdr) && !bits.empty())
            {
                if (isIdr) sinceIdr = 0;
                server.SendVideoFrame(bits.data(), bits.size(), isIdr);
            }
            else if (wantKey) server.ForceIdr();
            capture.ReleaseFrame();
        }
        else
        {
            ++noFrame;
            if (server.ConsumeIdrRequest() || noFrame > 2000) { nudge(); noFrame = 0; }
            ::Sleep(1);
        }
    }

    std::fprintf(stderr, "[server] Shutting down...\n");
    server.Stop();
    injector.Stop();
    if (encoder) encoder->Shutdown();
    capture.Stop();
    ::WSACleanup();
    finish();
}

// ═══════════════════════════════════════════════════════════════════
//  System Tray
// ═══════════════════════════════════════════════════════════════════
static void AddTrayIcon(HWND hwnd)
{
    if (g_trayActive) return;
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize           = sizeof(g_nid);
    g_nid.hWnd             = hwnd;
    g_nid.uID              = 1;
    g_nid.uFlags           = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon            = ::LoadIconW(g_hInst, MAKEINTRESOURCEW(IDI_APPICON));
    wcscpy_s(g_nid.szTip, L"GL ExtraScreen");
    ::Shell_NotifyIconW(NIM_ADD, &g_nid);
    g_trayActive = true;
}

static void RemoveTrayIcon()
{
    if (!g_trayActive) return;
    ::Shell_NotifyIconW(NIM_DELETE, &g_nid);
    g_trayActive = false;
}

static void ShowTrayMenu(HWND hwnd)
{
    POINT pt; ::GetCursorPos(&pt);
    HMENU m = ::CreatePopupMenu();
    ::AppendMenuW(m, MF_STRING, IDM_TRAY_SHOW, L"Show GL ExtraScreen");
    ::AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(m, MF_STRING, IDM_TRAY_EXIT, L"Exit");
    ::SetForegroundWindow(hwnd);
    ::TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    ::DestroyMenu(m);
}

// ═══════════════════════════════════════════════════════════════════
//  Refresh UI text for current language
// ═══════════════════════════════════════════════════════════════════
static void RefreshLanguageUi()
{
    if (g_lblHowTo)  ::SetWindowTextW(g_lblHowTo, g_lang->howToConnect);
    if (g_lblSteps)  ::SetWindowTextW(g_lblSteps, g_lang->stepsText);
    if (g_lblLog)    ::SetWindowTextW(g_lblLog, g_lang->logLabel);
    if (g_btnStart)  ::InvalidateRect(g_btnStart, nullptr, TRUE);
    if (g_btnLang)   ::InvalidateRect(g_btnLang, nullptr, TRUE);
}

// ═══════════════════════════════════════════════════════════════════
//  Window Procedure
// ═══════════════════════════════════════════════════════════════════
static void DrawRoundedRect(Gdiplus::Graphics& g, Gdiplus::Brush* fill, Gdiplus::Pen* outline,
                            int x, int y, int w, int h, int r)
{
    Gdiplus::GraphicsPath path;
    int d = r * 2;
    path.AddArc(x, y, d, d, 180, 90);
    path.AddArc(x + w - d, y, d, d, 270, 90);
    path.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    path.AddArc(x, y + h - d, d, d, 90, 90);
    path.CloseFigure();
    if (fill) g.FillPath(fill, &path);
    if (outline) g.DrawPath(outline, &path);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = ::BeginPaint(hwnd, &ps);
        RECT rc;
        ::GetClientRect(hwnd, &rc);

        Gdiplus::Graphics g(hdc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);

        // Main background
        {
            Gdiplus::SolidBrush bg(Gdiplus::Color(255, 15, 15, 22));
            g.FillRectangle(&bg, 0, 0, rc.right, rc.bottom);
        }

        // Header background with subtle gradient
        {
            Gdiplus::LinearGradientBrush hdr(
                Gdiplus::Point(0, 0), Gdiplus::Point(0, 82),
                Gdiplus::Color(255, 22, 22, 34),
                Gdiplus::Color(255, 18, 18, 28));
            g.FillRectangle(&hdr, 0, 0, rc.right, 82);
        }

        // Separator — thin gradient line
        {
            Gdiplus::LinearGradientBrush sep(
                Gdiplus::Point(20, 82), Gdiplus::Point(rc.right - 20, 82),
                Gdiplus::Color(0, 99, 102, 241),
                Gdiplus::Color(255, 99, 102, 241));
            Gdiplus::Pen sepPen(&sep, 1.0f);
            g.DrawLine(&sepPen, 20, 82, rc.right - 20, 82);
            Gdiplus::LinearGradientBrush sep2(
                Gdiplus::Point(20, 82), Gdiplus::Point(rc.right - 20, 82),
                Gdiplus::Color(255, 99, 102, 241),
                Gdiplus::Color(0, 99, 102, 241));
            Gdiplus::Pen sepPen2(&sep2, 1.0f);
            g.DrawLine(&sepPen2, rc.right / 2, 82, rc.right - 20, 82);
        }

        // Card with border
        {
            Gdiplus::SolidBrush cardBg(Gdiplus::Color(255, 24, 24, 36));
            Gdiplus::Pen cardBorder(Gdiplus::Color(60, 99, 102, 241), 1.0f);
            DrawRoundedRect(g, &cardBg, &cardBorder, 20, 96, rc.right - 40, 228, 10);
        }

        // Status dot with glow
        {
            BYTE r_ = GetRValue(g_dotColor);
            BYTE g_ = GetGValue(g_dotColor);
            BYTE b_ = GetBValue(g_dotColor);

            // Outer glow
            Gdiplus::SolidBrush glow(Gdiplus::Color(30, r_, g_, b_));
            g.FillEllipse(&glow, 22, 337, 22, 22);

            // Inner dot
            Gdiplus::SolidBrush dot(Gdiplus::Color(255, r_, g_, b_));
            g.FillEllipse(&dot, 26, 341, 14, 14);
        }

        ::EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_CTLCOLORSTATIC:
    {
        HDC hdc = reinterpret_cast<HDC>(wp);
        HWND ctl = reinterpret_cast<HWND>(lp);
        ::SetBkMode(hdc, TRANSPARENT);

        RECT ctlRect;
        ::GetWindowRect(ctl, &ctlRect);
        ::ScreenToClient(hwnd, reinterpret_cast<POINT*>(&ctlRect));

        if (ctlRect.top < 82)
        {
            if (ctlRect.top < 36)
                ::SetTextColor(hdc, Theme::TextMain);
            else
                ::SetTextColor(hdc, Theme::TextSub);
            return reinterpret_cast<LRESULT>(g_headerBrush);
        }
        else if (ctlRect.top >= 96 && ctlRect.top < 324)
        {
            if (ctl == g_lblHowTo)
                ::SetTextColor(hdc, Theme::TextAccent);
            else
                ::SetTextColor(hdc, Theme::TextMain);
            return reinterpret_cast<LRESULT>(g_cardBrush);
        }
        else
        {
            if (ctl == g_lblLog)
                ::SetTextColor(hdc, Theme::TextSub);
            else
                ::SetTextColor(hdc, Theme::TextMain);
            return reinterpret_cast<LRESULT>(g_bgBrush);
        }
    }

    case WM_CTLCOLOREDIT:
    {
        HDC hdc = reinterpret_cast<HDC>(wp);
        ::SetBkColor(hdc, Theme::BgLog);
        ::SetTextColor(hdc, Theme::TextLog);
        return reinterpret_cast<LRESULT>(g_logBrush);
    }

    case WM_DRAWITEM:
    {
        auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
        if (dis->CtlID == IDC_BTN_START)
        {
            Gdiplus::Graphics g(dis->hDC);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);

            int w = dis->rcItem.right - dis->rcItem.left;
            int h = dis->rcItem.bottom - dis->rcItem.top;

            // Clear background with parent color
            Gdiplus::SolidBrush clearBr(Gdiplus::Color(255, 15, 15, 22));
            g.FillRectangle(&clearBr, 0, 0, w, h);

            // Gradient button
            Gdiplus::Color c1, c2;
            if (dis->itemState & ODS_SELECTED) {
                c1 = Gdiplus::Color(255, 70, 72, 200);
                c2 = Gdiplus::Color(255, 90, 50, 180);
            } else if (g_btnHover) {
                c1 = Gdiplus::Color(255, 115, 118, 255);
                c2 = Gdiplus::Color(255, 140, 90, 240);
            } else {
                c1 = Gdiplus::Color(255, 99, 102, 241);
                c2 = Gdiplus::Color(255, 120, 70, 220);
            }
            Gdiplus::LinearGradientBrush grad(
                Gdiplus::Point(0, 0), Gdiplus::Point(w, 0), c1, c2);
            DrawRoundedRect(g, &grad, nullptr, 0, 0, w, h, 10);

            // Button text
            Gdiplus::Font font(dis->hDC, g_fontBold);
            Gdiplus::SolidBrush textBr(Gdiplus::Color(255, 255, 255, 255));
            Gdiplus::StringFormat fmt;
            fmt.SetAlignment(Gdiplus::StringAlignmentCenter);
            fmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);
            const wchar_t* btnText = g_serverRunning.load() ? g_lang->btnStop : g_lang->btnStart;
            Gdiplus::RectF rect(0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h));
            g.DrawString(btnText, -1, &font, rect, &fmt, &textBr);
            return TRUE;
        }
        if (dis->CtlID == IDC_BTN_LANG)
        {
            Gdiplus::Graphics g(dis->hDC);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);

            int w = dis->rcItem.right - dis->rcItem.left;
            int h = dis->rcItem.bottom - dis->rcItem.top;

            // Clear with header color
            Gdiplus::SolidBrush clearBr(Gdiplus::Color(255, 22, 22, 34));
            g.FillRectangle(&clearBr, 0, 0, w, h);

            Gdiplus::Color bgCol = g_langHover
                ? Gdiplus::Color(255, 40, 40, 60)
                : Gdiplus::Color(255, 30, 30, 45);
            Gdiplus::SolidBrush bg(bgCol);
            Gdiplus::Pen border(Gdiplus::Color(40, 99, 102, 241), 1.0f);
            DrawRoundedRect(g, &bg, &border, 0, 0, w, h, 6);

            Gdiplus::Font font(dis->hDC, g_fontSub);
            Gdiplus::SolidBrush textBr(Gdiplus::Color(255, 120, 120, 150));
            Gdiplus::StringFormat fmt;
            fmt.SetAlignment(Gdiplus::StringAlignmentCenter);
            fmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);
            const wchar_t* langText = g_isSpanish ? L"ES" : L"EN";
            Gdiplus::RectF rect(0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h));
            g.DrawString(langText, -1, &font, rect, &fmt, &textBr);
            return TRUE;
        }
        break;
    }

    case WM_MOUSEMOVE:
    {
        POINT pt = { LOWORD(lp), HIWORD(lp) };
        RECT btnRc, langRc;
        if (g_btnStart) ::GetWindowRect(g_btnStart, &btnRc);
        if (g_btnLang) ::GetWindowRect(g_btnLang, &langRc);
        ::ScreenToClient(hwnd, reinterpret_cast<POINT*>(&btnRc));
        ::ScreenToClient(hwnd, reinterpret_cast<POINT*>(&btnRc) + 1);
        ::ScreenToClient(hwnd, reinterpret_cast<POINT*>(&langRc));
        ::ScreenToClient(hwnd, reinterpret_cast<POINT*>(&langRc) + 1);
        bool overBtn  = g_btnStart && ::PtInRect(&btnRc, pt);
        bool overLang = g_btnLang && ::PtInRect(&langRc, pt);
        if (overBtn != g_btnHover) {
            g_btnHover = overBtn;
            ::InvalidateRect(g_btnStart, nullptr, FALSE);
        }
        if (overLang != g_langHover) {
            g_langHover = overLang;
            ::InvalidateRect(g_btnLang, nullptr, FALSE);
        }
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
        ::TrackMouseEvent(&tme);
        break;
    }
    case WM_MOUSELEAVE:
        if (g_btnHover)  { g_btnHover = false;  ::InvalidateRect(g_btnStart, nullptr, FALSE); }
        if (g_langHover) { g_langHover = false; ::InvalidateRect(g_btnLang, nullptr, FALSE); }
        break;

    case WM_COMMAND:
        switch (LOWORD(wp))
        {
        case IDC_BTN_START:
            if (!g_serverRunning)
            {
                g_stopRequested = false;
                if (g_serverThread.joinable()) g_serverThread.join();
                g_serverThread = std::thread(ServerMain);
            }
            else
            {
                g_stopRequested = true;
                SetStatus(g_lang->stopping, Theme::DotWarn);
                ::EnableWindow(g_btnStart, FALSE);
            }
            return 0;
        case IDC_BTN_LANG:
            g_isSpanish = !g_isSpanish;
            g_lang = g_isSpanish ? &g_strES : &g_strEN;
            RefreshLanguageUi();
            if (!g_serverRunning.load())
                SetStatus(g_lang->ready, Theme::DotIdle);
            else if (g_statusKind == KindLive)
                SetStatus(g_lang->connected);
            WebSendState();
            return 0;
        case IDM_TRAY_SHOW:
            ::ShowWindow(hwnd, SW_SHOW);
            ::SetForegroundWindow(hwnd);
            RemoveTrayIcon();
            return 0;
        case IDM_TRAY_EXIT:
            g_stopRequested = true;
            ::Sleep(300);
            RemoveTrayIcon();
            ::DestroyWindow(hwnd);
            return 0;
        }
        break;

    case WM_TRAYICON:
        if (lp == WM_LBUTTONDBLCLK)
        {
            ::ShowWindow(hwnd, SW_SHOW);
            ::SetForegroundWindow(hwnd);
            RemoveTrayIcon();
        }
        else if (lp == WM_RBUTTONUP) ShowTrayMenu(hwnd);
        return 0;

    case WM_STATUSTEXT:
    {
        wchar_t* text = reinterpret_cast<wchar_t*>(lp);
        g_statusKind = static_cast<StatusKind>(wp);
        g_statusText = text ? text : L"";
        free(text);
        WebSendStatus();
        return 0;
    }

    case WM_SIZE:
        WebResize();
        break;

    case WM_DPICHANGED:
    {
        const RECT* r = reinterpret_cast<const RECT*>(lp);
        ::SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                       SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }

    case WM_SERVERLOG:
    {
        char* text = reinterpret_cast<char*>(lp);
        if (text)
        {
            std::wstring line = Utf8ToWide(text);
            g_logHistory.push_back(line);
            if (g_logHistory.size() > 400) g_logHistory.erase(g_logHistory.begin());
            WebSendLog(line);
        }
        if (text && g_logEdit && ::IsWindow(g_logEdit))
        {
            int len = ::GetWindowTextLengthA(g_logEdit);
            ::SendMessageA(g_logEdit, EM_SETSEL, len, len);
            ::SendMessageA(g_logEdit, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(text));
            ::SendMessageA(g_logEdit, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>("\r\n"));
            ::SendMessageA(g_logEdit, EM_SCROLLCARET, 0, 0);
        }
        free(text);
        return 0;
    }

    case WM_STATUSUPDATE:
    {
        bool running = g_serverRunning.load();
        if (!running && g_serverThread.joinable()) g_serverThread.join();
        ::InvalidateRect(g_btnStart, nullptr, TRUE);
        ::EnableWindow(g_btnStart, TRUE);
        if (!running) SetStatus(g_lang->ready, Theme::DotIdle);
        WebSendState();
        if (!running && g_restartPending)
        {
            g_restartPending = false;
            ::SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(IDC_BTN_START, BN_CLICKED), 0);
        }
        return 0;
    }

    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_MINIMIZE)
        {
            ::ShowWindow(hwnd, SW_HIDE);
            AddTrayIcon(hwnd);
            return 0;
        }
        break;

    case WM_CLOSE:
        if (g_serverRunning)
        {
            ::ShowWindow(hwnd, SW_HIDE);
            AddTrayIcon(hwnd);
            return 0;
        }
        RemoveTrayIcon();
        ::DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        g_stopRequested = true;
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

// ═══════════════════════════════════════════════════════════════════
//  Entry Point
// ═══════════════════════════════════════════════════════════════════
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int nCmdShow)
{
    g_hInst = hInstance;
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // ── GDI+ init ──
    Gdiplus::GdiplusStartupInput gdipInput;
    Gdiplus::GdiplusStartup(&g_gdipToken, &gdipInput, nullptr);

    LoadQuality();

    // CLI overrides
    for (int i = 1; i < __argc; ++i)
    {
        if (::lstrcmpiA(__argv[i], "--width")   == 0 && i+1 < __argc) g_width   = static_cast<DWORD>(::atoi(__argv[++i]));
        if (::lstrcmpiA(__argv[i], "--height")  == 0 && i+1 < __argc) g_height  = static_cast<DWORD>(::atoi(__argv[++i]));
        if (::lstrcmpiA(__argv[i], "--fps")     == 0 && i+1 < __argc) g_fps     = static_cast<DWORD>(::atoi(__argv[++i]));
        if (::lstrcmpiA(__argv[i], "--bitrate") == 0 && i+1 < __argc) g_bitrate = static_cast<DWORD>(::atoi(__argv[++i]));
    }

    SetupStderrRedirect();

    // ── Language detection ──
    {
        LANGID lid = ::GetUserDefaultUILanguage();
        g_isSpanish = (PRIMARYLANGID(lid) == LANG_SPANISH);
        g_lang = g_isSpanish ? &g_strES : &g_strEN;
    }

    // ── Fonts ──
    g_fontTitle = ::CreateFontW(-28, 0, 0, 0, FW_BOLD,    0,0,0, DEFAULT_CHARSET,0,0, CLEARTYPE_QUALITY, 0, L"Segoe UI Variable Display");
    g_fontSub   = ::CreateFontW(-12, 0, 0, 0, FW_NORMAL,  0,0,0, DEFAULT_CHARSET,0,0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_fontBody  = ::CreateFontW(-14, 0, 0, 0, FW_NORMAL,  0,0,0, DEFAULT_CHARSET,0,0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_fontMono  = ::CreateFontW(-13, 0, 0, 0, FW_NORMAL,  0,0,0, DEFAULT_CHARSET,0,0, CLEARTYPE_QUALITY, 0, L"Cascadia Mono");
    g_fontBold  = ::CreateFontW(-15, 0, 0, 0, FW_SEMIBOLD,0,0,0, DEFAULT_CHARSET,0,0, CLEARTYPE_QUALITY, 0, L"Segoe UI");

    // ── Theme brushes ──
    g_bgBrush     = ::CreateSolidBrush(Theme::BgMain);
    g_headerBrush = ::CreateSolidBrush(RGB(20, 20, 32));
    g_cardBrush   = ::CreateSolidBrush(Theme::BgCard);
    g_logBrush    = ::CreateSolidBrush(Theme::BgLog);
    g_dotColor    = Theme::DotIdle;

    // ── Window class ──
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance      = hInstance;
    wc.hIcon          = ::LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APPICON));
    wc.hCursor        = ::LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW
    wc.hbrBackground  = g_bgBrush;
    wc.lpszClassName  = L"GLExtraScreenWnd";
    wc.hIconSm        = wc.hIcon;
    ::RegisterClassExW(&wc);

    const DWORD winStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    const UINT dpi = ::GetDpiForSystem();
    RECT wr = {0, 0, ::MulDiv(540, dpi, 96), ::MulDiv(720, dpi, 96)};
    ::AdjustWindowRectExForDpi(&wr, winStyle, FALSE, 0, dpi);
    int sw = ::GetSystemMetrics(SM_CXSCREEN), sh = ::GetSystemMetrics(SM_CYSCREEN);
    int ww = wr.right - wr.left, wh = wr.bottom - wr.top;

    g_mainWnd = ::CreateWindowExW(0, L"GLExtraScreenWnd", L"GL ExtraScreen", winStyle,
        (sw - ww) / 2, (sh - wh) / 2, ww, wh,
        nullptr, nullptr, hInstance, nullptr);

    {
        BOOL dark = TRUE;
        ::DwmSetWindowAttribute(g_mainWnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
        COLORREF caption = RGB(9, 9, 15);
        ::DwmSetWindowAttribute(g_mainWnd, 35 /*DWMWA_CAPTION_COLOR*/, &caption, sizeof(caption));
        ::DwmSetWindowAttribute(g_mainWnd, 34 /*DWMWA_BORDER_COLOR*/, &caption, sizeof(caption));
    }

    // ── Helper: create STATIC control ──
    auto mkStatic = [&](const wchar_t* text, int x, int y, int cx, int cy,
                        HFONT font, DWORD style = SS_LEFT) -> HWND
    {
        HWND h = ::CreateWindowExW(0, L"STATIC", text,
            WS_CHILD | WS_VISIBLE | style,
            x, y, cx, cy, g_mainWnd, nullptr, hInstance, nullptr);
        ::SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        return h;
    };

    // ── Header (y 0 – 82) ──
    mkStatic(L"GL ExtraScreen", 28, 18, 360, 30, g_fontTitle);
    mkStatic(L"v1.0 \x2014 Gianfranco Lucarelli", 28, 48, 350, 16, g_fontSub);

    // Language toggle button (top-right)
    g_btnLang = ::CreateWindowExW(0, L"BUTTON", L"",
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
        ww - 88, 20, 44, 26, g_mainWnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_LANG)),
        hInstance, nullptr);

    // ── Card (y 96 – 324) ──
    g_lblHowTo = mkStatic(g_lang->howToConnect, 36, 108, 460, 20, g_fontBold);
    g_lblSteps = mkStatic(g_lang->stepsText, 36, 132, 460, 182, g_fontBody);

    // ── Status (y 338) ──
    g_lblStatus = mkStatic(g_lang->ready, 50, 340, 440, 22, g_fontBold);

    // ── Start button (y 374) ──
    g_btnStart = ::CreateWindowExW(0, L"BUTTON", L"",
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
        24, 374, 490, 44, g_mainWnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_START)),
        hInstance, nullptr);

    // ── Log (y 432+) ──
    g_lblLog = mkStatic(g_lang->logLabel, 28, 432, 100, 16, g_fontSub);

    g_logEdit = ::CreateWindowExW(0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
        24, 452, 490, 700 - 452 - 10, g_mainWnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LOG_EDIT)),
        hInstance, nullptr);
    ::SendMessageW(g_logEdit, WM_SETFONT, reinterpret_cast<WPARAM>(g_fontMono), TRUE);

    // ── Start log reader ──
    g_logReader = std::thread(LogReaderLoop);

    InitWebUi(g_mainWnd);

    ::ShowWindow(g_mainWnd, nCmdShow);
    ::UpdateWindow(g_mainWnd);

    // ── Message loop ──
    MSG msg{};
    while (::GetMessageW(&msg, nullptr, 0, 0))
    {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }

    // ── Cleanup ──
    g_stopRequested = true;
    if (g_serverThread.joinable())  g_serverThread.join();
    if (g_vddPingThread.joinable()) g_vddPingThread.join();

    if (g_pipeWr >= 0) { _close(g_pipeWr); g_pipeWr = -1; }
    if (g_logReader.joinable()) g_logReader.join();
    if (g_pipeRd >= 0) { _close(g_pipeRd); g_pipeRd = -1; }

    ::FreeConsole();
    ::DeleteObject(g_fontTitle);
    ::DeleteObject(g_fontSub);
    ::DeleteObject(g_fontBody);
    ::DeleteObject(g_fontMono);
    ::DeleteObject(g_fontBold);
    ::DeleteObject(g_bgBrush);
    ::DeleteObject(g_headerBrush);
    ::DeleteObject(g_cardBrush);
    ::DeleteObject(g_logBrush);

    Gdiplus::GdiplusShutdown(g_gdipToken);
    g_webview.Reset();
    g_wvController.Reset();
    ::CoUninitialize();

    return static_cast<int>(msg.wParam);
}
