#include "net.h"

#include <ws2tcpip.h>

#include <chrono>
#include <cstring>

#include "log.h"
#include "protocol_util.h"
#include "sssc.h"

namespace {
constexpr size_t kQueueCap = 4u * 1024u * 1024u;
constexpr uint64_t kPingIntervalMs = 5000;
constexpr uint64_t kPongTimeoutMs = 15000;
constexpr uint64_t kConnectTimeoutMs = 1000;
constexpr uint64_t kHandshakeTimeoutMs = 3000;
constexpr uint64_t kRetryDelayMs = 1000;
}  // namespace

NetClient::NetClient() = default;

NetClient::~NetClient()
{
    Shutdown();
}

bool NetClient::Init(const std::string& ip, int port)
{
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        LogError("WSAStartup failed: %d", WSAGetLastError());
        return false;
    }
    m_wsaStarted = true;

    std::memset(&m_addr, 0, sizeof(m_addr));
    m_addr.sin_family = AF_INET;
    m_addr.sin_port = htons(static_cast<u_short>(port));
    if (inet_pton(AF_INET, ip.c_str(), &m_addr.sin_addr) != 1) {
        LogError("invalid server ip '%s'", ip.c_str());
        Shutdown();
        return false;
    }
    m_state = State::RetryWait;
    m_nextRetry = 0;
    return true;
}

void NetClient::Shutdown()
{
    if (m_sock != INVALID_SOCKET) {
        closesocket(m_sock);
        m_sock = INVALID_SOCKET;
    }
    m_queue.clear();
    m_queuedBytes = 0;
    m_rx.clear();
    m_state = State::RetryWait;
    if (m_wsaStarted) {
        WSACleanup();
        m_wsaStarted = false;
    }
}

void NetClient::SetHelloParams(int width, int height, int format, int fps)
{
    m_helloW = width;
    m_helloH = height;
    m_helloFormat = format;
    m_helloFps = fps;
}

bool NetClient::ConsumeReadyEdge()
{
    const bool edge = m_readyEdge;
    m_readyEdge = false;
    return edge;
}

bool NetClient::TakeQuality(NetQuality* out)
{
    if (!m_qualityPending) {
        return false;
    }
    m_qualityPending = false;
    *out = m_quality;
    return true;
}

void NetClient::Tick(uint64_t nowMs)
{
    if (m_state == State::RetryWait) {
        if (nowMs >= m_nextRetry) {
            StartConnect(nowMs);
        }
        return;
    }

    if (m_state == State::Connecting) {
        fd_set writeSet;
        FD_ZERO(&writeSet);
        FD_SET(m_sock, &writeSet);
        timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = 0;
        const int rc = select(0, nullptr, &writeSet, nullptr, &tv);
        if (rc > 0 && FD_ISSET(m_sock, &writeSet)) {
            int err = 0;
            int optLen = static_cast<int>(sizeof(err));
            if (getsockopt(m_sock, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &optLen) !=
                0) {
                err = WSAGetLastError();
            }
            if (err == 0) {
                OnConnected(nowMs);
                return;
            }
            CloseAndRetry(nowMs, "connect refused");
            return;
        }
        if (rc < 0) {
            CloseAndRetry(nowMs, "select failed");
            return;
        }
        if (nowMs - m_stateSince > kConnectTimeoutMs) {
            CloseAndRetry(nowMs, "connect timeout");
        }
        return;
    }

    if (m_state == State::Handshaking) {
        PumpRecv(nowMs);
        if (m_state != State::Handshaking) {
            return;
        }
        PumpSend(nowMs);
        if (m_state != State::Handshaking) {
            return;
        }
        if (nowMs - m_stateSince > kHandshakeTimeoutMs) {
            CloseAndRetry(nowMs, "HELLO_ACK timeout");
        }
        return;
    }

    PumpRecv(nowMs);
    if (m_state != State::Ready) {
        return;
    }
    PumpSend(nowMs);
    if (m_state != State::Ready) {
        return;
    }
    if (nowMs - m_lastPing >= kPingIntervalMs) {
        EnqueuePing(nowMs);
        m_lastPing = nowMs;
    }
    if (nowMs - m_lastPong > kPongTimeoutMs) {
        CloseAndRetry(nowMs, "PONG timeout");
    }
}

