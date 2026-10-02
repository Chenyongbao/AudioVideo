#pragma once
// ============================================================================
// AAC 音频编码器（PC 版）：PCM s16le 交织 → AAC-LC。
// ----------------------------------------------------------------------------
// 【为何 PC 用 AAC、板端用 G.711A】
//   AAC：压缩率高（64kbps 即清晰），WSL 软编零成本；
//   板端换 G.711A：RKMPI AI 通道硬件直接输出 G.711A 包（64kbps，无需编码），
//   且标准音视频同步以音频为主时钟，样本计数推进 pts —— 本类在板端整体退役，
//   音频帧直接从 AI 通道进封装器。
// 【帧容器复用】编码输出复用 VideoFrame 结构（type=Audio 仅作语义标记），
//   避免为纯 data+pts 载体再定义一个结构；字段解释见各赋值处。
// 【FFmpeg 版本兼容】本机 libavutil 56（FFmpeg 4.x）用旧 channel API
//   （ctx_->channels/channel_layout）；FFmpeg 5+ 需换 ch_layout（板端注意）。
// ============================================================================
#include "platform/common.hpp"
#include "encoder/h264_encoder.hpp"  // 复用 libavcodec include
#include <memory>

class AACEncoder {
public:
    ~AACEncoder();

    // 打开编码器。AAC 内置编码器要求 planar float 输入（AV_SAMPLE_FMT_FLTP），
    // frame_size 由编码器决定（通常 1024 样本），encode() 内部按此组装
    bool open(uint32_t sample_rate, int channels, int bitrate_bps = 64000);

    // 编码一包 PCM。注意：AAC 编码器有内部缓冲（frame_size 粒度），
    // 并非每包输入都有输出 —— 返回 false 多数情况是"缓冲未满"，正常现象。
    bool encode(const AudioFrame& in, VideoFrame& out);
    void close();

    // AAC 参数（含 extradata），供封装器写 MP4 音频轨头；板端 G.711A 同样需要
    const AVCodecParameters* codecParams() const { return &par_; }

private:
    AVCodecContext* ctx_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVCodecParameters par_{};
    int64_t next_pts_ = 0;  // 按采样数计的 pts（音频主时钟的样本计数）
    uint32_t rate_ = 16000;
    int channels_ = 1;
};
