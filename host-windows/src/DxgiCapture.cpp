#include "DxgiCapture.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace
{
    constexpr uint32_t VendorAmd = 0x1002u;
    constexpr uint32_t VendorNvidia = 0x10DEu;

    void LogLine(const char* text)
    {
        std::fputs(text, stderr);
        std::fputc('\n', stderr);
    }

    std::string ToAscii(const WCHAR* text)
    {
        std::string result;
        if (text == nullptr)
        {
            return result;
        }
        const int required = ::WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
        if (required > 1)
        {
            result.resize(static_cast<size_t>(required) - 1);
            ::WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), required, nullptr, nullptr);
        }
        return result;
    }

    struct OutputCandidate
    {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        Microsoft::WRL::ComPtr<IDXGIOutput> output;
        DXGI_OUTPUT_DESC desc;
        DXGI_ADAPTER_DESC1 adapterDesc;
        bool isPrimary;
        bool nameMatches;
        bool resolutionMatches;
    };

    bool IsPrimaryMonitor(const RECT& desktopRect)
    {
        const HMONITOR primary = ::MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        if (!::GetMonitorInfoW(primary, &info))
        {
            return false;
        }
        return info.rcMonitor.left == desktopRect.left
            && info.rcMonitor.top == desktopRect.top
            && info.rcMonitor.right == desktopRect.right
            && info.rcMonitor.bottom == desktopRect.bottom;
    }

    bool AnyDeskRectangleMatches(const DXGI_OUTPUT_DESC& desc, uint32_t width, uint32_t height)
    {
        return desc.DesktopCoordinates.right - desc.DesktopCoordinates.left == static_cast<LONG>(width)
            && desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top == static_cast<LONG>(height);
    }

    bool NameHintsVirtualDisplay(const DXGI_OUTPUT_DESC& desc)
    {
        std::string lower = ToAscii(desc.DeviceName);
        for (char& character : lower)
        {
            const unsigned char raw = static_cast<unsigned char>(character);
            character = static_cast<char>(std::tolower(raw));
        }
        return lower.find("virtual") != std::string::npos
            || lower.find("vdd") != std::string::npos
            || lower.find("mtt") != std::string::npos;
    }
}

DxgiCapture::~DxgiCapture()
{
    Stop();
}

bool DxgiCapture::Start(uint32_t expectedWidth, uint32_t expectedHeight)
{
    HRESULT hr = ::CreateDXGIFactory1(__uuidof(IDXGIFactory6), reinterpret_cast<void**>(m_factory.GetAddressOf()));
    if (FAILED(hr))
    {
        LogLine("[dxgi] CreateDXGIFactory1 failed");
        return false;
    }

    std::vector<OutputCandidate> candidates;
    UINT adapterIndex = 0;
    while (true)
    {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        hr = m_factory->EnumAdapters1(adapterIndex, &adapter);
        if (hr == DXGI_ERROR_NOT_FOUND || adapterIndex > 4096)
        {
            break;
        }
        ++adapterIndex;

        DXGI_ADAPTER_DESC1 adapterDesc{};
        if (FAILED(adapter->GetDesc1(&adapterDesc)))
        {
            continue;
        }

        UINT outputIndex = 0;
        while (true)
        {
            Microsoft::WRL::ComPtr<IDXGIOutput> output;
            hr = adapter->EnumOutputs(outputIndex, &output);
            if (hr == DXGI_ERROR_NOT_FOUND || outputIndex > 256)
            {
                break;
            }
            ++outputIndex;

            DXGI_OUTPUT_DESC desc{};
            if (FAILED(output->GetDesc(&desc)) || !desc.AttachedToDesktop)
            {
                continue;
            }

            OutputCandidate candidate{};
            candidate.adapter = adapter;
            candidate.output = output;
            candidate.desc = desc;
            candidate.adapterDesc = adapterDesc;
            candidate.isPrimary = IsPrimaryMonitor(desc.DesktopCoordinates);
            candidate.nameMatches = NameHintsVirtualDisplay(desc);
            candidate.resolutionMatches = AnyDeskRectangleMatches(desc, expectedWidth, expectedHeight);
            if (!candidate.resolutionMatches)
            {
                continue;
            }
            candidates.push_back(std::move(candidate));
        }
    }

    if (candidates.empty())
    {
        LogLine("[dxgi] no attached output matches the requested virtual display resolution");
        return false;
    }

    OutputCandidate* best = &candidates.front();
    for (OutputCandidate& candidate : candidates)
    {
        const int score = static_cast<int>(candidate.nameMatches) * 2 + static_cast<int>(candidate.resolutionMatches) - static_cast<int>(candidate.isPrimary);
        const int bestScore = static_cast<int>(best->nameMatches) * 2 + static_cast<int>(best->resolutionMatches) - static_cast<int>(best->isPrimary);
        if (score > bestScore)
        {
            best = &candidate;
        }
    }

    m_adapter = best->adapter;
    m_output = best->output;
    m_vendorId = best->adapterDesc.VendorId;
    m_adapterName = best->adapterDesc.Description;
    m_monitorRect = best->desc.DesktopCoordinates;
    m_width = static_cast<uint32_t>(m_monitorRect.right - m_monitorRect.left);
    m_height = static_cast<uint32_t>(m_monitorRect.bottom - m_monitorRect.top);

    const char* vendorName = "unknown";
    if (m_vendorId == VendorAmd)
    {
        vendorName = "AMD Radeon 780M";
    }
    else if (m_vendorId == VendorNvidia)
    {
        vendorName = "NVIDIA RTX";
    }
    std::fprintf(stderr, "[dxgi] virtual display owned by %s (vendorId 0x%04X)%s\n", vendorName, m_vendorId, best->isPrimary ? " [primary-penalized]" : "");

    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
    const D3D_FEATURE_LEVEL levels[] =
    {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };

    hr = ::D3D11CreateDevice(
        m_adapter.Get(),
        D3D_DRIVER_TYPE_UNKNOWN,
        nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
        levels,
        static_cast<UINT>(std::size(levels)),
        D3D11_SDK_VERSION,
        &device,
        &featureLevel,
        &context);
    if (FAILED(hr))
    {
        std::fprintf(stderr, "[dxgi] D3D11CreateDevice failed hr=0x%08lX\n", static_cast<unsigned long>(hr));
        return false;
    }

    m_device = std::move(device);
    m_context = std::move(context);

    Microsoft::WRL::ComPtr<IDXGIOutput1> output1;
    hr = m_output.As(&output1);
    if (FAILED(hr))
    {
        LogLine("[dxgi] IDXGIOutput1 unavailable");
        return false;
    }

    hr = output1->DuplicateOutput(m_device.Get(), &m_duplication);
    if (FAILED(hr))
    {
        std::fprintf(stderr, "[dxgi] DuplicateOutput failed hr=0x%08lX\n", static_cast<unsigned long>(hr));
        return false;
    }

    DXGI_OUTDUPL_DESC duplicationDesc{};
    m_duplication->GetDesc(&duplicationDesc);
    m_width = duplicationDesc.ModeDesc.Width;
    m_height = duplicationDesc.ModeDesc.Height;
    std::fprintf(stderr, "[dxgi] duplication active %ux%u format=%d\n", m_width, m_height, static_cast<int>(duplicationDesc.ModeDesc.Format));
    return true;
}

