// 录制状态机主流程：采集线程 -> 编码 -> 环形预录缓冲 -> 分段写盘
// 状态：IDLE / RECORDING。demo 策略：编码后帧持续写盘（预录缓冲同步更新），
// 板端移植时改为"触发才落盘 + 回填预录"（见 triggerRecord 注释）。
#include "middleware/recorder.hpp"
#include "middleware/integrity.hpp"
#include "middleware/trusted_time.hpp"
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

    // 0) 可信时间对时（6.4.17）：失败不阻塞录制（降级为不可信墙钟，OSD 带标记）
    {
        auto& tc = TrustedClock::instance();
        tc.configure(TrustedClock::Config{});
        tc.sync();
    }

    // 1) 打开视频源（PC: V4L2；板端换 create_rkmpi_source）
    video_ = create_v4l2_source();
    if (!video_->open("/dev/video0", cfg_.width, cfg_.height, cfg_.fps)) return false;
    cfg_.width = video_->width();
    cfg_.height = video_->height();

    // 2) 编码器（板端换 MPP VENC）
    encoder_ = create_x264_encoder();
    if (!encoder_->open(cfg_.width, cfg_.height, cfg_.fps, cfg_.bitrate_kbps)) return false;

    // 2.5) OSD 初始化（编码前烧帧：时间戳+设备编号；板端换 RGA 通道）
    osd_enabled_ = osd_.init(cfg_.width, cfg_.height, cfg_.fps, "RV1126-DEMO-001");

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
        // 哈希链防篡改（B11）：每段链哈希含前段链哈希，任何段被替换/删除即断链
        chain_append(cfg_.output_dir + "/manifest.sha256", p);
        // 满盘循环覆盖（6.4.12）：每段落盘后检查配额，超限删最旧段（重点标记保护）
        enforce_storage_quota(cfg_.output_dir, cfg_.storage_quota_bytes,
                              cfg_.output_dir + "/manifest.sha256");
    });

    running_ = true;
    state_ = State::Idle;
    worker_ = std::thread(&Recorder::run, this);
    return true;
}

// 重点文件标记（6.2.20）：录像中将当前分段写入 manifest 标记行（附时间可信状态）
void Recorder::triggerMark(const std::string& note) {
    if (state_ != State::Recording && state_ != State::StoppingPending) return;
    const std::string& seg = seg_.currentSegment();
    if (seg.empty()) return;
    auto& tc = TrustedClock::instance();
    // 可信状态写进标记行：时间不可信的标记证据效力打折，取证时一眼可辨
    std::string full_note = (tc.trusted() ? "[trusted-time] " : "[untrusted-time] ") + note;
    mark_segment(cfg_.output_dir + "/manifest.sha256", seg, full_note);
}

// 用户触发"停止录像"：RECORDING -> StoppingPending（延录 post_record_ms 后由 run 线程收尾）
void Recorder::triggerStop() {
    if (state_ != State::Recording) return;
    want_stop_ = true;  // 由 run() 线程在帧边界切换状态（与触发同一线程，无锁竞争）
}

// 用户触发"开始录像"：IDLE -> RECORDING（回填预录）；延录期(StoppingPending)再触发
// 则重置延录窗口回到 RECORDING（真实设备语义：延录中再按"开始"=继续录，非忽略）。
// 预录回填与实时写盘在同一线程（run 循环）串行执行，保证帧序与 PTS 单调。
void Recorder::triggerRecord() {
    if (!running_ || state_ == State::Recording) return;
    want_record_ = true;  // 置位标志，由 run() 线程在帧边界执行（避免跨线程操作封装器）
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
        // OSD 烧帧（编码前叠加，GA/T 947.2 6.2.8）：时间源为可信时钟（6.4.17），
        // 不可信时加 * 前缀警示（证据效力打折，提醒校时）
        if (osd_enabled_) {
            auto& tc = TrustedClock::instance();
            osd_.apply(raw, tc.wallClockSecs(), tc.trusted());
        }
        VideoFrame enc;
        if (!encoder_->encode(raw, enc)) continue;
        ring_->push(enc);  // 预录缓冲始终更新（编码帧，~0.5MB/s）

        // 触发检查（帧边界原子切换）：音频预录先回填 -> 视频预录回填 -> 进实时流
        if (want_record_.exchange(false)) {
            if (state_ == State::StoppingPending) {
                // 延录期再触发：重置延录窗口回 RECORDING（写盘不中断，无预录回填
                // ——当前段一直在写，回填语义只适用于 IDLE 冷启动）
                state_ = State::Recording;
            } else if (state_ == State::Idle) {
                // 音频在前：回填触发前缓存帧（pts 沿用样本计数，单调）
                for (auto& a : drainAudioRing()) writeAudioFrame(a.data.data(), a.data.size(), a.pts);
                // 视频预录：从最近 I 帧开始的触发前帧
                auto pre = ring_->drainForPrerecord();
                for (auto& f : pre) writeFrame(f);
                state_ = State::Recording;
            }
        }
        // 停止触发：RECORDING -> StoppingPending（延录），到截止时刻自动收尾回 Idle
        if (want_stop_.exchange(false) && state_ == State::Recording) {
            state_ = State::StoppingPending;
            post_deadline_ms_ = now_ms() + cfg_.post_record_ms;
        }
        if (state_ == State::StoppingPending && now_ms() >= post_deadline_ms_) {
            seg_.closeCurrent();   // 延录到点：收尾当前段，回 Idle（可再次触发）
            state_ = State::Idle;
        }
        if (state_ == State::Recording ||
            (state_ == State::StoppingPending && now_ms() < post_deadline_ms_))
            writeFrame(enc);
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
        // Recording 与延录(StoppingPending)期间都持续写盘，延录到点回 Idle 后停
        if (state_ == State::Idle) continue;
        writeAudioFrame(enc.data.data(), enc.data.size(), pts);
    }
}
