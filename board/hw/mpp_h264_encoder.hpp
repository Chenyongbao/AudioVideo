// ============================================================================
// MPP VENC 封装:NV12 → H.264 CBR(板端)
// 参考 SDK external/mpp/test/mpi_enc_test.c:
//   cfg 全套参数(prep/rc/h264) → encode_put_frame → encode_get_packet
// 首次 encode 前 control(MPP_ENC_GET_HDR_SYNC) 取 SPS/PPS(annexb 头)。
// ============================================================================
#pragma once
#include <cstdint>
#include <functional>
#include <string>

extern "C" {
#include "rk_mpi.h"
}

class MppH264Encoder {
public:
    ~MppH264Encoder();
    // w/h: 分辨率; fps: 帧率; bps: 目标码率(CBR)
    bool open(int w, int h, int fps, int bps);
    // 零拷贝模式:返回内部输入 buffer 及其 DMA fd(RGA 按 fd 写入后直接编码)
    MppBuffer inputBuffer() const { return frm_buf_; }
    int inputFd() const { return frm_buf_ ? mpp_buffer_get_fd(frm_buf_) : -1; }
    // 直接编码 inputBuffer()(配合 inputFd 零拷贝链路,不做拷贝)
    bool encodeDirect(const std::function<bool(const uint8_t *, uint32_t, bool keyframe)> &on_packet);
    // 编码一帧 NV12(紧凑布局,stride==w);on_packet 同步回调,返回 false 中止
    // 返回 false 且 lastError 非空 = 编码失败
    bool encode(const uint8_t *nv12,
                const std::function<bool(const uint8_t *, uint32_t, bool keyframe)> &on_packet);
    const std::string &lastError() const { return err_; }
    int width() const { return w_; }
    int height() const { return h_; }

private:
    MppCtx ctx_ = nullptr;
    MppApi *mpi_ = nullptr;
    MppEncCfg cfg_ = nullptr;
    MppBufferGroup buf_grp_ = nullptr;
    MppBuffer frm_buf_ = nullptr;    // 复用的输入帧 buffer
    MppPacket hdr_ = nullptr;        // SPS/PPS,首包前先发
    bool hdr_sent_ = false;
    int w_ = 0, h_ = 0;
    int frame_cnt_ = 0;   // 已编码帧数(GOP 推算关键帧用)
    int gop_ = 50;
    std::string err_;
};
