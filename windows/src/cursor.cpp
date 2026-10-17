#include "cursor.h"

#include <algorithm>
#include <vector>

#include "log.h"

CursorCompositor::~CursorCompositor()
{
    Shutdown();
}

bool CursorCompositor::Init()
{
    m_auxDc = CreateCompatibleDC(nullptr);
    if (m_auxDc == nullptr) {
        LogError("cursor: CreateCompatibleDC failed: %lu", GetLastError());
        return false;
    }
    return true;
}

void CursorCompositor::Shutdown()
{
    if (m_auxDc != nullptr) {
        DeleteDC(m_auxDc);
        m_auxDc = nullptr;
    }
    m_visible = false;
}

bool CursorCompositor::Compose(HDC memDc, uint8_t* frame, int width, int height, int stride,
                               const RECT& monitorRect)
{
    m_visible = false;
    CURSORINFO ci{};
    ci.cbSize = sizeof(ci);
    if (GetCursorInfo(&ci) == FALSE || (ci.flags & CURSOR_SHOWING) == 0) {
        return false;
    }
    m_pos = ci.ptScreenPos;

    ICONINFO icon{};
    if (GetIconInfo(ci.hCursor, &icon) == FALSE) {
        return false;
    }

    int iconW = 0;
    int iconH = 0;
    BITMAP bm{};
    if (icon.hbmColor != nullptr &&
        GetObjectW(icon.hbmColor, sizeof(bm), &bm) == static_cast<int>(sizeof(bm))) {
        iconW = bm.bmWidth;
        iconH = bm.bmHeight;
    } else {
        iconW = GetSystemMetrics(SM_CXCURSOR);
        iconH = GetSystemMetrics(SM_CYCURSOR);
    }

    bool drawn = false;
    if (icon.hbmColor != nullptr && iconW > 0 && iconH > 0) {
        drawn = BlendIcon(icon, ci.hCursor, monitorRect, frame, width, height, stride);
    }
    if (!drawn && iconW > 0 && iconH > 0) {
        drawn = DrawIconFallback(memDc, ci.hCursor, icon, monitorRect, iconW, iconH);
    }

    if (icon.hbmColor != nullptr) {
        DeleteObject(icon.hbmColor);
    }
    if (icon.hbmMask != nullptr) {
        DeleteObject(icon.hbmMask);
    }
    return drawn;
}

bool CursorCompositor::BlendIcon(const ICONINFO& icon, HCURSOR cursor, const RECT& monitorRect,
                                 uint8_t* frame, int width, int height, int stride)
{
    (void)cursor;
    BITMAP bm{};
    if (GetObjectW(icon.hbmColor, sizeof(bm), &bm) != static_cast<int>(sizeof(bm))) {
        return false;
    }
    const int iconW = bm.bmWidth;
    const int iconH = bm.bmHeight;
    if (iconW <= 0 || iconH <= 0 || iconW > 512 || iconH > 512) {
        return false;
    }

    std::vector<uint8_t> pixels(static_cast<size_t>(iconW) * static_cast<size_t>(iconH) * 4u);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = iconW;
    bi.bmiHeader.biHeight = -iconH;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    if (m_auxDc == nullptr ||
        GetDIBits(m_auxDc, icon.hbmColor, 0, static_cast<UINT>(iconH), pixels.data(), &bi,
                  DIB_RGB_COLORS) == 0) {
        return false;
    }

    bool hasAlpha = false;
    for (size_t i = 3; i < pixels.size(); i += 4) {
        if (pixels[i] != 0) {
            hasAlpha = true;
            break;
        }
    }
    if (!hasAlpha) {
        return false;  // 交给 DrawIconEx 走掩码路径
    }

    const int left = m_pos.x - static_cast<int>(icon.xHotspot) - monitorRect.left;
    const int top = m_pos.y - static_cast<int>(icon.yHotspot) - monitorRect.top;
    const int x0 = std::max(0, -left);
    const int y0 = std::max(0, -top);
    const int x1 = std::min(iconW, width - left);
    const int y1 = std::min(iconH, height - top);
    if (x0 >= x1 || y0 >= y1) {
        return true;  // 光标可见但完全在采集区外
    }

    for (int yy = y0; yy < y1; ++yy) {
        const uint8_t* src = pixels.data() + (static_cast<size_t>(yy) * iconW + x0) * 4u;
        uint8_t* dst = frame + static_cast<size_t>(top + yy) * stride +
                       static_cast<size_t>(left + x0) * 4u;
        for (int xx = x0; xx < x1; ++xx) {
            const unsigned a = src[3];
            if (a == 255u) {
                dst[0] = src[0];
                dst[1] = src[1];
                dst[2] = src[2];
                dst[3] = 255;
            } else if (a != 0u) {
                const unsigned inv = 255u - a;
                for (int c = 0; c < 3; ++c) {
                    dst[c] = static_cast<uint8_t>((src[c] * a + dst[c] * inv + 127u) / 255u);
                }
                dst[3] = 255;
            }
            src += 4;
            dst += 4;
        }
    }
    m_visible = true;
    return true;
}

bool CursorCompositor::DrawIconFallback(HDC memDc, HCURSOR cursor, const ICONINFO& icon,
                                        const RECT& monitorRect, int iconW, int iconH)
{
    const int left = m_pos.x - static_cast<int>(icon.xHotspot) - monitorRect.left;
    const int top = m_pos.y - static_cast<int>(icon.yHotspot) - monitorRect.top;
    if (DrawIconEx(memDc, left, top, cursor, iconW, iconH, 0, nullptr, DI_NORMAL) == FALSE) {
        return false;
    }
    m_visible = true;
    return true;
}
