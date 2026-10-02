#pragma once
// AAC 编码封装：PCM s16le -> AAC。板端移植时替换为 G.711A（无需编码，直接打包）。
#include "platform/common.hpp"
#include "encoder/h264_encoder.hpp"  // 复用 libavcodec include
#include <memory>

class AACEncoder {
public:
    ~AACEncoder();
    bool open(uint32_t sample_rate, int channels, int bitrate_bps = 64000);
    // 输入 PCM s16le interleaved，输出编码帧（out.pts_ms 沿用输入）
    bool encode(const AudioFrame& in, VideoFrame& out);  // 复用 VideoFrame 作编码帧容器
    void close();
    const AVCodecParameters* codecParams() const { return &par_; }

private:
    AVCodecContext* ctx_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVCodecParameters par_{};
    int64_t next_pts_ = 0;  // 按采样数计的 pts（音频主时钟的样本计）
    uint32_t rate_ = 16000;
    int channels_ = 1;
};