void NetClient::StartConnect(uint64_t nowMs)
{
    if (m_sock != INVALID_SOCKET) {
        closesocket(m_sock);
        m_sock = INVALID_SOCKET;
    }
    m_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (m_sock == INVALID_SOCKET) {
        m_nextRetry = nowMs + kRetryDelayMs;
        if (nowMs - m_lastRetryLog >= 5000) {
            LogWarn("socket() failed: %d, retrying", WSAGetLastError());
            m_lastRetryLog = nowMs;
        }
        return;
    }
    u_long nonBlocking = 1;
    ioctlsocket(m_sock, FIONBIO, &nonBlocking);
    m_stateSince = nowMs;
    const int rc = connect(m_sock, reinterpret_cast<const sockaddr*>(&m_addr), sizeof(m_addr));
    if (rc == 0) {
        OnConnected(nowMs);
        return;
    }
    if (WSAGetLastError() == WSAEWOULDBLOCK) {
        m_state = State::Connecting;
        return;
    }
    CloseAndRetry(nowMs, "connect failed");
}

void NetClient::OnConnected(uint64_t nowMs)
{
    int flag = 1;
    setsockopt(m_sock, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&flag),
               static_cast<int>(sizeof(flag)));

    m_rx.clear();
    m_state = State::Handshaking;
    m_stateSince = nowMs;
    m_lastPing = nowMs;
    m_lastPong = nowMs;

    std::vector<uint8_t> payload;
    SsscPutU16(payload, static_cast<uint16_t>(m_helloW));
    SsscPutU16(payload, static_cast<uint16_t>(m_helloH));
    payload.push_back(static_cast<uint8_t>(m_helloFormat));
    payload.push_back(static_cast<uint8_t>(m_helloFps));
    SsscPutU32(payload, 0);
    const std::vector<uint8_t> msg =
        SsscMakeMessage(SSSC_TYPE_HELLO, payload.data(), static_cast<uint32_t>(payload.size()));
    Enqueue(msg.data(), msg.size());
    LogInfo("connected, HELLO sent (%dx%d fmt=%d fps=%d)", m_helloW, m_helloH, m_helloFormat,
            m_helloFps);
}

void NetClient::CloseAndRetry(uint64_t nowMs, const char* reason)
{
    const bool wasReady = (m_state == State::Ready);
    if (m_sock != INVALID_SOCKET) {
        closesocket(m_sock);
        m_sock = INVALID_SOCKET;
    }
    m_queue.clear();
    m_queuedBytes = 0;
    m_rx.clear();
    m_state = State::RetryWait;
    m_nextRetry = nowMs + kRetryDelayMs;
    m_qualityPending = false;
    m_readyEdge = false;
    if (wasReady || nowMs - m_lastRetryLog >= 5000) {
        LogWarn("connection lost (%s), retrying in 1s", reason);
        m_lastRetryLog = nowMs;
    }
}

void NetClient::PumpRecv(uint64_t nowMs)
{
    uint8_t buf[16384];
    for (;;) {
        const int n = recv(m_sock, reinterpret_cast<char*>(buf), static_cast<int>(sizeof(buf)), 0);
        if (n > 0) {
            m_rx.insert(m_rx.end(), buf, buf + n);
            continue;
        }
        if (n == 0) {
            CloseAndRetry(nowMs, "peer closed");
            return;
        }
        const int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK) {
            break;
        }
        CloseAndRetry(nowMs, "recv failed");
        return;
    }

    size_t offset = 0;
    while (m_rx.size() - offset >= sizeof(sssc_hdr)) {
        sssc_hdr hdr{};
        std::memcpy(&hdr, m_rx.data() + offset, sizeof(hdr));
        if (hdr.magic != SSSC_MAGIC || hdr.version != SSSC_VERSION) {
            CloseAndRetry(nowMs, "bad header");
            return;
        }
        if (hdr.length > SSSC_MAX_MSG_ACCEPT) {
            CloseAndRetry(nowMs, "payload too large");
            return;
        }
        const size_t total = sizeof(sssc_hdr) + hdr.length;
        if (m_rx.size() - offset < total) {
            break;
        }
        HandleMessage(hdr.type, m_rx.data() + offset + sizeof(sssc_hdr), hdr.length, nowMs);
        if (m_state != State::Handshaking && m_state != State::Ready) {
            return;
        }
        offset += total;
    }
    if (offset > 0) {
        m_rx.erase(m_rx.begin(), m_rx.begin() + static_cast<std::ptrdiff_t>(offset));
    }
}

void NetClient::PumpSend(uint64_t nowMs)
{
    while (!m_queue.empty()) {
        OutMsg& msg = m_queue.front();
        const size_t remaining = msg.data.size() - msg.sent;
        if (remaining == 0) {
            m_queue.pop_front();
            continue;
        }
        const int n = send(m_sock, reinterpret_cast<const char*>(msg.data.data() + msg.sent),
                           static_cast<int>(remaining), 0);
        if (n > 0) {
            msg.sent += static_cast<size_t>(n);
            m_txBytes += static_cast<uint64_t>(n);
            m_queuedBytes -= static_cast<size_t>(n);
            if (msg.sent == msg.data.size()) {
                m_queue.pop_front();
            }
            continue;
        }
        if (n == 0) {
            CloseAndRetry(nowMs, "send closed");
            return;
        }
        const int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK) {
            return;
        }
        CloseAndRetry(nowMs, "send failed");
        return;
    }
}

