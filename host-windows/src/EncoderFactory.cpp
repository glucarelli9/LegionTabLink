#include "EncoderFactory.hpp"

#include "AmfEncoder.hpp"
#include "MfEncoder.hpp"
#include "NvencEncoder.hpp"
#include "packet_spec.h"

#include <cstdio>
#include <d3d11.h>

namespace
{
    constexpr uint32_t VendorAmd = 0x1002u;
    constexpr uint32_t VendorNvidia = 0x10DEu;

    template <typename Encoder, typename... Args>
    std::unique_ptr<IVideoEncoder> Try(const char* label, ID3D11Device* device, uint32_t width, uint32_t height,
                                       uint32_t bitrate, Args... args)
    {
        auto encoder = std::make_unique<Encoder>(args...);
        if (encoder->Initialize(device, width, height, bitrate))
            return encoder;
        std::fprintf(stderr, "[encoder] %s unavailable, trying next\n", label);
        encoder->Shutdown();
        return nullptr;
    }
}

std::unique_ptr<IVideoEncoder> CreateVideoEncoder(
    uint32_t vendorId,
    ID3D11Device* device,
    uint32_t width,
    uint32_t height,
    uint32_t fps,
    uint32_t bitrate,
    uint8_t tabletCodecMask,
    bool forceMediaFoundation,
    uint8_t& outCodec)
{
    const uint8_t mask = tabletCodecMask ? tabletCodecMask : CodecMaskHevc;
    std::unique_ptr<IVideoEncoder> encoder;

    if (mask & CodecMaskHevc)
    {
        if (!forceMediaFoundation && vendorId == VendorAmd)
            encoder = Try<AmfEncoder>("AMF HEVC", device, width, height, bitrate, fps);
        if (!encoder && !forceMediaFoundation && vendorId == VendorNvidia)
            encoder = Try<NvencEncoder>("NVENC HEVC", device, width, height, bitrate, fps);
        if (!encoder)
            encoder = Try<MfEncoder>("Media Foundation HEVC", device, width, height, bitrate, fps, true);
        if (encoder)
        {
            outCodec = LtlVideoCodecHevc;
            return encoder;
        }
    }
    if (mask & CodecMaskAvc)
    {
        encoder = Try<MfEncoder>("Media Foundation H.264", device, width, height, bitrate, fps, false);
        if (encoder)
        {
            outCodec = LtlVideoCodecAvc;
            return encoder;
        }
    }
    std::fprintf(stderr, "[encoder] no encoder works on adapter 0x%04X for codec mask 0x%02X\n", vendorId, mask);
    return nullptr;
}
