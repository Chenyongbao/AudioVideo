// ============================================================================
// MPP VENC 实现:NV12 → H.264 CBR(板端,见 mpp_h264_encoder.hpp)
// 参考 SDK mpi_enc_test.c 的标准配置流程:
//   prep(尺寸/stride/format) + rc(CBR/bps/fps/gop) + h264(profile/level)
//   → MPP_ENC_SET_CFG → MPP_ENC_GET_HDR_SYNC 取 SPS/PPS
//   → encode_put_frame / encode_get_packet 循环
// ============================================================================
#include "mpp_h264_encoder.hpp"
#include "rga.h"      // RK_FORMAT_YCbCr_420_SP
#include <cstring>
#include <cstdio>

MppH264Encoder::~MppH264Encoder() {
    if (ctx_) {
        mpi_->reset(ctx_);
        mpp_destroy(ctx_);
    }
    if (cfg_) mpp_enc_cfg_deinit(cfg_);
    if (frm_buf_) mpp_buffer_put(frm_buf_);
    if (buf_grp_) mpp_buffer_group_put(buf_grp_);
    if (hdr_) mpp_packet_deinit(&hdr_);
}

bool MppH264Encoder::open(int w, int h, int fps, int bps) {
    w_ = w; h_ = h;
    MPP_RET ret = mpp_create(&ctx_, &mpi_);
    if (ret != MPP_OK) { err_ = "mpp_create"; return false; }

    ret = mpp_init(ctx_, MPP_CTX_ENC, MPP_VIDEO_CodingAVC);
    if (ret != MPP_OK) { err_ = "mpp_init AVC"; return false; }

    mpp_enc_cfg_init(&cfg_);

    // --- prep:输入格式(紧凑 NV12, stride=w) ---
    mpp_enc_cfg_set_s32(cfg_, "prep:width", w);
    mpp_enc_cfg_set_s32(cfg_, "prep:height", h);
    mpp_enc_cfg_set_s32(cfg_, "prep:hor_stride", w);
    mpp_enc_cfg_set_s32(cfg_, "prep:ver_stride", h);
    mpp_enc_cfg_set_s32(cfg_, "prep:format", MPP_FMT_YUV420SP);   // 必须用 MPP 枚举,RGA 的 0xa00 硬件不认(实测)

    // --- rc:CBR 4Mbps 档(参数化) ---
    mpp_enc_cfg_set_s32(cfg_, "rc:mode", MPP_ENC_RC_MODE_CBR);
    mpp_enc_cfg_set_s32(cfg_, "rc:fps_in_flex", 0);
    mpp_enc_cfg_set_s32(cfg_, "rc:fps_in_num", fps);
    mpp_enc_cfg_set_s32(cfg_, "rc:fps_in_denorm", 1);
    mpp_enc_cfg_set_s32(cfg_, "rc:fps_out_flex", 0);
    mpp_enc_cfg_set_s32(cfg_, "rc:fps_out_num", fps);
    mpp_enc_cfg_set_s32(cfg_, "rc:fps_out_denorm", 1);
    mpp_enc_cfg_set_s32(cfg_, "rc:gop", fps);              // GOP=1s:新观看端更快同步,低延迟显示
    mpp_enc_cfg_set_s32(cfg_, "rc:bps_target", bps);
    mpp_enc_cfg_set_s32(cfg_, "rc:bps_max", bps * 17 / 16);
    mpp_enc_cfg_set_s32(cfg_, "rc:bps_min", bps * 15 / 16);
    mpp_enc_cfg_set_s32(cfg_, "rc:qp_init", 26);
    mpp_enc_cfg_set_s32(cfg_, "rc:qp_max", 51);
    mpp_enc_cfg_set_s32(cfg_, "rc:qp_min", 10);
    mpp_enc_cfg_set_s32(cfg_, "rc:qp_max_i", 51);
    mpp_enc_cfg_set_s32(cfg_, "rc:qp_min_i", 10);
    mpp_enc_cfg_set_s32(cfg_, "rc:qp_ip", 2);
    mpp_enc_cfg_set_u32(cfg_, "rc:drop_mode", MPP_ENC_RC_DROP_FRM_DISABLED);

    // --- h264: High profile L4.0 ---
    mpp_enc_cfg_set_s32(cfg_, "codec:type", MPP_VIDEO_CodingAVC);
    mpp_enc_cfg_set_s32(cfg_, "h264:profile", 100);
    mpp_enc_cfg_set_s32(cfg_, "h264:level", 40);
    mpp_enc_cfg_set_s32(cfg_, "h264:cabac_en", 1);
    mpp_enc_cfg_set_s32(cfg_, "h264:cabac_idc", 0);
    mpp_enc_cfg_set_s32(cfg_, "h264:trans8x8", 1);

    ret = mpi_->control(ctx_, MPP_ENC_SET_CFG, cfg_);
    if (ret != MPP_OK) { err_ = "SET_CFG ret=" + std::to_string(ret); return false; }

    // --- 输入帧 buffer 组(先建,hdr/帧 buffer 都从这里取) ---
    if (mpp_buffer_group_get_internal(&buf_grp_, MPP_BUFFER_TYPE_DRM) != MPP_OK) {
        err_ = "buf group"; return false;
    }

    // --- 取 SPS/PPS(SDK 同款:packet 挂 buffer 且长度清零,编码器往里填) ---
    MppBuffer hdr_buf = nullptr;
    if (mpp_buffer_get(buf_grp_, &hdr_buf, 1024) != MPP_OK) { err_ = "hdr buffer"; return false; }
    mpp_packet_init_with_buffer(&hdr_, hdr_buf);
    mpp_packet_set_length(hdr_, 0);
    ret = mpi_->control(ctx_, MPP_ENC_GET_HDR_SYNC, hdr_);
    if (ret != MPP_OK || mpp_packet_get_length(hdr_) == 0) {
        err_ = "GET_HDR_SYNC ret=" + std::to_string(ret);
        return false;
    }

    // --- 输入帧 buffer(复用) ---
    size_t frame_size = (size_t)w * h * 3 / 2;
    if (mpp_buffer_get(buf_grp_, &frm_buf_, frame_size) != MPP_OK) {
        err_ = "frm buffer"; return false;
    }
    printf("[venc] open %dx%d@%d bps=%d hdr=%uB\n", w, h, fps, bps,
           mpp_packet_get_length(hdr_));
    return true;
}

