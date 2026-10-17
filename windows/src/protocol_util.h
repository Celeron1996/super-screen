/*
 * SSSC 消息构造辅助（仅发送端使用）。
 * 序列化规则见 docs/PLAN.md §5.2；所有整数小端。
 */
#pragma once

#include <cstdint>
#include <vector>

#include "sssc.h"

inline void SsscPutU16(std::vector<uint8_t>& out, uint16_t v)
{
    out.push_back(static_cast<uint8_t>(v & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
}

inline void SsscPutU32(std::vector<uint8_t>& out, uint32_t v)
{
    out.push_back(static_cast<uint8_t>(v & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
}

inline std::vector<uint8_t> SsscMakeMessage(uint16_t type, const uint8_t* payload, uint32_t len)
{
    std::vector<uint8_t> msg;
    msg.reserve(12u + len);
    SsscPutU32(msg, SSSC_MAGIC);
    SsscPutU16(msg, SSSC_VERSION);
    SsscPutU16(msg, type);
    SsscPutU32(msg, len);
    if (len != 0 && payload != nullptr) {
        msg.insert(msg.end(), payload, payload + len);
    }
    return msg;
}
