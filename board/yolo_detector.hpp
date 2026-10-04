#ifndef YOLO_DETECTOR_HPP
#define YOLO_DETECTOR_HPP

// ============================================================================
// NPU 人/车检测器(RV1126 rknn_api):
//   - 独立线程持有 rknn 上下文,5fps 节拍推理(~70ms/帧,NPU 占用 <20%)
//   - pipeline 主循环 submitFrame() 仅一次 460KB memcpy(~0.3ms),latest-wins:
//     检测线程忙时帧被覆盖,天然丢帧,绝不阻塞 RGA→VENC 编码链路
//   - 结果经回调(检测线程上下文)交上层:自动打点 + 7778 JSON 旁路
// ============================================================================

#include <string>
#include <vector>
#include <functional>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <cstdint>

struct DetBox {
    int x1, y1, x2, y2;
    float prop;
    char name[16];
};

class YoloDetector {
public:
    // cb 在检测线程上下文回调;box 坐标为源帧(如 640x480)像素
    using ResultCb = std::function<void(const std::vector<DetBox> &, uint64_t frameTsMs)>;

    // 仅记录模型路径并检查文件存在;真正的 rknn_init 在推理线程内完成
    bool init(const std::string &modelPath);
    void start(ResultCb cb);
    void stop();
    // 主循环喂帧(非阻塞;分辨率变化时自动适配)
    void submitFrame(const uint8_t *nv12, int w, int h, uint64_t tsMs);
    bool running() const { return running_.load(); }

private:
    void loop();
    // NV12(带 stride 布局)→ RGB 拉伸到模型输入尺寸(BT.601)
    static void nv12ToRgbStretch(const uint8_t *nv12, int sw, int sh,
                                 uint8_t *rgb, int dw, int dh);

    std::string modelPath_;
    std::mutex slotM_;
    std::vector<uint8_t> slotBuf_;     // latest-wins 帧槽(NV12)
    int slotW_ = 0, slotH_ = 0;
    uint64_t slotTs_ = 0;
    bool slotValid_ = false;
    std::thread th_;
    std::atomic<bool> running_{false};
    std::condition_variable cv_;
    ResultCb cb_;
};

#endif // YOLO_DETECTOR_HPP
