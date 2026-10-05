// ============================================================================
// FFmpeg 软解 MJPEG → NV12(板端,VDEC 硬解攻关期间的替代,接口对齐 DecFrame)
// 板端已有 libavcodec.so.58(buildroot 产出),直接链接。
// 方案 2 决策:先用软解打通 RGA/VENC/全链路,VDEC 硬解(task API)后置攻关。
// ============================================================================
#pragma once
#include <cstdint>
#include <functional>
#include <string>

struct AVCodecContext;
struct SwsContext;
struct AVFrame;

// 与 mpp_jpeg_decoder.hpp 的 DecFrame 布局一致,方便上层换回硬解
struct DecFrame {
    uint8_t *data;
    int width, height;
    int hor_stride, ver_stride;
    int buf_size;
};

class SoftJpegDecoder {
public:
    ~SoftJpegDecoder();
    // 解码一帧 JPEG;内部持有输出缓冲,on_frame 返回前数据有效
    bool decode(const uint8_t *jpeg, uint32_t len,
                const std::function<bool(const DecFrame &)> &on_frame);
    const std::string &lastError() const { return err_; }

private:
    AVCodecContext *ctx_ = nullptr;
    const AVFrame *out_ = nullptr;   // sws 输出帧(复用)
    AVFrame *frm_ = nullptr;
    SwsContext *sws_ = nullptr;
    uint8_t *nv12_ = nullptr;        // 连续 NV12 输出缓冲
    int cap_ = 0;                    // nv12_ 容量
    int width_ = 0, height_ = 0;
    std::string err_;
};
