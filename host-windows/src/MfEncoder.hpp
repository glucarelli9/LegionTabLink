#pragma once

#include "IVideoEncoder.hpp"

#include <cstdint>
#include <memory>
#include <vector>

#include <d3d11.h>
#include <mfapi.h>
#include <mftransform.h>
#include <icodecapi.h>
#include <wrl/client.h>

/** Hardware encoder through a Media Foundation transform: works on Intel, AMD and NVIDIA. */
class MfEncoder final : public IVideoEncoder
{
public:
    MfEncoder(uint32_t fps, bool hevc);
    ~MfEncoder() override;

    MfEncoder(const MfEncoder&) = delete;
    MfEncoder& operator=(const MfEncoder&) = delete;

    bool Initialize(ID3D11Device* device, uint32_t width, uint32_t height, uint32_t bitrate) override;
    bool EncodeFrame(ID3D11Texture2D* frame, bool keyframe, std::vector<uint8_t>& outNal, bool& outIsIdr) override;
    void Shutdown() override;
    const char* Name() const override { return m_hevc ? "Media Foundation HEVC" : "Media Foundation H.264"; }

    struct EventState;

private:
    bool CreateConverter();
    bool CreateTransform();
    bool ConfigureTypes();
    bool ConvertToNv12(ID3D11Texture2D* source);
    bool CollectOutput(std::vector<uint8_t>& out, bool& isIdr);
    bool WaitFor(bool needInput, uint32_t timeoutMs, std::vector<uint8_t>& out, bool& isIdr);
    void RefreshSequenceHeader();

    uint32_t m_fps = 60;
    bool m_hevc = true;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    uint32_t m_bitrate = 0;
    uint64_t m_frameIndex = 0;
    bool m_async = false;
    bool m_mfStarted = false;
    size_t m_nextSurface = 0;
    std::vector<uint8_t> m_sequenceHeader;

    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
    Microsoft::WRL::ComPtr<ID3D11VideoDevice> m_videoDevice;
    Microsoft::WRL::ComPtr<ID3D11VideoContext> m_videoContext;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> m_vpEnum;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor> m_vp;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_bgra;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> m_inputView;
    std::vector<Microsoft::WRL::ComPtr<ID3D11Texture2D>> m_nv12;
    std::vector<Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView>> m_outputViews;

    UINT m_resetToken = 0;
    Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> m_dxgiManager;
    Microsoft::WRL::ComPtr<IMFActivate> m_activate;
    Microsoft::WRL::ComPtr<IMFTransform> m_transform;
    Microsoft::WRL::ComPtr<ICodecAPI> m_codecApi;
    Microsoft::WRL::ComPtr<IMFMediaEventGenerator> m_events;
    std::shared_ptr<EventState> m_state;
};
