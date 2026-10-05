// ============================================================================
// 全链路板端主程序:收 NV12 裸帧(8888,WSL 侧已解码) → RGA 拷贝到 VENC 输入 → 推流(9999)
// 端口与 relay 版一致,客户端 URL 不变(FFmpeg 自动探测 H.264 annexb 裸流)。
// 流程: 主线程收帧 → RGA imcopy(NV12→VENC 输入 buffer) → VENC CBR 4Mbps @15fps → 观看端
// 说明: 解码全部在 WSL 侧完成(板端 VDEC 解 422 JPEG 色度歧义、板端 libavcodec 解码丢色,
//      WSL 驱动 YUYV corrupted,均实测)——板端只做 RGA 搬运 + 编码,链路最短、格式无歧义。
// 用法: ./h264_pipeline [in_port=8888] [out_port=9999]
// ============================================================================
#include "mpp_h264_encoder.hpp"
#include "recorder.hpp"
#include "rtsp_server.hpp"
#include "hashchain.hpp"
#include "yolo_detector.hpp"
#include "onvif.hpp"
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include <csignal>

// ---- 优雅退出:信号处理器只置位,清理在 main 收尾统一做 ----
static volatile sig_atomic_t g_stop = 0;
static void onSignal(int) { g_stop = 1; }
#include <sys/statvfs.h>
#include <dirent.h>
#include <sys/stat.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <poll.h>
#include <stdio.h>
#include <vector>
#include <deque>
#include <thread>
#include <mutex>
#include <condition_variable>
#include "rga.h"
#include "im2d.h"

#define MAX_VIEWERS 8

static int send_all(int sock, const void *buf, size_t len);   // 前置声明(定义在下方)

// 每个观看端独立发送线程+有界队列:慢客户端只阻塞自己,绝不拖住处理管线
struct Viewer {
    int fd = -1;
    std::deque<std::vector<uint8_t>> q;   // 待发 H.264 包
    std::mutex m;
    std::condition_variable cv;
    bool dead = false;   // 发送失败/队列溢出,待主线程回收
    bool quit = false;
    std::thread th;
};
static Viewer *g_viewers[MAX_VIEWERS];   // nullptr = 空位(静态零初始化)
static std::mutex g_vlock;               // 保护 g_viewers[] 与 g_hdr
static uint8_t g_hdr[256];               // SPS/PPS 缓存,新观看端接入时先补发
static uint32_t g_hdr_len = 0;
static uint64_t g_frames = 0, g_bytes = 0;

// ---- 录像:预录环形缓冲 + fMP4 断电安全写盘 ----
static Recorder g_rec;
static RtspServer g_rtsp;   // 标准 RTSP 发布(8554/live),替代裸 TCP 9999 的对外段

