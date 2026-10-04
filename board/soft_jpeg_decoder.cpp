// ============================================================================
// FFmpeg 软解 MJPEG → NV12 实现(板端,VDEC 硬解攻关期间的过渡方案)
// 链路: avcodec 解码(输出 YUVJ420P/YUVJ422P 等) → sws 统一转 NV12 连续缓冲
// ============================================================================
#include "soft_jpeg_decoder.hpp"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}
#include <cstring>

SoftJpegDecoder::~SoftJpegDecoder() {
    if (sws_) sws_freeContext(sws_);
    if (frm_) av_frame_free(&frm_);
    if (ctx_) avcodec_free_context(&ctx_);
    free(nv12_);
}

bool SoftJpegDecoder::decode(const uint8_t *jpeg, uint32_t len,
                             const std::function<bool(const DecFrame &)> &on_frame) {
    if (!ctx_) {
        const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_MJPEG);
        if (!codec) { err_ = "no mjpeg decoder"; return false; }
        ctx_ = avcodec_alloc_context3(codec);
        if (!ctx_) { err_ = "alloc ctx"; return false; }
        if (avcodec_open2(ctx_, codec, nullptr) < 0) { err_ = "open decoder"; return false; }
        frm_ = av_frame_alloc();
    }

    AVPacket *pkt = av_packet_alloc();
    av_new_packet(pkt, len);
    memcpy(pkt->data, jpeg, len);
    int ret = avcodec_send_packet(ctx_, pkt);
    av_packet_free(&pkt);
    if (ret < 0) { err_ = "send_packet " + std::to_string(ret); return false; }

    ret = avcodec_receive_frame(ctx_, frm_);
    if (ret < 0) { err_ = "receive_frame " + std::to_string(ret); return false; }

    // 诊断(前2帧):dump sws 之前的原始解码平面,区分"解码坏"vs"sws 坏"
    {
        static int dbg_n = 0;
        if (dbg_n++ < 2) {
            char path[64];
            snprintf(path, sizeof(path), "/tmp/frm_%d.raw", dbg_n);
            FILE *f = fopen(path, "wb");
            if (f) {
                int h = frm_->height;
                for (int pl = 0; pl < 3; pl++) {
                    if (!frm_->data[pl]) break;
                    for (int r = 0; r < h >> (pl ? 1 : 0); r++)
                        fwrite(frm_->data[pl] + (size_t)r * frm_->linesize[pl], 1,
                               frm_->linesize[pl], f);
                }
                fclose(f);
            }
            fprintf(stderr, "[swsdump] fmt=%d %dx%d linesize %d/%d/%d -> %s\n",
                    frm_->format, frm_->width, frm_->height,
                    frm_->linesize[0], frm_->linesize[1], frm_->linesize[2], path);
        }
    }

    int w = frm_->width, h = frm_->height;
    size_t need = (size_t)w * h * 3 / 2;
    if (need > (size_t)cap_) {
        free(nv12_);
        nv12_ = (uint8_t *)malloc(need);
        cap_ = (int)need;
        width_ = 0;   // 重建 sws
    }
    if (!nv12_) { err_ = "oom"; return false; }

    // 分辨率/格式变化时重建 sws(输出统一 NV12)
    if (!sws_ || width_ != w || height_ != h) {
        if (sws_) sws_freeContext(sws_);
        sws_ = sws_getContext(w, h, (AVPixelFormat)frm_->format,
                              w, h, AV_PIX_FMT_NV12,
                              SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws_) { err_ = "sws_getContext"; return false; }
        width_ = w; height_ = h;
    }

    // 一次调用写入 NV12 双平面:dst[0]=Y,dst[1]=交织 UV(sws 对 NV12 的标准用法)
    uint8_t *dstPlanes[4] = { nv12_, nv12_ + (size_t)w * h, nullptr, nullptr };
    int strides[4] = { w, w, 0, 0 };
    const uint8_t *src[4] = { frm_->data[0], frm_->data[1], frm_->data[2], nullptr };
    int srcStride[4] = { frm_->linesize[0], frm_->linesize[1], frm_->linesize[2], 0 };
    sws_scale(sws_, src, srcStride, 0, h, dstPlanes, strides);

    DecFrame df;
    df.data = nv12_;
    df.width = w; df.height = h;
    df.hor_stride = w; df.ver_stride = h;   // 软解输出紧凑无 padding
    df.buf_size = (int)need;
    return on_frame(df);
}
