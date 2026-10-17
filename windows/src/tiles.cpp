#include "tiles.h"

#include <algorithm>
#include <cstring>

#include "sssc.h"

void TileEngine::Configure(int screenW, int screenH, int tileSize)
{
    m_w = screenW;
    m_h = screenH;
    m_tile = tileSize;
    m_cols = (screenW + tileSize - 1) / tileSize;
    m_rows = (screenH + tileSize - 1) / tileSize;
    m_tiles.clear();
    for (int row = 0; row < m_rows; ++row) {
        for (int col = 0; col < m_cols; ++col) {
            TileRect rect;
            rect.x = col * tileSize;
            rect.y = row * tileSize;
            rect.w = std::min(tileSize, screenW - rect.x);
            rect.h = std::min(tileSize, screenH - rect.y);
            m_tiles.push_back(rect);
        }
    }
    m_prev.assign(static_cast<size_t>(screenW) * static_cast<size_t>(screenH) * 4u, 0);
    m_prevValid = false;
    m_consecutiveHigh = 0;
    m_lastFullMs = 0;
}

void TileEngine::Invalidate()
{
    m_prevValid = false;
}

void TileEngine::CopyTile(const uint8_t* frame, int stride, const TileRect& rect)
{
    const int prevStride = m_w * 4;
    const size_t rowBytes = static_cast<size_t>(rect.w) * 4u;
    for (int yy = 0; yy < rect.h; ++yy) {
        const uint8_t* src = frame + static_cast<size_t>(rect.y + yy) * stride +
                             static_cast<size_t>(rect.x) * 4u;
        uint8_t* dst = m_prev.data() + static_cast<size_t>(rect.y + yy) * prevStride +
                       static_cast<size_t>(rect.x) * 4u;
        std::memcpy(dst, src, rowBytes);
    }
}

void TileEngine::CollectDirty(const uint8_t* frame, int stride, std::vector<TileRect>* dirty)
{
    dirty->clear();
    if (!m_prevValid) {
        for (const TileRect& rect : m_tiles) {
            dirty->push_back(rect);
            CopyTile(frame, stride, rect);
        }
        m_prevValid = true;
        return;
    }

    const int prevStride = m_w * 4;
    for (const TileRect& rect : m_tiles) {
        bool changed = false;
        const size_t rowBytes = static_cast<size_t>(rect.w) * 4u;
        for (int yy = 0; yy < rect.h; ++yy) {
            const uint8_t* a = frame + static_cast<size_t>(rect.y + yy) * stride +
                               static_cast<size_t>(rect.x) * 4u;
            const uint8_t* b = m_prev.data() + static_cast<size_t>(rect.y + yy) * prevStride +
                               static_cast<size_t>(rect.x) * 4u;
            if (std::memcmp(a, b, rowBytes) != 0) {
                changed = true;
                break;
            }
        }
        if (changed) {
            dirty->push_back(rect);
            CopyTile(frame, stride, rect);
        }
    }
}

bool TileEngine::ShouldFullRefresh(int dirtyCount, uint64_t nowMs)
{
    if (dirtyCount > static_cast<int>(SSSC_FULL_DIRTY_THRESH)) {
        ++m_consecutiveHigh;
        if (m_consecutiveHigh >= 3) {
            return true;
        }
    } else {
        m_consecutiveHigh = 0;
    }
    if (dirtyCount > 0 && m_fullSec > 0 &&
        nowMs - m_lastFullMs >= static_cast<uint64_t>(m_fullSec) * 1000u) {
        return true;
    }
    return false;
}

void TileEngine::OnFullRefresh(uint64_t nowMs)
{
    m_consecutiveHigh = 0;
    m_lastFullMs = nowMs;
}
