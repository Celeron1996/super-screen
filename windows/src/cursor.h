/*
 * 光标合成：VDD 使用硬件光标，画面采集不含光标，必须由应用合成。
 * 设计见 docs/plan-windows.md §4.6。
 */
#pragma once

#include <windows.h>

#include <cstdint>

class CursorCompositor {
public:
    ~CursorCompositor();

    bool Init();
    void Shutdown();

    // 将当前光标绘制到帧缓冲；返回 true 表示已绘制（光标可见且在采集区域内）。
    bool Compose(HDC memDc, uint8_t* frame, int width, int height, int stride,
                 const RECT& monitorRect);

    bool Visible() const { return m_visible; }
    POINT Position() const { return m_pos; }

private:
    bool BlendIcon(const ICONINFO& icon, HCURSOR cursor, const RECT& monitorRect, uint8_t* frame,
                   int width, int height, int stride);
    bool DrawIconFallback(HDC memDc, HCURSOR cursor, const ICONINFO& icon,
                          const RECT& monitorRect, int iconW, int iconH);

    HDC m_auxDc = nullptr;
    bool m_visible = false;
    POINT m_pos{};
};
