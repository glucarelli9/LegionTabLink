#include "EncoderFactory.hpp"

#include "AmfEncoder.hpp"
#include "NvencEncoder.hpp"

#include <cstdio>
#include <d3d11.h>

namespace
{
    constexpr uint32_t VendorAmd = 0x1002u;
    constexpr uint32_t VendorNvidia = 0x10DEu;
}

std::unique_ptr<IVideoEncoder> CreateEncoderForAdapterVendor(
    uint32_t vendorId,
    ID3D11Device* device,
    uint32_t width,
    uint32_t height,
    uint32_t fps,
    uint32_t bitrate)
{
    if (vendorId == VendorAmd)
    {
        auto encoder = std::make_unique<AmfEncoder>(fps);
        if (!encoder->Initialize(device, width, height, bitrate))
        {
            std::fprintf(stderr, "[encoder] AMF initialization failed\n");
            return nullptr;
        }
        return encoder;
    }

    if (vendorId == VendorNvidia)
    {
        auto encoder = std::make_unique<NvencEncoder>(fps);
        if (!encoder->Initialize(device, width, height, bitrate))
        {
            std::fprintf(stderr, "[encoder] NVENC initialization failed\n");
            return nullptr;
        }
        return encoder;
    }

    std::fprintf(stderr, "[encoder] no backend for vendorId 0x%04X\n", vendorId);
    return nullptr;
}
