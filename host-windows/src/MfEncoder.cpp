#include "MfEncoder.hpp"

#include <initguid.h>
#include <codecapi.h>
#include <d3d11_4.h>
#include <dxgi.h>
#include <mferror.h>
#include <mfidl.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")

using Microsoft::WRL::ComPtr;

namespace
{
    constexpr size_t SurfaceCount = 4;
    constexpr uint32_t InputWaitMs = 100;
    constexpr uint32_t OutputWaitMs = 30;

    bool SetCodecValue(ICodecAPI* api, const GUID& key, UINT32 value)
    {
        VARIANT v{};
        v.vt = VT_UI4;
        v.ulVal = value;
        return SUCCEEDED(api->SetValue(&key, &v));
    }

    bool SetCodecBool(ICodecAPI* api, const GUID& key, bool value)
    {
        VARIANT v{};
        v.vt = VT_BOOL;
        v.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
        return SUCCEEDED(api->SetValue(&key, &v));
    }

    bool HasParameterSets(const uint8_t* data, size_t size, bool hevc)
    {
        for (size_t i = 0; i + 3 < size; ++i)
        {
            if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)
            {
                const uint8_t header = data[i + 3];
                const int type = hevc ? (header >> 1) & 0x3F : header & 0x1F;
                if (hevc ? (type == 32 || type == 33) : type == 7)
                    return true;
                i += 2;
            }
        }
        return false;
    }
}

// Async MFTs report NeedInput/HaveOutput as events; a callback counts them and signals the encoder thread.
struct MfEncoder::EventState
{
    std::mutex lock;
    HANDLE signal = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    int needInput = 0;
    int haveOutput = 0;
    std::atomic<bool> stopping{ false };
    ~EventState() { ::CloseHandle(signal); }
};

namespace
{
    class EventSink final : public IMFAsyncCallback
    {
    public:
        EventSink(ComPtr<IMFMediaEventGenerator> events, std::shared_ptr<MfEncoder::EventState> state)
            : m_events(std::move(events)), m_state(std::move(state)) {}

        STDMETHODIMP QueryInterface(REFIID riid, void** out) override
        {
            if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFAsyncCallback))
            {
                *out = static_cast<IMFAsyncCallback*>(this);
                AddRef();
                return S_OK;
            }
            *out = nullptr;
            return E_NOINTERFACE;
        }
        STDMETHODIMP_(ULONG) AddRef() override { return ++m_refs; }
        STDMETHODIMP_(ULONG) Release() override
        {
            const ULONG refs = --m_refs;
            if (refs == 0) delete this;
            return refs;
        }
        STDMETHODIMP GetParameters(DWORD*, DWORD*) override { return E_NOTIMPL; }

        STDMETHODIMP Invoke(IMFAsyncResult* result) override
        {
            ComPtr<IMFMediaEvent> event;
            if (FAILED(m_events->EndGetEvent(result, &event)) || !event)
                return S_OK;

            MediaEventType type = MEUnknown;
            event->GetType(&type);
            {
                std::lock_guard<std::mutex> guard(m_state->lock);
                if (type == METransformNeedInput) ++m_state->needInput;
                else if (type == METransformHaveOutput) ++m_state->haveOutput;
            }
            ::SetEvent(m_state->signal);
            if (!m_state->stopping.load())
                m_events->BeginGetEvent(this, nullptr);
            return S_OK;
        }

    private:
        std::atomic<ULONG> m_refs{ 1 };
        ComPtr<IMFMediaEventGenerator> m_events;
        std::shared_ptr<MfEncoder::EventState> m_state;
    };
}

MfEncoder::MfEncoder(uint32_t fps, bool hevc) : m_fps(fps ? fps : 60), m_hevc(hevc) {}

MfEncoder::~MfEncoder()
{
    Shutdown();
}

