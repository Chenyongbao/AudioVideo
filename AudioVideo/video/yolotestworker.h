#ifndef YOLOTESTWORKER_H
#define YOLOTESTWORKER_H

// ============================================================================
// YOLO 准确性测试台 · 视频推流线程:
//   选定视频 → ffmpeg 转一整段 640x480 NV12 → TCP 8888 推给板端
//   (协议与 WSL mjpeg_nv12_sender 相同: [4B 小端长度][NV12])
//   循环播放直到 stop();进度/状态经信号上报 UI。
// 线程模型:std::thread(与 StreamWorker 一致),Qt 信号跨线程投递。
// ============================================================================

#include <QObject>
#include <QAtomicInt>
#include <QString>
#include <thread>
#include <vector>

class YoloTestWorker : public QObject
{
    Q_OBJECT
public:
    explicit YoloTestWorker(QObject *parent = nullptr);
    ~YoloTestWorker() override;

    bool start(const QString &videoPath, const QString &boardIp,
               int fps = 15, int width = 640, int height = 480);
    void stop();          // 幂等;结束后线程退出
    bool running() const { return m_run.loadRelaxed(); }

signals:
    void stateChanged(const QString &text);      // 状态行(转码中/推流中/失败原因)
    void progress(int frameNo, double fps);      // 推流进度(每秒一次)

private:
    void loop();                                  // 线程主体:转码+推流
    static qint64 probeTotalFrames(const QString &ffprobe, const QString &video);

    QString m_video, m_ip;
    int m_fps = 15, m_w = 640, m_h = 480;
    QAtomicInt m_run{0};
    std::thread m_th;
};

#endif // YOLOTESTWORKER_H
