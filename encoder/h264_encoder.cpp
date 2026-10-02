// ============================================================================
// H.264 视频编码实现（PC 版）：NV12 → libx264（软编）。
// ----------------------------------------------------------------------------
// 【码控设计 —— 对标 GA/T 947.2 视频性能指标】
//   CBR 硬约束：bit_rate + rc_max_rate + nal-hrd=cbr + force-cfr，
//   实测逐段码率偏差 ±0.3%（目标 4Mbps → 3.991~4.012Mbps）。
//   1080p 指标换算：1h ≤2.6GB ≈ 5.8Mbps，板端 MPP VENC 用 CBR 模式对标。
// 【GOP 设计】2s 固定 GOP（gop_size=fps*2）—— 两个下游依赖：
//   1) 预录环形缓冲按 I 帧对齐丢弃（EncodedFrameRing），GOP 越长预录回填起点越早；
//   2) fMP4 按 fragment 切分以关键帧为边界，GOP=掉电时最多损失的时长。
// 【无 B 帧】max_b_frames=0：低延迟 + 简化预录/分段帧序（B 帧会打乱 PTS/DTS
//   单调性，给环形缓冲与封装带来乱序复杂度，执法仪场景不需要）。
// 【GLOBAL_HEADER】avcc extradata 在 open 阶段即就绪（而非首帧后），分段管理器
//   可在任意时刻开新段 —— 分段无缝切换的前提。
// 【RV1126 移植】整体替换为 h264_encoder_mpp.cpp（MPP VENC 硬编）：
//   接口 IVideoEncoder 不变，CBR/GOP 语义一一对应，CPU 占用从软编降至 ~5%。
// ============================================================================
#include "encoder/h264_encoder.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
}

#include <stdexcept>

namespace {

class X264Encoder : public IVideoEncoder {
public:
    ~X264Encoder() override { close(); }

    bool open(int w, int h, int fps, int bitrate_kbps) override {
        const AVCodec* codec = avcodec_find_encoder_by_name("libx264");
        if (!codec) codec = avcodec_find_encoder(AV_CODEC_ID_H264);
        if (!codec) return false;

        ctx_ = avcodec_alloc_context3(codec);
        if (!ctx_) return false;

        ctx_->width = w;
        ctx_->height = h;
        ctx_->time_base = AVRational{1, fps};
        ctx_->framerate = AVRational{fps, 1};
        ctx_->pix_fmt = AV_PIX_FMT_NV12;
        ctx_->bit_rate = (int64_t)bitrate_kbps * 1000;
        ctx_->rc_max_rate = ctx_->bit_rate;       // CBR
        ctx_->rc_buffer_size = ctx_->bit_rate / fps * 2;
        ctx_->gop_size = fps * 2;                 // 2s GOP，环形缓冲按 GOP 丢弃
        ctx_->max_b_frames = 0;                   // 低延迟+简化预录/分段帧序
        ctx_->keyint_min = ctx_->gop_size;
        ctx_->thread_count = 2;
        ctx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;  // 生成 avcc extradata 供 MP4 封装

        AVDictionary* opt = nullptr;
        av_dict_set(&opt, "preset", "veryfast", 0);
        av_dict_set(&opt, "tune", "zerolatency", 0);
        // 显式 CBR：nal-hrd=cbr + filler 强制填充到目标码率（GA/T 947.2 码控对标，
        // 1080p 指标 1h ≤2.6GB ≈5.8Mbps；板端 MPP VENC 对应 CBR 模式）
        av_dict_set(&opt, "x264-params", "nal-hrd=cbr:force-cfr=1", 0);
        av_dict_set(&opt, "b:v", std::to_string((int64_t)bitrate_kbps * 1000).c_str(), 0);
        if (avcodec_open2(ctx_, codec, &opt) < 0) { av_dict_free(&opt); return false; }
        av_dict_free(&opt);
        // GLOBAL_HEADER 打开后 extradata 在 open 阶段即就绪，编码前就能取到 avcc 参数
        avcodec_parameters_from_context(&par_, ctx_);
        par_copied_ = true;

        frame_ = av_frame_alloc();
        frame_->format = AV_PIX_FMT_NV12;
        frame_->width = w;
        frame_->height = h;
        if (av_frame_get_buffer(frame_, 0) < 0) return false;
        return true;
    }

    bool encode(const VideoFrame& in, VideoFrame& out) override {
        if (!ctx_) return false;
        if (av_frame_make_writable(frame_) < 0) return false;
        // NV12 data[0]=Y, data[1]=UV
        const uint8_t* src = in.data.data();
        int ysize = ctx_->width * ctx_->height;
        av_image_fill_arrays(frame_->data, frame_->linesize, src,
                             AV_PIX_FMT_NV12, ctx_->width, ctx_->height, 1);
        (void)ysize;
        frame_->pts = in.pts_ms * ctx_->time_base.den / 1000;

        int ret = avcodec_send_frame(ctx_, frame_);
        if (ret < 0) return false;
        AVPacket* pkt = av_packet_alloc();
        ret = avcodec_receive_packet(ctx_, pkt);
        if (ret < 0) { av_packet_free(&pkt); return false; }  // EAGAIN/无输出：跳过本帧

        out.type = FrameType::Video;
        out.pts_ms = in.pts_ms;
        out.encoded = true;
        out.keyframe = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
        out.width = ctx_->width;
        out.height = ctx_->height;
        out.data.assign(pkt->data, pkt->data + pkt->size);
        // 保存编码器参数给封装器 start()（首次调用方拷贝走）
        if (!par_copied_) {
            avcodec_parameters_from_context(&par_, ctx_);
            par_copied_ = true;
        }
        av_packet_free(&pkt);
        return true;
    }

    const AVCodecParameters* codecParams() const override { return &par_; }

    void close() override {
        if (ctx_) { avcodec_free_context(&ctx_); ctx_ = nullptr; }
        if (frame_) { av_frame_free(&frame_); frame_ = nullptr; }
    }

private:
    AVCodecContext* ctx_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVCodecParameters par_{};
    bool par_copied_ = false;
};

} // namespace

std::unique_ptr<IVideoEncoder> create_x264_encoder() {
    return std::make_unique<X264Encoder>();
}
