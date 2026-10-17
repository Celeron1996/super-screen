#include "scaler.h"

#include "log.h"

FrameScaler::~FrameScaler()
{
    Shutdown();
}

bool FrameScaler::Init(int width, int height)
{
    Shutdown();
    if (width <= 0 || height <= 0) {
        return false;
    }

    m_memDc = CreateCompatibleDC(nullptr);
    if (m_memDc == nullptr) {
        LogError("scaler: CreateCompatibleDC failed: %lu", GetLastError());
        return false;
    }

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = width;
    bi.bmiHeader.biHeight = -height;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    m_bmp = CreateDIBSection(m_memDc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (m_bmp == nullptr || bits == nullptr) {
        LogError("scaler: CreateDIBSection failed: %lu", GetLastError());
        Shutdown();
        return false;
    }
    m_oldBmp = SelectObject(m_memDc, m_bmp);
    m_bits = static_cast<uint8_t*>(bits);
    m_width = width;
    m_height = height;
    m_stride = width * 4;
    return true;
}

bool FrameScaler::Resize(int width, int height)
{
    if (Ready() && width == m_width && height == m_height) {
        return true;
    }
    return Init(width, height);
}

void FrameScaler::Shutdown()
{
    if (m_memDc != nullptr && m_oldBmp != nullptr) {
        SelectObject(m_memDc, m_oldBmp);
    }
    m_oldBmp = nullptr;
    if (m_bmp != nullptr) {
        DeleteObject(m_bmp);
        m_bmp = nullptr;
    }
    if (m_memDc != nullptr) {
        DeleteDC(m_memDc);
        m_memDc = nullptr;
    }
    m_bits = nullptr;
    m_width = 0;
    m_height = 0;
    m_stride = 0;
}

bool FrameScaler::Scale(HDC srcDc, int srcWidth, int srcHeight)
{
    if (m_memDc == nullptr || m_bits == nullptr || srcDc == nullptr || srcWidth <= 0 ||
        srcHeight <= 0) {
        return false;
    }
    SetStretchBltMode(m_memDc, HALFTONE);
    SetBrushOrgEx(m_memDc, 0, 0, nullptr);
    return StretchBlt(m_memDc, 0, 0, m_width, m_height, srcDc, 0, 0, srcWidth, srcHeight,
                      SRCCOPY) != FALSE;
}