bool MfEncoder::Initialize(ID3D11Device* device, uint32_t width, uint32_t height, uint32_t bitrate)
{
    m_device = device;
    m_device->GetImmediateContext(&m_context);
    m_width = width & ~1u;
    m_height = height & ~1u;
    m_bitrate = bitrate;

    ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(::MFStartup(MF_VERSION, MFSTARTUP_LITE)))
        return false;
    m_mfStarted = true;

    // MF worker threads share this device with the capture thread
    ComPtr<ID3D11Multithread> multithread;
    if (SUCCEEDED(m_device.As(&multithread)))
        multithread->SetMultithreadProtected(TRUE);

    if (!CreateConverter())
    {
        std::fprintf(stderr, "[mf] NV12 converter unavailable\n");
        return false;
    }
    if (!CreateTransform() || !ConfigureTypes())
        return false;

    m_transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    m_transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    if (m_async)
    {
        m_state = std::make_shared<EventState>();
        auto* sink = new EventSink(m_events, m_state);
        const HRESULT hr = m_events->BeginGetEvent(sink, nullptr);
        sink->Release();
        if (FAILED(hr))
            return false;
    }

    std::fprintf(stderr, "[mf] %s ready: %ux%u @ %u fps, %u bps CBR%s\n",
        Name(), m_width, m_height, m_fps, m_bitrate, m_async ? " (async)" : "");
    return true;
}

bool MfEncoder::CreateConverter()
{
    if (FAILED(m_device.As(&m_videoDevice)) || FAILED(m_context.As(&m_videoContext)))
        return false;

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
    content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    content.InputWidth = m_width;
    content.InputHeight = m_height;
    content.OutputWidth = m_width;
    content.OutputHeight = m_height;
    content.InputFrameRate = { m_fps, 1 };
    content.OutputFrameRate = { m_fps, 1 };
    content.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
    if (FAILED(m_videoDevice->CreateVideoProcessorEnumerator(&content, &m_vpEnum)) ||
        FAILED(m_videoDevice->CreateVideoProcessor(m_vpEnum.Get(), 0, &m_vp)))
        return false;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = m_width;
    desc.Height = m_height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;

    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(m_device->CreateTexture2D(&desc, nullptr, &m_bgra)))
        return false;

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputDesc{};
    inputDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    if (FAILED(m_videoDevice->CreateVideoProcessorInputView(m_bgra.Get(), m_vpEnum.Get(), &inputDesc, &m_inputView)))
        return false;

    desc.Format = DXGI_FORMAT_NV12;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputDesc{};
    outputDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    for (size_t i = 0; i < SurfaceCount; ++i)
    {
        ComPtr<ID3D11Texture2D> surface;
        ComPtr<ID3D11VideoProcessorOutputView> view;
        if (FAILED(m_device->CreateTexture2D(&desc, nullptr, &surface)) ||
            FAILED(m_videoDevice->CreateVideoProcessorOutputView(surface.Get(), m_vpEnum.Get(), &outputDesc, &view)))
            return false;
        m_nv12.push_back(surface);
        m_outputViews.push_back(view);
    }

    D3D11_VIDEO_PROCESSOR_COLOR_SPACE input{};
    input.RGB_Range = 0; // full-range desktop RGB
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE output{};
    output.YCbCr_Matrix = 1; // BT.709
    output.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    m_videoContext->VideoProcessorSetStreamColorSpace(m_vp.Get(), 0, &input);
    m_videoContext->VideoProcessorSetOutputColorSpace(m_vp.Get(), &output);
    m_videoContext->VideoProcessorSetStreamFrameFormat(m_vp.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    return true;
}

