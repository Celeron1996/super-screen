/*
 * super_screen_win 入口：配置、采集循环、分块发送、统计与调试开关。
 * 主循环见 docs/plan-windows.md §4.1/§4.8；协议见 docs/PLAN.md §5。
 */
#include <winsock2.h>
#include <windows.h>
#include <psapi.h>
#include <timeapi.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "capture_dxgi.h"
#include "capture_gdi.h"
#include "config.h"
#include "cursor.h"
#include "log.h"
#include "lz4wrap.h"
#include "monitor.h"
#include "net.h"
#include "protocol_util.h"
#include "sssc.h"
#include "tiles.h"

namespace {

volatile LONG g_stop = 0;

BOOL WINAPI ConsoleCtrlHandler(DWORD type)
{
    switch (type) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            InterlockedExchange(&g_stop, 1);
            return TRUE;
        default:
            return FALSE;
    }
}

uint64_t SteadyMs()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

uint64_t UnixMs()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

uint64_t CurrentRssKb()
{
    PROCESS_MEMORY_COUNTERS pmc{};
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)) == FALSE) {
        return 0;
    }
    return static_cast<uint64_t>(pmc.WorkingSetSize / 1024u);
}

bool SaveBmp(const char* path, const uint8_t* frame, int width, int height, int stride)
{
    const uint32_t imageSize =
        static_cast<uint32_t>(width) * static_cast<uint32_t>(height) * 4u;
    BITMAPFILEHEADER fileHeader{};
    fileHeader.bfType = 0x4D42;
    fileHeader.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    fileHeader.bfSize = fileHeader.bfOffBits + imageSize;
    BITMAPINFOHEADER infoHeader{};
    infoHeader.biSize = sizeof(BITMAPINFOHEADER);
    infoHeader.biWidth = width;
    infoHeader.biHeight = height;
    infoHeader.biPlanes = 1;
    infoHeader.biBitCount = 32;
    infoHeader.biCompression = BI_RGB;
    infoHeader.biSizeImage = imageSize;

    FILE* fp = nullptr;
    if (fopen_s(&fp, path, "wb") != 0 || fp == nullptr) {
        LogError("dump-frame: cannot open '%s'", path);
        return false;
    }
    bool ok = std::fwrite(&fileHeader, sizeof(fileHeader), 1, fp) == 1 &&
              std::fwrite(&infoHeader, sizeof(infoHeader), 1, fp) == 1;
    if (ok) {
        for (int y = height - 1; y >= 0; --y) {
            if (std::fwrite(frame + static_cast<size_t>(y) * stride,
                            static_cast<size_t>(width) * 4u, 1, fp) != 1) {
                ok = false;
                break;
            }
        }
    }
    std::fclose(fp);
    if (!ok) {
        LogError("dump-frame: write failed");
    }
    return ok;
}

void ConvertToRgb565(const uint8_t* src, uint8_t* dst, int pixels)
{
    for (int i = 0; i < pixels; ++i) {
        const uint8_t b = src[i * 4 + 0];
        const uint8_t g = src[i * 4 + 1];
        const uint8_t r = src[i * 4 + 2];
        const uint16_t v =
            static_cast<uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        dst[i * 2 + 0] = static_cast<uint8_t>(v & 0xFFu);
        dst[i * 2 + 1] = static_cast<uint8_t>(v >> 8);
    }
}

class FrameSender {
public:
    void Init(NetClient* net, int format, bool useLz4)
    {
        m_net = net;
        m_format = format;
        m_useLz4 = useLz4;
    }

    void ResetSeq() { m_seq = 1; }
    void SetFormat(int format) { m_format = format; }
    int Format() const { return m_format; }
    uint64_t TilesSent() const { return m_tiles; }
    uint64_t RawBytes() const { return m_rawBytes; }
    uint64_t CompressedBytes() const { return m_compBytes; }

