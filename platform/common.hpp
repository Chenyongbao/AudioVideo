#pragma once
// 统一帧类型：平台层产出、中间件消费。移植到 RV1126 时 RKMPI 帧转为该结构即可。
#include <cstdint>
#include <cstring>
#include <vector>
#include <chrono>
#include <mutex>

// 单调毫秒时钟（音视频统一时间源；板端对齐 audio master clock 逻辑）
int64_t now_ms();

enum class FrameType { Video, Audio };

struct VideoFrame {
    FrameType type = FrameType::Video;
    int64_t pts_ms = 0;          // 统一毫秒时间戳
    int width = 0, height = 0;
    std::vector<uint8_t> data;   // NV12(YUV420SP) 或编码帧
    bool keyframe = false;
    bool encoded = false;        // true=已编码(H.264 NALU)，false=原始帧
};

struct AudioFrame {
    FrameType type = FrameType::Audio;
    int64_t pts_ms = 0;
    std::vector<uint8_t> data;   // PCM s16le 单声道（板端换成 G.711A 包）
    uint32_t sample_rate = 8000;
    uint32_t samples = 0;
};

// 通用线程安全环形缓冲（编码帧/GOP 对齐由上层保证）
template <typename T>
class RingBuffer {
public:
    explicit RingBuffer(size_t capacity) : buf_(capacity) {}
    void push(T v) {
        std::lock_guard<std::mutex> lk(m_);
        buf_[head_] = std::move(v);
        head_ = (head_ + 1) % buf_.size();
        if (count_ < buf_.size()) ++count_; else tail_ = (tail_ + 1) % buf_.size();
    }
    // 按 predicate 丢弃队头（如按 GOP 丢弃旧 I 帧之前的 P 帧）
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
    // 遍历快照（不弹出），用于预录回填
    template <typename F> void snapshot(F&& f) const {
        std::lock_guard<std::mutex> lk(m_);
        for (size_t i = 0; i < count_; ++i) f(buf_[(tail_ + i) % buf_.size()]);
    }
private:
    mutable std::mutex m_;
    std::vector<T> buf_;
    size_t head_ = 0, tail_ = 0, count_ = 0;
};
