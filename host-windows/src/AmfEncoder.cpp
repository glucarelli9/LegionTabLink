#include "AmfEncoder.hpp"

#include <windows.h>
#include <d3d11.h>

#include "AMF/core/Factory.h"
#include "AMF/core/Context.h"
#include "AMF/core/Buffer.h"
#include "AMF/components/VideoEncoderHEVC.h"

AmfEncoder::AmfEncoder(uint32_t fps)
    : m_fps(fps)
{
}

AmfEncoder::~AmfEncoder()
{
    Shutdown();
}

amf::AMFFactory* AmfEncoder::LoadFactory()
{
    m_amfModule = ::LoadLibraryExW(AMF_DLL_NAME, nullptr, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (m_amfModule == nullptr)
    {
        std::fprintf(stderr, "[amf] amfrt64.dll not found (Radeon driver runtime missing)\n");
        return nullptr;
    }

    const auto queryVersion = reinterpret_cast<AMFQueryVersion_Fn>(::GetProcAddress(m_amfModule, AMF_QUERY_VERSION_FUNCTION_NAME));
    const auto amfInit = reinterpret_cast<AMFInit_Fn>(::GetProcAddress(m_amfModule, AMF_INIT_FUNCTION_NAME));
    if (queryVersion == nullptr || amfInit == nullptr)
    {
        std::fprintf(stderr, "[amf] amfrt64.dll exports missing\n");
        ::FreeLibrary(m_amfModule);
        m_amfModule = nullptr;
        return nullptr;
    }

    amf_uint64 runtimeVersion = 0;
    if (queryVersion(&runtimeVersion) != AMF_OK)
    {
        std::fprintf(stderr, "[amf] AMFQueryVersion failed\n");
        ::FreeLibrary(m_amfModule);
        m_amfModule = nullptr;
        return nullptr;
    }

    amf::AMFFactory* factory = nullptr;
    if (amfInit(runtimeVersion, &factory) != AMF_OK || factory == nullptr)
    {
        std::fprintf(stderr, "[amf] AMFInit failed\n");
        ::FreeLibrary(m_amfModule);
        m_amfModule = nullptr;
        return nullptr;
    }
    return factory;
}

bool AmfEncoder::Initialize(ID3D11Device* device, uint32_t width, uint32_t height, uint32_t bitrate)
{
    m_width = width;
    m_height = height;

    m_factory = LoadFactory();
    if (m_factory == nullptr || device == nullptr)
    {
        return false;
    }

    amf::AMFContext* context = nullptr;
    if (m_factory->CreateContext(&context) != AMF_OK || context == nullptr)
    {
        std::fprintf(stderr, "[amf] CreateContext failed\n");
        return false;
    }
    m_context = context;

    if (m_context->InitDX11(device) != AMF_OK)
    {
        std::fprintf(stderr, "[amf] InitDX11 failed (adapter mismatch?)\n");
        return false;
    }

    amf::AMFComponent* component = nullptr;
    if (m_factory->CreateComponent(m_context, AMFVideoEncoder_HEVC, &component) != AMF_OK || component == nullptr)
    {
        std::fprintf(stderr, "[amf] AMFVideoEncoderHW_HEVC component unavailable\n");
        return false;
    }
    m_encoder = component;

    m_encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_USAGE, amf::AMFVariant(static_cast<amf_int64>(AMF_VIDEO_ENCODER_HEVC_USAGE_ULTRA_LOW_LATENCY)));
    m_encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET, amf::AMFVariant(static_cast<amf_int64>(AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_SPEED)));
    m_encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD, amf::AMFVariant(static_cast<amf_int64>(AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_CBR)));
    m_encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_TARGET_BITRATE, amf::AMFVariant(static_cast<amf_int64>(bitrate)));
    m_encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_VBV_BUFFER_SIZE, amf::AMFVariant(static_cast<amf_int64>(bitrate / (m_fps == 0 ? 60u : m_fps))));
    m_encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_INITIAL_VBV_BUFFER_FULLNESS, amf::AMFVariant(static_cast<amf_int64>(64)));
    m_encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_NUM_GOPS_PER_IDR, amf::AMFVariant(static_cast<amf_int64>(0)));
    m_encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_LOWLATENCY_MODE, amf::AMFVariant(true));
    m_encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_HEADER_INSERTION_MODE, amf::AMFVariant(static_cast<amf_int64>(AMF_VIDEO_ENCODER_HEVC_HEADER_INSERTION_MODE_IDR_ALIGNED)));
    m_encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_FRAMERATE, amf::AMFVariant(AMFConstructRate(m_fps == 0 ? 60u : m_fps, 1u)));

    if (m_encoder->Init(amf::AMF_SURFACE_BGRA, static_cast<amf_int32>(m_width), static_cast<amf_int32>(m_height)) != AMF_OK)
    {
        std::fprintf(stderr, "[amf] encoder Init(BGRA) failed\n");
        return false;
    }

    // Extract VPS/SPS/PPS extra data from the encoder after init.
    // AMF pipeline has a 1-frame delay; the first IDR output may lack headers,
    // so we prepend these when sending keyframes.
    amf::AMFVariant extraDataVar;
    if (m_encoder->GetProperty(AMF_VIDEO_ENCODER_HEVC_EXTRADATA, &extraDataVar) == AMF_OK)
    {
        amf::AMFInterface* iface = extraDataVar.pInterface;
        if (iface != nullptr)
        {
            amf::AMFBuffer* buf = nullptr;
            if (iface->QueryInterface(amf::AMFBuffer::IID(), reinterpret_cast<void**>(&buf)) == AMF_OK && buf != nullptr)
            {
                const auto* bytes = static_cast<const uint8_t*>(buf->GetNative());
                m_extraData.assign(bytes, bytes + buf->GetSize());
                std::fprintf(stderr, "[amf] extracted %zu bytes of VPS/SPS/PPS extra data\n", m_extraData.size());
                buf->Release();
            }
        }
    }
    if (m_extraData.empty())
    {
        std::fprintf(stderr, "[amf] WARNING: no extra data (VPS/SPS/PPS) from encoder\n");
    }

    std::fprintf(stderr, "[amf] VCN 4.0 HEVC ready: %ux%u @ %u fps, %u bps CBR\n", m_width, m_height, m_fps, bitrate);
    return true;
}

