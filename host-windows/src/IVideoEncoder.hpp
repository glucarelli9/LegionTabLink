#pragma once

#include <cstdint>
#include <vector>

struct ID3D11Device;
struct ID3D11Texture2D;

class IVideoEncoder
{
public:
    virtual ~IVideoEncoder() = default;

    virtual bool Initialize(ID3D11Device* device, uint32_t width, uint32_t height, uint32_t bitrate) = 0;
    virtual bool EncodeFrame(ID3D11Texture2D* frame, bool keyframe, std::vector<uint8_t>& outNal) = 0;
    virtual void Shutdown() = 0;
};
