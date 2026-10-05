#include "yolotestworker.h"

#include <QTcpSocket>
#include <QProcess>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QElapsedTimer>
#include <QThread>
#include <QDateTime>

// ffmpeg/ffprobe 可执行:D:/FFmpeg 直下或版本子目录(如 ffmpeg-9.0.2-*/bin/)。
static QString findTool(const char *name)
{
    const QString base = QStringLiteral("D:/FFmpeg");
    const QString exe = QString::fromLatin1(name) + QStringLiteral(".exe");
    const QStringList cands = {
        base + QStringLiteral("/bin/") + exe,
        base + QStringLiteral("/") + exe,
    };
    for (const QString &c : cands)
        if (QFileInfo::exists(c))
            return c;
    // 版本子目录兜底:取字典序最大的(最新版本)
    QDir d(base);
    const QStringList subs = d.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (auto it = subs.rbegin(); it != subs.rend(); ++it) {
        const QString c = base + QLatin1Char('/') + *it + QStringLiteral("/bin/") + exe;
        if (QFileInfo::exists(c))
            return c;
    }
    return QString::fromLatin1(name);   // 最后指望 PATH
}

YoloTestWorker::YoloTestWorker(QObject *parent) : QObject(parent) {}

YoloTestWorker::~YoloTestWorker() { stop(); }

qint64 YoloTestWorker::probeTotalFrames(const QString &ffprobe, const QString &video)
{
    QProcess p;
    p.start(ffprobe, {QStringLiteral("-v"), QStringLiteral("error"),
                      QStringLiteral("-select_streams"), QStringLiteral("v:0"),
                      QStringLiteral("-count_packets"),
                      QStringLiteral("-show_entries"), QStringLiteral("stream=nb_read_packets"),
                      QStringLiteral("-of"), QStringLiteral("csv=p=0"), video});
    if (!p.waitForFinished(15000))
        return -1;
    return p.readAllStandardOutput().trimmed().toLongLong();
}

bool YoloTestWorker::start(const QString &videoPath, const QString &boardIp,
                           int fps, int width, int height)
{
    if (m_run.loadRelaxed())
        return false;
    QFileInfo f(videoPath);
    if (!f.exists() || !f.isFile())
        return false;
    m_video = videoPath; m_ip = boardIp;
    m_fps = fps; m_w = width; m_h = height;
    m_run.storeRelaxed(1);
    m_th = std::thread(&YoloTestWorker::loop, this);
    return true;
}

void YoloTestWorker::stop()
{
    if (!m_run.testAndSetRelaxed(1, 0))
        return;
    if (m_th.joinable())
        m_th.join();
}

void YoloTestWorker::loop()
{
    const QString ffmpeg = findTool("ffmpeg");
    if (ffmpeg.isEmpty()) {
        emit stateChanged(QStringLiteral("YOLO测试: 找不到 ffmpeg"));
        m_run.storeRelaxed(0);
        return;
    }

    // ---- 1) 转码:整段视频 → 640x480 NV12 原始流(内存里存得下;~14MB 视频≈4.2GB? 否:
    //        NV12 = w*h*1.5B/帧,640*480*1.5=460800B,15fps 37s≈830帧≈380MB,可控;
    //        更保险落临时文件,这里选临时文件,大视频也不炸内存) ----
    const QString raw = QDir::tempPath() + QStringLiteral("/yolo_test_nv12.raw");
    emit stateChanged(QStringLiteral("YOLO测试: ffmpeg 转码中…"));
    {
        QProcess p;
        p.start(ffmpeg, {QStringLiteral("-v"), QStringLiteral("error"),
                         QStringLiteral("-i"), m_video,
                         QStringLiteral("-vf"), QStringLiteral("%1:%2").arg(m_w).arg(m_h).prepend("scale="),
                         QStringLiteral("-pix_fmt"), QStringLiteral("nv12"),
                         QStringLiteral("-an"), QStringLiteral("-f"), QStringLiteral("rawvideo"),
                         QStringLiteral("-y"), raw});
        if (!p.waitForFinished(-1) || p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0) {
            emit stateChanged(QStringLiteral("YOLO测试: 转码失败 ") + QString::fromLocal8Bit(p.readAllStandardError().left(200)));
            m_run.storeRelaxed(0);
            return;
        }
    }
    QFile rf(raw);
    if (!rf.open(QIODevice::ReadOnly) || rf.size() < (qint64)m_w * m_h * 3 / 2) {
        emit stateChanged(QStringLiteral("YOLO测试: 转码产物异常"));
        m_run.storeRelaxed(0);
        return;
    }

    // ---- 2) 连接板端 8888 并循环推流 ----
    QTcpSocket sock;
    sock.connectToHost(m_ip, 8888);
    if (!sock.waitForConnected(5000)) {
        emit stateChanged(QStringLiteral("YOLO测试: 连不上板端 8888"));
        rf.close(); QFile::remove(raw);
        m_run.storeRelaxed(0);
        return;
    }

    const qint64 frameBytes = (qint64)m_w * m_h * 3 / 2;
    const qint64 total = rf.size() / frameBytes;
    quint8 hdr[4];
    const quint32 fb = (quint32)frameBytes;
    hdr[0] = fb & 0xFF; hdr[1] = (fb >> 8) & 0xFF; hdr[2] = (fb >> 16) & 0xFF; hdr[3] = (fb >> 24) & 0xFF;

    emit stateChanged(QStringLiteral("YOLO测试: 推流中(0/%1帧)").arg(total));
    qint64 frameNo = 0;
    QElapsedTimer clk, tick;
    clk.start(); tick.start();
    const qint64 intervalMs = 1000 / m_fps;
    QByteArray frame;
    frame.resize((int)frameBytes);

    while (m_run.loadRelaxed()) {
        qint64 n = rf.read(frame.data(), frameBytes);
        if (n < frameBytes) {                       // 循环播放
            rf.seek(0);
            n = rf.read(frame.data(), frameBytes);
            if (n < frameBytes) break;
        }
        if (sock.write(reinterpret_cast<char *>(hdr), 4) != 4 ||
            sock.write(frame) != frameBytes) {
            emit stateChanged(QStringLiteral("YOLO测试: 板端断开"));
            break;
        }
        // realtime 模式:按 15fps 绝对节拍防漂移(与摄像头源一致)
        const qint64 due = qint64(++frameNo) * intervalMs;
        const qint64 d = due - clk.elapsed();
        if (d > 0) {
            if (!sock.waitForBytesWritten(qMin<qint64>(d, 100)))
                sock.flush();
            QThread::msleep((ulong)qMin<qint64>(d, 1000));
        } else {
            sock.flush();
        }
        if (tick.elapsed() >= 1000) {
            emit progress((int)(frameNo % (total ? total : 1)),
                          1000.0 * frameNo / clk.elapsed());
            emit stateChanged(QStringLiteral("YOLO测试: 推流中(%1/%2帧)")
                              .arg(frameNo % (total ? total : 1)).arg(total));
            tick.restart();
        }
    }
    sock.disconnectFromHost();
    rf.close();
    QFile::remove(raw);
    emit stateChanged(QStringLiteral("YOLO测试: 已停止"));
    m_run.storeRelaxed(0);
}
