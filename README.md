# LegionTabLink

**Use any Android tablet as a second USB display for your Windows PC.**
A lightweight alternative to SuperDisplay — zero-latency hardware-accelerated HEVC streaming over USB, with cursor overlay and pen/touch input passthrough.

## How it works

```text
Windows PC (host-streamer.exe)                  Android tablet (LegionTabLink app)
+---------------------------------+             +---------------------------------+
| Virtual Display Driver (IDD)    |             |  SurfaceView (immersive)        |
|   2000x1200 @ 90 Hz            |             |            ^                    |
|            v                    |             |            | releaseOutputBuffer|
|  DXGI Desktop Duplication       |             |  MediaCodec HEVC decoder        |
|  + GDI cursor composite         |   USB 2.0   |  (KEY_LOW_LATENCY)             |
|            v                    | ----------->|            ^                    |
|  AMD AMF HEVC encoder           |  video:27183|  Wire protocol reader           |
|  (or NVENC for NVIDIA GPUs)     |             |                                 |
|            v                    | <-----------|  Touch / pen input events       |
|  TCP sender (TCP_NODELAY)       |  input:27184|                                 |
+---------------------------------+             +---------------------------------+
        Connected via ADB reverse tunnels over USB
```

## Prerequisites

| Component | Details |
|-----------|---------|
| **OS** | Windows 10/11 |
| **Virtual Display** | [Virtual-Display-Driver](https://github.com/VirtualDrivers/Virtual-Display-Driver) (IddCx) configured for your tablet resolution |
| **GPU** | AMD (AMF via `amfrt64.dll`) or NVIDIA (NVENC via `nvEncodeAPI64.dll`) — loaded at runtime, no SDK needed |
| **ADB** | `adb.exe` on PATH ([Android Platform Tools](https://developer.android.com/tools/releases/platform-tools)) |
| **Tablet** | Android 12+ (SDK 31) with USB debugging enabled |

## Setup from scratch

### 1. Install Virtual Display Driver

1. Download and install [Virtual-Display-Driver](https://github.com/VirtualDrivers/Virtual-Display-Driver)
2. Edit `C:\VirtualDisplayDriver\vdd_settings.xml` to add your tablet's resolution:
   ```xml
   <Resolution>
     <Width>2000</Width>
     <Height>1200</Height>
     <RefreshRates>
       <Rate>90</Rate>
       <Rate>60</Rate>
     </RefreshRates>
   </Resolution>
   ```
3. Enable the driver in Device Manager → Display adapters → Virtual Display Driver

### 2. Build the Windows host

**Requirements:** Visual Studio 2022 with C++ workload, or MinGW-w64.

```powershell
# CMake (recommended)
cd host-windows
mkdir build && cd build
cmake ..
cmake --build . --config Release
```

The output is `host-windows/build/Release/host-streamer.exe`.

**MinGW alternative:**
```powershell
g++ -std=c++20 -O2 -DWIN32_LEAN_AND_MEAN -DNOMINMAX -D_WIN32_WINNT=0x0A00 `
    -I host-windows/src -I host-windows/include -I protocol `
    host-windows/src/*.cpp -o host-streamer.exe `
    -static-libgcc -static-libstdc++ `
    -lws2_32 -ld3d11 -ldxgi -lole32 -luser32 -lgdi32
```

### 3. Build the Android APK

**Requirements:** Android Studio or just the Android SDK with Gradle.

```powershell
cd client-android

# Set SDK path if not using Android Studio
echo "sdk.dir=C:\\Users\\YourUser\\AppData\\Local\\Android\\Sdk" > local.properties

./gradlew assembleDebug
```

The APK is at `client-android/app/build/outputs/apk/debug/app-debug.apk`.

### 4. Install APK on tablet

```powershell
adb install -r client-android/app/build/outputs/apk/debug/app-debug.apk
```

## Running

```powershell
# 1. Connect tablet via USB, allow USB debugging prompt on tablet

# 2. Set up ADB reverse tunnels
adb reverse tcp:27183 tcp:27183
adb reverse tcp:27184 tcp:27184

# 3. Launch the app on tablet
adb shell am start -n com.legion.tablink/.MainActivity

# 4. Start the host streamer
.\host-windows\build\Release\host-streamer.exe
```

The tablet should now mirror the virtual display. Press **Escape** on the host to stop.

### Host options

```
host-streamer.exe [options]
  --width N      virtual display width  (default 2000)
  --height N     virtual display height (default 1200)
  --fps N        encoder frame rate     (default 30)
  --bitrate N    target bitrate in Mbps (default 20)
```

## Current configuration (what works)

| Setting | Value | Notes |
|---------|-------|-------|
| FPS | 30 | Balances quality and USB bandwidth |
| Bitrate | 20 Mbps | Enough for sharp desktop content |
| IDR interval | 30 frames | ~1 keyframe/second for fast recovery |
| Decoder queue | 4 frames | Low latency, drops old frames on overflow |
| Codec | HEVC | Hardware encode (AMF/NVENC) + hardware decode (MediaCodec) |

## Wire protocol

Little-endian, `#pragma pack(1)`. Header: magic `0x4C54`, type, version, payload size (u32).

| Packet | Type | Direction | Description |
|--------|------|-----------|-------------|
| Hello | 1 | client → host | Max resolution, preferred codec |
| Config | 2 | host → client | Width, height, fps, bitrate, codec |
| Video | 3 | host → client | Keyframe flag (4B) + Annex-B HEVC NALs |
| IdrRequest | 4 | client → host | Request keyframe (header only) |
| Input | 5 | client → host | Touch/pen events (up to 10 pointers) |
| Bye | 6 | both | Session end |

## Repo layout

```
protocol/packet_spec.h        Wire protocol structs (shared by both sides)
host-windows/
  src/main.cpp                 Main loop: capture → encode → send
  src/DxgiCapture.cpp/hpp      DXGI Desktop Duplication + cursor composite
  src/AmfEncoder.cpp/hpp       AMD AMF HEVC encoder with extradata handling
  src/NvencEncoder.cpp/hpp     NVIDIA NVENC HEVC encoder
  src/IVideoEncoder.hpp        Encoder interface
  src/AdbSocketServer.cpp/hpp  TCP transport over ADB reverse tunnels
  src/PenTouchInjector.cpp     Windows Synthetic Pointer injection
  include/                     Vendored AMF + NVENC headers (MIT)
client-android/
  app/src/main/java/com/legion/tablink/
    MainActivity.kt            Surface lifecycle management
    StreamClient.kt            Wire protocol reader
    LowLatencyDecoder.kt       MediaCodec HEVC decoder with queue management
    InputSender.kt             Touch/pen event sender
ROADMAP.md                     Future plans (UI, multi-tablet, installer)
```

## Editing the code

### Modifying the host

All host source is in `host-windows/src/`. After editing:

```powershell
cmake --build host-windows/build --config Release
```

Key files to know:
- **main.cpp** — frame rate, bitrate, IDR interval, dirty frame logic
- **AmfEncoder.cpp** — AMD-specific encoding, extradata extraction, IDR detection
- **DxgiCapture.cpp** — frame capture, cursor overlay, display enumeration

### Modifying the Android app

All Kotlin source is in `client-android/app/src/main/java/com/legion/tablink/`. After editing:

```powershell
cd client-android
./gradlew assembleDebug
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

Key files to know:
- **LowLatencyDecoder.kt** — queue depth, timeouts, keyframe handling
- **StreamClient.kt** — wire protocol parsing, connection management
- **MainActivity.kt** — surface setup, fullscreen/immersive mode

### Debugging

```powershell
# Android decoder logs
adb logcat -s LTL-DEC LTL-MAIN LTL

# Host logs appear in stderr (the console window)
```

## Tested hardware

| PC | GPU | Tablet | Resolution | Status |
|----|-----|--------|------------|--------|
| Custom desktop | AMD RX 9060 XT | Lenovo Tab Plus | 2000x1200 @ 90Hz | ✅ Working |

## Known issues & fixes applied

See [ROADMAP.md](ROADMAP.md) for planned features. Major bugs fixed during development:

1. **AMF pipeline delay** — First encoded frame is not the requested IDR. Fixed by extracting VPS/SPS/PPS from `AMF_VIDEO_ENCODER_HEVC_EXTRADATA` after encoder init.
2. **DPI scaling** — DXGI reports scaled coordinates without `SetProcessDpiAwarenessContext`. Fixed.
3. **Decoder stall** — `awaitingKeyframe` flag never reset after codec creation. Fixed in `pollFrameIfKeyframe()`.
4. **Cursor invisible** — Desktop Duplication separates cursor from frame. Fixed with GDI-compatible staging texture + `DrawIconEx`.

## License

See individual files. AMF and NVENC headers are MIT-licensed.
