// ============================================================================
// RTSP 服务器封装(live555):把 VENC 输出的 H.264 以标准 RTSP 发布
//   URL: rtsp://<board>:8554/live
// 设计:
//   - push(): 编码线程调用;annexb 拆成单 NAL 去头入共享队列,eventTrigger
//     唤醒 live555 事件循环(跨线程唯一合法交互);无观看端时队列有界防积压
//   - SPS/PPS 常驻缓存,每个 source 实例创建时重新注入(客户端任意时刻接入
//     都能拿到参数集,SDP sprop 完整)
//   - H264VideoStreamDiscreteFramer + H264VideoRTPSink(FU-A),
//     reuseFirstSource=True 单源服务多观看端
// ============================================================================
#pragma once
#include <cstdint>
#include <string>
#include <atomic>
#include <pthread.h>

class RtspServer {
public:
    bool start(int port, const std::string &path);   // false = 端口占用等
    void stop();
    void push(const uint8_t *data, uint32_t len);    // 任意线程;annexb(带起始码)
    bool running() const { return m_running.load(); }

private:
    static void *threadEntry(void *arg);
    void *loop(int port, const std::string &path);
    std::atomic<bool> m_running{false};
    int m_port = 8554;
    std::string m_path = "live";
    pthread_t m_tid{};
};