    bool Send(const uint8_t* frame, int stride, const std::vector<TileRect>& rects,
              bool fullRefresh)
    {
        if (fullRefresh && !SendMarker(SSSC_TYPE_FULL_BEGIN)) {
            return false;
        }
        m_payload.assign(8, 0);
        m_count = 0;
        for (const TileRect& rect : rects) {
            if (!SendTile(frame, stride, rect)) {
                return false;
            }
        }
        if (!FlushTiles()) {
            return false;
        }
        if (fullRefresh && !SendMarker(SSSC_TYPE_FULL_END)) {
            return false;
        }
        return true;
    }

private:
    bool SendMarker(uint16_t type)
    {
        std::vector<uint8_t> payload;
        SsscPutU32(payload, m_seq++);
        SsscPutU32(payload, 0);
        const std::vector<uint8_t> msg =
            SsscMakeMessage(type, payload.data(), static_cast<uint32_t>(payload.size()));
        return m_net->Enqueue(msg.data(), msg.size());
    }

    bool SendTile(const uint8_t* frame, int stride, const TileRect& rect)
    {
        const int bpp = (m_format == SSSC_FMT_RGB565) ? 2 : 4;
        const int rawLen = rect.w * rect.h * bpp;
        m_scratch.resize(static_cast<size_t>(rawLen));
        for (int yy = 0; yy < rect.h; ++yy) {
            const uint8_t* src = frame + static_cast<size_t>(rect.y + yy) * stride +
                                 static_cast<size_t>(rect.x) * 4u;
            uint8_t* dst =
                m_scratch.data() +
                static_cast<size_t>(yy) * static_cast<size_t>(rect.w) * static_cast<size_t>(bpp);
            if (bpp == 4) {
                std::memcpy(dst, src, static_cast<size_t>(rect.w) * 4u);
            } else {
                ConvertToRgb565(src, dst, rect.w);
            }
        }

        const uint8_t* data = m_scratch.data();
        int dataLen = rawLen;
        uint8_t codec = static_cast<uint8_t>(SSSC_CODEC_RAW);
        if (m_useLz4) {
            const int bound = SsscLz4Bound(rawLen);
            m_comp.resize(static_cast<size_t>(bound));
            const int packed = SsscLz4Compress(m_scratch.data(), rawLen, m_comp.data(), bound);
            if (packed > 0 && packed < rawLen) {
                data = m_comp.data();
                dataLen = packed;
                codec = static_cast<uint8_t>(SSSC_CODEC_LZ4);
                m_rawBytes += static_cast<uint64_t>(rawLen);
                m_compBytes += static_cast<uint64_t>(packed);
            }
        }

        if (m_payload.size() + sizeof(sssc_tile_hdr) + static_cast<size_t>(dataLen) >
            SSSC_MAX_MSG_PAYLOAD) {
            if (!FlushTiles()) {
                return false;
            }
        }

        SsscPutU16(m_payload, static_cast<uint16_t>(rect.x));
        SsscPutU16(m_payload, static_cast<uint16_t>(rect.y));
        SsscPutU16(m_payload, static_cast<uint16_t>(rect.w));
        SsscPutU16(m_payload, static_cast<uint16_t>(rect.h));
        m_payload.push_back(codec);
        m_payload.push_back(0);
        SsscPutU32(m_payload, static_cast<uint32_t>(dataLen));
        m_payload.insert(m_payload.end(), data, data + dataLen);
        ++m_count;
        return true;
    }

    bool FlushTiles()
    {
        if (m_count == 0) {
            return true;
        }
        m_payload[0] = static_cast<uint8_t>(m_seq & 0xFFu);
        m_payload[1] = static_cast<uint8_t>((m_seq >> 8) & 0xFFu);
        m_payload[2] = static_cast<uint8_t>((m_seq >> 16) & 0xFFu);
        m_payload[3] = static_cast<uint8_t>((m_seq >> 24) & 0xFFu);
        ++m_seq;
        m_payload[4] = static_cast<uint8_t>(m_count & 0xFFu);
        m_payload[5] = static_cast<uint8_t>((m_count >> 8) & 0xFFu);
        m_payload[6] = 0;
        m_payload[7] = 0;
        const std::vector<uint8_t> msg =
            SsscMakeMessage(SSSC_TYPE_TILES, m_payload.data(),
                            static_cast<uint32_t>(m_payload.size()));
        const bool ok = m_net->Enqueue(msg.data(), msg.size());
        if (ok) {
            m_tiles += m_count;
        }
        m_count = 0;
        m_payload.assign(8, 0);
        return ok;
    }

    NetClient* m_net = nullptr;
    int m_format = 0;
    bool m_useLz4 = true;
    uint32_t m_seq = 1;
    uint16_t m_count = 0;
    uint64_t m_tiles = 0;
    uint64_t m_rawBytes = 0;
    uint64_t m_compBytes = 0;
    std::vector<uint8_t> m_scratch;
    std::vector<uint8_t> m_comp;
    std::vector<uint8_t> m_payload;
};

