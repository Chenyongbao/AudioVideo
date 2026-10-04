// ============================================================================
// NPU 检测器实现:rknn 上下文生命周期全部在推理线程内(初始化/推理/销毁)
// 推理节奏:wait_for(200ms) → 实测推理 ~70ms,自然节拍 ≈5fps
// ============================================================================
#include "yolo_detector.hpp"
#include "yolo_postprocess.h"

#include <cstdio>
#include <cstring>
#include <chrono>

extern "C" {
#include "rknn_api.h"
}

bool YoloDetector::init(const std::string &modelPath) {
    FILE *f = fopen(modelPath.c_str(), "rb");
    if (!f) { fprintf(stderr, "[yolo] model not found: %s\n", modelPath.c_str()); return false; }
    fclose(f);
    modelPath_ = modelPath;
    return true;
}

void YoloDetector::start(ResultCb cb) {
    if (running_.load()) return;
    cb_ = std::move(cb);
    running_.store(true);
    th_ = std::thread(&YoloDetector::loop, this);
}

void YoloDetector::stop() {
    running_.store(false);
    cv_.notify_all();
    if (th_.joinable()) th_.join();
}

void YoloDetector::submitFrame(const uint8_t *nv12, int w, int h, uint64_t tsMs) {
    if (!running_.load()) return;
    std::lock_guard<std::mutex> lk(slotM_);
    size_t need = (size_t)w * h * 3 / 2;
    if (slotBuf_.size() != need) slotBuf_.resize(need);
    memcpy(slotBuf_.data(), nv12, need);
    slotW_ = w; slotH_ = h; slotTs_ = tsMs;
    slotValid_ = true;
    cv_.notify_one();
}

// NV12 → RGB(带 2:1 下采样色度恢复),同时最近邻拉伸到模型输入
void YoloDetector::nv12ToRgbStretch(const uint8_t *nv12, int sw, int sh,
                                    uint8_t *rgb, int dw, int dh)
{
    const uint8_t *yP = nv12;
    const uint8_t *uvP = nv12 + (size_t)sw * sh;
    for (int dy = 0; dy < dh; dy++) {
        int sy = (int)((float)dy * sh / dh); if (sy >= sh) sy = sh - 1;
        const uint8_t *yRow = yP + (size_t)sy * sw;
        const uint8_t *uvRow = uvP + (size_t)(sy / 2) * sw;
        for (int dx = 0; dx < dw; dx++) {
            int sx = (int)((float)dx * sw / dw); if (sx >= sw) sx = sw - 1;
            float Y = yRow[sx];
            int uIdx = (sx / 2) * 2;               // UV 交织 U,V
            float U = uvRow[uIdx] - 128.0f;
            float V = uvRow[uIdx + 1] - 128.0f;
            float R = Y + 1.402f * V;
            float G = Y - 0.344136f * U - 0.714136f * V;
            float B = Y + 1.772f * U;
            uint8_t *d = rgb + ((size_t)dy * dw + dx) * 3;
            d[0] = R < 0 ? 0 : (R > 255 ? 255 : (uint8_t)R);
            d[1] = G < 0 ? 0 : (G > 255 ? 255 : (uint8_t)G);
            d[2] = B < 0 ? 0 : (B > 255 ? 255 : (uint8_t)B);
        }
    }
}