bool AmfEncoder::EncodeFrame(ID3D11Texture2D* frame, bool keyframe, std::vector<uint8_t>& outNal, bool& outIsIdr)
{
    outNal.clear();
    outIsIdr = false;
    if (m_encoder == nullptr || m_context == nullptr || frame == nullptr)
    {
        return false;
    }

    amf::AMFSurface* surface = nullptr;
    if (m_context->CreateSurfaceFromDX11Native(frame, &surface, nullptr) != AMF_OK || surface == nullptr)
    {
        std::fprintf(stderr, "[amf] CreateSurfaceFromDX11Native failed\n");
        return false;
    }

    if (keyframe)
    {
        surface->SetProperty(AMF_VIDEO_ENCODER_HEVC_FORCE_PICTURE_TYPE, amf::AMFVariant(static_cast<amf_int64>(AMF_VIDEO_ENCODER_HEVC_PICTURE_TYPE_IDR)));
    }

    const AMF_RESULT submitted = m_encoder->SubmitInput(surface);
    surface->Release();
    if (submitted != AMF_OK && submitted != AMF_INPUT_FULL)
    {
        std::fprintf(stderr, "[amf] SubmitInput failed res=%d\n", static_cast<int>(submitted));
        return false;
    }

    amf::AMFData* outputData = nullptr;
    if (m_encoder->QueryOutput(&outputData) != AMF_OK || outputData == nullptr)
    {
        return true;
    }

    amf::AMFBuffer* buffer = nullptr;
    if (outputData->QueryInterface(amf::AMFBuffer::IID(), reinterpret_cast<void**>(&buffer)) != AMF_OK || buffer == nullptr)
    {
        outputData->Release();
        return true;
    }

    const void* rawBytes = buffer->GetNative();
    const amf_size rawSize = buffer->GetSize();
    const auto* begin = static_cast<const uint8_t*>(rawBytes);

    // Detect whether the encoder actually produced an IDR by checking for VPS NAL.
    // AMF has a 1-frame pipeline delay: the output corresponds to the PREVIOUS
    // submission, so the keyframe flag we passed may not match the actual output.
    bool outputIsIdr = false;
    for (size_t i = 0; i + 5 < rawSize; ++i)
    {
        if (begin[i] == 0 && begin[i + 1] == 0 && begin[i + 2] == 0 && begin[i + 3] == 1)
        {
            const uint8_t nalType = (begin[i + 4] >> 1) & 0x3F;
            if (nalType == 32) // VPS → this is an IDR with parameter sets
            {
                outputIsIdr = true;
                break;
            }
            if (nalType == 19 || nalType == 20) // IDR_W_RADL or IDR_N_LP without headers
            {
                outputIsIdr = true;
                break;
            }
        }
    }

    outIsIdr = outputIsIdr;
    if (outputIsIdr && !m_extraData.empty())
    {
        // Check if VPS is already present; if not, prepend extra data
        bool hasVps = false;
        for (size_t i = 0; i + 5 < rawSize; ++i)
        {
            if (begin[i] == 0 && begin[i + 1] == 0 && begin[i + 2] == 0 && begin[i + 3] == 1)
            {
                if (((begin[i + 4] >> 1) & 0x3F) == 32) { hasVps = true; break; }
            }
        }
        if (!hasVps)
        {
            outNal.reserve(m_extraData.size() + rawSize);
            outNal.assign(m_extraData.begin(), m_extraData.end());
            outNal.insert(outNal.end(), begin, begin + rawSize);
            outputData->Release();
            return true;
        }
    }

    outNal.assign(begin, begin + rawSize);
    outputData->Release();
    return true;
}

void AmfEncoder::Shutdown()
{
    if (m_encoder != nullptr)
    {
        m_encoder->Terminate();
        m_encoder->Release();
        m_encoder = nullptr;
    }
    if (m_context != nullptr)
    {
        m_context->Terminate();
        m_context->Release();
        m_context = nullptr;
    }
    m_factory = nullptr;
    if (m_amfModule != nullptr)
    {
        ::FreeLibrary(m_amfModule);
        m_amfModule = nullptr;
    }
}
