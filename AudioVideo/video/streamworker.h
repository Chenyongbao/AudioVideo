#ifndef STREAMWORKER_H
#define STREAMWORKER_H

// 拉流解码线程：avformat 拉流 -> avcodec 解码 -> sws 转 RGB -> QImage 信号发 UI。
// 线程模型：decodeLoop 跑在 std::thread（不用 QThread，避免 moveToThread 样板），
// 帧通过 Qt::QueuedConnection 信号投递 UI 线程，UI 永不阻塞。
// 特性：
//  - RTSP 强制 TCP 传输（UDP 丢包花屏）+ 连接/读超时；
//  - 断线自动重连（可配间隔）；
//  - 帧率对齐：解码快于源帧率时按 pts 睡眠，防止空转吃满 CPU；
//  - FFmpeg 9 API：av_packet_alloc/av_frame_alloc（无 av_init_packet 栈用法）。

#include <QObject>
#include <QImage>
#include <QString>
#include <atomic>
#include <thread>
#include <cstdint>

struct AVFormatContext;
struct AVCodecContext;
struct SwsContext;

class StreamWorker : public QObject
{
    Q_OBJECT
public:
    explicit StreamWorker(QObject *parent = nullptr);
    ~StreamWorker() override;

    void start(const QString &url, int reconnectMs = 3000);
    void stop();

    bool isRunning() const { return m_running.load(); }

signals:
    // 队列连接发 UI 线程；QImage 隐式共享，跨线程传值安全
    void frameDecoded(const QImage &img);
    // fps=解码帧率 lostPkts=网络丢帧+解码掩码帧 uiDropped=投递侧节流丢弃
    void statsUpdated(double fps, quint64 lostPkts, quint64 uiDropped);
    void stateChanged(const QString &stateText);   // 连接中/在线/重连中...

private:
    void decodeLoop();
    bool openStream();     // avformat/avcodec 打开（成功后填充成员）
    void closeStream();    // 释放全部 FFmpeg 上下文

    std::atomic<bool> m_running{false};
    std::thread m_thread;
    QString m_url;
    int m_reconnectMs = 3000;

    // FFmpeg 上下文（仅 decodeLoop 线程触碰）
    AVFormatContext *m_fmt = nullptr;
    AVCodecContext  *m_dec = nullptr;
    SwsContext      *m_sws = nullptr;
    int m_videoStream = -1;

    // 统计
    std::uint64_t m_lost = 0;
};

#endif // STREAMWORKER_H
