#pragma once
// ============================================================================
// H.264 视频编码抽象接口 —— 平台层的"编码器"。
// 【设计目的】把"像素怎么变码流"与"码流怎么存"解耦：
//   - PC/WSL 阶段：libx264 软编实现（h264_encoder.cpp）；
//   - RV1126 移植：新增 h264_encoder_mpp.cpp（MPP VENC 硬编，CBR 对标
//     1080p25≈5.8Mbps），本接口与中间件层一行不改。
// 【帧流约定】输入 NV12 原始帧 → 输出 H.264 NALU 编码帧（复用 VideoFrame，
//   encoded=true）。keyframe 标记是两个下游机制的依据：
//   预录缓冲 GOP 对齐回填、fMP4 fragment 切分与掉电 flush 边界。
// ============================================================================
#include "platform/common.hpp"
#include <memory>

extern "C" {
#include <libavcodec/avcodec.h>
}

class IVideoEncoder {
public:
    virtual ~IVideoEncoder() = default;

    // 打开编码器。bitrate_kbps = CBR 目标码率（实际由 nal-hrd=cbr 强制）
    virtual bool open(int width, int height, int fps, int bitrate_kbps) = 0;

    // 编码一帧。返回 false = 本帧无输出（编码器内部缓冲），调用方跳过即可。
    virtual bool encode(const VideoFrame& in, VideoFrame& out) = 0;
    virtual void close() = 0;

    // 编码器参数（含 avcc extradata），供封装器 start() 写 MP4 头。
    // 【时序】GLOBAL_HEADER 模式下 open() 返回后即就绪（不必等首帧），
    //   这是分段管理器"任意时刻开新段"的前提。
    virtual const AVCodecParameters* codecParams() const = 0;
};

// 工厂函数：返回 PC 版 libx264 实现（板端换 create_mpp_encoder，调用处改一行）
std::unique_ptr<IVideoEncoder> create_x264_encoder();