bool MfEncoder::CreateTransform()
{
    if (FAILED(::MFCreateDXGIDeviceManager(&m_resetToken, &m_dxgiManager)) ||
        FAILED(m_dxgiManager->ResetDevice(m_device.Get(), m_resetToken)))
        return false;

    LUID adapterLuid{};
    {
        ComPtr<IDXGIDevice> dxgiDevice;
        ComPtr<IDXGIAdapter> adapter;
        DXGI_ADAPTER_DESC adapterDesc{};
        if (SUCCEEDED(m_device.As(&dxgiDevice)) && SUCCEEDED(dxgiDevice->GetAdapter(&adapter)) &&
            SUCCEEDED(adapter->GetDesc(&adapterDesc)))
            adapterLuid = adapterDesc.AdapterLuid;
    }

    MFT_REGISTER_TYPE_INFO outputInfo{ MFMediaType_Video, m_hevc ? MFVideoFormat_HEVC : MFVideoFormat_H264 };
    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    if (FAILED(::MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
            nullptr, &outputInfo, &activates, &count)) || count == 0)
    {
        std::fprintf(stderr, "[mf] no hardware %s encoder found\n", m_hevc ? "HEVC" : "H.264");
        return false;
    }

    // Prefer the encoder on the adapter that owns the captured output (matters on hybrid laptops)
    UINT32 chosen = 0;
    for (UINT32 i = 0; i < count; ++i)
    {
        LUID luid{};
        if (SUCCEEDED(activates[i]->GetBlob(MFT_ENUM_ADAPTER_LUID, reinterpret_cast<UINT8*>(&luid), sizeof(luid), nullptr)) &&
            luid.LowPart == adapterLuid.LowPart && luid.HighPart == adapterLuid.HighPart)
        {
            chosen = i;
            break;
        }
    }
    m_activate = activates[chosen];
    for (UINT32 i = 0; i < count; ++i) activates[i]->Release();
    ::CoTaskMemFree(activates);

    wchar_t name[128] = L"?";
    m_activate->GetString(MFT_FRIENDLY_NAME_Attribute, name, 128, nullptr);
    if (FAILED(m_activate->ActivateObject(IID_PPV_ARGS(&m_transform))))
        return false;
    std::fprintf(stderr, "[mf] using \"%ls\"\n", name);

    ComPtr<IMFAttributes> attributes;
    if (SUCCEEDED(m_transform->GetAttributes(&attributes)))
    {
        UINT32 async = 0;
        m_async = SUCCEEDED(attributes->GetUINT32(MF_TRANSFORM_ASYNC, &async)) && async;
        if (m_async) attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
        attributes->SetUINT32(MF_LOW_LATENCY, TRUE);
        UINT32 d3dAware = 0;
        if (FAILED(attributes->GetUINT32(MF_SA_D3D11_AWARE, &d3dAware)) || !d3dAware)
        {
            std::fprintf(stderr, "[mf] encoder is not D3D11-aware\n");
            return false;
        }
    }
    if (FAILED(m_transform->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(m_dxgiManager.Get()))))
        return false;
    if (m_async && FAILED(m_transform.As(&m_events)))
        return false;

    if (SUCCEEDED(m_transform.As(&m_codecApi)))
    {
        SetCodecValue(m_codecApi.Get(), CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
        SetCodecValue(m_codecApi.Get(), CODECAPI_AVEncCommonMeanBitRate, m_bitrate);
        SetCodecBool(m_codecApi.Get(), CODECAPI_AVLowLatencyMode, true);
        SetCodecValue(m_codecApi.Get(), CODECAPI_AVEncMPVDefaultBPictureCount, 0);
        SetCodecValue(m_codecApi.Get(), CODECAPI_AVEncMPVGOPSize, m_fps * 2);
        SetCodecValue(m_codecApi.Get(), CODECAPI_AVEncCommonQualityVsSpeed, 0);
    }
    return true;
}

