#pragma once

#include "IVideoEncoder.hpp"

#include <memory>
#include <cstdint>

struct ID3D11Device;

constexpr uint8_t CodecMaskHevc = 0x01;
constexpr uint8_t CodecMaskAvc = 0x02;

/**
 * Picks the best encoder the adapter and the tablet both support:
 * vendor SDK (AMF/NVENC, HEVC) -> Media Foundation HEVC -> Media Foundation H.264.
 * outCodec receives the LtlVideoCodec actually produced.
 */
std::unique_ptr<IVideoEncoder> CreateVideoEncoder(
    uint32_t vendorId,
    ID3D11Device* device,
    uint32_t width,
    uint32_t height,
    uint32_t fps,
    uint32_t bitrate,
    uint8_t tabletCodecMask,
    bool forceMediaFoundation,
    uint8_t& outCodec);
