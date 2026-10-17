/*
 * GDI BitBlt 采集（v1 基准路径，DXGI 为 M3 可选）。
 * 设计见 docs/plan-windows.md §4.3。
 */
#pragma once

#include <windows.h>

#include <cstdint>

class GdiCapturer {
public:
    ~GdiCapturer();

    bool Init(int width, int height);
    void Shutdown();
    bool Capture(const RECT& rect);

    const uint8_t* Data() const { return m_bits; }
    int Stride() const { return m_stride; }
    int Width() const { return m_width; }
    int Height() const { return m_height; }
    HDC MemDC() const { return m_memDc; }

private:
    HDC m_screenDc = nullptr;
    HDC m_memDc = nullptr;
    HBITMAP m_bmp = nullptr;
    HGDIOBJ m_oldBmp = nullptr;
    uint8_t* m_bits = nullptr;
    int m_width = 0;
    int m_height = 0;
    int m_stride = 0;
};
