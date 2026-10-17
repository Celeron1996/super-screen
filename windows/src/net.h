/*
 * TCP 连接、握手、发送队列、心跳与消息分发。
 * 连接与心跳规则见 docs/PLAN.md §5.1/§5.7，发送队列见 docs/plan-windows.md §4.7。
 */
#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include <winsock2.h>

struct NetQuality {
    int format = 0;
    int fps = 0;
};

class NetClient {
public:
    NetClient();
    ~NetClient();

    bool Init(const std::string& ip, int port);
    void Shutdown();

    void SetHelloParams(int width, int height, int format, int fps);
    void Tick(uint64_t nowMs);

    bool IsReady() const { return m_state == State::Ready; }
    bool ConsumeReadyEdge();
    int SessionFormat() const { return m_sessionFormat; }
    int SessionWidth() const { return m_sessionWidth; }
    int SessionHeight() const { return m_sessionHeight; }

    bool Enqueue(const void* data, size_t len);
    size_t QueueBytes() const { return m_queuedBytes; }
    void FlushUnsent();
    bool TakeQuality(NetQuality* out);
    uint64_t TxBytes() const { return m_txBytes; }

private:
    enum class State { RetryWait, Connecting, Handshaking, Ready };
    struct OutMsg {
        std::vector<uint8_t> data;
        size_t sent = 0;
    };

    void StartConnect(uint64_t nowMs);
    void OnConnected(uint64_t nowMs);
    void CloseAndRetry(uint64_t nowMs, const char* reason);
    void PumpRecv(uint64_t nowMs);
    void PumpSend(uint64_t nowMs);
    void HandleMessage(uint16_t type, const uint8_t* payload, uint32_t len, uint64_t nowMs);
    void EnqueuePing(uint64_t nowMs);

    SOCKET m_sock = INVALID_SOCKET;
    sockaddr_in m_addr{};
    State m_state = State::RetryWait;
    uint64_t m_stateSince = 0;
    uint64_t m_nextRetry = 0;
    uint64_t m_lastPing = 0;
    uint64_t m_lastPong = 0;
    uint64_t m_lastRetryLog = 0;
    uint64_t m_lastDropLog = 0;
    uint64_t m_txBytes = 0;
    bool m_readyEdge = false;
    bool m_wsaStarted = false;

    int m_helloW = 0;
    int m_helloH = 0;
    int m_helloFormat = 0;
    int m_helloFps = 20;
    int m_sessionFormat = 0;
    int m_sessionWidth = 0;
    int m_sessionHeight = 0;

    std::deque<OutMsg> m_queue;
    size_t m_queuedBytes = 0;
    std::vector<uint8_t> m_rx;

    bool m_qualityPending = false;
    NetQuality m_quality{};
};
