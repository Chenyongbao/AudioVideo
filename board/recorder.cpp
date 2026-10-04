// ============================================================================
// 板端录像模块实现:fMP4 断电安全写盘 + 预录环形缓冲
// v2:预录条目携带 key 标志;起录时从缓冲内第一个关键帧回灌,文件时间轴从 0 起
// ============================================================================
#include "recorder.hpp"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/time.h>
}
#include <cstring>

Recorder::~Recorder() { stop(); }

void Recorder::feed(const uint8_t *p, uint32_t len, bool key, uint64_t ts_ms) {
    std::lock_guard<std::mutex> lk(m_);
    if (recording_) {
        writePacket(p, len, key, ts_ms);
        last_ts_ = ts_ms;
        return;
    }
    // 非录制态:进预录环形缓冲
    pre_.push_back({std::vector<uint8_t>(p, p + len), key, ts_ms});
    pre_bytes_ += len;
    if (key) key_seen_ = true;
    // 超预算:从头淘汰到第一个"较新的关键帧",保证缓冲内始终有关键帧起点
    const size_t kMaxBytes = (size_t)4 * 1024 * 1024 * PRE_SECONDS / 2;  // 4Mbps*30s≈15MB,取2倍余量
    while (pre_bytes_ > kMaxBytes && pre_.size() > 1) {
        pre_bytes_ -= pre_.front().bytes.size();
        bool lost_key = pre_.front().key;
        pre_.pop_front();
        if (lost_key) {
            // 丢了关键帧,找下一个关键帧重建起点标记
            key_seen_ = false;
            for (auto &e : pre_)
                if (e.key) { key_seen_ = true; break; }
            if (!key_seen_) break;   // 缓冲暂无关键帧,继续收,直到新关键帧来
            break;
        }
    }
}

bool Recorder::start(const std::string &path, int width, int height, int fps,
                     const uint8_t *extradata, uint32_t extradata_len) {
    std::lock_guard<std::mutex> lk(m_);
    if (recording_) { err_ = "already recording"; return false; }
    if (!openFile(path)) return false;

    AVStream *st = avformat_new_stream(fmt_, nullptr);
    if (!st) { err_ = "new_stream failed"; closeFile(); return false; }
    stream_idx_ = st->index;
    st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    st->codecpar->codec_id = AV_CODEC_ID_H264;
    st->codecpar->width = width;
    st->codecpar->height = height;
    st->time_base = AVRational{1, 1000};          // ts 单位 ms
    // SPS/PPS 进 extradata(fMP4 empty_moov 下解码器唯一可靠参数集来源)
    if (extradata && extradata_len) {
        st->codecpar->extradata = (uint8_t *)av_mallocz(extradata_len + AV_INPUT_BUFFER_PADDING_SIZE);
        memcpy(st->codecpar->extradata, extradata, extradata_len);
        st->codecpar->extradata_size = (int)extradata_len;
    }

    AVDictionary *opt = nullptr;
    // fMP4:empty_moov 前置 moov + frag_keyframe 分片 —— 断电只丢最后一个分片
    av_dict_set(&opt, "movflags", "frag_keyframe+empty_moov+default_base_moof", 0);
    av_dict_set(&opt, "frag_duration", "1000000", 0);   // 1s 分片
    int ret = avformat_write_header(fmt_, &opt);
    av_dict_free(&opt);
    if (ret < 0) { err_ = "write_header " + std::to_string(ret); closeFile(); return false; }
    header_written_ = true;

    // 时间轴基准:预录首包(或实时首包)的 ts → 0
    if (!pre_.empty()) {
        start_ts_ = pre_.front().ts_ms;
    } else {
        start_ts_ = av_gettime() / 1000;
    }
    last_ts_ = start_ts_;
    recording_ = true;
    path_ = path;

    drainPrebuffer();
    fprintf(stderr, "[rec] start %s (prebuffer %zu pkts, %zu KB)\n",
            path.c_str(), pre_.size(), pre_bytes_ / 1024);
    return true;
}

