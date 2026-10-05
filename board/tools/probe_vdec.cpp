// 决定性实验:色卡 JPEG → VDEC 硬解 → 全量 dump 输出 buffer
// dump 尺寸取 stride*ver_stride*2(NV16 上限),拉回 PC 后分析 UV 平面布局:
//   若真输出 NV12(1.5x):[1.0x, 1.5x) 区间为 UV,之后是垃圾/重复
//   若真输出 NV16(2.0x):[1.0x, 2.0x) 区间为完整 UV(行数=height)
// 用法: ./probe_vdec <in.jpg> <out.raw>
#include "mpp_jpeg_decoder.hpp"
#include <cstdio>
#include <cstdlib>
#include <vector>

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s <in.jpg> <out.raw>\n", argv[0]); return 1; }
    FILE *fi = fopen(argv[1], "rb");
    if (!fi) { perror("open"); return 1; }
    std::vector<uint8_t> jpg;
    uint8_t tmp[4096]; size_t n;
    while ((n = fread(tmp, 1, sizeof(tmp), fi)) > 0) jpg.insert(jpg.end(), tmp, tmp + n);
    fclose(fi);
    fprintf(stderr, "jpeg %zu bytes\n", jpg.size());

    FILE *fo = fopen(argv[2], "wb");
    int wrote = 0;
    MppJpegDecoder dec;
    dec.setExpectSize(640, 480);
    bool ok = dec.decode(jpg.data(), (uint32_t)jpg.size(), [&](const DecFrame &df) {
        fprintf(stderr, "[probe] %dx%d stride %dx%d buf_size=%d is422=%d\n",
                df.width, df.height, df.hor_stride, df.ver_stride, df.buf_size, df.is422);
        // 全量 dump:hor_stride * (ver_stride*2) —— 覆盖 NV12/NV16 两种布局的全部有效区
        int total = df.hor_stride * df.ver_stride * 2;
        fwrite(df.data, 1, total, fo);
        wrote = total;
        return true;
    });
    fclose(fo);
    fprintf(stderr, "decode %s, wrote %d bytes\n", ok ? "OK" : "FAIL", wrote);
    return ok ? 0 : 1;
}
