#ifndef ONVIF_HPP
#define ONVIF_HPP

// ============================================================================
// ONVIF Profile S 最小实现(零第三方依赖):
//   - WS-Discovery:监听 UDP 3702 组播,应答 Probe(设备类型 NetworkVideoTransmitter)
//   - HTTP 8899:手写 SOAP 响应,四个必选动作
//       GetDeviceInformation / GetCapabilities / GetProfiles / GetStreamUri
//   - GetStreamUri 返回外部传入的 RTSP 地址(即 live555 的 rtsp://ip:8554/live)
// ONVIF 只是"设备管理层":NVR 靠它发现设备、拿到流地址,视频流本身仍走 RTSP。
// ============================================================================

#include <string>
#include <atomic>
#include <thread>

class OnvifServer {
public:
    // rtspUri 例: rtsp://192.168.137.250:8554/live(NVR 拿到后直接拉流)
    bool start(int httpPort, const std::string &rtspUri);
    void stop();
    bool running() const { return running_.load(); }

private:
    void discoveryLoop();   // WS-Discovery 应答线程
    void httpLoop();        // HTTP/SOAP 服务线程

    std::string rtspUri_;
    int httpPort_ = 8899;
    std::atomic<bool> running_{false};
    std::thread thDisc_, thHttp_;
    int httpFd_ = -1;       // stop() 时关闭以唤醒 accept
};

#endif // ONVIF_HPP
