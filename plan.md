# Project: LegionTabLink (Zero-Latency USB Second Display)

## 1. Hardware Target & Power Profiles
- **Host PC:** 2025 Lenovo Legion 5 Gen 10
  - **APU / iGPU:** AMD Ryzen 7 260 with **Radeon 780M** (RDNA 3, VCN 4.0 hardware encoder, `VendorID: 0x1002`).
  - **dGPU:** NVIDIA GeForce RTX 5060 8GB GDDR7 (9th-Gen NVENC, `VendorID: 0x10DE`).
  - **Memory:** 32GB DDR5-5600, Windows 11 Home 64-bit.
- **Power & MUX Modes:**
  - **College / Portable Mode (Hybrid-iGPU Only / 100W Ugreen Nexode USB-C PD):** RTX 5060 is asleep (`D3cold`) or disabled. **MUST NOT wake or query the NVIDIA dGPU.** All DXGI capture and hardware encoding must run natively on the **Radeon 780M via AMD AMF**.
  - **Desk Mode (245W AC Brick / dGPU Mode):** If the target virtual display output is owned by the RTX 5060, use **NVIDIA NVENC**.
- **Client Tablet:** Samsung Galaxy Tab S6 Lite 2022 (Snapdragon 720G/732G, 2000x1200 @ 60Hz, S-Pen, One UI 6.1.1).
- **Physical Bottleneck:** Tablet USB port is **USB 2.0 High-Speed (480 Mbps theoretical, ~280 Mbps real-world throughput)**.
- **Latency Target:** < 10ms end-to-end glass-to-glass.

## 2. Coding Standards for AI Agents (CRITICAL)
- **Brace Style:** ALWAYS use **Allman style braces** (opening brace on a new line) for all C++, Rust, C#, Kotlin, and Java code.
- **Comments:** Do NOT add comments to code unless explicitly requested by the user. Write self-documenting code.
- **Zero-Copy & Same-Adapter Rule:** Never copy frames between CPU RAM and GPU VRAM, and never copy frames across PCIe between the RTX 5060 and Radeon 780M. Always instantiate the encoder on the **exact same `IDXGIAdapter1`** that owns the `IDXGIOutputDuplication` virtual monitor.

---

## 3. Reverse-Engineered System Architecture

### Component A: Virtual Monitor & Pen Driver Layer (Pre-Signed Dependencies)
1. **Virtual Display:** Interface with **Virtual-Display-Driver (MTT IDD)** configured to `2000x1200 @ 60Hz`. In Hybrid-iGPU mode, Windows binds the IDD swapchain directly to the **AMD Radeon 780M**.
2. **Stylus & Touch Injection:** Use the native Win32 **Synthetic Pointer API** (`CreateSyntheticPointerDevice` with `PT_PEN` and `PT_TOUCH`, `InjectSyntheticPointerInput`).

### Component B: Windows Host Daemon (`host-streamer` - C++20)
1. **Adapter-Aware Capture Engine (`DxgiCapture.cpp`):**
   - Enumerate adapters via `IDXGIFactory6::EnumAdapters1` without waking asleep dGPUs.
   - Locate the `IDXGIOutput` matching the `2000x1200` virtual display.
   - Inspect `DXGI_ADAPTER_DESC1.VendorId` of the owning adapter:
     - `0x1002` (AMD Radeon 780M) -> Instantiate `AmfEncoder` on the same `ID3D11Device`.
     - `0x10DE` (NVIDIA RTX 5060) -> Instantiate `NvencEncoder` on the same `ID3D11Device`.
   - Acquire frames via `IDXGIOutputDuplication::AcquireNextFrame` as `ID3D11Texture2D` (`DXGI_FORMAT_B8G8R8A8_UNORM`).

2. **Hardware Encoder Abstraction (`IVideoEncoder.hpp`):**
   - Pure virtual interface: `Initialize(ID3D11Device*, uint32_t width, uint32_t height, uint32_t bitrate)`, `EncodeFrame(ID3D11Texture2D*, bool forceIdr, std::vector<uint8_t>& outNal)`, `Shutdown()`.

