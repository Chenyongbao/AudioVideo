// aac_test：绕过 ALSA，直接合成 PCM 验证 AACEncoder + 双流 fMP4 封装
#include "encoder/aac_encoder.hpp"
#include "muxer/fmp4_segment_writer.hpp"
#include "middleware/integrity.hpp"
#include <cmath>
#include <vector>
#include <cstdio>

extern "C" {
#include <libavcodec/avcodec.h>
}

int main() {
    const uint32_t RATE = 16000;
    const int SECONDS = 3;
    AACEncoder aac;
    if (!aac.open(RATE, 1)) { fprintf(stderr, "aac open fail\n"); return 1; }

    // 视频假参数：跳过真采集，仅注册视频流参数（复用真实 avcc 生成逻辑太重，手造最小参数）
    AVCodecParameters vpar{};
    vpar.codec_type = AVMEDIA_TYPE_VIDEO;
    vpar.codec_id = AV_CODEC_ID_H264;
    vpar.width = 640; vpar.height = 480;
    // avcc extradata 最小结构（实际值由 demo 路径验证，此处仅测封装器双流写盘）
    vpar.extradata_size = 7;
    vpar.extradata = (uint8_t*)av_mallocz(7);
    const uint8_t avcc[7] = {0x01, 0x64, 0x00, 0x1f, 0xff, 0xe1, 0x00};
    memcpy(vpar.extradata, avcc, 7);

    ISegmentWriter* w = create_fmp4_writer().release();
    if (!w->open("/tmp/aac_test/seg.mp4", 640, 480, 30, 1, RATE)) { fprintf(stderr, "open fail\n"); return 1; }
    if (!w->start(&vpar, aac.codecParams())) { fprintf(stderr, "start fail\n"); return 1; }

    // 每帧 20ms PCM 正弦（320 样本），写 3 秒
    int64_t sample_cnt = 0;
    for (int f = 0; f < RATE / 50 * SECONDS; ++f) {
        AudioFrame af;
        af.sample_rate = RATE;
        af.samples = 320;
        af.pts_ms = f * 20;
        af.data.resize(320 * sizeof(int16_t));
        int16_t* p = reinterpret_cast<int16_t*>(af.data.data());
        for (int i = 0; i < 320; ++i)
            p[i] = (int16_t)(8000.0 * sin(2 * M_PI * 440.0 * (sample_cnt + i) / RATE));
        sample_cnt += 320;

        VideoFrame enc;
        if (!aac.encode(af, enc)) continue;
        AVPacket pkt;
        av_init_packet(&pkt);
        pkt.data = enc.data.data();
        pkt.size = (int)enc.data.size();
        pkt.pts = pkt.dts = sample_cnt - 320;
        AVRational tb{1, (int)RATE};
        if (!w->writeAudio(&pkt, tb)) fprintf(stderr, "writeAudio fail @frame %d\n", f);
    }
    w->close();
    append_manifest("/tmp/aac_test/manifest.sha256", "/tmp/aac_test/seg.mp4");
    fprintf(stderr, "done, samples=%lld\n", (long long)sample_cnt);
    return 0;
}
