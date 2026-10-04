// ============================================================================
// 最小推流程序：V4L2 MJPG 采集 → TCP 发送（WSL 端）
// 用法: ./mjpeg_sender /dev/video0 <板子IP> [端口=8888]
// 协议（极简帧协议，板端接收程序同款）:
//   每帧: [4 字节小端长度 N][N 字节 MJPEG 数据]
// 延迟优化:
//   - TCP_NODELAY:禁用 Nagle 攒包,小帧立即发出(否则最多攒 ~40ms)
//   - 采集缓冲 4→2 帧:减少 V4L2 队列排队深度(每帧 -33ms)
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
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0) { perror("S_FMT"); return -1; }
    printf("camera: %ux%u pixfmt=%c%c%c%c\n", fmt.fmt.pix.width, fmt.fmt.pix.height,
           fmt.fmt.pix.pixelformat & 0xFF, (fmt.fmt.pix.pixelformat >> 8) & 0xFF,
           (fmt.fmt.pix.pixelformat >> 16) & 0xFF, (fmt.fmt.pix.pixelformat >> 24) & 0xFF);

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

int main(int argc, char** argv) {
    const char* dev = argc > 1 ? argv[1] : "/dev/video0";
    if (argc < 3) { fprintf(stderr, "usage: %s /dev/video0 <board_ip> [port]\n", argv[0]); return 1; }
    int port = argc > 3 ? atoi(argv[3]) : 8888;

    int cam = open_camera(dev, 640, 480);
    if (cam < 0) return 1;

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(argv[2]);
    if (connect(sock, (sockaddr*)&addr, sizeof(addr)) < 0) { perror("connect"); return 1; }
    // TCP_NODELAY:每帧立即发,不让 Nagle 攒小包(局域网低延迟关键项)
    int nd = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &nd, sizeof(nd));
    printf("connected to %s:%d (nodelay, nbuf=%d)\n", argv[2], port, NBUF);

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
