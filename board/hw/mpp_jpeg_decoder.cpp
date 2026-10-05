// ============================================================================
// MPP VDEC 硬解实现:MJPEG → NV12(task API 版,见 mpp_jpeg_decoder.hpp)
// 时序照抄 SDK mpi_dec_test.c 成功路径(-w -h 即 task API):
//   1. mpp_create → control(SET_PARSER_SPLIT_MODE=1) → mpp_init(MJPEG)
//   2. 预挂 DRM 帧组(hor*ver*4 × 4,MJPEG 422 输出要 2x w*h)
//   3. 每帧:packet(组内 buffer) → poll INPUT → dequeue INPUT task
//      → 挂 packet/frame → enqueue INPUT → poll OUTPUT → dequeue OUTPUT
//      → INFO_CHANGE 应答 READY / 数据帧回调
// ============================================================================
#include "mpp_jpeg_decoder.hpp"
#include <cstring>
#include <cstdio>

MppJpegDecoder::~MppJpegDecoder() {
    if (ctx_) {
        mpi_->reset(ctx_);
        mpp_destroy(ctx_);
    }
    if (frm_grp_) mpp_buffer_group_put(frm_grp_);
    if (pkt_grp_) mpp_buffer_group_put(pkt_grp_);
}

bool MppJpegDecoder::ensureInit() {
    if (ctx_) return true;
    MPP_RET ret = mpp_create(&ctx_, &mpi_);
    if (ret != MPP_OK) { err_ = "mpp_create"; return false; }

    // split 模式必须 init 前设置(SDK 注释要求)
    RK_U32 need_split = 1;
    mpi_->control(ctx_, MPP_DEC_SET_PARSER_SPLIT_MODE, &need_split);

    ret = mpp_init(ctx_, MPP_CTX_DEC, MPP_VIDEO_CodingMJPEG);
    if (ret != MPP_OK) { err_ = "mpp_init MJPEG"; mpp_destroy(ctx_); ctx_ = nullptr; return false; }
    return true;
}

void MppJpegDecoder::setExpectSize(int w, int h) {
    if (frm_grp_) return;
    if (!ensureInit()) return;
    w_ = w; h_ = h;

    int stride = (w + 15) & ~15;
    int vstride = (h + 15) & ~15;
    int frame_size = stride * vstride * 4;   // MJPEG 420/422 兼容(SDK 同款 2x)
    if (mpp_buffer_group_get_internal(&frm_grp_, MPP_BUFFER_TYPE_DRM) == MPP_OK) {
        for (int i = 0; i < 4; i++) {
            MppBuffer b = nullptr;
            if (mpp_buffer_get(frm_grp_, &b, frame_size) == MPP_OK)
                mpp_buffer_put(b);           // 归还空闲池,解码器按需取
        }
        mpi_->control(ctx_, MPP_DEC_SET_EXT_BUF_GROUP, frm_grp_);
        fprintf(stderr, "[vdec] pre-group %dx%d size %d x4 (drm)\n", w, h, frame_size);
    } else {
        err_ = "pre-group failed";
    }
}

// JPEG SOF 头解析:按色度垂直采样因子判定 4:2:2 / 4:2:0
// (task 模式下 mpp_frame_get_fmt 读回的是我们自己挂的 fmt,循环论证不可用)
static bool jpegIs422(const uint8_t *p, uint32_t len) {
    if (len < 4 || p[0] != 0xFF || p[1] != 0xD8) return false;
    uint32_t i = 2;                        // 跳过 SOI
    while (i + 12 < len) {
        if (p[i] != 0xFF) { i++; continue; }
        uint8_t m = p[i + 1];
        if (m == 0xFF) { i++; continue; }  // 填充字节
        if (m == 0xC0 || m == 0xC1 || m == 0xC2) {          // SOF0/1/2
            // 布局: len(2) precision(1) h(2) w(2) ncomp(1) [id(1) sampling(1) quant(1)]*
            uint8_t v = p[i + 11] & 0x0F;   // 首分量(Y)垂直采样
            return v == 1;                  // 4:2:2 → VDEC 输出 NV16
        }
        if (m == 0xD8 || m == 0xD9 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) { i += 2; continue; }
        uint16_t seglen = ((uint16_t)p[i + 2] << 8) | p[i + 3];
        if (seglen < 2) return false;
        i += 2 + seglen;
    }
    return false;
}

