#pragma once
// OSD 字符叠加：drawtext 烧帧（时间/编号），编码前叠加（烧进码流，不可分离）。
// GA/T 947.2 6.2.8。板端移植时替换为 RGA/VE 通道叠加，接口不变。
// 数据流：NV12 采集帧 -> sws 转 YUV420P -> drawtext -> YUV420P 给编码器。
#include "platform/common.hpp"
#include <string>
#include <cstdint>

extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
#include <libswscale/swscale.h>
}

class OsdOverlay {
public:
    ~OsdOverlay();
    bool init(int width, int height, int fps, const std::string& device_id);
    // 输入 NV12 帧，原地替换为叠加后的 YUV420P 帧数据（布局变为 yuv420p 三平面）
    // wall_secs: 可信时间源（UTC 秒）；OSD 显示为本地时间 YYYY-MM-DD HH:MM:SS
    // time_trusted: false 时时间戳前加 "* " 警示（6.4.17 证据效力）
    bool apply(VideoFrame& frame, int64_t wall_secs, bool time_trusted = true);

private:
    bool rebuildGraph(const std::string& text);  // 时间戳每秒变化时重建图（低成本路径）
    int w_ = 0, h_ = 0, fps_ = 30;
    std::string device_id_;
    AVFilterGraph* graph_ = nullptr;
    AVFilterContext* src_ctx_ = nullptr;
    AVFilterContext* sink_ctx_ = nullptr;
    SwsContext* sws_to_yuv_ = nullptr;     // NV12 -> YUV420P
    SwsContext* sws_from_ = nullptr;       // filter 输出 -> 紧凑 YUV420P
    AVFrame* filter_frame_ = nullptr;
    int64_t last_text_sec_ = -1;           // 每秒重建一次图（刷新时间显示）
    std::string text_cache_;
};
