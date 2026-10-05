// ============================================================================
// NPU 冒烟测试:加载 SDK 现货 yolov5s_rv1109_rv1126 模型,BMP 输入,
// 打印检测框与推理耗时。后处理直接复用 demo 的 postprocess.cc(链接期同译)。
// 用法: ./detect_test <model.rknn> <in.bmp>
//       (cwd 下须有 model/coco_80_labels_list.txt,postprocess 内部按相对路径读)
// ============================================================================
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <vector>
#include <sys/time.h>

#include "rknn_api.h"
#include "postprocess.h"

static double nowMs() {
    struct timeval t; gettimeofday(&t, nullptr);
    return t.tv_sec * 1000.0 + t.tv_usec / 1000.0;
}

// ---- 最小 24bit BMP 解码(BGR→RGB,兼容底朝上/倒置) ----
static uint8_t *load_bmp24(const char *path, int *w, int *h) {
    FILE *f = fopen(path, "rb");
    if (!f) return nullptr;
    uint8_t hdr[54];
    if (fread(hdr, 1, 54, f) != 54 || hdr[0] != 'B' || hdr[1] != 'M') { fclose(f); return nullptr; }
    uint32_t off = hdr[10] | (hdr[11] << 8) | (uint32_t)hdr[12] << 16 | (uint32_t)hdr[13] << 24;
    int32_t iw = hdr[18] | (hdr[19] << 8) | (int32_t)hdr[20] << 16 | (int32_t)hdr[21] << 24;
    int32_t ih = hdr[22] | (hdr[23] << 8) | (int32_t)hdr[24] << 16 | (int32_t)hdr[25] << 24;
    bool flip = ih > 0;
    *w = iw; *h = flip ? ih : -ih;
    uint8_t *rgb = (uint8_t *)malloc((size_t)*w * *h * 3);
    int rowSize = ((*w * 3) + 3) / 4 * 4;
    std::vector<uint8_t> row(rowSize);
    fseek(f, off, SEEK_SET);
    for (int y = 0; y < *h; y++) {
        if (fread(row.data(), 1, rowSize, f) != (size_t)rowSize) { fclose(f); free(rgb); return nullptr; }
        int dy = flip ? *h - 1 - y : y;
        for (int x = 0; x < *w; x++) {
            uint8_t *d = rgb + ((size_t)dy * *w + x) * 3;
            d[0] = row[x * 3 + 2]; d[1] = row[x * 3 + 1]; d[2] = row[x * 3];
        }
    }
    fclose(f);
    return rgb;
}

// 最近邻拉伸到模型输入(demo 亦为无保持比例拉伸,scale_x/y 独立映射回原图)
static uint8_t *resize_nn(const uint8_t *s, int sw, int sh, int dw, int dh) {
    uint8_t *d = (uint8_t *)malloc((size_t)dw * dh * 3);
    for (int y = 0; y < dh; y++) {
        int sy = (int)((float)y * sh / dh); if (sy >= sh) sy = sh - 1;
        for (int x = 0; x < dw; x++) {
            int sx = (int)((float)x * sw / dw); if (sx >= sw) sx = sw - 1;
            memcpy(d + ((size_t)y * dw + x) * 3, s + ((size_t)sy * sw + sx) * 3, 3);
        }
    }
    return d;
}

int main(int argc, char **argv) {
    if (argc < 3) { printf("usage: %s <model.rknn> <in.bmp>\n", argv[0]); return 1; }
    int iw, ih;
    uint8_t *rgb = load_bmp24(argv[2], &iw, &ih);
    if (!rgb) { printf("load_bmp fail\n"); return 1; }
    printf("image %dx%d\n", iw, ih);

    FILE *f = fopen(argv[1], "rb");
    if (!f) { printf("open model fail\n"); return 1; }
    fseek(f, 0, SEEK_END); int msz = ftell(f); fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> model(msz);
    if (fread(model.data(), 1, msz, f) != (size_t)msz) { printf("read model fail\n"); return 1; }
    fclose(f);

    rknn_context ctx;
    if (rknn_init(&ctx, model.data(), msz, 0) < 0) { printf("rknn_init fail\n"); return 1; }

    rknn_sdk_version ver;
    rknn_query(ctx, RKNN_QUERY_SDK_VERSION, &ver, sizeof(ver));
    printf("api:%s drv:%s\n", ver.api_version, ver.drv_version);

    rknn_input_output_num ionum;
    rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &ionum, sizeof(ionum));
    rknn_tensor_attr iattr; memset(&iattr, 0, sizeof(iattr)); iattr.index = 0;
    rknn_query(ctx, RKNN_QUERY_INPUT_ATTR, &iattr, sizeof(iattr));
    int mw, mh;
    if (iattr.fmt == RKNN_TENSOR_NCHW) { mw = iattr.dims[0]; mh = iattr.dims[1]; }
    else                              { mw = iattr.dims[1]; mh = iattr.dims[2]; }
    printf("model in %dx%d, n_output=%d\n", mw, mh, ionum.n_output);

    std::vector<uint8_t> zps; std::vector<float> scales;
    for (uint32_t i = 0; i < ionum.n_output; i++) {
        rknn_tensor_attr a; memset(&a, 0, sizeof(a)); a.index = i;
        rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR, &a, sizeof(a));
        zps.push_back(a.zp); scales.push_back(a.scale);
    }

    uint8_t *in = resize_nn(rgb, iw, ih, mw, mh);

    rknn_input inputs[1]; memset(inputs, 0, sizeof(inputs));
    inputs[0].index = 0; inputs[0].type = RKNN_TENSOR_UINT8;
    inputs[0].size = (uint32_t)mw * mh * 3; inputs[0].fmt = RKNN_TENSOR_NHWC;
    inputs[0].pass_through = 0; inputs[0].buf = in;
    rknn_inputs_set(ctx, 1, inputs);

    double t0 = nowMs();
    if (rknn_run(ctx, nullptr) < 0) { printf("rknn_run fail\n"); return 1; }
    rknn_output outputs[16]; memset(outputs, 0, sizeof(outputs));
    for (uint32_t i = 0; i < ionum.n_output; i++) outputs[i].want_float = 0;
    rknn_outputs_get(ctx, ionum.n_output, outputs, nullptr);
    printf("inference %.1f ms\n", nowMs() - t0);

    detect_result_group_t grp;
    post_process((uint8_t *)outputs[0].buf, (uint8_t *)outputs[1].buf, (uint8_t *)outputs[2].buf,
                 mh, mw, 0.3f, 0.5f, 0.1f, (float)mw / iw, (float)mh / ih,
                 zps, scales, &grp);
    printf("detections: %d\n", grp.count);
    for (int i = 0; i < grp.count; i++)
        printf("  %s @(%d,%d,%d,%d) %.2f\n", grp.results[i].name,
               grp.results[i].box.left, grp.results[i].box.top,
               grp.results[i].box.right, grp.results[i].box.bottom, grp.results[i].prop);

    rknn_outputs_release(ctx, ionum.n_output, outputs);
    rknn_destroy(ctx);
    free(rgb); free(in);
    return 0;
}
