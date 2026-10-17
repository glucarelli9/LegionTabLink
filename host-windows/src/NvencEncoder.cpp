#include "NvencEncoder.hpp"

#include <cstdio>
#include <windows.h>
#include <d3d11.h>

NvencEncoder::NvencEncoder(uint32_t fps)
    : m_fps(fps == 0 ? 60u : fps)
{
}

NvencEncoder::~NvencEncoder()
{
    Shutdown();
}

bool NvencEncoder::Initialize(ID3D11Device* device, uint32_t width, uint32_t height, uint32_t bitrate)
{
    m_width = width;
    m_height = height;
    m_pitch = 0;

    m_nvencModule = ::LoadLibraryW(L"nvEncodeAPI64.dll");
    if (m_nvencModule == nullptr)
    {
        std::fprintf(stderr, "[nvenc] nvEncodeAPI64.dll not found (NVIDIA driver runtime missing?)\n");
        return false;
    }

    const auto createInstance = reinterpret_cast<NVENCSTATUS(NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*)>(
        ::GetProcAddress(m_nvencModule, "NvEncodeAPICreateInstance"));
    if (createInstance == nullptr)
    {
        std::fprintf(stderr, "[nvenc] NvEncodeAPICreateInstance export missing\n");
        return false;
    }

    NV_ENCODE_API_FUNCTION_LIST functions{};
    functions.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    if (createInstance(&functions) != NV_ENC_SUCCESS)
    {
        std::fprintf(stderr, "[nvenc] NV_ENCODE_API_FUNCTION_LIST unavailable (driver too old?)\n");
        return false;
    }
    m_nvenc = functions;

    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS sessionParams{};
    sessionParams.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    sessionParams.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    sessionParams.device = device;
    sessionParams.apiVersion = NVENCAPI_VERSION;

    if (m_nvenc.nvEncOpenEncodeSessionEx(&sessionParams, &m_session) != NV_ENC_SUCCESS || m_session == nullptr)
    {
        std::fprintf(stderr, "[nvenc] open encode session failed\n");
        return false;
    }

    NV_ENC_PRESET_CONFIG presetConfig{};
    presetConfig.version = NV_ENC_PRESET_CONFIG_VER;
    if (m_nvenc.nvEncGetEncodePresetConfigEx(
            m_session,
            NV_ENC_CODEC_HEVC_GUID,
            NV_ENC_PRESET_P1_GUID,
            NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY,
            &presetConfig) != NV_ENC_SUCCESS)
    {
        std::fprintf(stderr, "[nvenc] ultra-low-latency preset config unavailable\n");
        return false;
    }

    NV_ENC_CONFIG config = presetConfig.presetCfg;
    config.version = NV_ENC_CONFIG_VER;
    config.gopLength = NVENC_INFINITE_GOPLENGTH;
    config.frameIntervalP = 1;

    NV_ENC_RC_PARAMS& rc = config.rcParams;
    rc.version = NV_ENC_RC_PARAMS_VER;
    rc.rateControlMode = NV_ENC_PARAMS_RC_CBR;
    rc.averageBitRate = bitrate;
    rc.vbvBufferSize = bitrate / m_fps;
    rc.vbvInitialDelay = rc.vbvBufferSize;
    rc.zeroReorderDelay = 1;
    rc.enableLookahead = 0;
    rc.disableIadapt = 1;
    rc.disableBadapt = 1;
    rc.multiPass = NV_ENC_MULTI_PASS_DISABLED;

    NV_ENC_CONFIG_HEVC& hevc = config.encodeCodecConfig.hevcConfig;
    hevc.idrPeriod = NVENC_INFINITE_GOPLENGTH;

    NV_ENC_INITIALIZE_PARAMS initParams{};
    initParams.version = NV_ENC_INITIALIZE_PARAMS_VER;
    initParams.encodeGUID = NV_ENC_CODEC_HEVC_GUID;
    initParams.presetGUID = NV_ENC_PRESET_P1_GUID;
    initParams.encodeWidth = m_width;
    initParams.encodeHeight = m_height;
    initParams.darWidth = m_width;
    initParams.darHeight = m_height;
    initParams.frameRateNum = m_fps;
    initParams.frameRateDen = 1;
    initParams.enableEncodeAsync = 0;
    initParams.enablePTD = 0;
    initParams.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
    initParams.encodeConfig = &config;
    initParams.maxEncodeWidth = m_width;
    initParams.maxEncodeHeight = m_height;

    if (m_nvenc.nvEncInitializeEncoder(m_session, &initParams) != NV_ENC_SUCCESS)
    {
        std::fprintf(stderr, "[nvenc] NvEncInitializeEncoder failed\n");
        return false;
    }

    NV_ENC_CREATE_BITSTREAM_BUFFER bitstreamParams{};
    bitstreamParams.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
    if (m_nvenc.nvEncCreateBitstreamBuffer(m_session, &bitstreamParams) != NV_ENC_SUCCESS)
    {
        std::fprintf(stderr, "[nvenc] create bitstream buffer failed\n");
        return false;
    }
    m_bitstreamBuffer = bitstreamParams.bitstreamBuffer;

    std::fprintf(stderr, "[nvenc] HEVC ready: %ux%u @ %u fps, %u bps CBR, P1 ultra-low-latency\n", m_width, m_height, m_fps, bitrate);
    return true;
}

