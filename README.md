# LegionTabLink

**Zero-latency USB second display for the Legion 5 + Galaxy Tab S6 Lite.**
A Windows host daemon mirrors a virtual monitor to an Android tablet over USB and mirrors the tablet's S-Pen / touch input back onto the Windows desktop — glass-to-glass target under 10 ms.

## How it works

```text
Windows host (host-streamer.exe)                Android tablet (LegionTabLink app)
+---------------------------------+             +---------------------------------+
| Virtual-Display-Driver (IDD)    |             |  SurfaceView + Immersive UI     |
|   2000x1200 @ 60 Hz             |             |            ^                    |
|            v                    |             |            | releaseOutputBuffer|
|  IDXGIOutputDuplication  ------ removes BGRA    |            |                    |
|  (same ID3D11Device)            |             |  MediaCodec HEVC decoder        |
|            v                    |   USB 2.0   |  KEY_LOW_LATENCY, QTI low-lat   |
|  Encoder on the SAME adapter    | ----------->|            ^                    |
|   AMD AMF (Radeon 780M)  or     |  video 27183|  WireProtocol reader            |
|   NVENC (RTX 5060, dGPU mode)   |             |                                 |
|            v   HEVC Annex B     |             |  LtlCaptureView                 |
|  TCP frame sender, TCP_NODELAY  | <-----------|  unbuffered pen / touch events  |
|                                 |  input 27184|                                 |
|  Synthetic-Pointer injection    |             |                                 |
|   (S-Pen pressure/tilt/buttons) |             |                                 |
+---------------------------------+             +---------------------------------+
```

## Design rules

- **Zero-copy, same-adapter:** the encoder is instantiated on the exact `IDXGIAdapter1` that owns the duplicated output; the desktop-duplication texture is handed straight to the encoder — no CPU copies, no PCIe round-trips.
- **Hybrid-iGPU safe:** on battery / USB-C PD, the RTX 5060 stays asleep. Capture + encode run natively on the Radeon 780M via AMF. NVENC is only used when the virtual output itself is NVIDIA-owned (desk mode).
- **Single-frame VBV:** 25 Mbps CBR with a 416 666-bit VBV buffer (1 frame at 60 Hz), `ULTRA_LOW_LATENCY` usage, `SPEED` preset, infinite GOP (on-demand IDR on connect / packet loss).
- **Decoder-first transport:** vertices request an IDR over the input socket when the queue overflows or on (re)connect; the host consumes the request on the next encoded frame.

## Wire protocol (`protocol/packet_spec.h`, little-endian, `#pragma pack(1)`)

| Packet | Size | Direction | Payload |
|---|---|---|---|
| Header | 8 B | — | magic `0x4C 0x54`, packet type, version, `u32` payload size |
| Hello | 8 B | client → host | max tablet resolution, preferred codec (HEVC) |
| Config | 12 B | host → client | width, height, fps, bitrate Mbps, codec |
| Video | variable | host → client | 4 B keyframe flag + raw Annex-B HEVC NALs |
| IdrRequest | 8 B | client → host | empty (header only, on input socket) |
| Input | 204 B | client → host | device kind, action, 10 × 20 B pointer records (x, y, pressure `f32`, tilt `i16`, id/state/buttons/tool) |
| Bye | 8 B | both | session end |

Ports: **27183** video, **27184** input — both loopback. The host auto-spawns `adb forward tcp:27183 tcp:27183` / `tcp:27184 tcp:27184`, so everything flows over the USB cable.

## Requirements

- Windows 10/11 + [Virtual-Display-Driver](https://github.com/VirtualDrivers/Virtual-Display-Driver) (MTT IDD) set to **2000×1200 @ 60 Hz** (an existing output with matching resolution also works)
- `adb.exe` on `PATH`, tablet with USB debugging enabled
- AMD driver (provides `amfrt64.dll`) and/or NVIDIA driver (`nvEncodeAPI64.dll`) — both are loaded at runtime via `GetProcAddress`, no SDKs needed to run
- Android min SDK **31** (Android 12), target SDK 34, Kotlin 1.9 toolchain via Gradle

## Build — Windows host (C++20)

CMake (MSVC or MinGW):

```powershell
cmake -S host-windows -B build -G "Visual Studio 17 2022"
cmake --build build --config Release
```

Or directly with MinGW-w64 g++:

```powershell
$env:Path = "C:\msys64\ucrt64\bin;$env:Path"
g++ -std=c++20 -O2 -DWIN32_LEAN_AND_MEAN -DNOMINMAX -D_WIN32_WINNT=0x0A00 `
    -I host-windows/src -I host-windows/include -I protocol `
    host-windows/src/*.cpp -o host-streamer.exe `
    -static-libgcc -static-libstdc++ `
    -lws2_32 -ld3d11 -ldxgi -lole32 -luser32 -lgdi32
```

## Build — Android client

Open `client-android/` in Android Studio (or `./gradlew assembleDebug`), enable USB debugging on the tablet, and install:

```powershell
adb install app\build\outputs\apk\debug\app-debug.apk
```

## Run

1. Plug the tablet in via USB, allow debugging, keep the screen awake.
2. Start `host-streamer.exe` — it locates the 2000×1200 output, picks the encoder by the **owning adapter's vendor ID** (`0x1002` → AMF, `0x10DE` → NVENC), spawns the `adb forward` rules, and waits.
3. Launch the app; the tablet screen becomes the virtual monitor, S-Pen and touch input flow back to Windows.

```text
host-streamer [options]
  --width N      virtual display width  (default 2000)
  --height N     virtual display height (default 1200)
  --fps N        encoder frame rate     (default 60)
  --bitrate N    target bitrate in Mbps (default 25)
```

## Repo layout

```text
protocol/packet_spec.h        shared wire structs — canonical, imported by both sides
host-windows/                 host daemon: capture, AMF/NVENC backends, adb transport,
                              Synthetic-Pointer pen/touch injection
host-windows/include/         vendored AMF public headers (MIT) + nvEncodeAPI.h (MIT)
client-android/               Compose-free Kotlin app: StreamClient, LowLatencyDecoder,
                              InputSender (LtlCaptureView), MainActivity
plan.md                       full design document
```

## Status

- Windows host: builds clean with MinGW-w64 g++ 15.2, smoke-tested output enumeration (expects a matching virtual display to proceed).
- Android client: reviewed against the wire spec byte-for-byte; build it in Android Studio — no CI yet.
- Not yet done: virtual-display auto-install, per-session renegotiation of resolution, adaptive bitrate.
