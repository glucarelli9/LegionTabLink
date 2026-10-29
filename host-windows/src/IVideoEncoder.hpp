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
    // outIsIdr is set to true when the output bitstream is actually an IDR frame
    // (may differ from the 'keyframe' request due to pipeline delay).
    virtual bool EncodeFrame(ID3D11Texture2D* frame, bool keyframe, std::vector<uint8_t>& outNal, bool& outIsIdr) = 0;
    // Convenience overload (ignores IDR detection)
    bool EncodeFrame(ID3D11Texture2D* frame, bool keyframe, std::vector<uint8_t>& outNal)
    {
        bool ignored = false;
        return EncodeFrame(frame, keyframe, outNal, ignored);
    }
    virtual void Shutdown() = 0;
};
