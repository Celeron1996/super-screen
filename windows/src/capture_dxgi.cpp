#include "capture_dxgi.h"

#include <cstring>

#include "log.h"

DxgiCapturer::~DxgiCapturer()
{
    Shutdown();
}

bool DxgiCapturer::Init(const RECT& monitorRect, int width, int height)
{
    Shutdown();
    if (width <= 0 || height <= 0) {
        return false;
    }

    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        LogWarn("DXGI: CreateDXGIFactory1 failed: 0x%08lx", static_cast<unsigned long>(hr));
        return false;
    }

    Microsoft::WRL::ComPtr<IDXGIAdapter1> chosen;
    Microsoft::WRL::ComPtr<IDXGIOutput1> output;
    for (UINT ai = 0;; ++ai) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(ai, &adapter) == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        for (UINT oi = 0;; ++oi) {
            Microsoft::WRL::ComPtr<IDXGIOutput> out;
            if (adapter->EnumOutputs(oi, &out) == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            DXGI_OUTPUT_DESC desc{};
            if (SUCCEEDED(out->GetDesc(&desc)) && EqualRect(&desc.DesktopCoordinates, &monitorRect)) {
                Microsoft::WRL::ComPtr<IDXGIOutput1> out1;
                if (SUCCEEDED(out.As(&out1))) {
                    chosen = adapter;
                    output = out1;
                    break;
                }
            }
        }
        if (output) {
            break;
        }
    }
    if (!output) {
        LogWarn("DXGI: no output matches the capture rect");
        return false;
    }

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                        D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL feature = D3D_FEATURE_LEVEL_11_0;
    hr = D3D11CreateDevice(chosen.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels,
                           static_cast<UINT>(sizeof(levels) / sizeof(levels[0])),
                           D3D11_SDK_VERSION, &m_device, &feature, &m_context);
    if (FAILED(hr)) {
        LogWarn("DXGI: D3D11CreateDevice failed: 0x%08lx", static_cast<unsigned long>(hr));
        Shutdown();
        return false;
    }

    hr = output->DuplicateOutput(m_device.Get(), &m_dup);
    if (FAILED(hr)) {
        LogWarn("DXGI: DuplicateOutput failed: 0x%08lx", static_cast<unsigned long>(hr));
        Shutdown();
        return false;
    }

    DXGI_OUTDUPL_DESC dupDesc{};
    m_dup->GetDesc(&dupDesc);
    const DXGI_MODE_DESC& mode = dupDesc.ModeDesc;
    if (static_cast<int>(mode.Width) != width || static_cast<int>(mode.Height) != height ||
        mode.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
        LogWarn("DXGI: unexpected mode %ux%u fmt=%u", mode.Width, mode.Height,
                static_cast<unsigned>(mode.Format));
        Shutdown();
        return false;
    }

    D3D11_TEXTURE2D_DESC texDesc{};
    texDesc.Width = mode.Width;
    texDesc.Height = mode.Height;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = mode.Format;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_STAGING;
    texDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    hr = m_device->CreateTexture2D(&texDesc, nullptr, &m_staging);
    if (FAILED(hr)) {
        LogWarn("DXGI: CreateTexture2D(staging) failed: 0x%08lx", static_cast<unsigned long>(hr));
        Shutdown();
        return false;
    }

    m_width = width;
    m_height = height;
    LogInfo("DXGI: duplication ready %dx%d", m_width, m_height);
    return true;
}

void DxgiCapturer::Shutdown()
{
    m_staging.Reset();
    m_dup.Reset();
    m_context.Reset();
    m_device.Reset();
    m_width = 0;
    m_height = 0;
}

int DxgiCapturer::Capture(uint8_t* dst, int dstStride)
{
    if (!m_dup || !m_staging || dst == nullptr) {
        return -1;
    }

    DXGI_OUTDUPL_FRAME_INFO info{};
    Microsoft::WRL::ComPtr<IDXGIResource> resource;
    HRESULT hr = m_dup->AcquireNextFrame(0, &info, &resource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        return 0;
    }
    if (FAILED(hr)) {
        LogWarn("DXGI: AcquireNextFrame failed: 0x%08lx", static_cast<unsigned long>(hr));
        return -1;
    }

    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    hr = resource.As(&texture);
    if (SUCCEEDED(hr)) {
        m_context->CopyResource(m_staging.Get(), texture.Get());
    }
    if (FAILED(hr)) {
        m_dup->ReleaseFrame();
        LogWarn("DXGI: frame copy failed: 0x%08lx", static_cast<unsigned long>(hr));
        return -1;
    }

    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = m_context->Map(m_staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        m_dup->ReleaseFrame();
        LogWarn("DXGI: Map failed: 0x%08lx", static_cast<unsigned long>(hr));
        return -1;
    }

    const size_t rowBytes = static_cast<size_t>(m_width) * 4u;
    const uint8_t* src = static_cast<const uint8_t*>(mapped.pData);
    for (int y = 0; y < m_height; ++y) {
        std::memcpy(dst + static_cast<size_t>(y) * dstStride,
                    src + static_cast<size_t>(y) * mapped.RowPitch, rowBytes);
    }

    m_context->Unmap(m_staging.Get(), 0);
    m_dup->ReleaseFrame();
    return 1;
}
