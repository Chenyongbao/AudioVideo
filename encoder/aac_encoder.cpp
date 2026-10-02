// ============================================================================
// AAC 音频编码实现（PC 版）：PCM s16le 交织 → AAC-LC（内置软编）。
// ----------------------------------------------------------------------------
// 【采样格式转换】AAC 内置编码器只接受 planar float（FLTP）：
//   输入 s16le 交织 [L0 R0 L1 R1...] → 按通道拆平面并归一化到 [-1,1]。
// 【内部缓冲语义】编码器按 frame_size（AAC 通常 1024 样本）粒度输出，
//   encode() 每次送 20ms 包（320 样本），多数调用无输出（缓冲未满）——
//   返回 false 属正常，调用方跳过即可。
// 【pts 语义】next_pts_ 按送入样本数推进（time_base=1/rate），
//   即"音频样本计数=主时钟"；封装器侧按同一时基重放。
// ============================================================================
#include "encoder/aac_encoder.hpp"

extern "C" {
#include <libavutil/opt.h>
}

AACEncoder::~AACEncoder() { close(); }

bool AACEncoder::open(uint32_t sample_rate, int channels, int bitrate_bps) {
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!codec) return false;
    ctx_ = avcodec_alloc_context3(codec);
    if (!ctx_) return false;
    ctx_->sample_fmt = AV_SAMPLE_FMT_FLTP;   // AAC 内置编码器要求 planar float
    ctx_->sample_rate = sample_rate;
    ctx_->channels = channels;               // FFmpeg 4.x channel API
    ctx_->channel_layout = (channels == 1) ? AV_CH_LAYOUT_MONO : AV_CH_LAYOUT_STEREO;
    ctx_->bit_rate = bitrate_bps;
    ctx_->time_base = AVRational{1, sample_rate};
    ctx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (avcodec_open2(ctx_, codec, nullptr) < 0) { close(); return false; }
    avcodec_parameters_from_context(&par_, ctx_);
    rate_ = sample_rate;
    channels_ = channels;

    frame_ = av_frame_alloc();
    frame_->format = AV_SAMPLE_FMT_FLTP;
    frame_->sample_rate = sample_rate;
    frame_->channels = channels;
    frame_->channel_layout = (channels == 1) ? AV_CH_LAYOUT_MONO : AV_CH_LAYOUT_STEREO;
    frame_->nb_samples = ctx_->frame_size;
    if (av_frame_get_buffer(frame_, 0) < 0) { close(); return false; }
    return true;
}

bool AACEncoder::encode(const AudioFrame& in, VideoFrame& out) {
    if (!ctx_) return false;
    if (av_frame_make_writable(frame_) < 0) return false;
    // s16le interleaved -> fltp planar
    const int16_t* src = reinterpret_cast<const int16_t*>(in.data.data());
    int n = in.samples;
    float* planar[2] = {nullptr, nullptr};
    for (int ch = 0; ch < channels_; ++ch) {
        planar[ch] = reinterpret_cast<float*>(frame_->data[ch]);
        for (int i = 0; i < n; ++i)
            planar[ch][i] = src[i * channels_ + ch] / 32768.0f;
    }
    frame_->nb_samples = n;
    frame_->pts = next_pts_;
    next_pts_ += n;

    if (avcodec_send_frame(ctx_, frame_) < 0) return false;
    AVPacket* pkt = av_packet_alloc();
    int ret = avcodec_receive_packet(ctx_, pkt);
    if (ret < 0) { av_packet_free(&pkt); return false; }

    out.type = FrameType::Audio;
    out.pts_ms = in.pts_ms;
    out.encoded = true;
    out.data.assign(pkt->data, pkt->data + pkt->size);
    av_packet_free(&pkt);
    return true;
}

void AACEncoder::close() {
    if (ctx_) { avcodec_free_context(&ctx_); ctx_ = nullptr; }
    if (frame_) { av_frame_free(&frame_); frame_ = nullptr; }
}
