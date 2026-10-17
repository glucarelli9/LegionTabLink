#pragma once

#include "IVideoEncoder.hpp"

#include <cstdint>
#include <vector>

#include <windows.h>

namespace amf
{
    class AMFFactory;
    class AMFContext;
    class AMFComponent;
}

class AmfEncoder final : public IVideoEncoder
{
public:
    explicit AmfEncoder(uint32_t fps);
    ~AmfEncoder() override;

    AmfEncoder(const AmfEncoder&) = delete;
    AmfEncoder& operator=(const AmfEncoder&) = delete;

    bool Initialize(ID3D11Device* device, uint32_t width, uint32_t height, uint32_t bitrate) override;
    bool EncodeFrame(ID3D11Texture2D* frame, bool keyframe, std::vector<uint8_t>& outNal) override;
    void Shutdown() override;

private:
    amf::AMFFactory* LoadFactory();

    uint32_t m_fps = 0;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    HMODULE m_amfModule = nullptr;
    amf::AMFFactory* m_factory = nullptr;
    amf::AMFContext* m_context = nullptr;
    amf::AMFComponent* m_encoder = nullptr;
};
