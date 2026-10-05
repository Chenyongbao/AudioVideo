#include "streamworker.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/time.h>
#include <libswscale/swscale.h>
}

#include <QDateTime>
#include <cstdio>

namespace {
// RGB 转换目标：QImage::Format_RGB888 对应 AV_PIX_FMT_RGB24
constexpr AVPixelFormat kDstFmt = AV_PIX_FMT_RGB24;
}

StreamWorker::StreamWorker(QObject *parent) : QObject(parent) {}

StreamWorker::~StreamWorker()
{
    stop();
}

void StreamWorker::start(const QString &url, int reconnectMs)
{
    if (m_running.load())
        return;
    m_url = url;
    m_reconnectMs = reconnectMs;
    m_running.store(true);
    emit stateChanged(QStringLiteral("连接中..."));
    m_thread = std::thread(&StreamWorker::decodeLoop, this);
}

void StreamWorker::stop()
{
    if (!m_running.exchange(false))
        return;
    if (m_thread.joinable())
        m_thread.join();     // 循环内超时读，最多 ~1s 后退出
    emit stateChanged(QStringLiteral("已停止"));
}

// ---------- 打开流：avformat + avcodec ----------
bool StreamWorker::openStream()
{
    // RTSP 专选项：TCP 传输（避免 UDP 丢包花屏）+ 超时（微秒）
    // 非 RTSP（如裸 tcp:// mjpeg）不加这些选项，交给探测器自动识别
    AVDictionary *opts = nullptr;
    if (m_url.startsWith(QStringLiteral("rtsp://"), Qt::CaseInsensitive)) {
        av_dict_set(&opts, "rtsp_transport", "tcp", 0);
        av_dict_set(&opts, "stimeout", "5000000", 0);          // 5s socket 超时
    }
    av_dict_set(&opts, "max_delay", "500000", 0);          // 500ms 最大延迟
    av_dict_set(&opts, "buffer_size", "1024000", 0);
    // 低延迟拉流参数:缩短探测期,连接即出画面(默认 probesize 5MB 要攒很久)
    av_dict_set(&opts, "probesize", "32768", 0);           // 32k 足够探明裸 h264/mjpeg 流
    av_dict_set(&opts, "analyzeduration", "0", 0);         // 跳过冗余码流分析
    av_dict_set(&opts, "fflags", "nobuffer", 0);           // 去 demux 层缓冲
    av_dict_set(&opts, "flags", "low_delay", 0);           // 解复用低延迟标志

    m_fmt = avformat_alloc_context();
    m_fmt->interrupt_callback.callback = [](void *opaque) -> int {
        // av_read_frame 阻塞时每帧回调：stop() 置 running=false 即刻中断退出
        auto *w = static_cast<std::atomic<bool> *>(opaque);
        return w->load() ? 0 : 1;
    };
    m_fmt->interrupt_callback.opaque = &m_running;

    if (avformat_open_input(&m_fmt, m_url.toUtf8().constData(), nullptr, &opts) < 0) {
        av_dict_free(&opts);
        return false;
    }
    av_dict_free(&opts);

    if (avformat_find_stream_info(m_fmt, nullptr) < 0)
        return false;

    m_videoStream = av_find_best_stream(m_fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (m_videoStream < 0)
        return false;

    const AVStream *st = m_fmt->streams[m_videoStream];
    const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec)
        return false;

    m_dec = avcodec_alloc_context3(codec);
    if (!m_dec || avcodec_parameters_to_context(m_dec, st->codecpar) < 0)
        return false;
    m_dec->thread_count = 2;                    // 多路预览时限制单路解码线程
    m_dec->has_b_frames = 0;                    // 禁参考帧重排缓冲(裸流低延迟,配合 GOP 无 B 帧)
    if (avcodec_open2(m_dec, codec, nullptr) < 0)
        return false;

    m_sws = nullptr;   // 首帧时按实际分辨率创建
    return true;
}

void StreamWorker::closeStream()
{
    if (m_sws)   { sws_freeContext(m_sws); m_sws = nullptr; }
    if (m_dec)   { avcodec_free_context(&m_dec); m_dec = nullptr; }
    if (m_fmt)   { avformat_close_input(&m_fmt); m_fmt = nullptr; }
    m_videoStream = -1;
}

