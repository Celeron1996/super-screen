/*
 * 帧缩放：把虚拟屏采集画面缩放到板端目标分辨率（M3 分辨率自适应）。
 * 板端算力有限，缩放统一在 Windows 端完成；见 docs/plan-windows.md。
 */
#pragma once

#include <windows.h>

#include <cstdint>

class FrameScaler {
public:
    ~FrameScaler();

    bool Init(int width, int height);
    bool Resize(int width, int height);
    void Shutdown();

    bool Scale(HDC srcDc, int srcWidth, int srcHeight);

    const uint8_t* Data() const { return m_bits; }
    int Stride() const { return m_stride; }
    int Width() const { return m_width; }
    int Height() const { return m_height; }
    bool Ready() const { return m_bits != nullptr; }

private:
    HDC m_memDc = nullptr;
    HBITMAP m_bmp = nullptr;
    HGDIOBJ m_oldBmp = nullptr;
    uint8_t* m_bits = nullptr;
    int m_width = 0;
    int m_height = 0;
    int m_stride = 0;
};
