/*
 * LZ4 封装（vendored v1.9.4）。
 * 每个 tile 独立 block，无字典；见 docs/PLAN.md §5.4。
 */
#pragma once

#include <cstdint>

int SsscLz4Bound(int srcLen);
int SsscLz4Compress(const uint8_t* src, int srcLen, uint8_t* dst, int dstCap);
