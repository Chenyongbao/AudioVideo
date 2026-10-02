#pragma once
// ============================================================================
// 平台层公共定义：统一帧类型 + 单调时钟 + 通用环形缓冲
// ----------------------------------------------------------------------------
// 【设计意图】本头文件是整个音视频子系统的"数据普通话"：
//   - 平台层（采集）负责产出帧，中间件层（预录/分段/封装）只消费帧；
//   - 移植到 RV1126 时，RKMPI VI/AI 通道的帧一律转换为本结构，
//     中间件层代码零改动 —— 这是"平台无关中间件"能成立的关键。
// 【线程约定】结构本身不带锁，跨线程传递由各队列/管线自行同步。
// ============================================================================
#include <cstdint>
#include <cstring>
#include <vector>
#include <chrono>
#include <mutex>

// 单调毫秒时钟（CLOCK_MONOTONIC 语义：不受用户改墙钟/NTP 回跳影响）。
// 【为什么必须单调】pts_ms 驱动分段文件名（seg_<ms>.mp4）与切段判断，
//   墙钟一旦回跳（settimeofday/NTP 校正）文件名会冲突、切段逻辑会紊乱。
//   板端移植时：若接了可信时间源，墙钟值另行经 TrustedClock 注入 OSD/manifest，
//   与这里的单调钟严格分离。
int64_t now_ms();

enum class FrameType { Video, Audio };

// 视频帧：采集时为原始 NV12，编码后复用同一结构携带 H.264 NALU 流。
struct VideoFrame {
    FrameType type = FrameType::Video;
    int64_t pts_ms = 0;          // 单调毫秒时间戳（采集时刻，全局统一时钟源）
    int width = 0, height = 0;
    std::vector<uint8_t> data;   // 原始帧: NV12（Y平面 + 交错UV平面，4:2:0）
                                 // 编码帧: H.264 Annex-B NALU 流
    bool keyframe = false;       // 是否关键帧（I 帧）：预录回填的 GOP 对齐依据、
                                 // fMP4 分 fragment 的 flush 边界
    bool encoded = false;        // true=已编码数据，false=原始帧（编码器输入）
};

// 音频帧：采集为 PCM s16le 交织格式；板端移植时换成 G.711A 包（无需编码，
// 直接打包，码率 64kbps）。pts_ms 为采集时刻，与视频共享同一单调时钟。
struct AudioFrame {
    FrameType type = FrameType::Audio;
    int64_t pts_ms = 0;
    std::vector<uint8_t> data;   // PCM s16le 交织（单声道 16kHz @20ms 包）
    uint32_t sample_rate = 8000; // 采样率（实际由采集端填 16000）
    uint32_t samples = 0;        // 本包采样数（pts 推进单位：样本计数为主时钟）
};

// 通用线程安全环形缓冲（FIFO，容量固定，满则覆盖最旧）。
// 注意：预录缓冲实际用的是 middleware/ring_buffer.hpp 的 EncodedFrameRing
// （GOP 对齐淘汰），本模板供其他场景复用（如音频缓存备选实现）。
template <typename T>
class RingBuffer {
public:
    explicit RingBuffer(size_t capacity) : buf_(capacity) {}
    // 入队：满时自动覆盖最旧元素（tail_ 前移），语义=固定窗口
    void push(T v) {
        std::lock_guard<std::mutex> lk(m_);
        buf_[head_] = std::move(v);
        head_ = (head_ + 1) % buf_.size();
        if (count_ < buf_.size()) ++count_; else tail_ = (tail_ + 1) % buf_.size();
    }
    // 出队：空返回 false
    bool pop(T& out) {
        std::lock_guard<std::mutex> lk(m_);
        if (count_ == 0) return false;
        out = std::move(buf_[tail_]);
        tail_ = (tail_ + 1) % buf_.size();
        --count_;
        return true;
    }
    bool empty() const { std::lock_guard<std::mutex> lk(m_); return count_ == 0; }
    size_t size() const { std::lock_guard<std::mutex> lk(m_); return count_; }
    size_t capacity() const { return buf_.size(); }
    // 快照遍历（不弹出）：按队头→队尾顺序访问所有缓存元素，用于预录回填等
    // 需要整体读取但保留队列的场景。回调内不得修改本队列（持有锁期间）。
    template <typename F> void snapshot(F&& f) const {
        std::lock_guard<std::mutex> lk(m_);
        for (size_t i = 0; i < count_; ++i) f(buf_[(tail_ + i) % buf_.size()]);
    }
private:
    mutable std::mutex m_;   // mutable: const 查询接口也要加锁
    std::vector<T> buf_;     // 固定容量槽位数组
    size_t head_ = 0, tail_ = 0, count_ = 0;  // 写位/读位/元素数
};
