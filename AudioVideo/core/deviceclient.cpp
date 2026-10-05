#include "deviceclient.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>

const char *DeviceClient::kBoardIpDefault = "192.168.137.250";

DeviceClient::DeviceClient(QObject *parent) : QObject(parent) {}

void DeviceClient::setBoard(const QString &ip, quint16 controlPort)
{
    m_ip = ip;
    m_port = controlPort;
}

// ---- 同步 UDP 命令(原 mainwindow::recCommand 原样抽离) ----
QString DeviceClient::command(const QString &cmd)
{
    QUdpSocket udp;
    udp.writeDatagram(cmd.toUtf8(), QHostAddress(m_ip), m_port);
    if (!udp.waitForReadyRead(2000))
        return QStringLiteral("ERR timeout");
    char buf[16384] = {0};   // HASH_VERIFY 报告可超 256B,放大缓冲
    qint64 n = udp.readDatagram(buf, sizeof(buf) - 1);
    return n > 0 ? QString::fromUtf8(buf, (int)n) : QStringLiteral("ERR empty");
}

QString DeviceClient::recordStart(int w, int h, int fps)
{
    return command(QStringLiteral("REC_START %1 %2 %3").arg(w).arg(h).arg(fps));
}

QString DeviceClient::recordStop()
{
    return command(QStringLiteral("REC_STOP"));
}

QString DeviceClient::recordStatus(quint64 *elapsedMs)
{
    const QString r = command(QStringLiteral("REC_STAT"));
    if (elapsedMs && r.startsWith(QStringLiteral("REC")))
        *elapsedMs = r.section(QStringLiteral(" "), 1, 1).toULongLong();
    return r;
}

QString DeviceClient::mark(const QString &text)
{
    return command(QStringLiteral("REC_MARK %1").arg(text));
}

QString DeviceClient::timeSync()
{
    return command(QStringLiteral("TIME_SET %1").arg(QDateTime::currentSecsSinceEpoch()));
}

QString DeviceClient::fileList()      { return command(QStringLiteral("FILE_LIST")); }
QString DeviceClient::fileDelete(const QString &name) { return command(QStringLiteral("FILE_DEL %1").arg(name)); }
QString DeviceClient::hashVerify()    { return command(QStringLiteral("HASH_VERIFY")); }
QString DeviceClient::detections()    { return command(QStringLiteral("DET_GET")); }

// ---- 检测统计器(原 mainwindow 的 m_yolo* 业务整体迁入) ----
void DeviceClient::beginStats()
{
    m_statsOn = true;
    m_polls = m_hitPolls = m_boxTotal = 0;
    m_cls.clear();
    m_statT.restart();
}

void DeviceClient::endStats()
{
    m_statsOn = false;
}

void DeviceClient::accumulateStats(const QString &boxesJson)
{
    if (!m_statsOn || boxesJson.startsWith(QStringLiteral("ERR")))
        return;
    m_polls++;
    const QJsonArray arr = QJsonDocument::fromJson(boxesJson.toUtf8()).array();
    if (!arr.isEmpty()) m_hitPolls++;
    m_boxTotal += arr.size();
    for (const QJsonValue &v : arr) {
        const QJsonObject o = v.toObject();
        const QString cls = o[QStringLiteral("n")].toString();
        const double p = o[QStringLiteral("p")].toDouble();
        if (cls.isEmpty()) continue;
        auto &e = m_cls[cls];
        e.first++;
        e.second += p;
    }
    emit statsUpdated(statsSummary());
}

QString DeviceClient::statsSummary() const
{
    if (!m_statsOn || m_polls == 0)
        return QStringLiteral("等待检测数据…");
    const double secs = m_statT.elapsed() / 1000.0;
    QString s = QStringLiteral("检出率 %1% | 框 %2 | 平均 %3/s")
                    .arg(100.0 * m_hitPolls / m_polls, 0, 'f', 0)
                    .arg(m_boxTotal)
                    .arg(m_boxTotal / (secs > 0 ? secs : 1), 0, 'f', 1);
    QStringList parts;
    for (auto it = m_cls.constBegin(); it != m_cls.constEnd(); ++it)
        parts << QStringLiteral("%1×%2(%3)").arg(it.key()).arg(it.value().first)
                     .arg(it.value().second / it.value().first, 0, 'f', 2);
    if (!parts.isEmpty())
        s += QStringLiteral(" | ") + parts.join(QStringLiteral(", "));
    return s;
}
