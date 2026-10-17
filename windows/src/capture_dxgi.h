/*
 * DXGI Desktop Duplication 采集（M3，失败回退 GDI）。
 * 设计见 docs/plan-windows.md §4.4。
 */
#pragma once

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <cstdint>

class DxgiCapturer {
public:
    ~DxgiCapturer();

    bool Init(const RECT& monitorRect, int width, int height);
    void Shutdown();

    // 1=新帧已拷入 dst；0=本轮无新帧；-1=错误/丢失（调用方应回退 GDI）。
    int Capture(uint8_t* dst, int dstStride);

private:
    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> m_dup;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_staging;
    int m_width = 0;
    int m_height = 0;
};
