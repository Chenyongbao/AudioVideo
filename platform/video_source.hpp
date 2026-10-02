#pragma once
// 视频采集抽象：PC 版走 V4L2(/dev/video*)，RV1126 移植时实现为 RKMPI VI 通道
#include "platform/common.hpp"
#include <string>
#include <functional>
#include <memory>

class IVideoSource {
public:
    virtual ~IVideoSource() = default;
    // 打开设备：宽度/高度/帧率
    virtual bool open(const std::string& dev, int width, int height, int fps) = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;
    // 阻塞抓一帧（NV12 原始帧），失败返回 false
    virtual bool read(VideoFrame& out) = 0;
    virtual int fps() const = 0;
    virtual int width() const = 0;
    virtual int height() const = 0;
    // 像素格式选择：YUYV 未压缩（等时传输，usbipd 下可能不出流），
    // MJPG 压缩（带宽低，usbipd/带宽受限场景用），内部解码回 NV12
    enum class PixFmt { YUYV, MJPG };
    virtual void setPixelFormat(PixFmt f) = 0;
};

std::unique_ptr<IVideoSource> create_v4l2_source();