void DxgiCapture::Stop()
{
    m_activeFrame.Reset();
    m_duplication.Reset();
    m_output.Reset();
    m_adapter.Reset();
    m_context.Reset();
    m_device.Reset();
    m_factory.Reset();
    m_vendorId = 0;
    m_width = 0;
    m_height = 0;
    m_monitorRect = RECT{};
    m_adapterName.clear();
}

void DxgiCapture::ReinitializeDuplication()
{
    m_duplication.Reset();
    Microsoft::WRL::ComPtr<IDXGIOutput1> output1;
    if (SUCCEEDED(m_output.As(&output1)))
    {
        output1->DuplicateOutput(m_device.Get(), &m_duplication);
    }
}

ID3D11Device* DxgiCapture::Device() const
{
    return m_device.Get();
}

uint32_t DxgiCapture::VendorId() const
{
    return m_vendorId;
}

uint32_t DxgiCapture::Width() const
{
    return m_width;
}

uint32_t DxgiCapture::Height() const
{
    return m_height;
}

void DxgiCapture::GetMonitorRect(RECT& outRect) const
{
    outRect = m_monitorRect;
}

const wchar_t* DxgiCapture::AdapterName() const
{
    return m_adapterName.c_str();
}

bool DxgiCapture::AcquireFrame(ID3D11Texture2D** frame, uint32_t timeoutMs)
{
    *frame = nullptr;
    if (!m_duplication)
    {
        return false;
    }

    DXGI_OUTDUPL_FRAME_INFO frameInfo{};
    Microsoft::WRL::ComPtr<IDXGIResource> resource;
    HRESULT hr = m_duplication->AcquireNextFrame(timeoutMs, &frameInfo, &resource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT)
    {
        return false;
    }
    if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_INVALID_CALL || hr == DXGI_ERROR_SESSION_DISCONNECTED)
    {
        ReinitializeDuplication();
        return false;
    }
    if (FAILED(hr) || !resource)
    {
        return false;
    }

    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    hr = resource.As(&texture);
    if (FAILED(hr) || !texture)
    {
        m_duplication->ReleaseFrame();
        return false;
    }

    m_activeFrame = std::move(texture);
    *frame = m_activeFrame.Get();
    return true;
}

void DxgiCapture::ReleaseFrame()
{
    m_activeFrame.Reset();
    if (m_duplication)
    {
        m_duplication->ReleaseFrame();
    }
}
