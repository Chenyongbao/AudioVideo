#pragma once
// 录制器：组合 采集 -> 编码 -> 预录环形缓冲 -> 分段写盘 -> manifest
#include "middleware/segment_manager.hpp"
#include "middleware/ring_buffer.hpp"
#include "encoder/h264_encoder.hpp"
#include "encoder/aac_encoder.hpp"
#include "middleware/osd.hpp"
#include "platform/video_source.hpp"
#include "platform/audio_source.hpp"
#include <atomic>
#include <thread>
#include <mutex>
#include <deque>
#include <vector>

class Recorder {
public:
    struct Config {
        std::string output_dir = "./recordings";
        int width = 1280, height = 720, fps = 25;
        int bitrate_kbps = 4000;         // 对标 1080p 1h≤2.6GB ≈5.8Mbps，demo 用 4Mbps
        int64_t prerecord_ms = 30 * 1000; // 预录 30s（标准 ≥3s）
        int64_t segment_ms  = 30 * 1000;  // 分段 30s
        int64_t post_record_ms = 25 * 60 * 1000; // 延录 25min（标准/采购要求，可配；demo 可调小）
        int64_t storage_quota_bytes = 2LL * 1024 * 1024 * 1024; // 满盘配额（6.4.12），超限循环覆盖最旧段
    };

    ~Recorder() { stop(); }
    bool start(const Config& c);
    void triggerRecord();   // 状态机 IDLE -> RECORDING（含预录回填）
    void triggerStop();     // 停止触发：RECORDING -> StoppingPending（延录 post_record_ms 后停）
    void triggerMark(const std::string& note = "");  // 重点文件标记（6.2.20）：写入 manifest
    void stop();            // 立即停止（丢弃延录，收尾当前段）

    enum class State { Idle, Recording, StoppingPending };  // StoppingPending=延录中
    State state() const { return state_; }
    bool isRunning() const { return running_; }

private:
    void run();
    void audioLoop();
    void writeFrame(const VideoFrame& enc);  // 统一写帧入口（run 线程内调用）
    void writeAudioFrame(const uint8_t* data, size_t size, int64_t pts);  // 音频写帧入口（实时+回填共用）

    Config cfg_;
    std::atomic<bool> running_{false};
    std::atomic<State> state_{State::Idle};
    std::atomic<bool> want_record_{false};   // 触发标志：run 线程在帧边界消费
    std::atomic<bool> want_stop_{false};     // 停止触发标志：run 线程切 StoppingPending
    int64_t post_deadline_ms_ = 0;           // 延录截止时刻（StoppingPending 进入时刻 + post_record_ms）
    std::thread worker_;
    std::thread audio_thread_;
    bool has_audio_ = false;
    int64_t frame_index_ = 0;  // 单调递增帧号（pts 源，ms 取整会产生重复 dts）

    std::unique_ptr<IVideoSource> video_;
    std::unique_ptr<IAudioSource> audio_;
    std::unique_ptr<IVideoEncoder> encoder_;
    std::unique_ptr<AACEncoder> aac_;
    std::unique_ptr<EncodedFrameRing> ring_;
    SegmentManager seg_;
    OsdOverlay osd_;                 // OSD 烧帧（编码前，烧进码流不可分离）
    bool osd_enabled_ = false;
    int64_t audio_sample_cnt_ = 0;  // 音频样本计数（单调 pts 源，样本数=主时钟）

    // 音频预录环形缓存：编码后 AAC 帧按时间窗淘汰（64kbps 30s 仅 ~240KB）。
    // 音频无 GOP 概念，无需对齐淘汰；触发时随视频一起回填。
    struct AudioEncFrame {
        int64_t pts;           // 样本计数 pts（time_base 1/rate）
        int64_t pts_ms;
        std::vector<uint8_t> data;
    };
    std::mutex audio_ring_m_;
    std::deque<AudioEncFrame> audio_ring_;
    void pushAudioRing(const VideoFrame& enc, int64_t pts, int64_t pts_ms);
    std::vector<AudioEncFrame> drainAudioRing();  // 触发时取走全部缓存（含锁定窗口内的 pts 基准）
};
