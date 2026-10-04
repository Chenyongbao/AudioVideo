// ============================================================================
// live555 RTSP 服务器实现
// 跨线程模型:编码线程 push() 只入共享 NAL 队列 + triggerEvent;
//            live555 事件循环线程内 FramedSource 消费队列。
// 客户端任意时刻接入:SPS/PPS 常驻缓存 → 手工拼 SDP sprop-parameter-sets,
//            且新 source 创建时重灌参数集到队首(双保险)。
// 防积压:无观看端/慢客户端时队列超限丢弃旧非参数 NAL(实时流,延迟优先)。
// ============================================================================
#include "rtsp_server.hpp"

#include <liveMedia.hh>
#include <BasicUsageEnvironment.hh>
#include <GroupsockHelper.hh>

#include <deque>
#include <vector>
#include <mutex>
#include <cstring>
#include <cstdio>
#include <sys/time.h>

// ---------------- 共享 NAL 队列(编码线程 <-> 事件循环线程) ----------------
namespace {

std::mutex g_qlock;
std::deque<std::vector<uint8_t>> g_nals;      // 单 NAL,无起始码
std::vector<uint8_t> g_sps, g_pps;            // 参数集缓存(SDP/重灌用)
UsageEnvironment *g_env = nullptr;
EventTriggerId g_trigger = 0;
char g_watch = 0;

const size_t kQueueMax = 512;

bool isParamNal(const std::vector<uint8_t> &v) {
    if (v.empty()) return false;
    uint8_t t = v[0] & 0x1F;
    return t == 7 || t == 8;
}

std::string b64(const uint8_t *p, size_t n) {
    static const char *T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    for (; i + 2 < n; i += 3) {
        uint32_t v = (p[i] << 16) | (p[i+1] << 8) | p[i+2];
        out += T[(v >> 18) & 63]; out += T[(v >> 12) & 63];
        out += T[(v >> 6) & 63];  out += T[v & 63];
    }
    if (i + 1 == n) {
        uint32_t v = p[i] << 16;
        out += T[(v >> 18) & 63]; out += T[(v >> 12) & 63]; out += "==";
    } else if (i + 2 == n) {
        uint32_t v = (p[i] << 16) | (p[i+1] << 8);
        out += T[(v >> 18) & 63]; out += T[(v >> 12) & 63];
        out += T[(v >> 6) & 63];  out += '=';
    }
    return out;
}

// ---------------- 自定义 FramedSource:从共享队列取 NAL ----------------
class H264LiveSource : public FramedSource {
public:
    static H264LiveSource *createNew(UsageEnvironment &env) {
        return new H264LiveSource(env);
    }
    H264LiveSource(UsageEnvironment &env) : FramedSource(env) { g_src = this; }
    ~H264LiveSource() override { if (g_src == this) g_src = nullptr; }

    static H264LiveSource *g_src;   // 事件循环线程内访问(单实例:reuseFirstSource)

    // 事件循环线程调用:队列有数据且在等数据 → 交付一 NAL
    void pullIfWaiting() {
        if (!isCurrentlyAwaitingData()) return;
        std::vector<uint8_t> nal;
        { std::lock_guard<std::mutex> lk(g_qlock);
          if (g_nals.empty()) return;
          nal.swap(g_nals.front()); g_nals.pop_front(); }
        deliverNal(nal);
    }

    void doGetNextFrame() override { pullIfWaiting(); }   // 有数据立即交付,否则等 trigger

private:
    void deliverNal(const std::vector<uint8_t> &nal) {
        if (nal.size() > (size_t)fMaxSize) {              // 超出帧缓冲:截断(实测不会发生)
            fFrameSize = fMaxSize;
            fNumTruncatedBytes = (unsigned)(nal.size() - fMaxSize);
        } else {
            fFrameSize = (unsigned)nal.size();
            fNumTruncatedBytes = 0;
        }
        memcpy(fTo, nal.data(), fFrameSize);
        // gettimeofday 与 groupsock 头的重载有歧义,用 clock_gettime 等价填充
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        fPresentationTime.tv_sec = ts.tv_sec;
        fPresentationTime.tv_usec = ts.tv_nsec / 1000;
        fDurationInMicroseconds = 1000000 / 15;           // 15fps
        FramedSource::afterGetting(this);
    }
};
H264LiveSource *H264LiveSource::g_src = nullptr;

void onEventTrigger(void *) {
    if (H264LiveSource::g_src)
        H264LiveSource::g_src->pullIfWaiting();
}

// ---------------- OnDemandServerMediaSubsession ----------------
class H264LiveSubsession : public OnDemandServerMediaSubsession {
public:
    H264LiveSubsession(UsageEnvironment &env)
        : OnDemandServerMediaSubsession(env, True) {}   // 分辨率/帧率由 sprop 行与流本身表达

    FramedSource *createNewStreamSource(unsigned, unsigned &estBitrate) override {
        estBitrate = 4000;
        // 新客户端接入:把缓存的参数集重灌队首,framer 立即拿到 SPS/PPS
        { std::lock_guard<std::mutex> lk(g_qlock);
          if (!g_sps.empty()) g_nals.push_front(g_sps);
          if (!g_pps.empty()) g_nals.push_front(g_pps); }
        H264LiveSource::g_src = H264LiveSource::createNew(envir());
        return H264VideoStreamDiscreteFramer::createNew(envir(), H264LiveSource::g_src);
    }