bool MppJpegDecoder::decode(const uint8_t *jpeg, uint32_t len,
                            const std::function<bool(const DecFrame &)> &on_frame) {
    if (!ensureInit()) return false;

    // --- packet:组内 buffer 挂载(SDK task 路径同款) ---
    if (!pkt_grp_ && mpp_buffer_group_get_internal(&pkt_grp_, MPP_BUFFER_TYPE_DRM) != MPP_OK) {
        err_ = "pkt group"; return false;
    }
    MppBuffer pkt_buf = nullptr;
    if (mpp_buffer_get(pkt_grp_, &pkt_buf, len + 16) != MPP_OK) { err_ = "pkt buf get"; return false; }
    memcpy(mpp_buffer_get_ptr(pkt_buf), jpeg, len);

    MppPacket pkt = nullptr;
    mpp_packet_init_with_buffer(&pkt, pkt_buf);
    mpp_packet_set_pos(pkt, mpp_buffer_get_ptr(pkt_buf));
    mpp_packet_set_length(pkt, len);

    // --- 输出帧对象(task 模式要求 OUTPUT frame 自带 buffer,SDK: mpp_frame_set_buffer) ---
    // 首帧也必须挂(INFO_CHANGE 尚未到达时用预期尺寸算;SDK -w -h 路径同款)
    MppFrame frame = nullptr;
    mpp_frame_init(&frame);
    MppBuffer out_buf = nullptr;
    {
        int bw = info_ready_ ? width_ : w_;
        int bh = info_ready_ ? height_ : h_;
        int bs = info_ready_ ? buf_size_ : (int)(((bw + 15) & ~15) * ((bh + 15) & ~15) * 2);  // MJPEG 422 需求量
        if (bw > 0 && frm_grp_) {
            mpp_frame_set_width(frame, bw);
            mpp_frame_set_height(frame, bh);
            mpp_frame_set_hor_stride(frame, (bw + 15) & ~15);
            mpp_frame_set_ver_stride(frame, (bh + 15) & ~15);
            mpp_frame_set_fmt(frame, MPP_FMT_YUV420SP);
            if (mpp_buffer_get(frm_grp_, &out_buf, bs) == MPP_OK)
                mpp_frame_set_buffer(frame, out_buf);
        }
    }

    // --- task API 三段式 ---
    bool ok = false;
    MPP_RET ret = mpi_->poll(ctx_, MPP_PORT_INPUT, MPP_POLL_BLOCK);
    if (ret != MPP_OK) { err_ = "poll in"; goto OUT; }
    {
        MppTask task = nullptr;
        ret = mpi_->dequeue(ctx_, MPP_PORT_INPUT, &task);
        if (ret != MPP_OK || !task) { err_ = "dequeue in"; goto OUT; }

        mpp_task_meta_set_packet(task, KEY_INPUT_PACKET, pkt);
        mpp_task_meta_set_frame(task, KEY_OUTPUT_FRAME, frame);

        ret = mpi_->enqueue(ctx_, MPP_PORT_INPUT, task);
        if (ret != MPP_OK) { err_ = "enqueue in"; goto OUT; }

        ret = mpi_->poll(ctx_, MPP_PORT_OUTPUT, (MppPollType)100);  // 100ms 超时(枚举 1~8000 为 ms)
        if (ret != MPP_OK) { err_ = "poll out"; goto OUT; }

        MppTask out_task = nullptr;
        ret = mpi_->dequeue(ctx_, MPP_PORT_OUTPUT, &out_task);
        if (ret != MPP_OK || !out_task) { err_ = "dequeue out"; goto OUT; }

        MppFrame out_frame = nullptr;
        mpp_task_meta_get_frame(out_task, KEY_OUTPUT_FRAME, &out_frame);
        mpi_->enqueue(ctx_, MPP_PORT_OUTPUT, out_task);   // 归还输出 task

        if (out_frame) {
            if (mpp_frame_get_info_change(out_frame)) {
                width_      = mpp_frame_get_width(out_frame);
                height_     = mpp_frame_get_height(out_frame);
                hor_stride_ = mpp_frame_get_hor_stride(out_frame);
                ver_stride_ = mpp_frame_get_ver_stride(out_frame);
                buf_size_   = mpp_frame_get_buf_size(out_frame);
                fprintf(stderr, "[vdec] info_change %dx%d stride %dx%d size %d\n",
                        width_, height_, hor_stride_, ver_stride_, buf_size_);
                mpi_->control(ctx_, MPP_DEC_SET_INFO_CHANGE_READY, nullptr);
                info_ready_ = true;
            } else if (!mpp_frame_get_eos(out_frame)) {
                RK_U32 err = mpp_frame_get_errinfo(out_frame) | mpp_frame_get_discard(out_frame);
                MppBuffer mbuf = mpp_frame_get_buffer(out_frame);
                fprintf(stderr, "[vdec] out frame: err=%u buf=%p\n", err, (void *)mbuf);
                // task 路径实测无 INFO_CHANGE,数据帧直接出;stride/格式取实际帧参数
                // 关键:摄像头 JPEG 源是 4:2:2(yuvj422p),VDEC 如实输出 NV16,
                //       按 NV12 解读会只剩上半幅色度(实测色度 bug 根因)
                //       task 模式帧 fmt 是自己挂的,不可读回判断 → 用 JPEG SOF 解析
                bool is422 = jpegIs422(jpeg, len);
                if (!err && mbuf) {
                    DecFrame df;
                    df.data       = (uint8_t *)mpp_buffer_get_ptr(mbuf);
                    df.fd         = mpp_buffer_get_fd(mbuf);
                    df.width      = mpp_frame_get_width(out_frame);
                    df.height     = mpp_frame_get_height(out_frame);
                    df.hor_stride = mpp_frame_get_hor_stride(out_frame);
                    df.ver_stride = mpp_frame_get_ver_stride(out_frame);
                    df.is422      = is422 ? 1 : 0;
                    df.buf_size   = (int)((size_t)df.hor_stride * df.ver_stride * (is422 ? 2 : 1.5));
                    ok = on_frame(df);
                } else if (err) {
                    err_ = "frame err " + std::to_string(err);
                }
            }
            mpp_frame_deinit(&out_frame);
        }
    }
OUT:
    mpp_frame_deinit(&frame);   // frame_deinit 幂等,且与 out_frame 共享引用时安全
    mpp_packet_deinit(&pkt);
    mpp_buffer_put(pkt_buf);
    return ok;
}