// ---- NPU 人/车检测(5fps 抽帧,不阻塞编码链)----
static YoloDetector g_yolo;
// ---- ONVIF Profile S(WS-Discovery 3702 + SOAP 8899,对外扮标准 IPC)----
static OnvifServer g_onvif;
static std::mutex g_detLock;                 // 保护 g_lastDets(7778 推送线程读)
static std::vector<DetBox> g_lastDets;       // 最新一帧检测结果(源帧坐标)
static uint64_t g_lastDetTs = 0;
static uint64_t g_lastMarkMs = 0;            // 自动打点冷却(同类 10s)
static uint64_t nowMs() {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// 自动打点阈值(默认 0.5;YOLO_MARK_TH 环境变量可覆盖,测试/调试用)
static float markTh() {
    static float th = -1;
    if (th < 0) {
        const char *e = getenv("YOLO_MARK_TH");
        th = (e && atof(e) > 0) ? atof(e) : 0.5f;
    }
    return th;
}

// 检测线程上下文回调:缓存最新框(7778 推送用)+ 人/车自动打点(10s 冷却)
static void onDetections(const std::vector<DetBox> &dets, uint64_t ts) {
    bool interesting = false;
    float bestProp = 0;
    const char *bestName = "";
    for (const auto &d : dets) {
        if (!strcmp(d.name, "person") || !strcmp(d.name, "car") || !strcmp(d.name, "truck") ||
            !strcmp(d.name, "bus") || !strcmp(d.name, "motorcycle") || !strcmp(d.name, "bicycle")) {
            interesting = true;
            if (d.prop > bestProp) { bestProp = d.prop; bestName = d.name; }
        }
    }
    {
        std::lock_guard<std::mutex> lk(g_detLock);
        g_lastDets = dets;
        g_lastDetTs = ts;
    }
    if (interesting && bestProp >= markTh()) {
        uint64_t now = nowMs();
        if (now - g_lastMarkMs > 10000) {          // 同类事件 10s 冷却防刷屏
            g_lastMarkMs = now;
            char txt[64];
            snprintf(txt, sizeof(txt), "AUTO %s %.2f", bestName, bestProp);
            if (g_rec.mark(txt))                    // 落 .meta → 时间轴▲ → 证据链体系
                fprintf(stderr, "[yolo] auto mark: %s\n", txt);
        }
    }
}

// /userdata 所在分区剩余字节(df);获取失败返回 -1(不拦截)
static long long diskFreeBytes() {
    struct statvfs st;
    if (statvfs("/userdata", &st) < 0) return -1;
    return (long long)st.f_bavail * st.f_frsize;
}

// UDP 7777 控制通道:REC_START <w> <h> <fps> / REC_STOP / REC_STAT
static void *control_thread(void *) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(7777);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (sockaddr *)&a, sizeof(a)) < 0) { perror("control bind"); return nullptr; }
    char buf[256];
    while (true) {
        sockaddr_in from{}; socklen_t fl = sizeof(from);
        ssize_t n = recvfrom(s, buf, sizeof(buf) - 1, 0, (sockaddr *)&from, &fl);
        if (n <= 0) continue;
        buf[n] = 0;
        char resp[128];
        if (!strncmp(buf, "REC_START", 9)) {
            int w = 640, h = 480, fps = 15;
            sscanf(buf, "REC_START %d %d %d", &w, &h, &fps);
            // 空间保护:剩余 <50MB 拒录(4Mbps ≈ 30MB/min,50MB 只够 1 分多钟)
            long long free_b = diskFreeBytes();
            if (free_b >= 0 && free_b < 50LL * 1024 * 1024) {
                snprintf(resp, sizeof(resp), "ERR disk low %lld MB", free_b / 1024 / 1024);
                sendto(s, resp, strlen(resp), 0, (sockaddr *)&from, fl);
                continue;
            }
            char path[128];
            // 可信时间戳:TIME_SET 校准后文件名即真实时间;未校准(1970)回退单调 ms
            {
                time_t t = time(nullptr);
                if (t > 1600000000) {
                    struct tm tmv;
                    localtime_r(&t, &tmv);
                    snprintf(path, sizeof(path), "/userdata/rec_%04d%02d%02d_%02d%02d%02d.mp4",
                             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                             tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
                } else {
                    snprintf(path, sizeof(path), "/userdata/rec_%llu.mp4", (unsigned long long)nowMs());
                }
            }
            bool ok;
            {
                // SPS/PPS 缓存随 g_vlock 保护;可能尚未收到(观看端未接入过),为空则录出文件缺参数集
                std::lock_guard<std::mutex> lk(g_vlock);
                ok = g_rec.start(path, w, h, fps, g_hdr_len ? g_hdr : nullptr, g_hdr_len);
            }
            snprintf(resp, sizeof(resp), ok ? "OK %s" : "ERR %s", ok ? path : g_rec.lastError().c_str());
        } else if (!strncmp(buf, "REC_STOP", 8)) {
            uint64_t d = g_rec.recordedMs();
            std::string lastPath = g_rec.currentPath();
            g_rec.stop();
            // 证据链:录像停止即算 SHA-256 入链(任何后段篡改都会断链被发现)
            bool chained = false;
            if (!lastPath.empty()) chained = hashchain_append(lastPath);
            snprintf(resp, sizeof(resp), "OK stopped %llu ms%s",
                     (unsigned long long)d, chained ? " [chained]" : "");
        } else if (!strncmp(buf, "HASH_VERIFY", 11)) {
            // 证据链校验:逐段重算哈希 + 链式衔接检查,单包返回报告
            static std::string report;   // static 缓冲避免悬空(单线程控制通道,安全)
            report = hashchain_verify();
            sendto(s, report.data(), report.size(), 0, (sockaddr *)&from, fl);
            continue;
        } else if (!strncmp(buf, "DET_GET", 7)) {
            // NPU 检测框拉取(Qt ~5fps 轮询):JSON 数组,仅预览画框用(不进录像帧)
            static std::string detJson;
            detJson = "[";
            {
                std::lock_guard<std::mutex> lk(g_detLock);
                char one[128];
                for (size_t i = 0; i < g_lastDets.size(); i++) {
                    const DetBox &d = g_lastDets[i];
                    snprintf(one, sizeof(one), "%s{\"n\":\"%s\",\"p\":%.2f,\"b\":[%d,%d,%d,%d]}",
                             i ? "," : "", d.name, d.prop, d.x1, d.y1, d.x2, d.y2);
                    detJson += one;
                }
            }
            detJson += "]";
            sendto(s, detJson.data(), detJson.size(), 0, (sockaddr *)&from, fl);
            continue;
        } else if (!strncmp(buf, "TIME_SET", 8)) {
            // 客户端把自己的 epoch 秒发来校准板钟(板无 RTC,开机为 1970)
            // 之后录像文件名/MP4 creation_time 均为真实时间
            long long ep = 0;
            sscanf(buf, "TIME_SET %lld", &ep);
            if (ep > 1600000000) {   // 合理性:2020 之后
                struct timeval tv{(time_t)ep, 0};
                settimeofday(&tv, nullptr);
                snprintf(resp, sizeof(resp), "OK time %lld", ep);
            } else {
                snprintf(resp, sizeof(resp), "ERR bad epoch %lld", ep);
            }
        } else if (!strncmp(buf, "FILE_LIST", 9)) {
            // 列出 /userdata 下的录像:每行 "文件名 字节数",单包回复(录像文件量级 ~几十个)
            DIR *d = opendir("/userdata");
            if (!d) {
                snprintf(resp, sizeof(resp), "ERR opendir");
                sendto(s, resp, strlen(resp), 0, (sockaddr *)&from, fl);
                continue;
            }
            static char list[16384];
            size_t off = 0;
            struct dirent *e;
            while ((e = readdir(d)) && off < sizeof(list) - 64) {
                const char *nm = e->d_name;
                if (strncmp(nm, "rec_", 4) || strlen(nm) < 8 || strcmp(nm + strlen(nm) - 4, ".mp4"))
                    continue;
                struct stat st;
                char full[160];
                snprintf(full, sizeof(full), "/userdata/%s", nm);
                if (stat(full, &st) < 0) continue;
                off += (size_t)snprintf(list + off, sizeof(list) - off, "%s %lld\n",
                                        nm, (long long)st.st_size);
            }
            closedir(d);
            if (off == 0) off = (size_t)snprintf(list, sizeof(list), "(empty)\n");
            sendto(s, list, off, 0, (sockaddr *)&from, fl);
            continue;
        } else if (!strncmp(buf, "FILE_DEL ", 9)) {
            // 删除指定录像:严格校验文件名(只允许 rec_ 开头/.mp4 结尾/无路径分隔),防路径穿越
            const char *nm = buf + 9;
            bool safe = !strncmp(nm, "rec_", 4) && strlen(nm) > 8 &&
                        !strcmp(nm + strlen(nm) - 4, ".mp4") &&
                        !strchr(nm, '/') && !strchr(nm, '\\') && !strstr(nm, "..");
            if (!safe) {
                snprintf(resp, sizeof(resp), "ERR bad name");
            } else {
                char full[160];
                snprintf(full, sizeof(full), "/userdata/%s", nm);
                snprintf(resp, sizeof(resp), unlink(full) == 0 ? "OK deleted %s" : "ERR unlink %s", nm);
            }
        } else if (!strncmp(buf, "REC_MARK", 8)) {
            // 事件标记:录制中打点(正文为命令剩余部分,可含空格说明文字)
            const char *text = (n > 9) ? buf + 9 : "mark";
            snprintf(resp, sizeof(resp), g_rec.mark(text) ? "OK mark" : "ERR %s",
                     g_rec.lastError().c_str());
        } else if (!strncmp(buf, "REC_STAT", 8)) {
            snprintf(resp, sizeof(resp), "%s %llu ms", g_rec.recording() ? "REC" : "IDLE",
                     (unsigned long long)g_rec.recordedMs());
        } else {
            snprintf(resp, sizeof(resp), "ERR unknown");
        }
        sendto(s, resp, strlen(resp), 0, (sockaddr *)&from, fl);
    }
    return nullptr;
}