// 公共编码路径:inputBuffer() 里的数据已是最新时调用(encodeDirect 与 encode 共用)
bool MppH264Encoder::encodeDirect(const std::function<bool(const uint8_t *, uint32_t, bool)> &on_packet) {
    // 首帧前发 SPS/PPS
    if (!hdr_sent_) {
        size_t hdr_len = mpp_packet_get_length(hdr_);
        const uint8_t *hdr_ptr = (const uint8_t *)mpp_packet_get_pos(hdr_);
        if (!on_packet(hdr_ptr, (uint32_t)hdr_len, true)) return false;
        hdr_sent_ = true;
    }

    MppFrame frame = nullptr;
    mpp_frame_init(&frame);
    mpp_frame_set_width(frame, w_);
    mpp_frame_set_height(frame, h_);
    mpp_frame_set_hor_stride(frame, w_);
    mpp_frame_set_ver_stride(frame, h_);
    mpp_frame_set_fmt(frame, MPP_FMT_YUV420SP);   // NV12(MPP 自有枚举,非 RGA 的)
    mpp_frame_set_buffer(frame, frm_buf_);
    mpp_frame_set_eos(frame, 0);

    MPP_RET ret = mpi_->encode_put_frame(ctx_, frame);
    mpp_frame_deinit(&frame);
    if (ret != MPP_OK) { err_ = "put_frame ret=" + std::to_string(ret); return false; }

    MppPacket pkt = nullptr;
    ret = mpi_->encode_get_packet(ctx_, &pkt);
    if (ret != MPP_OK || !pkt) { err_ = "get_packet ret=" + std::to_string(ret); return false; }

    bool ok = true;
    if (pkt && !mpp_packet_get_eos(pkt)) {
        uint8_t *ptr = (uint8_t *)mpp_packet_get_pos(pkt);
        uint32_t len = (uint32_t)mpp_packet_get_length(pkt);
        // 此 SDK 版本无 keyframe 查询 API,用帧序号推算(GOP 首帧即关键帧)
        bool key = (frame_cnt_ % gop_) == 0;
        frame_cnt_++;
        ok = on_packet(ptr, len, key);
    }
    mpp_packet_deinit(&pkt);
    return ok;
}

bool MppH264Encoder::encode(const uint8_t *nv12,
                            const std::function<bool(const uint8_t *, uint32_t, bool)> &on_packet) {
    // 拷入复用 buffer(NV12 紧凑布局)后走公共路径(兼容软件输入;零拷贝链路用 encodeDirect)
    memcpy(mpp_buffer_get_ptr(frm_buf_), nv12, (size_t)w_ * h_ * 3 / 2);
    return encodeDirect(on_packet);
}