bool NvencEncoder::EncodeFrame(ID3D11Texture2D* frame, bool keyframe, std::vector<uint8_t>& outNal)
{
    outNal.clear();
    if (m_session == nullptr || frame == nullptr)
    {
        return false;
    }

    NV_ENC_REGISTER_RESOURCE registerParams{};
    registerParams.version = NV_ENC_REGISTER_RESOURCE_VER;
    registerParams.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
    registerParams.width = m_width;
    registerParams.height = m_height;
    registerParams.pitch = 0;
    registerParams.subResourceIndex = 0;
    registerParams.resourceToRegister = frame;
    registerParams.bufferFormat = NV_ENC_BUFFER_FORMAT_ARGB;
    registerParams.bufferUsage = NV_ENC_INPUT_IMAGE;
    if (m_nvenc.nvEncRegisterResource(m_session, &registerParams) != NV_ENC_SUCCESS)
    {
        std::fprintf(stderr, "[nvenc] register texture failed\n");
        return false;
    }
    m_registeredResource = registerParams.registeredResource;

    NV_ENC_MAP_INPUT_RESOURCE mapParams{};
    mapParams.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
    mapParams.registeredResource = m_registeredResource;
    if (m_nvenc.nvEncMapInputResource(m_session, &mapParams) != NV_ENC_SUCCESS)
    {
        std::fprintf(stderr, "[nvenc] map input resource failed\n");
        m_nvenc.nvEncUnregisterResource(m_session, m_registeredResource);
        m_registeredResource = nullptr;
        return false;
    }

    NV_ENC_PIC_PARAMS picParams{};
    picParams.version = NV_ENC_PIC_PARAMS_VER;
    picParams.inputWidth = m_width;
    picParams.inputHeight = m_height;
    picParams.inputPitch = m_pitch;
    picParams.inputBuffer = mapParams.mappedResource;
    picParams.outputBitstream = m_bitstreamBuffer;
    picParams.bufferFmt = mapParams.mappedBufferFmt;
    picParams.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    if (keyframe)
    {
        picParams.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
    }

    NVENCSTATUS encodeStatus = m_nvenc.nvEncEncodePicture(m_session, &picParams);
    if (encodeStatus == NV_ENC_SUCCESS)
    {
        NV_ENC_LOCK_BITSTREAM lockParams{};
        lockParams.version = NV_ENC_LOCK_BITSTREAM_VER;
        lockParams.outputBitstream = m_bitstreamBuffer;
        if (m_nvenc.nvEncLockBitstream(m_session, &lockParams) == NV_ENC_SUCCESS)
        {
            const auto* begin = static_cast<const uint8_t*>(lockParams.bitstreamBufferPtr);
            const uint32_t size = lockParams.bitstreamSizeInBytes;
            outNal.assign(begin, begin + size);
            m_nvenc.nvEncUnlockBitstream(m_session, m_bitstreamBuffer);
        }
    }

    m_nvenc.nvEncUnmapInputResource(m_session, mapParams.mappedResource);
    m_nvenc.nvEncUnregisterResource(m_session, m_registeredResource);
    m_registeredResource = nullptr;

    if (encodeStatus != NV_ENC_SUCCESS)
    {
        std::fprintf(stderr, "[nvenc] NvEncEncodePicture failed status=%d\n", static_cast<int>(encodeStatus));
        return false;
    }
    return true;
}

void NvencEncoder::Shutdown()
{
    if (m_session != nullptr && m_nvenc.nvEncDestroyBitstreamBuffer != nullptr)
    {
        if (m_bitstreamBuffer != nullptr)
        {
            m_nvenc.nvEncDestroyBitstreamBuffer(m_session, m_bitstreamBuffer);
            m_bitstreamBuffer = nullptr;
        }
        m_nvenc.nvEncDestroyEncoder(m_session);
        m_session = nullptr;
    }
    m_registeredResource = nullptr;
    if (m_nvencModule != nullptr)
    {
        ::FreeLibrary(m_nvencModule);
        m_nvencModule = nullptr;
    }
}
