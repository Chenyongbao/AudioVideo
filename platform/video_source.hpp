#pragma once
// ============================================================================
// 视频采集抽象接口 —— 平台层的"摄像机"。
// 【设计目的】把"帧从哪来"与"帧怎么处理"彻底解耦：
//   - PC/WSL 阶段：V4L2 实现（video_v4l2.cpp），USB 摄像头；
//   - RV1126 移植：新增 video_rkmpi.cpp 实现（RKMPI VI 通道，CSI+ISP 硬件
//     流水线，帧带 3A 元数据），本接口与中间件层一行不改。
// 【与 V4L2 的概念对应】open=S_FMT+REQBUFS+STREAMON，read=DQBUF+QBUF，
//   close=STREAMOFF+munmap —— RKMPI 对应 GetMediaBuffer/ReleaseMediaBuffer，
//   心智模型同构，移植成本低。
// ============================================================================
#include "platform/common.hpp"
#include <string>
#include <functional>
#include <memory>

class IVideoSource {
public:
    virtual ~IVideoSource() = default;

    // 打开设备并开始出流。
    //   dev: 设备路径（PC: "/dev/video0"；RKMPI 实现可忽略此参数，用通道号）
    //   w/h/fps: 期望分辨率与帧率，实际值以设备能力为准（S_FMT 是协商不是命令，
    //            实测值用 width()/height() 取回 —— usbipd 下 YUYV 720p 仅 10fps 的教训）
    virtual bool open(const std::string& dev, int width, int height, int fps) = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;

    // 阻塞抓一帧，输出统一 VideoFrame（NV12 原始帧）。
    // 失败（设备错误/暂无数据）返回 false，调用方应稍后重试而非退出。
    virtual bool read(VideoFrame& out) = 0;

    virtual int fps() const = 0;
    virtual int width() const = 0;
    virtual int height() const = 0;

    // 像素格式选择（必须在 open() 之前调用）：
    //   YUYV — 未压缩 4:2:2，走 USB 等时传输，带宽 ~15MB/s；
    //          usbipd(vhci) 虚拟通道无法维持等时带宽，URB 全部 -104 失败（实测教训）
    //   MJPG — 压缩格式，带宽低 1~2 个量级，USB/IP 下可正常出流；
    //          实现内部用 libavcodec 解码回 NV12，对上层透明
    enum class PixFmt { YUYV, MJPG };
    virtual void setPixelFormat(PixFmt f) = 0;
};

// 工厂函数：返回 PC 版 V4L2 实现（板端新增 create_rkmpi_source，调用处只需改这一行）
std::unique_ptr<IVideoSource> create_v4l2_source();
