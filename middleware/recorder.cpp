// 录制状态机主流程：采集线程 -> 编码 -> 环形预录缓冲 -> 分段写盘
// 状态：IDLE / RECORDING。demo 策略：编码后帧持续写盘（预录缓冲同步更新），
// 板端移植时改为"触发才落盘 + 回填预录"（见 triggerRecord 注释）。
#include "middleware/recorder.hpp"
#include "middleware/integrity.hpp"
#include <sys/stat.h>
#include <thread>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/rational.h>
}

namespace {
void mkdirs(const std::string& p) { mkdir(p.c_str(), 0755); }
}

bool Recorder::start(const Recorder::Config& c) {
    if (running_) return false;
    cfg_ = c;
    mkdirs(cfg_.output_dir);

    // 1) 打开视频源（PC: V4L2；板端换 create_rkmpi_source）
    video_ = create_v4l2_source();
    if (!video_->open("/dev/video0", cfg_.width, cfg_.height, cfg_.fps)) return false;
    cfg_.width = video_->width();
    cfg_.height = video_->height();

    // 2) 编码器（板端换 MPP VENC）
    encoder_ = create_x264_encoder();
    if (!encoder_->open(cfg_.width, cfg_.height, cfg_.fps, cfg_.bitrate_kbps)) return false;

    // 3) 音频源：可选（WSL 常无声卡，失败不阻塞视频录制）
    audio_ = create_alsa_source();
    has_audio_ = audio_->open("default", 16000, 1);
    if (has_audio_) {
        aac_ = std::make_unique<AACEncoder>();
        if (!aac_->open(16000, 1)) { has_audio_ = false; aac_.reset(); }
    }
    if (has_audio_) audio_thread_ = std::thread(&Recorder::audioLoop, this);

    // 4) 预录环形缓冲：编码帧，按 GOP 对齐
    ring_ = std::make_unique<EncodedFrameRing>(1024, cfg_.prerecord_ms);

    // 5) 分段管理 + manifest 回调（每段收尾即算 SHA-256 追加清单）
    SegmentManager::Config sc;
    sc.dir = cfg_.output_dir;
    sc.segment_ms = cfg_.segment_ms;
    sc.width = cfg_.width; sc.height = cfg_.height; sc.fps = cfg_.fps;
    sc.has_audio = has_audio_ ? 1 : 0;
    sc.audio_rate = 16000;
    seg_.configure(sc);
    seg_.setCodecParams(encoder_->codecParams(),
                        has_audio_ ? aac_->codecParams() : nullptr);  // 双流参数
    seg_.onSegmentClosed([this](const std::string& p) {
        append_manifest(cfg_.output_dir + "/manifest.sha256", p);
    });

    running_ = true;
    state_ = State::Idle;
    worker_ = std::thread(&Recorder::run, this);
    return true;
}

// 用户触发"开始录像"：IDLE -> RECORDING。先回填预录（从最近 I 帧起），再进实时流。
// 预录回填与实时写盘在同一线程（run 循环）串行执行，保证帧序与 PTS 单调。
void Recorder::triggerRecord() {
    if (!running_ || state_ == State::Recording) return;
    want_record_ = true;  // 置位标志，由 run() 线程在帧边界执行回填（避免跨线程操作封装器）
}

void Recorder::stop() {
    running_ = false;
    if (worker_.joinable()) worker_.join();
    if (audio_thread_.joinable()) audio_thread_.join();
    seg_.closeCurrent();
    if (encoder_) encoder_->close();
    if (audio_) audio_->close();
    if (video_) video_->close();
    state_ = State::Idle;
}

void Recorder::run() {
    while (running_) {
        VideoFrame raw;
        if (!video_->read(raw)) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); continue; }
        VideoFrame enc;
        if (!encoder_->encode(raw, enc)) continue;
        ring_->push(enc);  // 预录缓冲始终更新（编码帧，~0.5MB/s）

        // 触发检查（帧边界原子切换）：音频预录先回填 -> 视频预录回填 -> 进实时流
        if (want_record_.exchange(false) && state_ == State::Idle) {
            // 音频在前：回填触发前缓存帧（pts 沿用样本计数，单调）
            for (auto& a : drainAudioRing()) writeAudioFrame(a.data.data(), a.data.size(), a.pts);
            // 视频预录：从最近 I 帧开始的触发前帧
            auto pre = ring_->drainForPrerecord();
            for (auto& f : pre) writeFrame(f);
            state_ = State::Recording;
        }
        if (state_ == State::Recording) writeFrame(enc);
    }
}

// 统一写帧入口：编码帧 -> AVPacket -> 分段管理（单调帧号 pts）
void Recorder::writeFrame(const VideoFrame& enc) {
    AVPacket pkt;
    av_init_packet(&pkt);
    pkt.data = const_cast<uint8_t*>(enc.data.data());
    pkt.size = (int)enc.data.size();
    // 用帧计数生成严格单调递增的 pts（ms 取整会产生重复 dts，被 muxer 拒绝）
    pkt.pts = pkt.dts = frame_index_++;
    if (enc.keyframe) pkt.flags |= AV_PKT_FLAG_KEY;
    AVRational tb{1, cfg_.fps};
    seg_.writeVideo(&pkt, tb, enc.pts_ms);
}

// 音频写帧入口（实时流与预录回填共用；pts=样本计数，严格单调）
void Recorder::writeAudioFrame(const uint8_t* data, size_t size, int64_t pts) {
    AVPacket pkt;
    av_init_packet(&pkt);
    pkt.data = const_cast<uint8_t*>(data);
    pkt.size = (int)size;
    pkt.pts = pkt.dts = pts;
    AVRational tb{1, 16000};
    seg_.writeAudio(&pkt, tb);
}

// 音频预录环形缓存：按时长淘汰（保留最近 prerecord_ms 的编码帧）
void Recorder::pushAudioRing(const VideoFrame& enc, int64_t pts, int64_t pts_ms) {
    std::lock_guard<std::mutex> lk(audio_ring_m_);
    audio_ring_.push_back({pts, pts_ms, enc.data});
    int64_t cutoff = pts_ms - cfg_.prerecord_ms;
    while (!audio_ring_.empty() && audio_ring_.front().pts_ms < cutoff)
        audio_ring_.pop_front();
}

std::vector<Recorder::AudioEncFrame> Recorder::drainAudioRing() {
    std::lock_guard<std::mutex> lk(audio_ring_m_);
    std::vector<AudioEncFrame> out(std::make_move_iterator(audio_ring_.begin()),
                                   std::make_move_iterator(audio_ring_.end()));
    audio_ring_.clear();
    return out;
}

void Recorder::audioLoop() {
    // 音频为主时钟：pts 以采样数为基准（time_base=1/rate），视频 PTS 在板端向其对齐
    // IDLE 期编码帧只进预录缓存不落盘；RECORDING 期实时写盘
    while (running_) {
        AudioFrame af;
        if (!audio_->read(af)) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); continue; }
        VideoFrame enc;  // 复用 VideoFrame 作编码帧容器
        if (!aac_->encode(af, enc)) continue;
        int64_t pts = audio_sample_cnt_;
        audio_sample_cnt_ += af.samples;
        pushAudioRing(enc, pts, af.pts_ms);
        if (state_ != State::Recording) continue;
        writeAudioFrame(enc.data.data(), enc.data.size(), pts);
    }
}
