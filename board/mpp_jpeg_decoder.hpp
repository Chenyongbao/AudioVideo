// ============================================================================
// MPP VDEC 硬解:MJPEG → NV12(task API 版)
// 实测依据(SDK mpi_dec_test.c:790):MJPEG 走非 simple 路径 = task API
//   (enqueue INPUT / poll / dequeue OUTPUT),且必须首包前预挂帧 buffer
//   (hor*ver*4,MJPEG 输出可能 422 要 2x w*h)——即 mpi_dec_test -w -h 成功的组合。
// put/get 简单接口对 JPEG 在本板实测 BUFFER_FULL,故此模块走 task API。
// ============================================================================
#pragma once
#include <cstdint>
#include <functional>
#include <string>

extern "C" {
#include "rk_mpi.h"
}

struct DecFrame {
    uint8_t *data;        // NV12 数据首地址(含 stride 布局)
    int fd = -1;          // DMA fd(零拷贝直通用;软件路径为 -1)
    int width, height;
    int hor_stride, ver_stride;
    int buf_size;
    int is422 = 0;        // 输出为 NV16(4:2:2,摄像头源即 422 时硬解如实输出)
};

class MppJpegDecoder {
public:
    ~MppJpegDecoder();
    // 必须在首帧 decode() 前调用:预建 DRM 帧组(hor*ver*4 × 4)
    void setExpectSize(int w, int h);
    // 解码一帧 JPEG;on_frame 同步回调,返回 false 可中止
    bool decode(const uint8_t *jpeg, uint32_t len,
                const std::function<bool(const DecFrame &)> &on_frame);
    const std::string &lastError() const { return err_; }

private:
    bool ensureInit();
    MppCtx ctx_ = nullptr;
    MppApi *mpi_ = nullptr;
    MppBufferGroup frm_grp_ = nullptr;   // 预挂 DRM 帧组
    MppBufferGroup pkt_grp_ = nullptr;   // packet 缓冲组
    int w_ = 0, h_ = 0;
    int width_ = 0, height_ = 0, hor_stride_ = 0, ver_stride_ = 0, buf_size_ = 0;
    bool info_ready_ = false;            // INFO_CHANGE 已应答
    std::string err_;
};