bool MfEncoder::ConfigureTypes()
{
    ComPtr<IMFMediaType> output;
    ::MFCreateMediaType(&output);
    output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    output->SetGUID(MF_MT_SUBTYPE, m_hevc ? MFVideoFormat_HEVC : MFVideoFormat_H264);
    output->SetUINT32(MF_MT_AVG_BITRATE, m_bitrate);
    output->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    output->SetUINT32(MF_MT_MPEG2_PROFILE, m_hevc ? eAVEncH265VProfile_Main_420_8 : eAVEncH264VProfile_Main);
    ::MFSetAttributeSize(output.Get(), MF_MT_FRAME_SIZE, m_width, m_height);
    ::MFSetAttributeRatio(output.Get(), MF_MT_FRAME_RATE, m_fps, 1);
    ::MFSetAttributeRatio(output.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    HRESULT hr = m_transform->SetOutputType(0, output.Get(), 0);
    if (FAILED(hr))
    {
        std::fprintf(stderr, "[mf] SetOutputType failed hr=0x%08lX\n", static_cast<unsigned long>(hr));
        return false;
    }

    ComPtr<IMFMediaType> input;
    for (DWORD i = 0; SUCCEEDED(m_transform->GetInputAvailableType(0, i, &input)); ++i)
    {
        GUID subtype{};
        if (SUCCEEDED(input->GetGUID(MF_MT_SUBTYPE, &subtype)) && subtype == MFVideoFormat_NV12)
            break;
        input.Reset();
    }
    if (!input)
    {
        ::MFCreateMediaType(&input);
        input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        input->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    }
    ::MFSetAttributeSize(input.Get(), MF_MT_FRAME_SIZE, m_width, m_height);
    ::MFSetAttributeRatio(input.Get(), MF_MT_FRAME_RATE, m_fps, 1);
    input->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    hr = m_transform->SetInputType(0, input.Get(), 0);
    if (FAILED(hr))
    {
        std::fprintf(stderr, "[mf] SetInputType(NV12) failed hr=0x%08lX\n", static_cast<unsigned long>(hr));
        return false;
    }
    RefreshSequenceHeader();
    return true;
}

void MfEncoder::RefreshSequenceHeader()
{
    ComPtr<IMFMediaType> current;
    UINT32 size = 0;
    if (SUCCEEDED(m_transform->GetOutputCurrentType(0, &current)) &&
        SUCCEEDED(current->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &size)) && size > 0)
    {
        m_sequenceHeader.resize(size);
        current->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, m_sequenceHeader.data(), size, nullptr);
    }
}

bool MfEncoder::ConvertToNv12(ID3D11Texture2D* source)
{
    D3D11_TEXTURE2D_DESC srcDesc{};
    source->GetDesc(&srcDesc);
    if (srcDesc.Width == m_width && srcDesc.Height == m_height)
    {
        m_context->CopyResource(m_bgra.Get(), source);
    }
    else
    {
        D3D11_BOX box{ 0, 0, 0, m_width, m_height, 1 };
        m_context->CopySubresourceRegion(m_bgra.Get(), 0, 0, 0, 0, source, 0, &box);
    }

    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = m_inputView.Get();
    return SUCCEEDED(m_videoContext->VideoProcessorBlt(m_vp.Get(), m_outputViews[m_nextSurface].Get(), 0, 1, &stream));
}

bool MfEncoder::CollectOutput(std::vector<uint8_t>& out, bool& isIdr)
{
    MFT_OUTPUT_STREAM_INFO info{};
    m_transform->GetOutputStreamInfo(0, &info);
    const bool providesSamples = (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;

    ComPtr<IMFSample> ownSample;
    if (!providesSamples)
    {
        ComPtr<IMFMediaBuffer> buffer;
        ::MFCreateSample(&ownSample);
        ::MFCreateMemoryBuffer(info.cbSize ? info.cbSize : m_width * m_height, &buffer);
        ownSample->AddBuffer(buffer.Get());
    }

    MFT_OUTPUT_DATA_BUFFER data{};
    data.pSample = ownSample.Get();
    DWORD status = 0;
    const HRESULT hr = m_transform->ProcessOutput(0, 1, &data, &status);
    if (data.pEvents) data.pEvents->Release();

    ComPtr<IMFSample> sample;
    if (providesSamples) sample.Attach(data.pSample); else sample = ownSample;

    if (hr == MF_E_TRANSFORM_STREAM_CHANGE)
    {
        ComPtr<IMFMediaType> type;
        if (SUCCEEDED(m_transform->GetOutputAvailableType(0, 0, &type)))
            m_transform->SetOutputType(0, type.Get(), 0);
        RefreshSequenceHeader();
        return false;
    }
    if (FAILED(hr) || !sample)
        return false;

    UINT32 cleanPoint = 0;
    sample->GetUINT32(MFSampleExtension_CleanPoint, &cleanPoint);

    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(sample->ConvertToContiguousBuffer(&buffer)))
        return false;
    BYTE* bytes = nullptr;
    DWORD length = 0;
    if (FAILED(buffer->Lock(&bytes, nullptr, &length)))
        return false;
    if (cleanPoint && !HasParameterSets(bytes, length, m_hevc))
    {
        if (m_sequenceHeader.empty()) RefreshSequenceHeader();
        out.insert(out.end(), m_sequenceHeader.begin(), m_sequenceHeader.end());
    }
    out.insert(out.end(), bytes, bytes + length);
    buffer->Unlock();
    isIdr |= cleanPoint != 0;
    return true;
}