const char* kUsage = "usage: super_screen_win [--config FILE] [--dump-frame FILE]\n";

std::wstring Utf8ToWide(const char* text)
{
    const int need = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (need <= 1) {
        return std::wstring();
    }
    std::wstring out(static_cast<size_t>(need), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, out.data(), need);
    out.resize(static_cast<size_t>(need - 1));
    return out;
}

std::wstring DefaultIniPath()
{
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring full(path);
    const size_t slash = full.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        full.resize(slash + 1);
    } else {
        full.clear();
    }
    full += L"config.ini";
    return full;
}

int Run(const Config& cfg, const char* dumpPath)
{
    NetClient net;
    if (!net.Init(cfg.serverIp, cfg.serverPort)) {
        return 2;
    }

    MonitorTarget target;
    std::vector<DisplayInfo> displays;
    bool haveTarget = FindMonitorTarget(cfg.monitorMatch, &target, &displays);
    if (!haveTarget) {
        LogWarn("target monitor '%s' not found (is the virtual display in extend mode?)",
                cfg.monitorMatch.c_str());
        LogDisplays(displays);
    }

    GdiCapturer capturer;
    DxgiCapturer dxgi;
    CursorCompositor cursor;
    TileEngine tiles;
    bool captureReady = false;
    bool cursorReady = false;
    bool useDxgi = false;
    bool dxgiHasFrame = false;
    std::vector<uint8_t> dxgiFrame;

    const auto setupCapture = [&]() -> bool {
        if (!capturer.Init(target.width, target.height)) {
            return false;
        }
        tiles.Configure(target.width, target.height, cfg.tileSize);
        tiles.SetFullRefreshSec(cfg.fullRefreshSec);
        if (cfg.compositeCursor && !cursorReady) {
            cursorReady = cursor.Init();
        }
        if (_stricmp(cfg.captureMode.c_str(), "dxgi") == 0) {
            if (dxgi.Init(target.rect, target.width, target.height)) {
                useDxgi = true;
                dxgiHasFrame = false;
                dxgiFrame.assign(static_cast<size_t>(target.width) *
                                     static_cast<size_t>(target.height) * 4u,
                                 0);
                LogInfo("capture mode: DXGI Desktop Duplication");
            } else {
                dxgi.Shutdown();
                useDxgi = false;
                LogWarn("DXGI duplication unavailable, falling back to GDI");
            }
        }
        return true;
    };
    if (haveTarget) {
        captureReady = setupCapture();
        if (!captureReady) {
            LogError("capture init failed");
            return 2;
        }
        LogInfo("capture target %ls %dx%d at (%d,%d)", target.device.c_str(), target.width,
                target.height, target.rect.left, target.rect.top);
    }

    FrameSender sender;
    sender.Init(&net, cfg.format, cfg.lz4);

    int requestedFormat = cfg.format;
    int requestedFps = cfg.fpsCap;
    int backpressureFps = cfg.fpsCap;
    bool pendingFull = true;
    int skipCount = 0;
    uint64_t burstGraceUntil = 0;
    uint64_t frames = 0;
    uint64_t fullRefreshes = 0;
    uint64_t nextFrameMs = 0;
    uint64_t nextFindMs = 0;
    uint64_t statsMs = SteadyMs();
    uint64_t lastFrames = 0;
    uint64_t lastTiles = 0;
    uint64_t lastTx = 0;
    uint64_t captureSum = 0;
    uint64_t captureMax = 0;
    uint64_t captureCount = 0;
    uint64_t dirtySum = 0;
    uint64_t dirtyMax = 0;
    uint64_t lastChangeUnix = 0;

    while (InterlockedCompareExchange(&g_stop, 1, 1) == 0) {
        const uint64_t now = SteadyMs();

        if (!captureReady && now >= nextFindMs) {
            haveTarget = FindMonitorTarget(cfg.monitorMatch, &target, &displays);
            if (haveTarget && setupCapture()) {
                captureReady = true;
                pendingFull = true;
                LogInfo("capture target %ls %dx%d at (%d,%d)", target.device.c_str(),
                        target.width, target.height, target.rect.left, target.rect.top);
            } else {
                LogDisplays(displays);
                LogWarn("waiting for monitor '%s' (extend mode required)",
                        cfg.monitorMatch.c_str());
                nextFindMs = now + 3000;
            }
        }

        if (captureReady) {
            net.SetHelloParams(target.width, target.height, requestedFormat, requestedFps);
            net.Tick(now);
        }

        if (net.ConsumeReadyEdge()) {
            pendingFull = true;
            backpressureFps = cfg.fpsCap;
            skipCount = 0;
            burstGraceUntil = 0;
            sender.ResetSeq();
            if (captureReady) {
                tiles.Invalidate();
            }
            LogInfo("session ready: scheduling full refresh");
        }

        NetQuality quality;
        if (net.TakeQuality(&quality)) {
            int newFps = quality.fps;
            if (newFps < static_cast<int>(SSSC_FPS_CAP_MIN)) {
                newFps = static_cast<int>(SSSC_FPS_CAP_MIN);
            }
            if (newFps > requestedFps) {
                newFps = requestedFps;
            }
            requestedFps = newFps;
            if (quality.format != sender.Format()) {
                requestedFormat = quality.format;
                sender.SetFormat(quality.format);
                net.FlushUnsent();
                if (captureReady) {
                    tiles.Invalidate();
                }
                pendingFull = true;
            }
            LogInfo("SET_QUALITY adopted: format=%d fps=%d", quality.format, requestedFps);
        }

        if (captureReady && net.IsReady()) {
            const int effectiveFps =
                std::max(1, std::min(cfg.fpsCap, std::min(requestedFps, backpressureFps)));
            if (now >= nextFrameMs) {
                nextFrameMs = now + static_cast<uint64_t>(1000 / effectiveFps);
                if (net.QueueBytes() > SSSC_SEND_QUEUE_SKIP) {
                    // 全刷突发是我们自己瞬间入队的，短暂豁免降帧，避免每次全刷后误降
                    if (now >= burstGraceUntil) {
                        ++skipCount;
                        if (skipCount >= 3) {
                            backpressureFps = std::max(
                                static_cast<int>(SSSC_FPS_CAP_MIN), backpressureFps / 2);
                            skipCount = 0;
                            LogWarn("backpressure: 3 consecutive skips, fps_cap -> %d",
                                    backpressureFps);
                        }
                    }
                } else {
                    skipCount = 0;
                    burstGraceUntil = 0;
                    const uint64_t captureStart = SteadyMs();
                    bool captured = false;
                    if (useDxgi) {
                        const int rc = dxgi.Capture(dxgiFrame.data(), target.width * 4);
                        if (rc > 0) {
                            dxgiHasFrame = true;
                        }
                        if (rc >= 0 && dxgiHasFrame) {
                            // 静态桌面时 DXGI 不产帧；复用缓存帧，保证光标合成基于无光标的桌面图
                            std::memcpy(capturer.MutableData(), dxgiFrame.data(), dxgiFrame.size());
                            captured = true;
                        } else if (rc < 0) {
                            useDxgi = false;
                            dxgi.Shutdown();
                            LogWarn("DXGI capture lost, falling back to GDI");
                            captured = capturer.Capture(target.rect);
                        }
                    } else {
                        captured = capturer.Capture(target.rect);
                    }
                    if (captured) {
                        if (cfg.compositeCursor && cursorReady) {
                            cursor.Compose(capturer.MemDC(),
                                           const_cast<uint8_t*>(capturer.Data()), target.width,
                                           target.height, capturer.Stride(), target.rect);
                        }
                        const uint64_t capMs = SteadyMs() - captureStart;
                        captureSum += capMs;
                        captureMax = std::max(captureMax, capMs);
                        ++captureCount;

                        std::vector<TileRect> dirty;
                        tiles.CollectDirty(capturer.Data(), capturer.Stride(), &dirty);
                        dirtySum += dirty.size();
                        dirtyMax = std::max(dirtyMax, static_cast<uint64_t>(dirty.size()));
                        const bool full =
                            pendingFull ||
                            tiles.ShouldFullRefresh(static_cast<int>(dirty.size()), now);
                        bool sent = true;
                        if (full) {
                            sent = sender.Send(capturer.Data(), capturer.Stride(),
                                               tiles.AllTiles(), true);
                            if (sent) {
                                tiles.OnFullRefresh(now);
                                pendingFull = false;
                                backpressureFps = cfg.fpsCap;
                                skipCount = 0;
                                burstGraceUntil = now + 5000;
                                ++fullRefreshes;
                                if (dumpPath != nullptr &&
                                    SaveBmp(dumpPath, capturer.Data(), target.width,
                                            target.height, capturer.Stride())) {
                                    LogInfo("dump-frame saved to %s", dumpPath);
                                }
                            }
                        } else if (!dirty.empty()) {
                            sent =
                                sender.Send(capturer.Data(), capturer.Stride(), dirty, false);
                        }
                        if (!sent) {
                            pendingFull = true;
                        }
                        if (!dirty.empty()) {
                            const uint64_t unixNow = UnixMs();
                            if (unixNow - lastChangeUnix > 1000) {
                                LogInfo("lat_probe tx_ts=%llu",
                                        static_cast<unsigned long long>(unixNow));
                            }
                            lastChangeUnix = unixNow;
                        }
                        ++frames;
                    }
                }
            }
        }

        if (now - statsMs >= 5000) {
            const double sec = static_cast<double>(now - statsMs) / 1000.0;
            const uint64_t tx = net.TxBytes();
            const uint64_t tilesTotal = sender.TilesSent();
            const int effectiveFps =
                std::max(1, std::min(cfg.fpsCap, std::min(requestedFps, backpressureFps)));
            const double ratio =
                sender.CompressedBytes() != 0
                    ? static_cast<double>(sender.RawBytes()) /
                          static_cast<double>(sender.CompressedBytes())
                    : 0.0;
            const double captureAvg =
                captureCount != 0
                    ? static_cast<double>(captureSum) / static_cast<double>(captureCount)
                    : 0.0;
            const double dirtyAvg =
                captureCount != 0
                    ? static_cast<double>(dirtySum) / static_cast<double>(captureCount)
                    : 0.0;
            LogStats("fps=%.1f tiles_s=%.1f tx_kbps=%.1f rss_kb=%llu queue_kb=%llu "
                     "full_refresh=%llu capture_ms_avg=%.2f capture_ms_max=%llu ratio=%.2f "
                     "dirty_avg=%.1f dirty_max=%llu fps_cap=%d format=%d",
                     static_cast<double>(frames - lastFrames) / sec,
                     static_cast<double>(tilesTotal - lastTiles) / sec,
                     static_cast<double>(tx - lastTx) * 8.0 / 1000.0 / sec,
                     static_cast<unsigned long long>(CurrentRssKb()),
                     static_cast<unsigned long long>(net.QueueBytes() / 1024u),
                     static_cast<unsigned long long>(fullRefreshes), captureAvg,
                     static_cast<unsigned long long>(captureMax), ratio, dirtyAvg,
                     static_cast<unsigned long long>(dirtyMax), effectiveFps,
                     sender.Format());
            statsMs = now;
            lastFrames = frames;
            lastTiles = tilesTotal;
            lastTx = tx;
            captureSum = 0;
            captureMax = 0;
            captureCount = 0;
            dirtySum = 0;
            dirtyMax = 0;
        }

        Sleep(1);
    }

    LogInfo("stopping");
    net.Shutdown();
    capturer.Shutdown();
    cursor.Shutdown();
    return 0;
}

}  // namespace

int main(int argc, char** argv)
{
    LogInit();
    timeBeginPeriod(1);  // Sleep(1) 默认量化 ~15.6ms，会让 20fps 上限掉到 ~16fps
    const char* dumpPath = nullptr;
    const char* configPath = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--dump-frame") == 0 && i + 1 < argc) {
            dumpPath = argv[++i];
        } else if (std::strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            configPath = argv[++i];
        } else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            std::printf("%s", kUsage);
            return 0;
        } else {
            LogError("unknown argument '%s'", argv[i]);
            std::printf("%s", kUsage);
            return 1;
        }
    }

    if (SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) == FALSE) {
        SetProcessDPIAware();
    }
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    const std::wstring iniPath =
        configPath != nullptr ? Utf8ToWide(configPath) : DefaultIniPath();
    Config cfg;
    if (!LoadConfig(iniPath.c_str(), &cfg)) {
        return 2;
    }
    LogInfo("super_screen_win starting");
    const int rc = Run(cfg, dumpPath);
    timeEndPeriod(1);
    LogInfo("exit code %d", rc);
    return rc;
}
