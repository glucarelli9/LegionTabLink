#pragma once

#include "IVideoEncoder.hpp"

#include <memory>
#include <cstdint>

struct ID3D11Device;

std::unique_ptr<IVideoEncoder> CreateEncoderForAdapterVendor(
    uint32_t vendorId,
    ID3D11Device* device,
    uint32_t width,
    uint32_t height,
    uint32_t fps,
    uint32_t bitrate);