static const size_t VIEWER_QMAX = 60;    // 队列上限,超限视为慢客户端直接踢掉(内存有界)

static void viewer_thread(Viewer *v) {
    while (true) {
        std::vector<uint8_t> pkt;
        {
            std::unique_lock<std::mutex> lk(v->m);
            v->cv.wait(lk, [&] { return v->quit || !v->q.empty(); });
            if (v->quit && v->q.empty()) break;
            pkt = std::move(v->q.front());
            v->q.pop_front();
        }
        if (send_all(v->fd, pkt.data(), pkt.size()) < 0) break;
    }
    std::lock_guard<std::mutex> lk(g_vlock);
    v->dead = true;   // 主线程 reap_dead 回收
}

// 异步广播:拷进各观看端队列立即返回(每帧 ~20KB 拷贝远比阻塞 send 便宜)
static void broadcast(const uint8_t *p, uint32_t len) {
    std::lock_guard<std::mutex> lk(g_vlock);
    for (int i = 0; i < MAX_VIEWERS; i++) {
        Viewer *v = g_viewers[i];
        if (!v || v->dead) continue;
        std::lock_guard<std::mutex> vl(v->m);
        if (v->q.size() >= VIEWER_QMAX) { v->dead = true; continue; }   // 慢客户端踢掉
        v->q.emplace_back(p, p + len);
        v->cv.notify_one();
    }
}

