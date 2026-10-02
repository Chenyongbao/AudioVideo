#pragma once
// H.264 编码封装：NV12 -> H.264 (libx264)。板端替换为 MPP VENC，接口不变。
#include "platform/common.hpp"
#include <memory>

extern "C" {
#include <libavcodec/avcodec.h>
}

class IVideoEncoder {
public:
    virtual ~IVideoEncoder() = default;
    virtual bool open(int width, int height, int fps, int bitrate_kbps) = 0;
    // 输入 NV12 原始帧，输出编码帧（keyframe 标记由编码器给出）
    virtual bool encode(const VideoFrame& in, VideoFrame& out) = 0;
    virtual void close() = 0;
    // 编码器参数（含 avcc extradata），供封装器 start() 使用
    virtual const AVCodecParameters* codecParams() const = 0;
};

std::unique_ptr<IVideoEncoder> create_x264_encoder();
