// ============================================================================
// ONVIF Profile S 最小实现(手写 SOAP,零第三方依赖)
//   发现层:WS-Discovery UDP 3702(NVR 组播 Probe → 我们回 ProbeMatch)
//   服务层:HTTP 8899 极简 SOAP 四动作(固定 XML 模板 + 命名空间,兼容主流 NVR)
// 已知简化:未实现 WS-UsernameToken 签名鉴权(无鉴权模式,多数 NVR 可配)、
//           不解析 SOAP 请求体(按动作名子串匹配,够 Profile S 拉流用)
// ============================================================================
#include "onvif.hpp"

#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

// SOAP 响应模板:公共信封(命名空间照抄规范,NVR 的 XML 解析器逐字段对)
static const char *kSoapHead =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: application/soap+xml; charset=utf-8\r\n"
    "Content-Length: %d\r\n"
    "Connection: close\r\n\r\n";

static const char *kSoapEnvBegin =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
    "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" "
    "xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\" "
    "xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\" "
    "xmlns:tt=\"http://www.onvif.org/ver10/schema\">"
    "<s:Body>";

static const char *kSoapEnvEnd = "</s:Body></s:Envelope>";

static std::string soapWrap(const std::string &body) {
    std::string xml = std::string(kSoapEnvBegin) + body + kSoapEnvEnd;
    char head[128];
    snprintf(head, sizeof(head), kSoapHead, (int)xml.size());
    return std::string(head) + xml;
}

bool OnvifServer::start(int httpPort, const std::string &rtspUri) {
    if (running_.load()) return true;
    httpPort_ = httpPort;
    rtspUri_ = rtspUri;
    running_.store(true);
    thDisc_ = std::thread(&OnvifServer::discoveryLoop, this);
    thHttp_ = std::thread(&OnvifServer::httpLoop, this);
    return true;
}

void OnvifServer::stop() {
    if (!running_.exchange(false)) return;   // 已停过就直接返回(exchange 原子读旧值)
    running_.store(false);
    if (httpFd_ >= 0) { ::shutdown(httpFd_, SHUT_RDWR); ::close(httpFd_); httpFd_ = -1; }
    if (thDisc_.joinable()) thDisc_.join();
    if (thHttp_.joinable()) thHttp_.join();
}

// ---- WS-Discovery:回应 Probe(组播 239.255.255.250:3702) ----
void OnvifServer::discoveryLoop() {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return;
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(3702);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(fd, (sockaddr *)&addr, sizeof(addr)) < 0) { close(fd); return; }

    // 加入组播组
    ip_mreq mreq{};
    mreq.imr_multiaddr.s_addr = inet_addr("239.255.255.250");
    mreq.imr_interface.s_addr = INADDR_ANY;
    setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));

    char buf[4096];
    while (running_.load()) {
        sockaddr_in from{};
        socklen_t fl = sizeof(from);
        ssize_t n = recvfrom(fd, buf, sizeof(buf) - 1, 0, (sockaddr *)&from, &fl);
        if (n <= 0) break;
        buf[n] = 0;
        if (!strstr(buf, "Probe") || !strstr(buf, "NetworkVideoTransmitter"))
            continue;                                   // 只回应摄像头类探测
        // 取 Probe 里的 MessageID 原样回(WS-Discovery 要求 relates to)
        char msgId[128] = "urn:uuid:0";
        const char *p = strstr(buf, "<a:MessageID>");
        if (p) { p += 13; const char *e = strchr(p, '<'); size_t L = e ? e - p : 0;
                 if (L > 0 && L < sizeof(msgId)) { memcpy(msgId, p, L); msgId[L] = 0; } }

        // 本机 IP:从 from 对端路由推断不可靠,直接用 UDP connect 技巧拿出口 IP
        int tfd = socket(AF_INET, SOCK_DGRAM, 0);
        char ip[32] = "0.0.0.0";
        if (tfd >= 0) {
            sockaddr_in ra{}; ra.sin_family = AF_INET; ra.sin_port = htons(80);
            ra.sin_addr = from.sin_addr;
            if (connect(tfd, (sockaddr *)&ra, sizeof(ra)) == 0) {
                sockaddr_in la{}; socklen_t ll = sizeof(la);
                getsockname(tfd, (sockaddr *)&la, &ll);
                inet_ntop(AF_INET, &la.sin_addr, ip, sizeof(ip));
            }
            close(tfd);
        }

        char body[1600];
        snprintf(body, sizeof(body),
            "<d:ProbeMatches xmlns:d=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\" "
            "xmlns:a=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\">"
            "<a:MessageID>%s</a:MessageID>"
            "<a:RelatesTo>%s</a:RelatesTo>"
            "<d:ProbeMatch><a:EndpointReference><a:Address>urn:uuid:11223344-5566-7788-99aa-bbccddeeff00</a:Address>"
            "</a:EndpointReference><d:Types>tdn:NetworkVideoTransmitter</d:Types>"
            "<d:XAddrs>http://%s:%d/onvif/device_service</d:XAddrs>"
            "<d:Scopes>onvif://www.onvif.org/Profile/Streaming onvif://www.onvif.org/name/BodyWornCam</d:Scopes>"
            "</d:ProbeMatch></d:ProbeMatches>",
            msgId, msgId, ip, httpPort_);
        std::string resp = soapWrap(body);
        sendto(fd, resp.data(), resp.size(), 0, (sockaddr *)&from, fl);
    }
    close(fd);
}