3. **Backend 1: AMD Radeon 780M Encoder (`AmfEncoder.cpp` - Primary for Hybrid/USB-C PD):**
   - Dynamically load `amfrt64.dll` via AMD **Advanced Media Framework (AMF)**.
   - Initialize context with `AMFContext::InitDX11(d3d11Device)` for zero-copy D3D11 texture input (`CreateSurfaceFromDX11Native`).
   - Use `AMFVideoEncoderHW_HEVC` (or `AMFVideoEncoderVCE_AVC`) on VCN 4.0:
     - `AMF_VIDEO_ENCODER_HEVC_USAGE` = `AMF_VIDEO_ENCODER_HEVC_USAGE_ULTRA_LOW_LATENCY`
     - `AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET` = `AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_SPEED`
     - `AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD` = `AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_CBR`
     - `AMF_VIDEO_ENCODER_HEVC_TARGET_BITRATE` = `25000000` (25 Mbps)
     - `AMF_VIDEO_ENCODER_HEVC_VBV_BUFFER_SIZE` = `416666` (1-frame VBV buffer at 60fps for zero buffering lag)
     - `AMF_VIDEO_ENCODER_HEVC_NUM_GOPS_PER_IDR` = `0` (infinite GOP with on-demand IDR / intra-refresh)
     - `AMF_VIDEO_ENCODER_HEVC_LOWLATENCY_MODE` = `true`

4. **Backend 2: NVIDIA RTX 5060 Encoder (`NvencEncoder.cpp` - Fallback for Dedicated GPU Mode):**
   - Dynamically load `nvEncodeAPI64.dll` ONLY when `VendorId == 0x10DE`.
   - Use DirectX 11 interop (`nvEncRegisterResource`), `NV_ENC_PRESET_P1_GUID`, `NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY`, 25 Mbps CBR, 0 B-Frames, single-frame VBV buffer.

5. **Transport Layer (`AdbSocketServer.cpp`):**
   - Automated `adb forward tcp:27183 tcp:27183` (Video) and `adb forward tcp:27184 tcp:27184` (Input) over USB 2.0.
   - `TCP_NODELAY = 1`, `SO_SNDBUF = 65536`.

6. **Input Injector (`PenTouchInjector.cpp`):**
   - Map normalized `[0.0f - 1.0f]` tablet coordinates to virtual monitor desktop bounds.
   - Map Android `MotionEvent` into `POINTER_TYPE_INFO` (`PT_PEN` with pressure `[0 - 1024]`, `tiltX`/`tiltY`, barrel/eraser flags; `PT_TOUCH` 10-finger multi-touch).

### Component C: Android Client App (`tablet-client` - Kotlin)
1. **Display Surface (`MainActivity.kt`):**
   - Full-screen immersive `SurfaceView` rendering directly to `SurfaceHolder.surface`.
2. **Hardware Decoder (`LowLatencyDecoder.kt`):**
   - `MediaCodec.createDecoderByType("video/hevc")`.
   - Enable `KEY_LOW_LATENCY = 1`, `KEY_PRIORITY = 0`, `KEY_OPERATING_RATE = Short.MAX_VALUE`, and Qualcomm vendor flag `"vendor.qti-ext-dec-low-latency.enable" = 1`.
3. **S-Pen & Touch Capture (`StylusCaptureView.kt`):**
   - Capture `onGenericMotionEvent` (hover/tilt) and `onTouchEvent` (contact/pressure) with `view.requestUnbufferedDispatch(event)`.

---

## 4. Directory Structure
```text
legion-tab-link/
├── plan.md
├── protocol/
│   └── packet_spec.h
├── host-windows/
│   ├── CMakeLists.txt
│   ├── include/
│   │   ├── nvEncodeAPI.h
│   │   └── AMF/
│   └── src/
│       ├── main.cpp
│       ├── DxgiCapture.hpp
│       ├── DxgiCapture.cpp
│       ├── IVideoEncoder.hpp
│       ├── EncoderFactory.hpp
│       ├── EncoderFactory.cpp
│       ├── AmfEncoder.hpp
│       ├── AmfEncoder.cpp
│       ├── NvencEncoder.hpp
│       ├── NvencEncoder.cpp
│       ├── AdbSocketServer.hpp
│       ├── AdbSocketServer.cpp
│       ├── PenTouchInjector.hpp
│       └── PenTouchInjector.cpp
└── client-android/
    ├── build.gradle.kts
    └── app/src/main/
        ├── AndroidManifest.xml
        └── java/com/legion/tablink/
            ├── MainActivity.kt
            ├── StreamClient.kt
            ├── LowLatencyDecoder.kt
            └── InputSender.kt