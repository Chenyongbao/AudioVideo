#pragma once
// ============================================================================
// 预录环形缓冲（EncodedFrameRing）—— GA/T 947.2 预录指标（≥3s，本项目 30s）
// 的核心实现，也是"触发才落盘"语义的数据基础。
// ----------------------------------------------------------------------------
// 【缓存编码帧而非原始帧】关键取舍：
//   原始 NV12 640x480@30 ≈ 13MB/s，1080p 更高——缓存 30s 需数百 MB；
//   编码后 4Mbps ≈ 0.5MB/s，30s 仅 ~15MB —— 量级差异决定了必须缓存编码帧，
//   因此本队列位于编码器之后、封装器之前。
// 【GOP 对齐淘汰】丢弃旧帧时必须连带丢弃其后的 P 帧（P 帧依赖 I 帧），
//   否则回填的预录段从 P 帧起播会花屏。实现：pop 队头时若非 I 帧则继续丢弃，
//   直到队头是 I 帧——保证 drainForPrerecord() 返回的第一帧永远是可解码起点。
// 【线程模型】audioLoop/run 双线程 push，run 线程触发时 drain —— 内部互斥锁。
// ============================================================================
// 环形预录缓冲：缓存编码帧（GOP 对齐，按 I 帧为单位丢弃）
// 内存估算：1080p 主码流 4Mbps ≈ 0.5MB/s，预录 30s 仅约 15MB —— 缓存编码帧而非原始帧。
#include "platform/common.hpp"
#include <deque>
#include <mutex>
#include <vector>

// GOP 对齐的编码帧环形队列：push 编码帧；触发预录回填时从最近的 I 帧开始取。
class EncodedFrameRing {
public:
    // max_frames: 帧数上限；max_ms: 时长窗口（超窗即淘汰，配合构成双保险）
    explicit EncodedFrameRing(size_t max_frames, int64_t max_ms)
        : max_frames_(max_frames), max_ms_(max_ms) {}

    // 入队编码帧。超容量/超时长时从队头淘汰，且淘汰后强制队头为 I 帧
    // （连带丢掉不完整的 GOP 头部 P 帧，保证缓存始终从可解码点开始）
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

    // 回填：从最近的 I 帧开始返回全部缓存帧（保证从 I 帧起播）。
    // 语义：触发录像瞬间调用一次，返回值按序写入封装器即为"触发前画面"；
    // 队列保持不变（不清空），持续滚动供下次触发使用。
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
