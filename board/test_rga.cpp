// RGA 单测:输入 NV12 文件 → im2d 缩放(640x480 → 320x240)→ 输出 NV12
// 用法: ./test_rga <in.yuv> <w> <h> <out.yuv>
// 验证:输出文件拉回 PC 用 ffmpeg 转图检查画面
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "rga.h"
#include "im2d.h"

int main(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "usage: %s <in.yuv> <w> <h> <out.yuv>\n", argv[0]); return 1; }
    int w = atoi(argv[2]), h = atoi(argv[3]);
    int ow = w / 2, oh = h / 2;   // 目标: 缩一半

    FILE *fi = fopen(argv[1], "rb");
    if (!fi) { perror("in"); return 1; }
    std::vector<uint8_t> in((size_t)w * h * 3 / 2);
    fread(in.data(), 1, in.size(), fi);
    fclose(fi);

    std::vector<uint8_t> out((size_t)ow * oh * 3 / 2, 0x80);

    // im2d API:wrapbuffer 封装 fd/指针 + 宽高 stride 格式
    rga_buffer_t src = wrapbuffer_virtualaddr(in.data(), w, h, RK_FORMAT_YCbCr_420_SP);
    rga_buffer_t dst = wrapbuffer_virtualaddr(out.data(), ow, oh, RK_FORMAT_YCbCr_420_SP);

    IM_STATUS st = imresize(src, dst, 0, 0, INTER_LINEAR, 1);   // fx/fy=0 按目标尺寸缩放,同步
    fprintf(stderr, "imresize: %d %s\n", st, imStrError(st));
    // IM_STATUS_SUCCESS=1 / IM_STATUS_NOERROR=2 均为成功,负值才是失败
    if ((int)st < 1) return 1;

    FILE *fo = fopen(argv[4], "wb");
    fwrite(out.data(), 1, out.size(), fo);
    fclose(fo);
    fprintf(stderr, "rga done: %dx%d -> %dx%d\n", w, h, ow, oh);
    return 0;
}