// ---- HTTP 8899:极简 SOAP 服务 ----
static std::string onvifActionResponse(const char *req, const std::string &rtspUri) {
    // 按 SOAP 动作名子串匹配(不解析完整 XML,Profile S 核心场景够用)
    if (strstr(req, "GetDeviceInformation")) {
        return soapWrap(
            "<tds:GetDeviceInformationResponse>"
            "<tds:Manufacturer>AtomBodyWorn</tds:Manufacturer>"
            "<tds:Model>RV1126-BWC</tds:Model>"
            "<tds:FirmwareVersion>1.0</tds:FirmwareVersion>"
            "<tds:SerialNumber>BWC-RV1126-0001</tds:SerialNumber>"
            "</tds:GetDeviceInformationResponse>");
    }
    if (strstr(req, "GetCapabilities")) {
        return soapWrap(
            "<tds:GetCapabilitiesResponse><tds:Capabilities>"
            "<tt:Media><tt:XAddr>http://MEDIA_XADDR_PLACEHOLDER/onvif/media</tt:XAddr>"
            "<tt:StreamingCapabilities><tt:RTPMulticast>false</tt:RTPMulticast>"
            "<tt:RTP_TCP>true</tt:RTP_TCP><tt:RTP_UDP>true</tt:RTP_UDP>"
            "</tt:StreamingCapabilities></tt:Media>"
            "</tds:Capabilities></tds:GetCapabilitiesResponse>");
    }
    if (strstr(req, "GetProfiles")) {
        return soapWrap(
            "<trt:GetProfilesResponse><trt:Profiles token=\"profile_1\" fixed=\"true\">"
            "<tt:Name>H264_640x480</tt:Name>"
            "<tt:VideoEncoderConfiguration>"
            "<tt:Name>H264Main</tt:Name><tt:UseCount>1</tt:UseCount>"
            "<tt:Encoding>H264</tt:Encoding>"
            "<tt:Resolution><tt:Width>640</tt:Width><tt:Height>480</tt:Height></tt:Resolution>"
            "<tt:RateControl><tt:FrameRateLimit>15</tt:FrameRateLimit>"
            "<tt:BitrateLimit>2048</tt:BitrateLimit></tt:RateControl>"
            "</tt:VideoEncoderConfiguration>"
            "</trt:Profiles></trt:GetProfilesResponse>");
    }
    if (strstr(req, "GetStreamUri")) {
        return soapWrap(
            "<trt:GetStreamUriResponse><trt:MediaUri>"
            "<tt:Uri>" + rtspUri + "</tt:Uri>"
            "<tt:InvalidAfterConnect>false</tt:InvalidAfterConnect>"
            "<tt:InvalidAfterReboot>false</tt:InvalidAfterReboot>"
            "<tt:Timeout>PT60S</tt:Timeout>"
            "</trt:MediaUri></trt:GetStreamUriResponse>");
    }
    // 未知动作:规范 Fault(不下线连接,NVR 会降级尝试其他动作)
    return soapWrap(
        "<s:Fault><s:Code><s:Value>s:Sender</s:Value></s:Code>"
        "<s:Reason><s:Text xml:lang=\"en\">ActionNotSupported</s:Text></s:Reason></s:Fault>");
}

void OnvifServer::httpLoop() {
    httpFd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (httpFd_ < 0) return;
    int yes = 1;
    setsockopt(httpFd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(httpPort_);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(httpFd_, (sockaddr *)&addr, sizeof(addr)) < 0 || listen(httpFd_, 4) < 0) {
        close(httpFd_); httpFd_ = -1; return;
    }
    while (running_.load()) {
        sockaddr_in from{};
        socklen_t fl = sizeof(from);
        int c = accept(httpFd_, (sockaddr *)&from, &fl);
        if (c < 0) break;
        char buf[8192];
        ssize_t n = recv(c, buf, sizeof(buf) - 1, 0);
        if (n > 0) {
            buf[n] = 0;
            std::string resp = onvifActionResponse(buf, rtspUri_);
            send(c, resp.data(), resp.size(), 0);
        }
        close(c);
    }
    if (httpFd_ >= 0) { close(httpFd_); httpFd_ = -1; }
}
