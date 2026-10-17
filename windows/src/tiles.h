/*
 * 64x64 分块 diff、脏块收集与全刷调度。
 * 规则见 docs/PLAN.md §5.5 与 docs/plan-windows.md §4.5。
 */
#pragma once

#include <cstdint>
#include <vector>

struct TileRect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

class TileEngine {
public:
    void Configure(int screenW, int screenH, int tileSize);
    void SetFullRefreshSec(int seconds) { m_fullSec = seconds; }
    void Invalidate();

    int Cols() const { return m_cols; }
    int Rows() const { return m_rows; }
    int TileCount() const { return static_cast<int>(m_tiles.size()); }
    const std::vector<TileRect>& AllTiles() const { return m_tiles; }

    // 与上一帧比较；返回本帧脏块列表（首帧/失效后为全部块）。
    void CollectDirty(const uint8_t* frame, int stride, std::vector<TileRect>* dirty);

    // 全刷触发判定：连续 3 帧脏块 > 阈值，或距上次全刷超过周期且存在脏块。
    bool ShouldFullRefresh(int dirtyCount, uint64_t nowMs);
    void OnFullRefresh(uint64_t nowMs);

private:
    void CopyTile(const uint8_t* frame, int stride, const TileRect& rect);

    int m_w = 0;
    int m_h = 0;
    int m_tile = 64;
    int m_cols = 0;
    int m_rows = 0;
    int m_fullSec = 5;
    int m_consecutiveHigh = 0;
    uint64_t m_lastFullMs = 0;
    std::vector<TileRect> m_tiles;
    std::vector<uint8_t> m_prev;
    bool m_prevValid = false;
};