static int recv_all(int sock, void *buf, size_t len) {
    uint8_t *p = (uint8_t *)buf;
    while (len > 0) {
        ssize_t n = recv(sock, p, len, 0);
        if (n == 0) return -1;
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        p += n; len -= n;
    }
    return 0;
}

static int send_all(int sock, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    while (len > 0) {
        ssize_t n = send(sock, p, len, MSG_NOSIGNAL);
        if (n <= 0) { if (n < 0 && errno == EINTR) continue; return -1; }
        p += n; len -= n;
    }
    return 0;
}

static void accept_viewer(int lsock) {
    std::lock_guard<std::mutex> lk(g_vlock);
    for (int i = 0; i < MAX_VIEWERS; i++) {
        if (g_viewers[i]) continue;   // 占用槽位(dead 由 reap 回收)
        int s = accept(lsock, nullptr, nullptr);
        if (s < 0) return;
        int nd = 1;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &nd, sizeof(nd));
        // 先补发 SPS/PPS,否则接入方错过 GOP 头会报 non-existing PPS
        if (g_hdr_len && send_all(s, g_hdr, g_hdr_len) < 0) { close(s); return; }
        Viewer *v = new Viewer();
        v->fd = s;
        v->th = std::thread(viewer_thread, v);
        g_viewers[i] = v;
        fprintf(stderr, "[viewer] connected slot %d (hdr %uB)\n", i, g_hdr_len);
        return;
    }
    int s = accept(lsock, nullptr, nullptr);
    if (s >= 0) close(s);   // 满员
}

// 回收死亡观看端(join 发送线程,释放槽位)
static void reap_dead() {
    std::lock_guard<std::mutex> lk(g_vlock);
    for (int i = 0; i < MAX_VIEWERS; i++) {
        Viewer *v = g_viewers[i];
        if (!v || !v->dead) continue;
        { std::lock_guard<std::mutex> vl(v->m); v->quit = true; }
        v->cv.notify_all();
        v->th.join();
        close(v->fd);
        delete v;
        g_viewers[i] = nullptr;
        fprintf(stderr, "[viewer] slot %d reaped\n", i);
    }
}

