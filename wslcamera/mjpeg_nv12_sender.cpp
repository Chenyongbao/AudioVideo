// ============================================================================
// 推流程序：V4L2 MJPEG 采集 → WSL 侧 ffmpeg 解码为 NV12 → TCP 发送
// 用法: ./mjpeg_nv12_sender   (设备/IP/端口定死在常量)
// 协议(极简帧协议,板端接收程序同款):
//   每帧: [4 字节小端长度 N][N 字节 NV12 裸数据(460800 = 640*480*1.5)]
// 为什么这样设计:
//   - 板端 VDEC 解 4:2:2 JPEG 色度格式歧义(色度 bug 根源),板端软解的
//     libavcodec 解出来也无色(实测 SATMAX 34→6) → 解码挪到 WSL 侧,
//     这里的 ffmpeg 实测 JPEG→NV12 无损(SATMAX 34→31)
//   - WSL 驱动 YUYV 模式内核报 corrupted data(实测),MJPEG 模式正常 → 采集用 MJPEG
//   - NV12 15fps ≈ 55 Mbps,板端百兆网口可承载
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

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <time.h>
}

// 延迟测量色块:画进视频帧,3s 周期 0-2s 白/2-3s 黑。
// 关键:用 CLOCK_REALTIME(epoch)——WSL2 与 Windows 共享系统时钟,
// Qt 端边框也用 epoch 毫秒取模,两边天然同相,相位差 = 纯链路延迟。
// (此前用 CLOCK_MONOTONIC/启动计时器,相位任意错开,测出的是偏移不是延迟)
// 位置:左上 (16,36) 48x48。
static void draw_latency_patch(uint8_t *nv12, int w, int h) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    long ms = (ts.tv_sec * 1000L + ts.tv_nsec / 1000000L) % 3000;
    uint8_t yv = (ms < 2000) ? 235 : 16;      // 白/黑 (BT.601 limited range)
    for (int r = 36; r < 84; r++) {
        uint8_t *row = nv12 + r * w;
        memset(row + 16, yv, 48);
    }
    // UV 平面:白=128(无色度),黑=128,相同,一块 memset 即可
    uint8_t *uv = nv12 + (size_t)w * h;
    for (int r = 36 / 2; r < 84 / 2; r++)
        memset(uv + r * w + 16 / 2, 128, 48 / 2);
}

// ---- OSD 时间戳:5x7 点阵字体烧进视频帧(采集时刻凭证) ----
// 行式点阵,bit0=最左列;覆盖 "0-9 : - 空格" 足够时间戳使用
static const uint8_t kFont[13][7] = {
    {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E},  // 0
    {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},  // 1
    {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F},  // 2
    {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E},  // 3
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02},  // 4
    {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},  // 5
    {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E},  // 6
    {0x1F,0x01,0x02,0x04,0x08,0x08,0x08},  // 7
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},  // 8
    {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},  // 9
    {0x00,0x04,0x00,0x00,0x04,0x00,0x00},  // :
    {0x00,0x00,0x00,0x0E,0x00,0x00,0x00},  // -
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00},  // 空格
};
static int fontIdx(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c == ':') return 10;
    if (c == '-') return 11;
    return 12;
}
static void drawCharY(uint8_t *nv12, int w, int h, int x0, int y0, char c, int sc, uint8_t yv) {
    const uint8_t *g = kFont[fontIdx(c)];
    for (int r = 0; r < 7; r++)
        for (int cbit = 0; cbit < 5; cbit++) {
            if (!((g[r] >> cbit) & 1)) continue;
            for (int dy = 0; dy < sc; dy++) {
                int y = y0 + r * sc + dy;
                if (y < 0 || y >= h) continue;
                uint8_t *row = nv12 + y * w;
                for (int dx = 0; dx < sc; dx++) {
                    int x = x0 + cbit * sc + dx;
                    if (x >= 0 && x < w) row[x] = yv;
                }
            }
        }
}
// "YYYY-MM-DD HH:MM:SS" 黑底白字,画在延迟色块下方(y=92,缩放2x)
static void draw_osd_time(uint8_t *nv12, int w, int h) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tmv;
    time_t sec = ts.tv_sec;
    localtime_r(&sec, &tmv);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv);

    const int sc = 2, cw = 6 * sc;             // 字宽(5+1间距)*缩放
    const int bw = (int)strlen(buf) * cw + 8, bh = 7 * sc + 8, bx = 12, by = 90;
    // 黑底(Y=16)+UV=128(无色)
    for (int r = by; r < by + bh && r < h; r++) {
        uint8_t *row = nv12 + r * w;
        memset(row + bx, 16, (bx + bw < w) ? bw : w - bx);
    }
    uint8_t *uv = nv12 + (size_t)w * h;
    for (int r = by / 2; r < (by + bh) / 2 && r < h / 2; r++)
        memset(uv + r * w + bx / 2, 128, bw / 2);
    for (int i = 0; buf[i]; i++)
        drawCharY(nv12, w, h, bx + 4 + i * cw, by + 4, buf[i], sc, 235);
}

