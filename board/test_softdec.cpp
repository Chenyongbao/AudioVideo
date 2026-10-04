// 软解单测:JPEG 文件 → NV12 dump(板端验证,格式同 VDEC 目标输出)
#include "soft_jpeg_decoder.hpp"
#include <cstdio>
#include <vector>

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s <in.jpg> <out.yuv>\n", argv[0]); return 1; }
    FILE *fi = fopen(argv[1], "rb");
    if (!fi) { perror("open"); return 1; }
    std::vector<uint8_t> jpg;
    uint8_t tmp[4096]; size_t n;
    while ((n = fread(tmp, 1, sizeof(tmp), fi)) > 0) jpg.insert(jpg.end(), tmp, tmp + n);
    fclose(fi);

    FILE *fo = fopen(argv[2], "wb");
    SoftJpegDecoder dec;
    bool ok = dec.decode(jpg.data(), (uint32_t)jpg.size(), [&](const DecFrame &df) {
        fprintf(stderr, "[soft] decoded %dx%d stride %dx%d\n", df.width, df.height, df.hor_stride, df.ver_stride);
        fwrite(df.data, 1, df.buf_size, fo);
        return true;
    });
    fclose(fo);
    fprintf(stderr, "decode %s err=%s\n", ok ? "OK" : "FAIL", dec.lastError().c_str());
    return ok ? 0 : 1;
}
