// ============================================================================
// 本地录像播放实现:节奏播放 + seek + 位置上报
// 复用 FFmpeg 解码链(avformat/avcodec/sws),线程模型同 StreamWorker
// ============================================================================
#include "playbackworker.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/avutil.h>
#include <libavutil/time.h>
}

#include <chrono>

PlaybackWorker::PlaybackWorker(QObject *parent) : QObject(parent) {}

PlaybackWorker::~PlaybackWorker() { stop(); }

void PlaybackWorker::start(const QString &file, qint64 startOffsetMs)
{
    if (m_running.load()) stop();
    m_file = file;
    m_seekReq.store(startOffsetMs > 0 ? startOffsetMs : 0);   // 打开后先 seek 到起点
    m_paused.store(false);
    m_running.store(true);
    m_thread = std::thread(&PlaybackWorker::decodeLoop, this);
}

void PlaybackWorker::stop()
{
    m_running.store(false);
    if (m_thread.joinable()) m_thread.join();
}

void PlaybackWorker::seek(qint64 fileMs) { m_seekReq.store(fileMs); }
void PlaybackWorker::setPaused(bool on)  { m_paused.store(on); }

void PlaybackWorker::decodeLoop()
{
    // ---- 打开文件 ----
    if (avformat_open_input(&m_fmt, m_file.toUtf8().constData(), nullptr, nullptr) < 0) {
        emit stateChanged(QStringLiteral("打开失败: %1").arg(m_file));
        m_running.store(false); return;
    }
    if (avformat_find_stream_info(m_fmt, nullptr) < 0) {
        emit stateChanged(QStringLiteral("无流信息"));
        m_running.store(false); return;
    }
    m_videoStream = av_find_best_stream(m_fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (m_videoStream < 0) { emit stateChanged(QStringLiteral("无视频流")); m_running.store(false); return; }

    const AVCodec *codec = avcodec_find_decoder(m_fmt->streams[m_videoStream]->codecpar->codec_id);
    m_dec = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(m_dec, m_fmt->streams[m_videoStream]->codecpar);
    avcodec_open2(m_dec, codec, nullptr);

    AVRational tb = m_fmt->streams[m_videoStream]->time_base;
    double durS = m_fmt->duration > 0 ? m_fmt->duration / (double)AV_TIME_BASE : 0;
    emit durationMs(qint64(durS * 1000));
    emit stateChanged(QStringLiteral("播放中"));

    m_sws = nullptr;
    QImage img;
    int64_t basePts = AV_NOPTS_VALUE;      // 文件起点 pts(节拍基准)
    int64_t wall0 = av_gettime();          // 播放墙钟起点

    AVPacket *pkt = av_packet_alloc();
    AVFrame  *frm = av_frame_alloc();

    while (m_running.load()) {
        // ---- 暂停:不读包不睡死,50ms 轮询 ----
        if (m_paused.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        // ---- seek 请求(av_seek_frame + flush) ----
        qint64 req = m_seekReq.exchange(-1);
        if (req >= 0) {
            int64_t target = int64_t(req / 1000.0 / av_q2d(tb));
            avcodec_flush_buffers(m_dec);
            if (av_seek_frame(m_fmt, m_videoStream, target, AVSEEK_FLAG_BACKWARD) >= 0) {
                basePts = AV_NOPTS_VALUE;  // 重建节拍基准
                wall0 = av_gettime();
            }
        }

        if (av_read_frame(m_fmt, pkt) < 0) {
            emit stateChanged(QStringLiteral("播放结束"));
            break;                          // 文件读完
        }
        if (pkt->stream_index != m_videoStream) { av_packet_unref(pkt); continue; }

        if (avcodec_send_packet(m_dec, pkt) < 0) { av_packet_unref(pkt); continue; }
        av_packet_unref(pkt);

        while (avcodec_receive_frame(m_dec, frm) >= 0) {
            // ---- pts 节奏:解码快于实时则睡到播放点 ----
            if (basePts == AV_NOPTS_VALUE) basePts = frm->pts;
            double fileSec = (frm->pts - basePts) * av_q2d(tb);
            double wallSec = (av_gettime() - wall0) / 1e6;
            if (fileSec > wallSec) {
                int64_t waitUs = int64_t((fileSec - wallSec) * 1e6);
                // 分片睡眠,保证 stop/seek 及时响应
                while (waitUs > 0 && m_running.load() && m_seekReq.load() < 0) {
                    int64_t chunk = waitUs > 20000 ? 20000 : waitUs;
                    std::this_thread::sleep_for(std::chrono::microseconds(chunk));
                    waitUs -= chunk;
                }
            }

            // ---- sws → QImage(首帧建转换器) ----
            if (!m_sws || img.isNull()) {
                m_sws = sws_getCachedContext(m_sws, frm->width, frm->height,
                                             (AVPixelFormat)frm->format,
                                             frm->width, frm->height, AV_PIX_FMT_RGB24,
                                             SWS_BILINEAR, nullptr, nullptr, nullptr);
                img = QImage(frm->width, frm->height, QImage::Format_RGB888);
            }
            uint8_t *dst[4] = { img.bits(), nullptr, nullptr, nullptr };
            int dstStride[4] = { int(img.bytesPerLine()), 0, 0, 0 };
            sws_scale(m_sws, frm->data, frm->linesize, 0, frm->height, dst, dstStride);

            // ---- 上报位置 + 帧(节流:每帧都报,时间轴 30fps 刷新够用) ----
            emit positionMs(int64_t(fileSec * 1000));
            emit frameDecoded(img.copy());
        }
    }

    av_frame_free(&frm);
    av_packet_free(&pkt);
    if (m_sws) { sws_freeContext(m_sws); m_sws = nullptr; }
    avcodec_free_context(&m_dec);
    avformat_close_input(&m_fmt);
    m_running.store(false);
}
