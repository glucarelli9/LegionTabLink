# LegionTabLink — Context for Claude sessions

## What this project is

A SuperDisplay clone: turns an Android tablet into a second USB display for Windows PCs. No WiFi — USB only via ADB reverse tunnels. Hardware-accelerated HEVC encoding (AMD AMF / NVIDIA NVENC) on the PC, hardware HEVC decoding (MediaCodec) on the tablet.

## Architecture overview

```
PC: Virtual Display (IddCx VDD) → DXGI Desktop Duplication → HEVC Encoder (AMF/NVENC) → TCP over ADB USB → Tablet: MediaCodec HEVC Decoder → SurfaceView
Tablet: Touch/Pen input → TCP over ADB USB → PC: Synthetic Pointer Injection
```

- **Video port:** tcp:27183 (ADB reverse tunnel)
- **Input port:** tcp:27184 (ADB reverse tunnel)
- **Wire protocol:** 8-byte header, magic 0x4C54, types: Hello(1), Config(2), Video(3), IdrRequest(4), Input(5), Bye(6)

## Current working configuration

- Host: 30fps, 20 Mbps HEVC, IDR every 30 frames (~1/sec)
- Decoder: queue depth 4, no batch processing
- Cursor: GDI composite via staging texture + DrawIconEx
- Dirty frame trigger: 1px cursor nudge at virtual display corner (every 2s or on IDR request)
- Tested on: AMD RX 9060 XT + Lenovo Tab Plus (2000x1200 @ 90Hz)

## Build requirements

### Windows host (C++20)
- Visual Studio 2022 with C++ workload (or MinGW-w64)
- CMake 3.15+
- No external SDKs needed — AMF and NVENC loaded at runtime via GetProcAddress
- AMF headers vendored in `host-windows/include/` (MIT)
- Links against: ws2_32, d3d11, dxgi, ole32, user32, gdi32

```powershell
cd host-windows/build
cmake ..
cmake --build . --config Release
# Output: host-windows/build/Release/host-streamer.exe
```

### Android client (Kotlin)
- Android SDK with API 34
- Gradle wrapper included in repo
- Min SDK 31 (Android 12), target SDK 34
- `local.properties` needs `sdk.dir` pointing to Android SDK

```powershell
cd client-android
echo "sdk.dir=C:\\Users\\Gian\\AppData\\Local\\Android\\Sdk" > local.properties
./gradlew assembleDebug
# Output: app/build/outputs/apk/debug/app-debug.apk
```

### Runtime requirements
- [Virtual-Display-Driver](https://github.com/VirtualDrivers/Virtual-Display-Driver) installed and enabled
- VDD config at `C:\VirtualDisplayDriver\vdd_settings.xml` with tablet resolution
- ADB on PATH, tablet with USB debugging enabled
- AMD GPU driver (amfrt64.dll) or NVIDIA driver (nvEncodeAPI64.dll)

## Key files and what they do

### Host (C++)
| File | Purpose | Key things to know |
|------|---------|-------------------|
| `main.cpp` | Main capture-encode-send loop | FPS, bitrate, IDR interval constants at top. `forceDirtyFrame` triggers DXGI on static screens. DPI awareness set at start. |
| `AmfEncoder.cpp` | AMD AMF HEVC encoder | Extracts VPS/SPS/PPS via `AMF_VIDEO_ENCODER_HEVC_EXTRADATA` after init (94 bytes). Detects actual IDR by scanning NAL types (32=VPS, 19/20=IDR). AMF has 1-frame pipeline delay — first output is NOT the requested IDR. |
| `DxgiCapture.cpp` | DXGI Desktop Duplication | Cursor composite via GDI-compatible staging texture. `CreateStagingTexture` uses `D3D11_RESOURCE_MISC_GDI_COMPATIBLE`. Cursor drawn with `DrawIconEx` via `IDXGISurface1::GetDC`. |
| `NvencEncoder.cpp` | NVIDIA NVENC encoder | Same interface as AMF. No pipeline delay. |
| `IVideoEncoder.hpp` | Encoder interface | `EncodeFrame(texture, keyframe, outNal, outIsIdr)` — the `outIsIdr` bool reports what the encoder ACTUALLY produced. |
| `AdbSocketServer.cpp` | TCP transport | Handles video + input sockets, wire protocol framing, IDR request flag. |

### Android (Kotlin)
| File | Purpose | Key things to know |
|------|---------|-------------------|
| `LowLatencyDecoder.kt` | MediaCodec HEVC decoder | Queue depth 4. `awaitingKeyframe` must be reset in `pollFrameIfKeyframe()` when a keyframe passes — this was a critical bug. Overflow clears queue + sets awaitingKeyframe + requests IDR (~66ms recovery). |
| `StreamClient.kt` | Wire protocol reader | Reads 8-byte headers, dispatches Video/Config/etc packets to decoder. |
| `MainActivity.kt` | Activity + SurfaceView | Immersive fullscreen. Surface lifecycle callbacks feed the decoder. |
| `InputSender.kt` | Touch/pen input sender | Sends touch events back to host over input socket. |

## Critical bugs to know about (already fixed)

1. **AMF pipeline delay**: First encoder output ≠ requested IDR. Must extract extradata separately and prepend to IDR frames that lack VPS headers.
2. **awaitingKeyframe never reset**: `pollFrameIfKeyframe()` must set `awaitingKeyframe = false` when it finds a keyframe. Without this, only ~1fps gets through (only IDRs).
3. **DPI scaling**: Without `SetProcessDpiAwarenessContext(PER_MONITOR_AWARE_V2)`, DXGI reports scaled coordinates and can't find the virtual display.
4. **Cursor invisible**: DXGI Desktop Duplication does NOT include cursor in the frame. Must composite manually with GDI staging texture.
5. **P-frame filter breaks references**: Do NOT filter small P-frames — breaks the decoder's reference chain and causes ghosting.
6. **forceDirtyFrame cursor teleport**: Moving cursor to center of virtual display is visible. Use 1px nudge at corner instead.

## Debugging

```powershell
# Tablet decoder logs
adb logcat -s LTL-DEC LTL-MAIN LTL

# Host logs go to stderr (console window)
```

Key log tags:
- `LTL-DEC`: Decoder internals (push/pump/submit/flush counts, queue state)
- `LTL-MAIN`: Surface lifecycle
- `LTL`: Wire protocol packets

## What's NOT done yet (see ROADMAP.md)

- Touch/pen input passthrough (code exists but untested)
- APK welcome UI with connection config (resolution/fps/bitrate picker)
- Distributable .exe with tray icon and auto-setup
- Multi-tablet support
- Adaptive bitrate