// ---------- 主循环：拉流 -> 解码 -> 转RGB -> 信号发UI ----------
void StreamWorker::decodeLoop()
{
    const AVPixFmtDescriptor *unused = nullptr;
    Q_UNUSED(unused);

    AVPacket *pkt = av_packet_alloc();   // FFmpeg 9：堆分配，无栈 av_init_packet
    AVFrame  *frm = av_frame_alloc();

    while (m_running.load()) {
        emit stateChanged(QStringLiteral("连接中..."));
        if (!openStream()) {
            emit stateChanged(QStringLiteral("连接失败，重连中..."));
            closeStream();
            // 可中断的等待（stop() 能即刻退出）
            for (int i = 0; m_running.load() && i < m_reconnectMs / 50; ++i)
                av_usleep(50 * 1000);
            continue;
        }
        emit stateChanged(QStringLiteral("在线"));

        // 帧率统计参考：源平均帧率（仅用于显示对比，不再用于节流——
        // 直播读端绝不能睡眠节流：消费速率一旦低于源速率，TCP 缓冲堆积，
        // 服务端就会 "reader is too slow" 丢帧，P 帧参考链断裂 → 花屏掩码错误）
        double srcFps = 30.0;
        if (m_fmt->streams[m_videoStream]->avg_frame_rate.den > 0)
            srcFps = av_q2d(m_fmt->streams[m_videoStream]->avg_frame_rate);
        Q_UNUSED(srcFps);

        std::uint64_t fpsCount = 0;
        qint64 fpsWindowStart = QDateTime::currentMSecsSinceEpoch();

        while (m_running.load()) {
            int ret = av_read_frame(m_fmt, pkt);
            if (ret < 0) {
                // 网络断/超时（interrupt_callback 也可能触发 AVERROR_EXIT）
                ++m_lost;
                emit statsUpdated(0.0, m_lost, 0);
                break;    // 跳外层重连
            }
            if (pkt->stream_index != m_videoStream) {
                av_packet_unref(pkt);
                continue;
            }

            ret = avcodec_send_packet(m_dec, pkt);
            av_packet_unref(pkt);
            if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_INVALIDDATA) {
                ++m_lost;
                continue;
            }

            while (m_running.load()) {
                ret = avcodec_receive_frame(m_dec, frm);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                    break;
                if (ret < 0) { ++m_lost; break; }

                // 首帧/分辨率变化：创建 sws
                if (!m_sws) {
                    m_sws = sws_getContext(frm->width, frm->height,
                                           (AVPixelFormat)frm->format,
                                           frm->width, frm->height, kDstFmt,
                                           SWS_BILINEAR, nullptr, nullptr, nullptr);
                    if (!m_sws) break;
                }

                // YUV -> RGB24 -> QImage
                QImage img(frm->width, frm->height, QImage::Format_RGB888);
                const uint8_t *srcSlice[] = { frm->data[0], frm->data[1], frm->data[2] };
                int srcStride[] = { frm->linesize[0], frm->linesize[1], frm->linesize[2] };
                uint8_t *dst[] = { img.bits() };
                int dstStride[] = { int(img.bytesPerLine()) };
                sws_scale(m_sws, srcSlice, srcStride, 0, frm->height, dst, dstStride);

                av_frame_unref(frm);

                // 无节流直发：消费速率必须 >= 源速率（30fps 的 sws+QImage 转换
                // 远快于 33ms/帧），UI 侧 VideoWidget 只保留最新帧自然丢帧。
                // 直播读端一旦睡眠节流，TCP 缓冲堆积 -> 服务端 discard -> 花屏。
                emit frameDecoded(img);   // 隐式共享，跨线程传值安全

                // 每秒发一次统计
                ++fpsCount;
                qint64 now = QDateTime::currentMSecsSinceEpoch();
                if (now - fpsWindowStart >= 1000) {
                    emit statsUpdated(fpsCount * 1000.0 / (now - fpsWindowStart), m_lost, 0);
                    fpsCount = 0;
                    fpsWindowStart = now;
                }
            }
        }

        closeStream();
        if (m_running.load())
            emit stateChanged(QStringLiteral("断线，重连中..."));
    }

    av_packet_free(&pkt);
    av_frame_free(&frm);
}
