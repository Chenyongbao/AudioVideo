// ============================================================================
// V4L2 视频采集实现（PC/WSL 版）—— IVideoSource 接口的 Linux 通用层落地。
// ----------------------------------------------------------------------------
// 【数据通路】/dev/video0 (UVC) → ioctl 协商格式 → 4 个 mmap 缓冲排队出流
//   → DQBUF 取帧 → (YUYV 软转 NV12 | MJPG 软解 NV12) → 统一 VideoFrame。
// 【实测教训：为什么默认 MJPG】
//   usbipd(vhci) 虚拟 USB 通道无法为 YUYV 的等时传输维持 ~15MB/s 带宽预留，
//   所有 URB 被 -ECONNRESET(-104) 取消——设备能枚举、能亮灯，但永远不出帧。
//   MJPG 是压缩格式带宽低 1~2 个量级，可通过 USB/IP 正常传输，
//   接收端用 libavcodec 软解回 NV12，对上层透明。
// 【RV1126 移植】本文件整体替换为 video_rkmpi.cpp（RKMPI VI 通道）：
//   VI 直接出 NV12（ISP 硬件完成），无需本文件的软件颜色转换；
//   mmap 缓冲队列对应 VB 池，DQBUF/QBUF 对应 Get/ReleaseMediaBuffer。
// ============================================================================
#include "platform/video_source.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#include <cerrno>
#include <cstdio>
#include <stdexcept>

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace {

// ioctl 重试封装：V4L2 调用可能被信号打断（EINTR），需自动重试
int xioctl(int fd, unsigned long req, void* arg) {
    int r;
    do { r = ioctl(fd, req, arg); } while (r < 0 && errno == EINTR);
    return r;
}

// YUYV422 打包格式 → NV12 (YUV420SP) 软件颜色转换。
// 内存布局：YUYV = [Y0 U Y1 V] 交织（每像素 2 字节，水平 4:2:2）；
//           NV12  = Y 平面整幅 + UV 交错平面（水平垂直均 4:2:0）。
// 转换规则：每行取 Y0/Y1 直接拷贝；UV 仅在偶数行取（丢弃奇数行色度=垂直降采样）。
// 性能说明：640x480@30 单核占用可控，demo 足够；板端 VI 硬件直接出 NV12 无此开销。
void yuyv_to_nv12(const uint8_t* src, uint8_t* dst, int w, int h) {
    uint8_t* y = dst;
    uint8_t* uv = dst + (size_t)w * h;
    for (int row = 0; row < h; ++row) {
        const uint8_t* s = src + (size_t)row * w * 2;
        bool odd = (row & 1);
        for (int col = 0; col < w; col += 2) {
            y[col]     = s[col * 2];       // Y0
            y[col + 1] = s[col * 2 + 2];   // Y1
            if (!odd) {                    // 色度只在偶数行写入（4:2:0 垂直降采样）
                uv[col]     = s[col * 2 + 1];  // U
                uv[col + 1] = s[col * 2 + 3];  // V
            }
        }
        y += w;
        if (!odd) uv += w;
    }
}

class V4l2Source : public IVideoSource {
public:
    ~V4l2Source() override { close(); }

    bool open(const std::string& dev, int w, int h, int fps) override {
        fd_ = ::open(dev.c_str(), O_RDWR);
        if (fd_ < 0) { perror("open video dev"); return false; }

        v4l2_format fmt{};
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fmt.fmt.pix.width = w;
        fmt.fmt.pix.height = h;
        // YUYV 为等时传输，usbipd(vhci) 通道无法维持其带宽（URB -104 全部失败）；
        // MJPG 为压缩格式，带宽要求低，USB/IP 下可正常出流，接收端软解回 NV12
        fmt.fmt.pix.pixelformat = (fmt_ == PixFmt::MJPG) ? V4L2_PIX_FMT_MJPEG
                                                         : V4L2_PIX_FMT_YUYV;
        if (xioctl(fd_, VIDIOC_S_FMT, &fmt) < 0) { perror("S_FMT"); return false; }
        w_ = fmt.fmt.pix.width; h_ = fmt.fmt.pix.height;

        v4l2_streamparm parm{};
        parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        parm.parm.capture.timeperframe.numerator = 1;
        parm.parm.capture.timeperframe.denominator = fps;
        ioctl(fd_, VIDIOC_S_PARM, &parm);  // 非致命：部分 UVC 摄像头不支持

        v4l2_requestbuffers req{};
        req.count = 4;
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;
        if (xioctl(fd_, VIDIOC_REQBUFS, &req) < 0) { perror("REQBUFS"); return false; }

        bufs_.resize(req.count);
        for (uint32_t i = 0; i < req.count; ++i) {
            v4l2_buffer b{};
            b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            b.memory = V4L2_MEMORY_MMAP;
            b.index = i;
            if (xioctl(fd_, VIDIOC_QUERYBUF, &b) < 0) { perror("QUERYBUF"); return false; }
            bufs_[i].len = b.length;
            bufs_[i].start = mmap(nullptr, b.length, PROT_READ | PROT_WRITE,
                                  MAP_SHARED, fd_, b.m.offset);
            if (bufs_[i].start == MAP_FAILED) { perror("mmap"); return false; }
            xioctl(fd_, VIDIOC_QBUF, &b);
        }

        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (xioctl(fd_, VIDIOC_STREAMON, &type) < 0) { perror("STREAMON"); return false; }
        return true;
    }

    void close() override {
        if (fd_ >= 0) {
            v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            ioctl(fd_, VIDIOC_STREAMOFF, &type);
            for (auto& b : bufs_)
                if (b.start && b.start != MAP_FAILED) munmap(b.start, b.len);
            bufs_.clear();
            ::close(fd_);
            fd_ = -1;
        }
    }

    bool isOpen() const override { return fd_ >= 0; }

    bool read(VideoFrame& out) override {
        v4l2_buffer b{};
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        if (xioctl(fd_, VIDIOC_DQBUF, &b) < 0) {
            static int err_cnt = 0;
            if (err_cnt++ < 5) fprintf(stderr, "[v4l2] DQBUF fail: %s (errno=%d)\n",
                                       strerror(errno), errno);
            return false;
        }

        out.pts_ms = now_ms();
        out.width = w_; out.height = h_;
        out.encoded = false;
        out.keyframe = false;
        out.data.resize((size_t)w_ * h_ * 3 / 2);
        if (fmt_ == PixFmt::MJPG)
            mjpeg_to_nv12((uint8_t*)bufs_[b.index].start, b.bytesused, out.data.data(), w_, h_);
        else
            yuyv_to_nv12((uint8_t*)bufs_[b.index].start, out.data.data(), w_, h_);

        xioctl(fd_, VIDIOC_QBUF, &b);
        return true;
    }

    int fps() const override { return fps_; }
    int width() const override { return w_; }
    int height() const override { return h_; }
    void setPixelFormat(PixFmt f) override { fmt_ = f; }

private:
    // MJPEG 解码 -> NV12（usbipd 等时传输带宽不足时用 MJPG 出流，接收端软解）
    bool initMjpegDecoder() {
        if (dec_) return true;
        const AVCodec* c = avcodec_find_decoder(AV_CODEC_ID_MJPEG);
        if (!c) return false;
        dec_ = avcodec_alloc_context3(c);
        if (!dec_) return false;
        if (avcodec_open2(dec_, c, nullptr) < 0) { avcodec_free_context(&dec_); return false; }
        dframe_ = av_frame_alloc();
        return dframe_ != nullptr;
    }

    bool mjpeg_to_nv12(uint8_t* jpeg, size_t len, uint8_t* dst, int w, int h) {
        if (!initMjpegDecoder()) return false;
        AVPacket* pkt = av_packet_alloc();
        pkt->data = jpeg;
        pkt->size = (int)len;
        bool ok = false;
        if (avcodec_send_packet(dec_, pkt) >= 0 &&
            avcodec_receive_frame(dec_, dframe_) >= 0) {
            // 解码输出 YUVJ420P（Y/U/V 三平面）-> 打包为 NV12
            memcpy(dst, dframe_->data[0], (size_t)w * h);
            uint8_t* uv = dst + (size_t)w * h;
            int cw = w / 2, ch = h / 2;
            for (int r = 0; r < ch; ++r) {
                for (int c2 = 0; c2 < cw; ++c2) {
                    uv[r * w + c2 * 2]     = dframe_->data[1][r * dframe_->linesize[1] + c2];
                    uv[r * w + c2 * 2 + 1] = dframe_->data[2][r * dframe_->linesize[2] + c2];
                }
            }
            ok = true;
        }
        av_packet_free(&pkt);
        return ok;
    }

    int fd_ = -1;
    int w_ = 0, h_ = 0, fps_ = 25;
    PixFmt fmt_ = PixFmt::MJPG;   // 默认 MJPG：usbipd 下 YUYV 等时传输不出流
    AVCodecContext* dec_ = nullptr;
    AVFrame* dframe_ = nullptr;
    struct MmapBuf { void* start = nullptr; size_t len = 0; };
    std::vector<MmapBuf> bufs_;
};

} // namespace

std::unique_ptr<IVideoSource> create_v4l2_source() {
    return std::make_unique<V4l2Source>();
}