void NetClient::HandleMessage(uint16_t type, const uint8_t* payload, uint32_t len, uint64_t nowMs)
{
    switch (type) {
        case SSSC_TYPE_HELLO_ACK: {
            if (m_state != State::Handshaking) {
                LogWarn("unexpected HELLO_ACK ignored");
                return;
            }
            if (len != sizeof(sssc_hello_ack)) {
                CloseAndRetry(nowMs, "HELLO_ACK size");
                return;
            }
            sssc_hello_ack ack{};
            std::memcpy(&ack, payload, sizeof(ack));
            if (ack.status != SSSC_STATUS_OK) {
                CloseAndRetry(nowMs, "HELLO_ACK rejected");
                return;
            }
            if (ack.tile_size != SSSC_TILE_SIZE) {
                CloseAndRetry(nowMs, "HELLO_ACK tile_size");
                return;
            }
            if (ack.width == 0 || ack.height == 0) {
                CloseAndRetry(nowMs, "HELLO_ACK target size");
                return;
            }
            if (ack.format != SSSC_FMT_BGRA32 && ack.format != SSSC_FMT_RGB565) {
                CloseAndRetry(nowMs, "HELLO_ACK format");
                return;
            }
            m_sessionFormat = ack.format;
            m_sessionWidth = static_cast<int>(ack.width);
            m_sessionHeight = static_cast<int>(ack.height);
            m_state = State::Ready;
            m_readyEdge = true;
            m_lastPong = nowMs;
            m_lastPing = nowMs;
            LogInfo("HELLO_ACK ok, target %ux%u (virtual %dx%d) format=%u", ack.width,
                    ack.height, m_helloW, m_helloH, ack.format);
            return;
        }
        case SSSC_TYPE_PONG:
            if (len != sizeof(sssc_ping)) {
                LogWarn("PONG bad size ignored");
                return;
            }
            m_lastPong = nowMs;
            return;
        case SSSC_TYPE_SET_QUALITY: {
            if (len != sizeof(sssc_set_quality)) {
                LogWarn("SET_QUALITY bad size ignored");
                return;
            }
            sssc_set_quality q{};
            std::memcpy(&q, payload, sizeof(q));
            if (q.format != SSSC_FMT_BGRA32 && q.format != SSSC_FMT_RGB565) {
                LogWarn("SET_QUALITY invalid format %u ignored", q.format);
                return;
            }
            m_quality.format = q.format;
            m_quality.fps = q.fps_cap;
            m_qualityPending = true;
            LogInfo("SET_QUALITY rx format=%u fps=%u", q.format, q.fps_cap);
            return;
        }
        default:
            LogWarn("unknown message type=0x%04x ignored", type);
            return;
    }
}

void NetClient::EnqueuePing(uint64_t nowMs)
{
    std::vector<uint8_t> payload;
    SsscPutU32(payload, static_cast<uint32_t>(nowMs));
    const std::vector<uint8_t> msg =
        SsscMakeMessage(SSSC_TYPE_PING, payload.data(), static_cast<uint32_t>(payload.size()));
    Enqueue(msg.data(), msg.size());
}

bool NetClient::Enqueue(const void* data, size_t len)
{
    if ((m_state != State::Handshaking && m_state != State::Ready) ||
        m_sock == INVALID_SOCKET || len == 0) {
        return false;
    }
    while (m_queuedBytes + len > kQueueCap && !m_queue.empty() && m_queue.front().sent == 0) {
        m_queuedBytes -= m_queue.front().data.size();
        m_queue.pop_front();
        const uint64_t nowTick = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
        if (nowTick - m_lastDropLog >= 5000) {
            LogWarn("send queue full, dropping unsent messages");
            m_lastDropLog = nowTick;
        }
    }
    const auto* bytes = static_cast<const uint8_t*>(data);
    OutMsg msg;
    msg.data.assign(bytes, bytes + len);
    m_queuedBytes += len;
    m_queue.push_back(std::move(msg));
    return true;
}

void NetClient::FlushUnsent()
{
    if (m_queue.empty()) {
        return;
    }
    size_t dropped = 0;
    if (m_queue.front().sent > 0) {
        while (m_queue.size() > 1) {
            dropped += m_queue.back().data.size();
            m_queuedBytes -= m_queue.back().data.size();
            m_queue.pop_back();
        }
    } else {
        for (const OutMsg& msg : m_queue) {
            dropped += msg.data.size();
        }
        m_queue.clear();
        m_queuedBytes = 0;
    }
    if (dropped > 0) {
        LogInfo("flushed %llu bytes of unsent data", static_cast<unsigned long long>(dropped));
    }
}
