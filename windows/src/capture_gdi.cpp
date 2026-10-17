#include "capture_gdi.h"

#include "log.h"

GdiCapturer::~GdiCapturer()
{
    Shutdown();
}

bool GdiCapturer::Init(int width, int height)
{
    Shutdown();
    if (width <= 0 || height <= 0) {
        return false;
    }
    m_screenDc = GetDC(nullptr);
    if (m_screenDc == nullptr) {
        LogError("GetDC(NULL) failed: %lu", GetLastError());
        return false;
    }
    m_memDc = CreateCompatibleDC(m_screenDc);
    if (m_memDc == nullptr) {
        LogError("CreateCompatibleDC failed: %lu", GetLastError());
        Shutdown();
        return false;
    }

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = width;
    bi.bmiHeader.biHeight = -height;  // top-down，与协议 tile 行主序一致
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    m_bmp = CreateDIBSection(m_screenDc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (m_bmp == nullptr || bits == nullptr) {
        LogError("CreateDIBSection failed: %lu", GetLastError());
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

void GdiCapturer::Shutdown()
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
    if (m_screenDc != nullptr) {
        ReleaseDC(nullptr, m_screenDc);
        m_screenDc = nullptr;
    }
    m_bits = nullptr;
    m_width = 0;
    m_height = 0;
    m_stride = 0;
}

bool GdiCapturer::Capture(const RECT& rect)
{
    if (m_memDc == nullptr || m_bits == nullptr) {
        return false;
    }
    if (BitBlt(m_memDc, 0, 0, m_width, m_height, m_screenDc, rect.left, rect.top, SRCCOPY) ==
        FALSE) {
        LogWarn("BitBlt failed: %lu", GetLastError());
        return false;
    }
    return true;
}