void YoloDetector::loop() {
    // ---- 模型加载 + 上下文创建(本线程) ----
    FILE *f = fopen(modelPath_.c_str(), "rb");
    if (!f) { fprintf(stderr, "[yolo] open model fail\n"); running_.store(false); return; }
    fseek(f, 0, SEEK_END); int msz = ftell(f); fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> model(msz);
    if (fread(model.data(), 1, msz, f) != (size_t)msz) { fclose(f); running_.store(false); return; }
    fclose(f);

    rknn_context ctx;
    if (rknn_init(&ctx, model.data(), msz, 0) < 0) {
        fprintf(stderr, "[yolo] rknn_init fail\n"); running_.store(false); return;
    }
    rknn_input_output_num ionum;
    if (rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &ionum, sizeof(ionum)) < 0 || ionum.n_output > 16) {
        fprintf(stderr, "[yolo] query fail\n"); rknn_destroy(ctx); running_.store(false); return;
    }
    rknn_tensor_attr iattr; memset(&iattr, 0, sizeof(iattr)); iattr.index = 0;
    rknn_query(ctx, RKNN_QUERY_INPUT_ATTR, &iattr, sizeof(iattr));
    int mw, mh;
    if (iattr.fmt == RKNN_TENSOR_NCHW) { mw = iattr.dims[0]; mh = iattr.dims[1]; }
    else                               { mw = iattr.dims[1]; mh = iattr.dims[2]; }
    std::vector<uint8_t> zps; std::vector<float> scales;
    for (uint32_t i = 0; i < ionum.n_output; i++) {
        rknn_tensor_attr a; memset(&a, 0, sizeof(a)); a.index = i;
        rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR, &a, sizeof(a));
        zps.push_back(a.zp); scales.push_back(a.scale);
    }
    fprintf(stderr, "[yolo] ready model %dx%d outputs=%u\n", mw, mh, ionum.n_output);

    std::vector<uint8_t> rgb((size_t)mw * mh * 3);
    std::vector<uint8_t> frame;          // 取走的工作副本

    while (running_.load()) {
        // ---- 取帧(若无帧等 200ms,节拍兜底 5fps) ----
        {
            std::unique_lock<std::mutex> lk(slotM_);
            if (!slotValid_)
                cv_.wait_for(lk, std::chrono::milliseconds(200));
            if (!slotValid_ || !running_.load()) continue;
            frame.swap(slotBuf_);        // 取走,槽位清空
            slotValid_ = false;
        }
        int sw = slotW_, sh = slotH_;
        uint64_t ts = slotTs_;

        // ---- 预处理:NV12 → RGB 拉伸 ----
        nv12ToRgbStretch(frame.data(), sw, sh, rgb.data(), mw, mh);

        // ---- 推理 ----
        rknn_input inputs[1]; memset(inputs, 0, sizeof(inputs));
        inputs[0].index = 0; inputs[0].type = RKNN_TENSOR_UINT8;
        inputs[0].size = (uint32_t)mw * mh * 3; inputs[0].fmt = RKNN_TENSOR_NHWC;
        inputs[0].pass_through = 0; inputs[0].buf = rgb.data();
        rknn_inputs_set(ctx, 1, inputs);
        if (rknn_run(ctx, nullptr) < 0) continue;
        rknn_output outputs[16]; memset(outputs, 0, sizeof(outputs));
        for (uint32_t i = 0; i < ionum.n_output; i++) outputs[i].want_float = 0;
        if (rknn_outputs_get(ctx, ionum.n_output, outputs, nullptr) < 0) continue;

        // ---- 后处理(框映射回源帧坐标) ----
        detect_result_group_t grp;
        post_process((uint8_t *)outputs[0].buf, (uint8_t *)outputs[1].buf, (uint8_t *)outputs[2].buf,
                     mh, mw, 0.3f, 0.5f, 0.25f,
                     (float)mw / sw, (float)mh / sh, zps, scales, &grp);
        rknn_outputs_release(ctx, ionum.n_output, outputs);

        std::vector<DetBox> boxes;
        for (int i = 0; i < grp.count; i++) {
            DetBox b;
            b.x1 = grp.results[i].box.left;  b.y1 = grp.results[i].box.top;
            b.x2 = grp.results[i].box.right; b.y2 = grp.results[i].box.bottom;
            b.prop = grp.results[i].prop;
            strncpy(b.name, grp.results[i].name, sizeof(b.name) - 1);
            b.name[sizeof(b.name) - 1] = 0;
            boxes.push_back(b);
        }
        if (cb_) cb_(boxes, ts);
    }

    rknn_destroy(ctx);
    fprintf(stderr, "[yolo] stopped\n");
}