    RTPSink *createNewRTPSink(Groupsock *gsock, unsigned char, FramedSource *) override {
        return H264VideoRTPSink::createNew(envir(), gsock, 96);
    }

    // 手工 SDP:保证 DESCRIBE 时刻 sprop 就绪(不等首帧解析)
    char const *getAuxSDPLine(RTPSink *sink, FramedSource *) override {
        std::vector<uint8_t> sps, pps;
        { std::lock_guard<std::mutex> lk(g_qlock); sps = g_sps; pps = g_pps; }
        if (sps.empty() || pps.empty()) return sink->auxSDPLine();
        static std::string line;
        char prof[8];
        snprintf(prof, sizeof(prof), "%02X%02X%02X",
                 sps.size() > 2 ? sps[1] : 66, sps.size() > 2 ? sps[2] : 0, sps.size() > 3 ? sps[3] : 64);
        line = "a=fmtp:96 packetization-mode=1;profile-level-id=";
        line += prof;
        line += ";sprop-parameter-sets=";
        line += b64(sps.data(), sps.size());
        line += ",";
        line += b64(pps.data(), pps.size());
        return line.c_str();
    }
};

} // namespace

// ---------------- 对外接口 ----------------
bool RtspServer::start(int port, const std::string &path) {
    if (m_running.load()) return true;
    m_port = port; m_path = path;
    if (pthread_create(&m_tid, nullptr, threadEntry, this) != 0) return false;
    // 等事件循环线程就绪(简单轮询)
    for (int i = 0; i < 50 && !m_running.load(); i++)
        usleep(100 * 1000);
    return m_running.load();
}

void RtspServer::stop() {
    if (!m_running.load()) return;
    g_watch = 1;                    // doEventLoop 退出
    pthread_join(m_tid, nullptr);
    m_running.store(false);
}

// 编码线程调用:annexb → 拆单 NAL(去起始码)入队
// 拆分逻辑:找到每个 00 00 01 起始码,NAL = 相邻两码之间(4 字节码的
// 前导 00 归还给边界,即 NAL 末尾若恰是下一码的前导 0 则去掉一个)
void RtspServer::push(const uint8_t *data, uint32_t len) {
    size_t i = 0;
    while (i + 2 < len && !(data[i] == 0 && data[i+1] == 0 && data[i+2] == 1)) i++;
    if (i + 2 >= len) return;               // 无起始码
    size_t nalStart = i + 3;

    {
        std::lock_guard<std::mutex> lk(g_qlock);
        auto emitNal = [&](size_t s, size_t e) {
            if (e <= s) return;
            std::vector<uint8_t> nal(data + s, data + e);
            uint8_t t = nal[0] & 0x1F;
            if (t == 7) g_sps = nal;
            else if (t == 8) g_pps = nal;
            g_nals.push_back(std::move(nal));
        };
        for (size_t j = nalStart; j + 2 < len; ) {
            if (data[j] == 0 && data[j+1] == 0 && data[j+2] == 1) {
                size_t e = j;
                if (e > nalStart && data[e-1] == 0) e--;   // 4 字节码前导 00
                emitNal(nalStart, e);
                nalStart = j + 3;
                j = nalStart;
            } else {
                j++;
            }
        }
        emitNal(nalStart, len);             // 末尾 NAL 直到包尾
        // 防积压:超限丢弃最旧的非参数 NAL(慢客户端/无观看端)
        while (g_nals.size() > kQueueMax) {
            if (isParamNal(g_nals.front())) { g_nals.pop_front(); continue; }
            bool dropped = false;
            for (auto it = g_nals.begin(); it != g_nals.end(); ++it)
                if (!isParamNal(*it)) { g_nals.erase(it); dropped = true; break; }
            if (!dropped) g_nals.pop_front();
        }
    }
    if (g_env && g_trigger)
        g_env->taskScheduler().triggerEvent(g_trigger, nullptr);
}

void *RtspServer::threadEntry(void *arg) {
    RtspServer *self = (RtspServer *)arg;
    self->loop(self->m_port, self->m_path);
    return nullptr;
}

void *RtspServer::loop(int port, const std::string &path) {
    TaskScheduler *sched = BasicTaskScheduler::createNew();
    UsageEnvironment *env = BasicUsageEnvironment::createNew(*sched);
    RTSPServer *rtsp = RTSPServer::createNew(*env, port, nullptr);
    if (!rtsp) {
        fprintf(stderr, "[rtsp] create failed (port %d busy?)\n", port);
        env->reclaim(); delete sched;
        return nullptr;
    }
    g_env = env;
    g_watch = 0;
    g_trigger = env->taskScheduler().createEventTrigger(onEventTrigger);

    ServerMediaSession *sms = ServerMediaSession::createNew(*env, path.c_str(),
                                                            "bodycam", "H.264 live from RV1126 VENC");
    sms->addSubsession(new H264LiveSubsession(*env));
    rtsp->addServerMediaSession(sms);
    {
        char *url = rtsp->rtspURL(sms);       // 返回 new[] 分配,用完释放
        fprintf(stderr, "[rtsp] serving %s\n", url);
        delete[] url;
    }

    m_running.store(true);
    env->taskScheduler().doEventLoop(&g_watch);

    // 收尾(本线程内销毁全部 live555 对象)
    Medium::close(rtsp);
    if (g_trigger) env->taskScheduler().deleteEventTrigger(g_trigger);
    g_trigger = 0;
    g_env = nullptr;
    env->reclaim();
    delete sched;
    fprintf(stderr, "[rtsp] stopped\n");
    return nullptr;
}
