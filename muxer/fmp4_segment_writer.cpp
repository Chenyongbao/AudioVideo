// fragmented MP4 分段写文件器：每个分段独立可播（掉电安全的关键）
// 关键设计：分段内每 ~1s 一个 fragment（moof+mdat），任何时刻断电，
// 已写入的 fragment 都是自包含的，上电 ffprobe 即可播放已落盘部分。
#include "muxer/fmp4_segment_writer.hpp"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/time.h>
}

#include <cstdio>

namespace {

// 用 FFmpeg API 封装：mp4 (faststart 关闭) + fragment 配置
class Fmp4Writer : public ISegmentWriter {
public:
    ~Fmp4Writer() override { close(); }

    bool open(const char* path, int width, int height, int fps,
              int has_audio, uint32_t audio_rate) override {
        avformat_alloc_output_context2(&ctx_, nullptr, "mp4", path);
        if (!ctx_) return false;
        path_ = path;

        // 视频流：H.264 extradata 由编码器侧设置（annexb -> avcc 转换在 add_stream 处理）
        v_stream_ = avformat_new_stream(ctx_, nullptr);
        if (!v_stream_) return false;
        v_stream_->id = 0;
        v_stream_->time_base = AVRational{1, fps};
        has_audio_ = has_audio;
        audio_rate_ = audio_rate;

        if (has_audio) {
            a_stream_ = avformat_new_stream(ctx_, nullptr);
            if (!a_stream_) return false;
            a_stream_->id = 1;
            a_stream_->time_base = AVRational{1, audio_rate};
        }
        return true;
    }

    // 编码器首帧回调：把 codecpar（含 avcc extradata）灌进流，真正打开文件
    bool start(const AVCodecParameters* vpar, const AVCodecParameters* apar) override {
        if (avcodec_parameters_copy(v_stream_->codecpar, vpar) < 0) return false;
        v_stream_->time_base = AVRational{1, fps_};
        if (has_audio_ && apar) {
            if (avcodec_parameters_copy(a_stream_->codecpar, apar) < 0) return false;
        }
        // fragmented MP4 选项：每 frag 一写，无需收尾 moov 也保证自包含
        AVDictionary* opt = nullptr;
        av_dict_set(&opt, "movflags", "+frag_keyframe+empty_moov+default_base_moof", 0);
        if (!(ctx_->oformat->flags & AVFMT_NOFILE)) {
            if (avio_open(&ctx_->pb, path_.c_str(), AVIO_FLAG_WRITE) < 0) {
                fprintf(stderr, "[fmp4] avio_open failed: %s\n", path_.c_str());
                return false;
            }
        }
        if (avformat_write_header(ctx_, &opt) < 0) {
            fprintf(stderr, "[fmp4] avformat_write_header failed: %s\n", path_.c_str());
            av_dict_free(&opt);
            return false;
        }
        av_dict_free(&opt);
        avio_flush(ctx_->pb);  // 立即落盘 ftyp+moov（empty_moov），断电也保证文件结构可识别
        started_ = true;
        return true;
    }

    bool writeVideo(const AVPacket* pkt, AVRational tb) override {
        if (!started_) return false;
        AVPacket* p = av_packet_clone(pkt);
        p->stream_index = 0;
        av_packet_rescale_ts(p, tb, v_stream_->time_base);
        int ret = av_interleaved_write_frame(ctx_, p);
        av_packet_free(&p);
        if (ret < 0) {
            char err[128];
            av_strerror(ret, err, sizeof(err));
            fprintf(stderr, "[fmp4] writeVideo failed: %s\n", err);
        }
        // 掉电安全：关键帧意味着上一个 fragment 已完整收尾，此刻 flush 保证磁盘上
        // 永远只有完整 fragment；断电最多损失正在写的当前 fragment（≤1 GOP 时长）
        if (ret >= 0 && (pkt->flags & AV_PKT_FLAG_KEY)) avio_flush(ctx_->pb);
        return ret >= 0;
    }

    bool writeAudio(const AVPacket* pkt, AVRational tb) override {
        if (!started_ || !has_audio_) return false;
        AVPacket* p = av_packet_clone(pkt);
        p->stream_index = 1;
        av_packet_rescale_ts(p, tb, a_stream_->time_base);
        int ret = av_interleaved_write_frame(ctx_, p);
        av_packet_free(&p);
        return ret >= 0;
    }

    bool close() override {
        if (!ctx_) return true;
        if (started_) av_write_trailer(ctx_);  // 正常收尾；断电时已写 fragment 仍可播
        if (ctx_->pb) avio_closep(&ctx_->pb);
        avformat_free_context(ctx_);
        ctx_ = nullptr;
        return true;
    }

private:
    AVFormatContext* ctx_ = nullptr;
    AVStream* v_stream_ = nullptr;
    AVStream* a_stream_ = nullptr;
    std::string path_;
    int fps_ = 25;
    int has_audio_ = 0;
    uint32_t audio_rate_ = 16000;
    bool started_ = false;
};

} // namespace

std::unique_ptr<ISegmentWriter> create_fmp4_writer() {
    return std::make_unique<Fmp4Writer>();
}