// ---- 固定配置(定死,不再走命令行参数) ----
static const char *kDev     = "/dev/video0";        // 摄像头设备(UVC)
static const char *kBoardIp = "192.168.137.250";    // RV1126 板子 IP
static const int   kPort    = 8888;                 // 板端收流端口
static const int   kW       = 640;
static const int   kH       = 480;

static int xioctl(int fd, unsigned long req, void* arg) {
    int r;
    do { r = ioctl(fd, req, arg); } while (r < 0 && errno == EINTR);
    return r;
}

static const int NBUF = 2;   // 浅缓冲低延迟
static void* g_ptr[8];
static uint32_t g_len[8];

static int open_camera(const char* dev, int w, int h) {
    int fd = ::open(dev, O_RDWR);
    if (fd < 0) { perror("open camera"); return -1; }

    v4l2_format fmt{};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = w;
    fmt.fmt.pix.height = h;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;   // WSL 驱动 YUYV 会 corrupt,只有 MJPEG 正常
    if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0) { perror("S_FMT"); return -1; }
    printf("camera: %ux%u MJPEG\n", fmt.fmt.pix.width, fmt.fmt.pix.height);

    // 15fps:NV12 裸流 55Mbps,百兆网口留裕量
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

// MJPEG → NV12(WSL 侧 ffmpeg,实测色度无损)
struct Nv12Codec {
    AVCodecContext *ctx = nullptr;
    AVFrame *frm = nullptr;
    SwsContext *sws = nullptr;
    uint8_t nv12[640 * 480 * 3 / 2];

    bool init() {
        const AVCodec *c = avcodec_find_decoder(AV_CODEC_ID_MJPEG);
        if (!c) return false;
        ctx = avcodec_alloc_context3(c);
        if (!ctx || avcodec_open2(ctx, c, nullptr) < 0) return false;
        frm = av_frame_alloc();
        return frm != nullptr;
    }
    // 返回 false = 解码失败
    bool decode(const uint8_t *jpg, uint32_t len) {
        AVPacket *pkt = av_packet_alloc();
        av_new_packet(pkt, len);
        memcpy(pkt->data, jpg, len);
        int ret = avcodec_send_packet(ctx, pkt);
        av_packet_free(&pkt);
        if (ret < 0) return false;
        ret = avcodec_receive_frame(ctx, frm);
        if (ret < 0) return false;
        sws = sws_getCachedContext(sws, frm->width, frm->height, (AVPixelFormat)frm->format,
                                   kW, kH, AV_PIX_FMT_NV12, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws) return false;
        uint8_t *dst[4] = { nv12, nv12 + kW * kH, nullptr, nullptr };
        int dstStride[4] = { kW, kW, 0, 0 };
        const uint8_t *src[4] = { frm->data[0], frm->data[1], frm->data[2], nullptr };
        int srcStride[4] = { frm->linesize[0], frm->linesize[1], frm->linesize[2], 0 };
        sws_scale(sws, src, srcStride, 0, frm->height, dst, dstStride);
        av_frame_unref(frm);
        return true;
    }
};

int main() {
    int cam = open_camera(kDev, kW, kH);
    if (cam < 0) return 1;

    Nv12Codec codec;
    if (!codec.init()) { fprintf(stderr, "nv12 codec init failed\n"); return 1; }

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(kPort);
    addr.sin_addr.s_addr = inet_addr(kBoardIp);
    if (connect(sock, (sockaddr*)&addr, sizeof(addr)) < 0) { perror("connect"); return 1; }
    int nd = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &nd, sizeof(nd));
    printf("connected to %s:%d (mjpeg->nv12, nbuf=%d)\n", kBoardIp, kPort, NBUF);

    uint64_t frames = 0;
    while (true) {
        v4l2_buffer b{};
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        if (xioctl(cam, VIDIOC_DQBUF, &b) < 0) { perror("DQBUF"); break; }
        if (b.flags & V4L2_BUF_FLAG_ERROR) {   // 坏帧丢弃,不发送
            xioctl(cam, VIDIOC_QBUF, &b);
            continue;
        }
        bool ok = codec.decode((const uint8_t*)g_ptr[b.index], b.bytesused);
        xioctl(cam, VIDIOC_QBUF, &b);
        if (!ok) { fprintf(stderr, "decode fail\n"); continue; }
        draw_latency_patch(codec.nv12, kW, kH);   // 延迟测量色块(WSL 时钟,画进帧里)
        draw_osd_time(codec.nv12, kW, kH);        // OSD 时间戳(采集时刻,随流进录像)

        uint32_t len = kW * kH * 3 / 2;
        uint8_t hdr[4] = {(uint8_t)(len & 0xFF), (uint8_t)(len >> 8), (uint8_t)(len >> 16), (uint8_t)(len >> 24)};
        if (send_all(sock, hdr, 4) < 0 || send_all(sock, codec.nv12, len) < 0) {
            perror("send"); break;
        }
        if (++frames % 15 == 0) printf("sent %lu frames\n", (unsigned long)frames);
    }
    close(sock); close(cam);
    return 0;
}
