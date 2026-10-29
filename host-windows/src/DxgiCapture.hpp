#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

class DxgiCapture
{
public:
    DxgiCapture() = default;
    ~DxgiCapture();

    DxgiCapture(const DxgiCapture&) = delete;
    DxgiCapture& operator=(const DxgiCapture&) = delete;

    bool Start(uint32_t expectedWidth, uint32_t expectedHeight);
    void Stop();

    ID3D11Device* Device() const;
    uint32_t VendorId() const;
    uint32_t Width() const;
    uint32_t Height() const;
    void GetMonitorRect(RECT& outRect) const;
    const wchar_t* AdapterName() const;

    bool AcquireFrame(ID3D11Texture2D** frame, uint32_t timeoutMs);
    void ReleaseFrame();

private:
    void ReinitializeDuplication();
    void ComposeCursor(ID3D11Texture2D* desktopFrame, const DXGI_OUTDUPL_FRAME_INFO& info);
    bool CreateStagingTexture();

    Microsoft::WRL::ComPtr<IDXGIFactory6> m_factory;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> m_adapter;
    Microsoft::WRL::ComPtr<IDXGIOutput> m_output;
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> m_duplication;
    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_activeFrame;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_stagingTexture;

    uint32_t m_vendorId = 0;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    RECT m_monitorRect{};
    std::wstring m_adapterName;

    // Cursor state
    std::vector<uint8_t> m_cursorShape;
    DXGI_OUTDUPL_POINTER_SHAPE_INFO m_cursorInfo{};
    POINT m_cursorPos{};
    bool m_cursorVisible = false;
};
