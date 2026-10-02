#pragma once
// 可信时间源（GA/T 947.2 6.4.17）：SNTP 网络校时 + 计时误差监控。
// 可信判定：自上次成功校时起，本地时钟流逝与单调钟流逝的偏差 ≤ max_drift_ms/天才算可信。
// 板端替换为 GPS/北斗 NMEA 授时（接口不变，replace syncWithNtp 即可）。
#include <cstdint>
#include <string>
#include <atomic>
#include <mutex>

class TrustedClock {
public:
    struct Config {
        std::string ntp_server = "ntp.aliyun.com";
        int max_drift_ms_per_day = 3000;  // 标准 ≤3s/天
        int sync_timeout_ms = 2000;
    };

    static TrustedClock& instance();

    void configure(const Config& c);
    // 阻塞对时（SNTP 单请求）；成功后系统时间被校正并记录可信基准
    bool sync();
    // 当前可信时间（UTC 秒）；trusted() 为 false 时返回系统墙钟（不可信）
    int64_t wallClockSecs() const;
    bool trusted() const { return trusted_; }
    // 上次校时记录的偏移（系统时钟 - NTP 时刻，ms）；负值=系统慢
    int64_t lastOffsetMs() const { return last_offset_ms_; }

private:
    TrustedClock() = default;
    bool sntpQuery(int64_t& ntp_unix_ms);  // 单次 SNTP 请求

    Config cfg_;
    std::atomic<bool> trusted_{false};
    std::atomic<int64_t> last_offset_ms_{0};
    // 可信基准：校时成功瞬间的 (steady_ms, corrected_unix_ms) 对
    mutable std::mutex base_m_;
    int64_t base_steady_ms_ = 0;
    int64_t base_unix_ms_ = 0;
};
