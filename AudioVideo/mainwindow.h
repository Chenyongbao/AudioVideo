#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QElapsedTimer>
#include <QMap>
#include <QPair>

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

class StreamWorker;
class YoloTestWorker;
class DeviceClient;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private slots:
    void onStartClicked();          // 连接/断开按钮
    void onFrame(const QImage &img);
    void onStats(double fps, quint64 lost, quint64 uiDropped);
    void onState(const QString &text);
    void onRecordToggled(bool on);
    void onShowFiles();   // 旧对话框版文件管理(已被"文件"Tab 页取代,保留备用)
    void refreshFileList();
    void onPullFile();
    void onDeleteFile();
    void onHashVerify();   // 证据链校验(HASH_VERIFY 命令,弹报告窗)
    void onDetPoll();      // NPU 检测框轮询(DET_GET,连接期间 200ms)
    void onYoloTestToggled(bool on);   // YOLO 测试台:选视频→推流→统计
    void onYoloTestState(const QString &text);
    void updateInfoPanel();   // 刷新预览页右侧信息栏
    void refreshTimeline();                                        // 桌面 rec_* → 时间轴段+标记
    void onSegmentClicked(const QString &path, qint64 offsetMs);   // 时间轴点中录像段→起播
    void onPbDuration(qint64 totalMs);                             // 播放器报时长
    void onPbPosition(qint64 fileMs);                              // 播放器报位置→游标/文本

private:
    void setupCentralVideo();       // 代码创建 VideoWidget（.ui 里没有）

    Ui::MainWindow *ui;
    StreamWorker *m_worker = nullptr;
    class VideoWidget *m_video = nullptr;
    class QAction *m_startAction = nullptr;
    class QAction *m_latAction = nullptr;   // 延迟测量开关按钮(工具栏)
    class QAction *m_recAction = nullptr;   // 录像开关按钮(UDP 7777 控制板端)
    class QTabWidget *m_tabs = nullptr;     // 主结构:实时预览/回放/文件
    class QListWidget *m_fileList = nullptr;
    class QLabel *m_fileInfo = nullptr;
    class QPushButton *m_btnPull = nullptr;
    class QPushButton *m_btnDel = nullptr;
    class QPushButton *m_btnHash = nullptr;   // 哈希校验按钮
    class QTimer *m_detTimer = nullptr;       // 检测框轮询(仅连接期间跑)
    DeviceClient *m_dev = nullptr;            // core 层设备客户端(UDP 命令+统计器)
    class QAction *m_yoloTestAction = nullptr; // 工具栏「YOLO测试」开关
    YoloTestWorker *m_yoloWorker = nullptr;   // 视频推流线程(测试台)
    // (统计状态已迁入 core/DeviceClient:beginStats/accumulateStats/statsSummary)
    class QTextBrowser *m_infoPanel = nullptr; // 预览页右侧信息栏(检测统计/录像/设备)
    QString m_recState = QStringLiteral("未录像"); // 录像状态(信息栏用)
    class Timeline *m_timeline = nullptr;   // 回放页时间轴(录像段/事件▲/拖动)
    class VideoWidget *m_pbVideo = nullptr;     // 回放画面(复用渲染控件)
    class PlaybackWorker *m_pbWorker = nullptr; // 本地解码线程
    class QPushButton *m_pbPlayBtn = nullptr;
    class QLabel *m_pbPosLabel = nullptr;
    qint64 m_pbSegStartDayMs = 0;               // 当前播放段在"天"内的起点毫秒
    qint64 m_pbSegDurMs = 0;                    // 当前播放段时长毫秒
    bool m_pbPaused = false;
    class QTimer *m_recTimer = nullptr;     // 录制时长轮询
    void syncBoardTime();   // 起录前把本机 epoch 发给板端(板无 RTC)
    // (recCommand 已上移 core/DeviceClient::command,UI 一律走语义化方法)
    class QLabel *m_stateLabel = nullptr;
    class QLabel *m_statsLabel = nullptr;
    bool m_playing = false;
    bool m_latTest = false;   // 延迟测量元素开关(F 键切换)
    QString m_url = "rtsp://192.168.137.250:8554/live";
};
#endif // MAINWINDOW_H
