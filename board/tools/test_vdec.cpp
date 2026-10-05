// VDEC 单测:读 JPEG 文件 → MPP 解码 → dump NV12 → 拉回 PC 用 ffplay 验证
// 用法: ./test_vdec <in.jpg> <out.yuv>
#include "mpp_jpeg_decoder.hpp"
#include <cstdio>
#include <cstdlib>
#include <vector>

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s <in.jpg> <out.yuv>\n", argv[0]); return 1; }

    FILE *fi = fopen(argv[1], "rb");
    if (!fi) { perror("open jpg"); return 1; }
    std::vector<uint8_t> jpg;
    uint8_t tmp[4096]; size_t n;
    while ((n = fread(tmp, 1, sizeof(tmp), fi)) > 0) jpg.insert(jpg.end(), tmp, tmp + n);
    fclose(fi);
    printf("jpeg %zu bytes\n", jpg.size());

    FILE *fo = fopen(argv[2], "wb");
    MppJpegDecoder dec;
    dec.setExpectSize(640, 480);   // MJPEG 硬解必须预分配输出 buffer
    bool ok = dec.decode(jpg.data(), (uint32_t)jpg.size(), [&](const DecFrame &df) {
        printf("decoded: %dx%d stride %dx%d bufsize %d\n",
               df.width, df.height, df.hor_stride, df.ver_stride, df.buf_size);
        // 按 stride 布局紧凑写 NV12(Y 平面 + UV 平面,去掉行尾 padding)
        for (int r = 0; r < df.height; r++)
            fwrite(df.data + (size_t)r * df.hor_stride, 1, df.width, fo);
        const uint8_t *uv = df.data + (size_t)df.hor_stride * df.ver_stride;
        for (int r = 0; r < df.height / 2; r++)
            fwrite(uv + (size_t)r * df.hor_stride, 1, df.width, fo);
        return true;
    });
    fclose(fo);
    printf("decode %s, error: %s\n", ok ? "OK" : "FAILED", dec.lastError().c_str());
    return ok ? 0 : 1;
}
