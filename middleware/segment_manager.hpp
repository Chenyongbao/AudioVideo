#pragma once
// 分段录像管理：无缝切换的关键 —— 切换在帧边界原子完成。
// 线程模型：视频写盘(run 线程)与音频写盘(audioLoop 线程)并发，延录到点的
// closeCurrent 也在 run 线程——用内部互斥锁把 写帧/切段/收尾 串行化，
// 消除"音频包写进已关闭 writer"的竞态窗口（原为静默丢帧）。
#include "muxer/fmp4_segment_writer.hpp"
#include <memory>
#include <string>
#include <functional>
#include <mutex>

class SegmentManager {
public:
    struct Config {
        std::string dir = "./recordings";   // 分段输出目录
        int64_t segment_ms = 30 * 1000;     // 每段 30s（可配）
        int width = 1280, height = 720, fps = 25;
        int has_audio = 0;
        uint32_t audio_rate = 16000;
    };

    void configure(const Config& c) { cfg_ = c; }
    void setCodecParams(const AVCodecParameters* vpar, const AVCodecParameters* apar) {
        vpar_ = vpar; apar_ = apar;
    }

    // 供外部（录音回放预录帧）使用的帧入口：自动处理切段
    bool writeVideo(const AVPacket* pkt, AVRational tb, int64_t pts_ms) {
        std::lock_guard<std::mutex> lk(m_);
        ensureSegment(pts_ms);
        if (!writer_) return false;
        return writer_->writeVideo(pkt, tb);
    }
    bool writeAudio(const AVPacket* pkt, AVRational tb) {
        std::lock_guard<std::mutex> lk(m_);
        if (!writer_) return false;
        return writer_->writeAudio(pkt, tb);
    }

    // 每段正常收尾后回调（用于 SHA-256 manifest）
    void onSegmentClosed(std::function<void(const std::string&)> cb) { closed_cb_ = std::move(cb); }

    void closeCurrent() {
        std::lock_guard<std::mutex> lk(m_);
        closeCurrentLocked();
    }

    // 当前分段路径（重点标记用；无活动段返回空）
    const std::string& currentSegment() const { return cur_path_; }

private:
    void closeCurrentLocked() {
        if (writer_) {
            writer_->close();
            if (closed_cb_ && !cur_path_.empty()) closed_cb_(cur_path_);
            writer_.reset();
        }
    }

    void ensureSegment(int64_t pts_ms) {
        if (writer_ && pts_ms - seg_start_ms_ < cfg_.segment_ms) return;
        closeCurrentLocked();
        char name[64];
        snprintf(name, sizeof(name), "/seg_%lld.mp4", (long long)pts_ms);
        cur_path_ = cfg_.dir + name;
        writer_ = create_fmp4_writer();
        if (!writer_->open(cur_path_.c_str(), cfg_.width, cfg_.height, cfg_.fps,
                           cfg_.has_audio, cfg_.audio_rate)) {
            writer_.reset();
            return;
        }
        if (!writer_->start(vpar_, apar_)) { writer_.reset(); return; }
        seg_start_ms_ = pts_ms;
    }

    Config cfg_;
    const AVCodecParameters* vpar_ = nullptr;
    const AVCodecParameters* apar_ = nullptr;
    std::mutex m_;  // 串行化 writeVideo/writeAudio/closeCurrent（跨线程安全）
    std::unique_ptr<ISegmentWriter> writer_;
    std::string cur_path_;
    int64_t seg_start_ms_ = 0;
    std::function<void(const std::string&)> closed_cb_;
};