int main(int argc, char **argv) {
    // 优雅退出:Ctrl-C(SIGINT)/kill(SIGTERM/SIGQUIT)→ 置停机标志,
    // 阻塞调用靠不设 SA_RESTART 被打断,清理统一走 main 收尾
    struct sigaction sa{};
    sa.sa_handler = onSignal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;   // 不设 SA_RESTART:accept/recv 立即返回 EINTR,循环感知 g_stop
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGQUIT, &sa, nullptr);

    // 控制通道线程(UDP 7777:REC_START/STOP/MARK/STAT/FILE_*/TIME_SET)
    pthread_t ctl_tid;
    pthread_create(&ctl_tid, nullptr, control_thread, nullptr);

    // RTSP 服务器(8554/live):标准协议对外发布 VENC 输出
    if (!g_rtsp.start(8554, "live"))
        fprintf(stderr, "[rtsp] start failed, 只保留裸 TCP 9999\n");

    // NPU 检测线程(模型缺失则功能自动关闭,不影响编码主链)
    if (g_yolo.init("/userdata/yolov5s.rknn"))
        g_yolo.start(onDetections);
    else
        fprintf(stderr, "[yolo] disabled (model missing)\n");

    // ONVIF:对外扮标准 IPC(NVR/ODM 自动发现 → 拿 GetStreamUri → 拉 RTSP)
    g_onvif.start(8899, "rtsp://192.168.137.250:8554/live");
    int in_port = argc > 1 ? atoi(argv[1]) : 8888;
    int out_port = argc > 2 ? atoi(argv[2]) : 9999;
    // g_viewers[] 静态零初始化(nullptr=空位)

    int reuse = 1;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = INADDR_ANY;

    // ---- 观看端监听(先开:观看端可先于发送端接入,连接不再报错) ----
    int lout = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(lout, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    a.sin_port = htons(out_port);
    if (bind(lout, (sockaddr *)&a, sizeof(a)) < 0 || listen(lout, MAX_VIEWERS) < 0) {
        perror("viewer bind/listen"); return 1;
    }
    fprintf(stderr, "serving h264 on %d...\n", out_port);

    // ---- 收流监听 ----
    int lin = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(lin, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    a.sin_port = htons(in_port);
    if (bind(lin, (sockaddr *)&a, sizeof(a)) < 0 || listen(lin, 1) < 0) {
        perror("input bind/listen"); return 1;
    }

    static uint8_t buf[4 * 1024 * 1024];
    struct pollfd pfd{};
    pfd.fd = lout;
    pfd.events = POLLIN;

    while (!g_stop) {   // 会话循环:发送端断开不退出,等下一个接入(观看端保持连接);Ctrl-C 优雅退出
        fprintf(stderr, "waiting sender on %d...\n", in_port);
        int in_sock = accept(lin, nullptr, nullptr);
        if (g_stop) break;                     // 信号打断 accept:直接退出
        if (in_sock < 0) { perror("accept sender"); usleep(500000); continue; }
        fprintf(stderr, "[sender] connected\n");

        // ---- 处理链(每会话新建,VENC 参数在首帧确定) ----
        MppH264Encoder enc;
        bool enc_ready = false;
        int w = 0, h = 0;
        uint8_t hdr[4];

        while (!g_stop) {
        if (recv_all(in_sock, hdr, 4) < 0) { fprintf(stderr, "[sender] closed\n"); break; }
        uint32_t len = hdr[0] | (hdr[1] << 8) | (hdr[2] << 16) | ((uint32_t)hdr[3] << 24);
        if (len == 0 || len > sizeof(buf)) break;
        if (recv_all(in_sock, buf, len) < 0) break;
        if (len != 614400 && len != 460800) { fprintf(stderr, "[in] bad nv12 len %u\n", len); break; }
        g_frames++;

        if (!enc_ready) {
            w = 640; h = 480;
            // VENC:CBR 4Mbps @15fps(与 YUYV 采集帧率对齐,避免编码节奏漂移)
            if (!enc.open(w, h, 15, 4000000)) {
                fprintf(stderr, "[venc] open failed: %s\n", enc.lastError().c_str());
                break;
            }
            enc_ready = true;
            fprintf(stderr, "[pipe] %dx%d pipeline ready (yuyv→nv12 @15fps)\n", w, h);
        }

        // NPU 检测喂帧(latest-wins 槽,非阻塞,不拖慢编码链)
        g_yolo.submitFrame(buf, w, h, nowMs());

        // ① RGA:收到的 NV12(虚拟地址)→ VENC 输入 fd,硬件搬运
        // 注意 wrapbuffer_fd 宏参数序:(fd, w, h, format, wstride, hstride)
        rga_buffer_t src = wrapbuffer_virtualaddr(buf, w, h, RK_FORMAT_YCbCr_420_SP, w, h);
        rga_buffer_t dst = wrapbuffer_fd(enc.inputFd(), w, h, RK_FORMAT_YCbCr_420_SP);
        IM_STATUS st = imcopy(src, dst, 1);
        if ((int)st < 1) {
            fprintf(stderr, "[rga] imcopy: %s\n", imStrError(st));
            break;
        }

        // 诊断 dump(仅首帧一次):RGA 写入后的 VENC 输入,验证 YUYV→NV12 正确
        if (g_frames == 1) {
            FILE *fd2 = fopen("/tmp/venc_in.raw", "wb");
            if (fd2) { fwrite(mpp_buffer_get_ptr(enc.inputBuffer()), 1, (size_t)w * h * 3 / 2, fd2); fclose(fd2); }
            fprintf(stderr, "[dump] venc_in written\n");
        }

        // ② VENC:直接编码 inputBuffer(RGA 已写入),广播给观看端;首包(SPS/PPS)缓存供新观看端补发
        enc.encodeDirect(
                          [&](const uint8_t *p, uint32_t plen, bool key) {
                              {
                                  std::lock_guard<std::mutex> lk(g_vlock);
                                  if (key && plen < sizeof(g_hdr) && g_hdr_len == 0) {
                                      memcpy(g_hdr, p, plen);
                                      g_hdr_len = plen;
                                  }
                              }
                              g_bytes += plen;
                              g_rtsp.push(p, plen);   // RTSP:标准协议对外(含 SPS/PPS 与视频 NAL,server 内拆分)
                              // SPS/PPS 头包(~38B)与首帧同 ts,喂进 recorder 会造成 DTS 重复,过滤
                              if (plen > 1000)
                                  g_rec.feed(p, plen, key, nowMs());   // 录像:实时包喂 recorder(录制中写盘/空闲入预录缓冲)
                              broadcast(p, plen);   // 异步:入队即返回,慢客户端不阻塞
                              return true;
                          });

        // 非阻塞接受新观看端 + 回收死亡观看端
        fcntl(lout, F_SETFL, O_NONBLOCK);
        if (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN))
            accept_viewer(lout);
        fcntl(lout, F_SETFL, 0);
        reap_dead();

        // 空间保护:录制中每秒查一次,剩余 <20MB 自动停录(防写满根分区)
        if (g_rec.recording() && (g_frames % 15) == 0) {
            long long fb = diskFreeBytes();
            if (fb >= 0 && fb < 20LL * 1024 * 1024) {
                fprintf(stderr, "[rec] disk low %lld MB, auto stop\n", fb / 1024 / 1024);
                g_rec.stop();
            }
        }

        if (g_frames % 100 == 0)
            fprintf(stderr, "[%lu] %.1f fps cum, %.1f KB out\n",
                    (unsigned long)g_frames, 0.0, g_bytes / 1024.0);
        }   // 帧循环结束(发送端断开/协议错)

        close(in_sock);
        fprintf(stderr, "sender left, total %lu frames, %.1f KB h264\n",
                (unsigned long)g_frames, g_bytes / 1024.0);
    }   // 会话循环:回到等下一个发送端(观看端保持连接)

    // 优雅退出收尾(正常走完或 Ctrl-C/kill 到此):资源全部释放,可立即重启
    fprintf(stderr, "[exit] shutting down...\n");
    if (g_rec.recording()) g_rec.stop();   // 录制中:封口 fMP4 + 入哈希链
    g_onvif.stop();   // 关 ONVIF(发现+SOAP)线程
    g_rtsp.stop();    // 关 RTSP 服务器(live555 事件循环退出,释放 8554)
    g_yolo.stop();    // 停 NPU 检测线程(join + rknn_destroy)
    {
        std::lock_guard<std::mutex> lk(g_vlock);
        for (int i = 0; i < MAX_VIEWERS; i++) {
            Viewer *v = g_viewers[i];
            if (!v) continue;
            std::lock_guard<std::mutex> vl(v->m);
            v->quit = true;
            v->cv.notify_all();
        }
    }
    for (int i = 0; i < MAX_VIEWERS; i++) {
        Viewer *v = g_viewers[i];
        if (!v) continue;
        if (v->th.joinable()) v->th.join();
        close(v->fd);
        delete v;
    }
    close(lin); close(lout);
    return 0;
}
