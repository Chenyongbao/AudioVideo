#ifndef PLAYBACKWORKER_H
#define PLAYBACKWORKER_H

// ============================================================================
// 本地录像播放线程:MP4(avformat) → avcodec 解码 → sws → QImage 信号
// 线程模型与 StreamWorker 一致(std::thread + QueuedConnection 信号),
// 差异:数据源是本地文件(无网络重连),新增:
//   - pts 节奏播放(解码快于实时则睡眠,支持暂停)
//   - seek(av_seek_frame + flush,时间轴拖动联动)
//   - duration/position 上报(驱动时间轴游标)
// ============================================================================

#include <QObject>
#include <QImage>
#include <QString>
#include <atomic>
#include <thread>
#include <cstdint>

struct AVFormatContext;
struct AVCodecContext;
struct SwsContext;

class PlaybackWorker : public QObject
{
    Q_OBJECT
public:
    explicit PlaybackWorker(QObject *parent = nullptr);
    ~PlaybackWorker() override;

    // startOffsetMs:起播位置(文件内毫秒,时间轴点选带入)
    void start(const QString &file, qint64 startOffsetMs = 0);
    void stop();
    void seek(qint64 fileMs);        // 线程安全:仅置请求,decodeLoop 消费
    void setPaused(bool on);

    bool isRunning() const { return m_running.load(); }

signals:
    void frameDecoded(const QImage &img);
    void positionMs(qint64 fileMs);          // 文件内播放位置(驱动时间轴)
    void durationMs(qint64 total);           // 打开成功后发一次
    void stateChanged(const QString &text);

private:
    void decodeLoop();

    std::atomic<bool> m_running{false};
    std::atomic<qint64> m_seekReq{-1};       // >=0 表示请求 seek 到该文件毫秒
    std::atomic<bool> m_paused{false};
    std::thread m_thread;
    QString m_file;

    // FFmpeg 上下文(仅 decodeLoop 线程触碰)
    AVFormatContext *m_fmt = nullptr;
    AVCodecContext  *m_dec = nullptr;
    SwsContext      *m_sws = nullptr;
    int m_videoStream = -1;
};

#endif // PLAYBACKWORKER_H
