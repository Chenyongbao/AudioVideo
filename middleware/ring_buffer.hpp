#pragma once
// 环形预录缓冲：缓存编码帧（GOP 对齐，按 I 帧为单位丢弃）
// 内存估算：1080p 主码流 4Mbps ≈ 0.5MB/s，预录 30s 仅约 15MB —— 缓存编码帧而非原始帧。
#include "platform/common.hpp"
#include <deque>
#include <mutex>
#include <vector>

// GOP 对齐的编码帧环形队列：push 编码帧；触发预录回填时从最近的 I 帧开始取。
class EncodedFrameRing {
public:
    explicit EncodedFrameRing(size_t max_frames, int64_t max_ms)
        : max_frames_(max_frames), max_ms_(max_ms) {}

    void push(const VideoFrame& f) {
        std::lock_guard<std::mutex> lk(m_);
        q_.push_back(f);
        while (!q_.empty()) {
            // 容量/时长超限：从队头丢，且保证队头从 I 帧开始（否则连 I 帧一起丢）
            if (q_.size() > max_frames_ || (q_.back().pts_ms - q_.front().pts_ms) > max_ms_) {
                q_.pop_front();
                while (!q_.empty() && !q_.front().keyframe) q_.pop_front();
            } else break;
        }
    }

    // 回填：从最近的 I 帧开始返回全部缓存帧（保证从 I 帧起播）
    std::vector<VideoFrame> drainForPrerecord() {
        std::lock_guard<std::mutex> lk(m_);
        std::vector<VideoFrame> out;
        size_t start = 0;
        while (start < q_.size() && !q_[start].keyframe) ++start;
        for (size_t i = start; i < q_.size(); ++i) out.push_back(q_[i]);
        return out;
    }

    void clear() { std::lock_guard<std::mutex> lk(m_); q_.clear(); }
    size_t size() const { std::lock_guard<std::mutex> lk(m_); return q_.size(); }

private:
    mutable std::mutex m_;
    std::deque<VideoFrame> q_;
    size_t max_frames_;
    int64_t max_ms_;
};
