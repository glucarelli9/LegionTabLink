#pragma once

#include "IVideoEncoder.hpp"

#include <cstdint>
#include <vector>

#include <windows.h>
#include <nvEncodeAPI.h>

class NvencEncoder final : public IVideoEncoder
{
public:
    explicit NvencEncoder(uint32_t fps);
    ~NvencEncoder() override;

    NvencEncoder(const NvencEncoder&) = delete;
    NvencEncoder& operator=(const NvencEncoder&) = delete;

    bool Initialize(ID3D11Device* device, uint32_t width, uint32_t height, uint32_t bitrate) override;
    bool EncodeFrame(ID3D11Texture2D* frame, bool keyframe, std::vector<uint8_t>& outNal, bool& outIsIdr) override;
    void Shutdown() override;

private:
    uint32_t m_fps = 0;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    uint32_t m_pitch = 0;
    HMODULE m_nvencModule = nullptr;
    void* m_session = nullptr;
    NV_ENC_REGISTERED_PTR m_registeredResource = nullptr;
    NV_ENC_OUTPUT_PTR m_bitstreamBuffer = nullptr;
    NV_ENCODE_API_FUNCTION_LIST m_nvenc{};
};
