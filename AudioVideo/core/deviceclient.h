#ifndef DEVICECLIENT_H
#define DEVICECLIENT_H

// ============================================================================
// DeviceClient —— 板端设备业务客户端(core 层,零 UI 依赖)
//   封装 UDP 7777 控制通道全部命令:录像/打点/校时/文件/哈希校验/检测框
//   UI 层只调方法、接信号;命令细节(超时/缓冲/地址)都在这里
// ============================================================================

#include <QObject>
#include <QString>
#include <QHostAddress>
#include <QUdpSocket>
#include <QElapsedTimer>
#include <QMap>
#include <QPair>

class DeviceClient : public QObject
{
    Q_OBJECT
public:
    explicit DeviceClient(QObject *parent = nullptr);

    void setBoard(const QString &ip, quint16 controlPort = 7777);

    // ---- 同步命令(2s 超时;HASH_VERIFY 报告可达数 KB,缓冲 16KB) ----
    QString command(const QString &cmd);

    // ---- 语义化封装( preferred,UI 不拼字符串) ----
    QString recordStart(int w, int h, int fps);
    QString recordStop();
    QString recordStatus(quint64 *elapsedMs);      // OK/REC <ms>,失败返回 ERR...
    QString mark(const QString &text);             // 手动/自动打点 → .meta
    QString timeSync();                            // 本机 epoch → 板端(板无 RTC)
    QString fileList();
    QString fileDelete(const QString &name);       // 服务端有路径穿越防护
    QString hashVerify();
    QString detections();                          // DET_GET 原始 JSON([]或[{...}])

    // ---- 检测统计器(原 mainwindow 内的 m_yolo* 全套业务) ----
    void beginStats();                             // 清零并开始累计
    void endStats();
    bool statsActive() const { return m_statsOn; }
    // 喂入一次 DET_GET 轮询结果(boxesJson 为 detections() 的返回)
    void accumulateStats(const QString &boxesJson);
    // 渲染统计摘要(检出率/框数/类别×次数(均置信度))
    QString statsSummary() const;

    static const char *kBoardIpDefault;            // "192.168.137.250"

signals:
    void statsUpdated(const QString &summary);     // accumulateStats 后发射,UI 订阅刷新

private:
    QString m_ip = QStringLiteral("192.168.137.250");
    quint16 m_port = 7777;

    // 统计状态
    bool m_statsOn = false;
    QElapsedTimer m_statT;
    int m_polls = 0, m_hitPolls = 0, m_boxTotal = 0;
    QMap<QString, QPair<int, double>> m_cls;       // 类别 → [次数, 置信度累计]
};

#endif // DEVICECLIENT_H
