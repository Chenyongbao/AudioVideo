// ============================================================================
// 推流程序：V4L2 YUYV 4:2:2 采集 → TCP 发送（WSL 端）
// 用法: ./yuyv_sender   (设备/IP/端口已定死,改配置直接改下方常量)
// 协议(极简帧协议,板端接收程序同款):
//   每帧: [4 字节小端长度 N][N 字节 YUYV 裸数据(614400 字节, 640x480)]
// 说明:
//   - 弃 MJPEG:板端 VDEC 解 4:2:2 JPEG 有色度格式歧义(色度 bug 根源),
//     改发 YUYV 裸流,板端 RGA 直接转 NV12,无任何解码环节
//   - 15fps 而非 30fps:YUYV@30 = 147Mbps 超板端百兆网口(100Mbps)
// 延迟优化:
//   - TCP_NODELAY:禁用 Nagle 攒包
//   - 采集缓冲 2 帧:浅缓冲低延迟
// ============================================================================
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <linux/videodev2.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cstdint>

// ---- 固定配置(定死,不再走命令行参数) ----
static const char *kDev     = "/dev/video0";        // 摄像头设备(UVC)
static const char *kBoardIp = "192.168.137.250";    // RV1126 板子 IP
static const int   kPort    = 8888;                 // 板端收流端口

static int xioctl(int fd, unsigned long req, void* arg) {
    int r;
    do { r = ioctl(fd, req, arg); } while (r < 0 && errno == EINTR);
    return r;
}

static const int NBUF = 2;   // 4→2:浅缓冲低延迟(采到即发,不做平滑)
static void* g_ptr[8];
static uint32_t g_len[8];

static int open_camera(const char* dev, int w, int h) {
    int fd = ::open(dev, O_RDWR);
    if (fd < 0) { perror("open camera"); return -1; }

    v4l2_format fmt{};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = w;
    fmt.fmt.pix.height = h;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;   // YUYV 4:2:2:色度无歧义,绕开 JPEG/VDEC 格式玄学
    if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0) { perror("S_FMT"); return -1; }
    printf("camera: %ux%u pixfmt=%c%c%c%c bytes/line=%u\n", fmt.fmt.pix.width, fmt.fmt.pix.height,
           fmt.fmt.pix.pixelformat & 0xFF, (fmt.fmt.pix.pixelformat >> 8) & 0xFF,
           (fmt.fmt.pix.pixelformat >> 16) & 0xFF, (fmt.fmt.pix.pixelformat >> 24) & 0xFF,
           fmt.fmt.pix.bytesperline);

    // 帧率:YUYV 640x480@30 裸流 147Mbps 超百兆网口,定 15fps(74Mbps 留裕量)
    v4l2_streamparm parm{};
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parm.parm.capture.timeperframe.numerator = 1;
    parm.parm.capture.timeperframe.denominator = 15;
    if (xioctl(fd, VIDIOC_S_PARM, &parm) < 0) perror("S_PARM (fps, 继续)");

    v4l2_requestbuffers req{};
    req.count = NBUF;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0) { perror("REQBUFS"); return -1; }

    for (uint32_t i = 0; i < req.count; i++) {
        v4l2_buffer b{};
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = i;
        if (xioctl(fd, VIDIOC_QUERYBUF, &b) < 0) { perror("QUERYBUF"); return -1; }
        g_ptr[i] = mmap(nullptr, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, b.m.offset);
        g_len[i] = b.length;
        if (g_ptr[i] == MAP_FAILED) { perror("mmap"); return -1; }
        if (xioctl(fd, VIDIOC_QBUF, &b) < 0) { perror("QBUF"); return -1; }
    }

    v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(fd, VIDIOC_STREAMON, &t) < 0) { perror("STREAMON"); return -1; }
    return fd;
}

static int send_all(int sock, const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    while (len > 0) {
        ssize_t n = send(sock, p, len, MSG_NOSIGNAL);
        if (n <= 0) { if (n < 0 && errno == EINTR) continue; return -1; }
        p += n; len -= n;
    }
    return 0;
}

int main() {
    int cam = open_camera(kDev, 640, 480);
    if (cam < 0) return 1;

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(kPort);
    addr.sin_addr.s_addr = inet_addr(kBoardIp);
    if (connect(sock, (sockaddr*)&addr, sizeof(addr)) < 0) { perror("connect"); return 1; }
    // TCP_NODELAY:每帧立即发,不让 Nagle 攒小包(局域网低延迟关键项)
    int nd = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &nd, sizeof(nd));
    printf("connected to %s:%d (nodelay, nbuf=%d)\n", kBoardIp, kPort, NBUF);

    uint64_t frames = 0;
    while (true) {
        v4l2_buffer b{};
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        if (xioctl(cam, VIDIOC_DQBUF, &b) < 0) { perror("DQBUF"); break; }
        // 发送: 4 字节小端长度 + MJPEG 数据
        uint32_t len = b.bytesused;
        uint8_t hdr[4] = {(uint8_t)(len & 0xFF), (uint8_t)(len >> 8), (uint8_t)(len >> 16), (uint8_t)(len >> 24)};
        if (send_all(sock, hdr, 4) < 0 || send_all(sock, g_ptr[b.index], len) < 0) {
            perror("send"); break;
        }
        xioctl(cam, VIDIOC_QBUF, &b);
        if (++frames % 30 == 0) printf("sent %lu frames (last %u bytes)\n", (unsigned long)frames, len);
    }
    close(sock); close(cam);
    return 0;
}
