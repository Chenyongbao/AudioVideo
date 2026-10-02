// sync_test：音视频同步验证（WSL 无声卡，用合成事件替代拍手）
// 方法：视频在第 150 帧（5s@30fps）插入全白帧（可视事件），
//       音频在 5.0s 处插入 440Hz beep（可听事件），两个事件逻辑上同刻发生。
//       经 X264Encoder + AACEncoder + fMP4 封装后，用 ffmpeg 回测：
//       失步 = beep 实测时刻 - 白帧实测时刻。目标 <100ms。
#include "encoder/h264_encoder.hpp"
#include "encoder/aac_encoder.hpp"
#include "muxer/fmp4_segment_writer.hpp"
#include <cmath>
#include <cstdio>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
}

int main() {
    const int W = 640, H = 480, FPS = 30;
    const uint32_t RATE = 16000;
    const int SECONDS = 8;
    const int FLASH_FRAME = 150;          // 5.0s @30fps：白帧事件
    const int64_t BEEP_START_MS = 5000;   // 5.0s：beep 事件（持续 500ms）

    auto enc = create_x264_encoder();
    if (!enc->open(W, H, FPS, 4000)) { fprintf(stderr, "enc open fail\n"); return 1; }
    AACEncoder aac;
    if (!aac.open(RATE, 1)) { fprintf(stderr, "aac open fail\n"); return 1; }

    auto writer = create_fmp4_writer();
    if (!writer->open("/tmp/sync_test/seg.mp4", W, H, FPS, 1, RATE)) return 1;
    writer->start(enc->codecParams(), aac.codecParams());

    // --- 视频：灰底，第 FLASH_FRAME 帧全白（多写 2 帧白便于 ffmpeg 检测）---
    std::vector<uint8_t> nv12((size_t)W * H * 3 / 2);
    for (int f = 0; f < SECONDS * FPS; ++f) {
        bool flash = (f == FLASH_FRAME || f == FLASH_FRAME + 1 || f == FLASH_FRAME + 2);
        memset(nv12.data(), flash ? 235 : 100, (size_t)W * H);          // Y
        memset(nv12.data() + (size_t)W * H, flash ? 128 : 100, (size_t)W * H / 2); // UV
        VideoFrame raw;
        raw.pts_ms = (int64_t)f * 1000 / FPS;
        raw.width = W; raw.height = H; raw.data = nv12;
        VideoFrame encf;
        if (!enc->encode(raw, encf)) continue;
        AVPacket pkt;
        av_init_packet(&pkt);
        pkt.data = encf.data.data();
        pkt.size = (int)encf.data.size();
        pkt.pts = pkt.dts = f;
        if (encf.keyframe) pkt.flags |= AV_PKT_FLAG_KEY;
        AVRational tb{1, FPS};
        writer->writeVideo(&pkt, tb);
    }

    // --- 音频：静音，[5.0s, 5.5s) 为 440Hz beep ---
    int64_t sample_cnt = 0;
    for (int f = 0; f < RATE / 50 * SECONDS; ++f) {  // 20ms 包
        AudioFrame af;
        af.sample_rate = RATE;
        af.samples = 320;
        af.pts_ms = f * 20;
        af.data.resize(320 * sizeof(int16_t));
        int16_t* p = reinterpret_cast<int16_t*>(af.data.data());
        int64_t base = sample_cnt;
        for (int i = 0; i < 320; ++i) {
            int64_t t_us = (base + i) * 1000000LL / RATE;
            bool beep = t_us >= BEEP_START_MS * 1000 && t_us < (BEEP_START_MS + 500) * 1000;
            p[i] = beep ? (int16_t)(9000.0 * sin(2 * M_PI * 440.0 * (base + i) / RATE)) : 0;
        }
        sample_cnt += 320;
        VideoFrame encf;
        if (!aac.encode(af, encf)) continue;
        AVPacket pkt;
        av_init_packet(&pkt);
        pkt.data = encf.data.data();
        pkt.size = (int)encf.data.size();
        pkt.pts = pkt.dts = base;
        AVRational tb{1, (int)RATE};
        writer->writeAudio(&pkt, tb);
    }
    writer->close();
    fprintf(stderr, "done: 逻辑事件同刻 %lldms（白帧@frame%d, beep@%lldms）\n",
            (long long)BEEP_START_MS, FLASH_FRAME, (long long)BEEP_START_MS);
    return 0;
}
