#include "lz4wrap.h"

#include "lz4/lz4.h"

int SsscLz4Bound(int srcLen)
{
    return LZ4_compressBound(srcLen);
}

int SsscLz4Compress(const uint8_t* src, int srcLen, uint8_t* dst, int dstCap)
{
    if (srcLen <= 0 || dstCap < LZ4_compressBound(srcLen)) {
        return -1;
    }
    const int written = LZ4_compress_default(reinterpret_cast<const char*>(src),
                                             reinterpret_cast<char*>(dst), srcLen, dstCap);
    return written > 0 ? written : -1;
}