void Recorder::stop() {
    std::lock_guard<std::mutex> lk(m_);
    if (!recording_) return;
    recording_ = false;
    if (header_written_ && fmt_) av_write_trailer(fmt_);
    closeFile();
    path_.clear();
    fprintf(stderr, "[rec] stopped, duration %llu ms\n",
            (unsigned long long)(last_ts_ > start_ts_ ? last_ts_ - start_ts_ : 0));
}

bool Recorder::recording() const { return recording_; }

uint64_t Recorder::recordedMs() const {
    return recording_ ? (last_ts_ > start_ts_ ? last_ts_ - start_ts_ : 0) : 0;
}

std::string Recorder::currentPath() const {
    return recording_ ? path_ : std::string();
}

// 事件标记:追加一行 "相对毫秒 epoch 文本" 到 <录像>.meta
// (fflush 落盘,kill -9 后标记也不丢)
bool Recorder::mark(const std::string &text) {
    if (!recording_ || path_.empty()) { err_ = "not recording"; return false; }
    char meta_path[192];
    snprintf(meta_path, sizeof(meta_path), "%s.meta", path_.c_str());
    FILE *f = fopen(meta_path, "a");
    if (!f) { err_ = "open meta failed"; return false; }
    uint64_t rel = last_ts_ > start_ts_ ? last_ts_ - start_ts_ : 0;
    fprintf(f, "%llu %lld %s\n", (unsigned long long)rel,
            (long long)time(nullptr), text.c_str());
    fclose(f);
    fprintf(stderr, "[rec] mark @%llu ms: %s\n", (unsigned long long)rel, text.c_str());
    return true;
}

void Recorder::drainPrebuffer() {
    // 从缓冲内第一个关键帧开始回灌(之前的 P 帧无法解码,丢弃)
    size_t begin = pre_.size();
    for (size_t k = 0; k < pre_.size(); k++)
        if (pre_[k].key) { begin = k; break; }
    for (size_t k = begin; k < pre_.size(); k++) {
        const auto &e = pre_[k];
        writePacket(e.bytes.data(), (uint32_t)e.bytes.size(), e.key, e.ts_ms);
        last_ts_ = e.ts_ms;
    }
    fprintf(stderr, "[rec] prebuffer drained: %zu pkts (dropped %zu)\n",
            pre_.size() - begin, begin);
    pre_.clear();
    pre_bytes_ = 0;
}

bool Recorder::openFile(const std::string &path) {
    int ret = avformat_alloc_output_context2(&fmt_, nullptr, "mp4", path.c_str());
    if (ret < 0 || !fmt_) { err_ = "alloc_output_ctx " + std::to_string(ret); return false; }
    ret = avio_open(&fmt_->pb, path.c_str(), AVIO_FLAG_WRITE);
    if (ret < 0) { err_ = "avio_open " + path + " " + std::to_string(ret); closeFile(); return false; }
    return true;
}

void Recorder::writePacket(const uint8_t *p, uint32_t len, bool key, uint64_t ts_ms) {
    if (!header_written_ || !fmt_) return;
    AVPacket pkt;
    av_init_packet(&pkt);
    pkt.data = const_cast<uint8_t *>(p);
    pkt.size = (int)len;
    pkt.stream_index = stream_idx_;
    pkt.flags = key ? AV_PKT_FLAG_KEY : 0;
    int64_t rel = (int64_t)(ts_ms - start_ts_);
    if (rel < 0) rel = 0;
    pkt.pts = pkt.dts = rel;
    pkt.duration = 0;
    pkt.pos = -1;
    int ret = av_interleaved_write_frame(fmt_, &pkt);
    if (ret < 0)
        fprintf(stderr, "[rec] write_frame %d\n", ret);
}

void Recorder::closeFile() {
    if (!fmt_) return;
    if (fmt_->pb) avio_closep(&fmt_->pb);
    avformat_free_context(fmt_);
    fmt_ = nullptr;
    header_written_ = false;
}