// Pump async events until the requested condition holds or the timeout expires; output is drained as it arrives.
bool MfEncoder::WaitFor(bool needInput, uint32_t timeoutMs, std::vector<uint8_t>& out, bool& isIdr)
{
    const ULONGLONG deadline = ::GetTickCount64() + timeoutMs;
    bool gotOutput = false;
    for (;;)
    {
        int outputs = 0;
        bool inputReady = false;
        {
            std::lock_guard<std::mutex> guard(m_state->lock);
            outputs = m_state->haveOutput;
            m_state->haveOutput = 0;
            inputReady = m_state->needInput > 0;
        }
        for (int i = 0; i < outputs; ++i)
            gotOutput |= CollectOutput(out, isIdr);

        if (needInput ? inputReady : gotOutput)
            return true;
        const ULONGLONG now = ::GetTickCount64();
        if (now >= deadline)
            return false;
        ::WaitForSingleObject(m_state->signal, static_cast<DWORD>(deadline - now));
    }
}

bool MfEncoder::EncodeFrame(ID3D11Texture2D* frame, bool keyframe, std::vector<uint8_t>& outNal, bool& outIsIdr)
{
    outNal.clear();
    outIsIdr = false;
    if (!m_transform || !frame)
        return false;

    if (!ConvertToNv12(frame))
        return false;

    ComPtr<IMFMediaBuffer> buffer;
    ComPtr<IMFSample> sample;
    if (FAILED(::MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), m_nv12[m_nextSurface].Get(), 0, FALSE, &buffer)) ||
        FAILED(::MFCreateSample(&sample)))
        return false;
    m_nextSurface = (m_nextSurface + 1) % m_nv12.size();

    DWORD length = 0;
    ComPtr<IMF2DBuffer> buffer2d;
    if (SUCCEEDED(buffer.As(&buffer2d)) && SUCCEEDED(buffer2d->GetContiguousLength(&length)))
        buffer->SetCurrentLength(length);
    sample->AddBuffer(buffer.Get());
    const LONGLONG duration = 10'000'000LL / m_fps;
    sample->SetSampleTime(static_cast<LONGLONG>(m_frameIndex++) * duration);
    sample->SetSampleDuration(duration);

    if (keyframe && m_codecApi)
        SetCodecValue(m_codecApi.Get(), CODECAPI_AVEncVideoForceKeyFrame, 1);

    if (m_async)
    {
        if (!WaitFor(true, InputWaitMs, outNal, outIsIdr))
            return !outNal.empty();
        {
            std::lock_guard<std::mutex> guard(m_state->lock);
            --m_state->needInput;
        }
        if (FAILED(m_transform->ProcessInput(0, sample.Get(), 0)))
            return false;
        if (outNal.empty())
            WaitFor(false, OutputWaitMs, outNal, outIsIdr);
        return true;
    }

    if (FAILED(m_transform->ProcessInput(0, sample.Get(), 0)))
        return false;
    while (CollectOutput(outNal, outIsIdr)) {}
    return true;
}

void MfEncoder::Shutdown()
{
    if (m_state) m_state->stopping.store(true);
    if (m_transform)
    {
        m_transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        m_transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        ComPtr<IMFShutdown> shutdown;
        if (SUCCEEDED(m_transform.As(&shutdown))) shutdown->Shutdown();
    }
    m_events.Reset();
    m_codecApi.Reset();
    m_transform.Reset();
    if (m_activate) { m_activate->ShutdownObject(); m_activate.Reset(); }
    m_dxgiManager.Reset();
    m_outputViews.clear();
    m_nv12.clear();
    m_inputView.Reset();
    m_bgra.Reset();
    m_vp.Reset();
    m_vpEnum.Reset();
    m_videoContext.Reset();
    m_videoDevice.Reset();
    m_context.Reset();
    m_device.Reset();
    if (m_mfStarted) { ::MFShutdown(); m_mfStarted = false; }
}
