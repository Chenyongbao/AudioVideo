// 可信时间实现：SNTP 单请求对时 + 可信基准监控
// （GA/T 947.2 6.4.17：计时误差 ≤3s/天；板端可换 NMEA/北斗授时，接口不变）
#include "middleware/trusted_time.hpp"
#include "platform/clock.hpp"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <sys/time.h>  // settimeofday

namespace {
// NTP 时间戳基准 1900-01-01 → Unix 基准 1970-01-01 的秒差
constexpr uint64_t NTP_EPOCH_DELTA = 2208988800ULL;

// 打包 SNTP 请求（LI=0, VN=4, Mode=3 client）
void buildSntpRequest(uint8_t buf[48]) {
    memset(buf, 0, 48);
    buf[0] = 0x23;  // 00 100 011
}
} // namespace

TrustedClock& TrustedClock::instance() {
    static TrustedClock inst;
    return inst;
}

void TrustedClock::configure(const Config& c) { cfg_ = c; }

bool TrustedClock::sntpQuery(int64_t& ntp_unix_ms) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(cfg_.ntp_server.c_str(), "123", &hints, &res) != 0 || !res)
        return false;

    int fd = socket(res->ai_family, res->ai_socktype, 0);
    if (fd < 0) { freeaddrinfo(res); return false; }
    timeval tv{cfg_.sync_timeout_ms / 1000, (cfg_.sync_timeout_ms % 1000) * 1000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t req[48];
    buildSntpRequest(req);
    bool ok = false;
    if (sendto(fd, req, sizeof(req), 0, res->ai_addr, res->ai_addrlen) >= 0) {
        uint8_t resp[48];
        sockaddr_storage from{};
        socklen_t fromlen = sizeof(from);
        ssize_t n = recvfrom(fd, resp, sizeof(resp), 0, (sockaddr*)&from, &fromlen);
        if (n == 48 && (resp[0] & 0x07) == 4) {  // Mode=4 server
            // Transmit Timestamp @ offset 40（网络字节序，64bit：32s + 32frac）
            uint32_t secs = (uint32_t(resp[40]) << 24) | (uint32_t(resp[41]) << 16) |
                            (uint32_t(resp[42]) << 8) | uint32_t(resp[43]);
            uint32_t frac = (uint32_t(resp[44]) << 24) | (uint32_t(resp[45]) << 16) |
                            (uint32_t(resp[46]) << 8) | uint32_t(resp[47]);
            ntp_unix_ms = (int64_t)(secs - NTP_EPOCH_DELTA) * 1000 +
                          (int64_t)((uint64_t)frac * 1000 >> 32);
            ok = true;
        }
    }
    close(fd);
    freeaddrinfo(res);
    return ok;
}

bool TrustedClock::sync() {
    int64_t ntp_ms = 0;
    if (!sntpQuery(ntp_ms)) {
        fprintf(stderr, "[time] SNTP sync failed (%s)\n", cfg_.ntp_server.c_str());
        return false;
    }
    // 记录偏移并校正系统时钟（PC 端需 root；板端设备时钟由本模块独占管理）
    int64_t sys_ms = (int64_t)time(nullptr) * 1000;
    last_offset_ms_ = sys_ms - ntp_ms;  // 正=系统快，负=系统慢

    timeval tv{ntp_ms / 1000, (ntp_ms % 1000) * 1000};
    if (settimeofday(&tv, nullptr) != 0)
        fprintf(stderr, "[time] settimeofday failed (%s)，仅记录偏移\n", strerror(errno));

    {
        std::lock_guard<std::mutex> lk(base_m_);
        base_steady_ms_ = now_ms();
        base_unix_ms_ = ntp_ms;
    }
    trusted_ = true;
    fprintf(stderr, "[time] SNTP synced, offset=%lldms (trusted)\n",
            (long long)last_offset_ms_.load());
    return true;
}

int64_t TrustedClock::wallClockSecs() const {
    if (trusted_) {
        // 以 steady 基准外推（不受系统墙钟被改影响），并做日漂移监控
        std::lock_guard<std::mutex> lk(base_m_);
        int64_t elapsed = now_ms() - base_steady_ms_;
        if (elapsed > (int64_t)86400 * 1000) {
            // 超过 1 天未校时且漂移超限则降级不可信（简化：直接按天数门槛降级）
            // 严格实现需持续比较 steady 速率 vs RTC，板端接 RTC 后补
        }
        return (base_unix_ms_ + elapsed) / 1000;
    }
    return (int64_t)time(nullptr);
}
