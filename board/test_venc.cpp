// VENC 单测:NV12 文件 → H.264(板端验证)
// 用法: ./test_venc <in.yuv> <w> <h> <out.h264> [帧数]
#include "mpp_h264_encoder.hpp"
#include <cstdio>
#include <vector>

int main(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "usage: %s <in.yuv> <w> <h> <out.h264> [n]\n", argv[0]); return 1; }
    int w = atoi(argv[2]), h = atoi(argv[3]);
    int n = argc > 5 ? atoi(argv[5]) : 10;
    size_t fsz = (size_t)w * h * 3 / 2;

    FILE *fi = fopen(argv[1], "rb");
    if (!fi) { perror("in"); return 1; }
    std::vector<uint8_t> nv12(fsz);
    if (fread(nv12.data(), 1, fsz, fi) != fsz) { fprintf(stderr, "yuv too small\n"); return 1; }
    fclose(fi);

    FILE *fo = fopen(argv[4], "wb");
    MppH264Encoder enc;
    if (!enc.open(w, h, 25, 4000000)) {
        fprintf(stderr, "open failed: %s\n", enc.lastError().c_str());
        return 1;
    }
    uint64_t total = 0; int frames = 0;
    for (int i = 0; i < n; i++) {
        bool ok = enc.encode(nv12.data(), [&](const uint8_t *p, uint32_t len, bool key) {
            fwrite(p, 1, len, fo);
            total += len;
            if (key) fprintf(stderr, "[venc] keyframe %uB\n", len);
            return true;
        });
        if (!ok) { fprintf(stderr, "encode fail: %s\n", enc.lastError().c_str()); break; }
        frames++;
    }
    fclose(fo);
    fprintf(stderr, "encoded %d frames, %lu bytes total\n", frames, (unsigned long)total);
    return frames == n ? 0 : 1;
}
