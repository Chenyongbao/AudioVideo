#include "mainwindow.h"
#include "./ui_mainwindow.h"
#include "video/videowidget.h"
#include "video/streamworker.h"
#include "video/timeline.h"
#include "video/playbackworker.h"

extern "C" {
#include <libavformat/avformat.h>
}

#include <QToolBar>
#include <QLineEdit>
#include <QAction>
#include <QLabel>
#include <QStatusBar>
#include <QUdpSocket>
#include <QTimer>
#include <QHostAddress>
#include <QDateTime>
#include <QDialog>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QListWidget>
#include <QPushButton>
#include <QMessageBox>
#include <QProcess>
#include <QTabWidget>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QFile>
#include <QPlainTextEdit>
#include <QFileDialog>
#include <QTextBrowser>
#include "core/deviceclient.h"
#include "video/yolotestworker.h"
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <algorithm>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);
    setWindowTitle(QStringLiteral("指挥室客户端 - AudioVideo"));

    setupCentralVideo();

    // 工具栏：地址输入 + 连接/断开
    auto *tb = addToolBar(QStringLiteral("stream"));
    auto *urlEdit = new QLineEdit(m_url, this);
    urlEdit->setMinimumWidth(360);
    tb->addWidget(new QLabel(QStringLiteral(" 地址: "), this));
    tb->addWidget(urlEdit);

    m_startAction = tb->addAction(QStringLiteral("连接"));
    m_startAction->setCheckable(true);

    connect(urlEdit, &QLineEdit::textChanged, this, [this](const QString &t) {
        m_url = t.trimmed();
    });
    connect(m_startAction, &QAction::toggled, this, &MainWindow::onStartClicked);

    // 状态栏：连接状态 + 帧率/丢包
    m_stateLabel = new QLabel(QStringLiteral("未连接"), this);
    m_statsLabel = new QLabel(QStringLiteral("-- fps | 丢包 --"), this);
    statusBar()->addWidget(m_stateLabel);
    statusBar()->addPermanentWidget(m_statsLabel);

    // 解码线程 -> UI 线程：队列连接
    m_worker = new StreamWorker(this);
    connect(m_worker, &StreamWorker::frameDecoded,
            m_video, &VideoWidget::presentFrame, Qt::QueuedConnection);
    connect(m_worker, &StreamWorker::statsUpdated,
            this, &MainWindow::onStats, Qt::QueuedConnection);
    connect(m_worker, &StreamWorker::stateChanged,
            this, &MainWindow::onState, Qt::QueuedConnection);

    // 工具栏按钮:开/关延迟测量元素(色块+边框 3s 周期闪烁,配合手机拍屏测端到端延迟)
    m_latAction = tb->addAction(QStringLiteral("延迟测量"));
    m_latAction->setCheckable(true);
    connect(m_latAction, &QAction::toggled, this, [this](bool on) {
        m_latTest = on;
        m_video->setLatencyTest(on);
        statusBar()->showMessage(on
            ? QStringLiteral("延迟测量:开(手机拍屏,对比色块与边框跳变相位差)")
            : QStringLiteral("延迟测量:关"), 5000);
    });

    // 录像按钮:UDP 7777 控制板端 recorder(预录30s环形缓冲 + fMP4断电安全)
    m_recAction = tb->addAction(QStringLiteral("● 录像"));
    m_recAction->setCheckable(true);
    connect(m_recAction, &QAction::toggled, this, &MainWindow::onRecordToggled);
    // (文件管理已升级为常驻 Tab 页,原工具栏"📁 文件"按钮移除)

    // 录像状态轮询:录制中每秒刷新已录时长
    m_recTimer = new QTimer(this);
    m_recTimer->setInterval(1000);
    connect(m_recTimer, &QTimer::timeout, this, [this] {
        QString resp = m_dev->recordStatus(nullptr);
        if (resp.startsWith(QStringLiteral("REC"))) {
            quint64 ms = resp.section(QStringLiteral(" "), 1, 1).toULongLong();
            m_statsLabel->setText(m_statsLabel->text() + QStringLiteral("  |  ● REC %1:%2")
                                  .arg(ms / 60000, 2, 10, QLatin1Char('0'))
                                  .arg(ms / 1000 % 60, 2, 10, QLatin1Char('0')));
        }
    });

    // core 层设备客户端:UDP 命令 + 检测统计(业务与视图分离的分界线)
    m_dev = new DeviceClient(this);
    connect(m_dev, &DeviceClient::statsUpdated, this, [this](const QString &s) {
        statusBar()->showMessage(QStringLiteral("[YOLO测试] ") + s);
        m_yoloTestAction->setToolTip(QStringLiteral("统计: ") + s);
        updateInfoPanel();
    });

    // NPU 检测框轮询(仅连接期间跑;板端无检测器时 DET_GET 返回 [],画面无框)
    m_detTimer = new QTimer(this);
    m_detTimer->setInterval(200);
    connect(m_detTimer, &QTimer::timeout, this, &MainWindow::onDetPoll);

    // YOLO 测试台:选本地视频→ffmpeg 转 NV12→推给板端 8888,DECT 轮询同步统计准确性
    m_yoloWorker = new YoloTestWorker(this);
    connect(m_yoloWorker, &YoloTestWorker::stateChanged,
            this, &MainWindow::onYoloTestState, Qt::QueuedConnection);
    m_yoloTestAction = tb->addAction(QStringLiteral("YOLO测试"));
    m_yoloTestAction->setCheckable(true);   // 连接的是 toggled,必须 checkable 才会发射
    m_yoloTestAction->setToolTip(QStringLiteral("选一段本地视频推给板端,统计 YOLO 检出率/类别/置信度(需先连接)"));
    connect(m_yoloTestAction, &QAction::toggled, this, &MainWindow::onYoloTestToggled);
}

void MainWindow::setupCentralVideo()
{
    // Tab 结构(对标专业 NVR/证据管理软件):实时预览 / 回放 / 文件
    m_tabs = new QTabWidget(this);
    setCentralWidget(m_tabs);

    // Tab1 实时预览:视频(左)+ 信息栏(右,常驻可见的 NVR 式布局)
    m_video = new VideoWidget(this);
    m_video->setOverlayText(QStringLiteral("no signal"));
    m_video->setMirror(true);   // 自拍预览镜像:现实往左,画面里也往左(照镜子方向感)
    auto *pvLay = new QHBoxLayout;
    auto *pvPage = new QWidget(m_tabs);
    pvPage->setLayout(pvLay);
    pvLay->addWidget(m_video, 1);

    // 右侧信息栏:NPU 检测统计 / 录像与连接状态 / 设备信息(磁盘/证据链)
    m_infoPanel = new QTextBrowser(pvPage);
    m_infoPanel->setMaximumWidth(280);
    m_infoPanel->setOpenExternalLinks(false);
    pvLay->addWidget(m_infoPanel);
    m_tabs->addTab(pvPage, QStringLiteral("实时预览"));

    // Tab2 回放:时间轴 + 本地播放(worker 线程解码,复用 VideoWidget 渲染)
    auto *playback = new QWidget(m_tabs);
    auto *pbLay = new QVBoxLayout(playback);
    m_pbVideo = new VideoWidget(playback);
    m_pbVideo->setOverlayText(QStringLiteral("回放:点时间轴蓝色段开始"));
    pbLay->addWidget(m_pbVideo, 1);

    auto *ctrlLay = new QHBoxLayout;
    m_pbPlayBtn = new QPushButton(QStringLiteral("⏸ 暂停"), playback);
    m_pbPosLabel = new QLabel(QStringLiteral("--:-- / --:--"), playback);
    ctrlLay->addWidget(m_pbPlayBtn);
    ctrlLay->addWidget(m_pbPosLabel);
    ctrlLay->addStretch(1);
    pbLay->addLayout(ctrlLay);

    m_timeline = new Timeline(playback);
    pbLay->addWidget(m_timeline);
    m_tabs->addTab(playback, QStringLiteral("回放"));

    // 回放 worker 信号(工作线程 → UI 线程,Queued)
    m_pbWorker = new PlaybackWorker(this);
    connect(m_pbWorker, &PlaybackWorker::frameDecoded, m_pbVideo, &VideoWidget::presentFrame, Qt::QueuedConnection);
    connect(m_pbWorker, &PlaybackWorker::durationMs, this, &MainWindow::onPbDuration);
    connect(m_pbWorker, &PlaybackWorker::positionMs, this, &MainWindow::onPbPosition);
    connect(m_pbWorker, &PlaybackWorker::stateChanged, this, [this](const QString &s) {
        statusBar()->showMessage(QStringLiteral("回放: ") + s, 3000);
    });
    connect(m_pbPlayBtn, &QPushButton::clicked, this, [this] {
        if (!m_pbWorker->isRunning()) return;
        m_pbPaused = !m_pbPaused;
        m_pbWorker->setPaused(m_pbPaused);
        m_pbPlayBtn->setText(m_pbPaused ? QStringLiteral("▶ 播放") : QStringLiteral("⏸ 暂停"));
    });
    connect(m_timeline, &Timeline::segmentClicked, this, &MainWindow::onSegmentClicked);
    connect(m_timeline, &Timeline::seekRequested, this, [this](qint64 dayMs) {
        if (m_pbWorker->isRunning() && m_pbSegDurMs > 0) {
            qint64 fileMs = dayMs - m_pbSegStartDayMs;
            if (fileMs < 0) fileMs = 0;
            if (fileMs > m_pbSegDurMs - 200) fileMs = m_pbSegDurMs - 200;
            m_pbWorker->seek(fileMs);
        }
    });

    // Tab3 文件:原对话框升级为常驻页(列表/拉取/删除)
    auto *filePage = new QWidget(m_tabs);
    auto *lay = new QVBoxLayout(filePage);
    m_fileList = new QListWidget(filePage);
    lay->addWidget(m_fileList, 1);
    m_fileInfo = new QLabel(filePage);
    lay->addWidget(m_fileInfo);
    auto *rowLay = new QHBoxLayout;
    auto *btnRefresh = new QPushButton(QStringLiteral("刷新"), filePage);
    m_btnPull = new QPushButton(QStringLiteral("拉取到桌面"), filePage);
    m_btnDel = new QPushButton(QStringLiteral("删除"), filePage);
    m_btnHash = new QPushButton(QStringLiteral("哈希校验"), filePage);
    rowLay->addWidget(btnRefresh); rowLay->addWidget(m_btnPull); rowLay->addWidget(m_btnDel);
    rowLay->addWidget(m_btnHash);
    rowLay->addStretch(1);
    lay->addLayout(rowLay);
    m_tabs->addTab(filePage, QStringLiteral("文件"));

    connect(btnRefresh, &QPushButton::clicked, this, &MainWindow::refreshFileList);
    connect(m_btnPull, &QPushButton::clicked, this, &MainWindow::onPullFile);
    connect(m_btnDel, &QPushButton::clicked, this, &MainWindow::onDeleteFile);
    connect(m_btnHash, &QPushButton::clicked, this, &MainWindow::onHashVerify);
    // 切到文件页时再刷新(启动时不发 UDP,避免板离线时 UI 卡 2s);回放页同理重建时间轴
    connect(m_tabs, &QTabWidget::currentChanged, this, [this](int idx) {
        if (idx < 0) return;
        const QString name = m_tabs->tabText(idx);
        if (name == QStringLiteral("文件"))
            refreshFileList();
        else if (name == QStringLiteral("回放"))
            refreshTimeline();
    });
}

// 从板端拉录像清单填充文件页(UDP FILE_LIST)
void MainWindow::refreshFileList()
{
    m_fileList->clear();
    QString resp = m_dev->fileList();
    if (resp.startsWith(QStringLiteral("ERR"))) {
        m_fileInfo->setText(QStringLiteral("获取失败: ") + resp);
        return;
    }
    quint64 total = 0;
    int n = 0;
    for (const QString &r : resp.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        QString name = r.section(QLatin1Char(' '), 0, 0);
        quint64 bytes = r.section(QLatin1Char(' '), 1, 1).toULongLong();
        if (name == QStringLiteral("(empty)")) continue;
        auto *it = new QListWidgetItem(
            QStringLiteral("%1   %2 MB").arg(name).arg(bytes / 1024 / 1024), m_fileList);
        it->setData(Qt::UserRole, name);
        it->setData(Qt::UserRole + 1, bytes);
        total += bytes; n++;
    }
    long long free_mb = -1;
    QString stat = m_dev->recordStatus(nullptr);
    m_fileInfo->setText(QStringLiteral("共 %1 段 / %2 MB    板端状态: %3")
                            .arg(n).arg(total / 1024 / 1024).arg(stat));
}

void MainWindow::onPullFile()
{
    auto *it = m_fileList->currentItem();
    if (!it) { QMessageBox::information(this, QStringLiteral("提示"), QStringLiteral("先选中一个文件")); return; }
    QString name = it->data(Qt::UserRole).toString();
    quint64 mb = it->data(Qt::UserRole + 1).toULongLong() / 1024 / 1024;
    m_btnPull->setEnabled(false);
    m_btnPull->setText(QStringLiteral("拉取中…(%1MB 约百秒,走 adb)").arg(mb));
    QCoreApplication::processEvents();
    // adb pull 走本机 adb(与调试同通道,不占 8888/9999 带宽)
    QProcess p;
    p.setProcessChannelMode(QProcess::ForwardedChannels);
    p.start(QStringLiteral("adb"), {QStringLiteral("-P"), QStringLiteral("5038"), QStringLiteral("pull"),
                 QStringLiteral("/userdata/") + name,
                 QStringLiteral("C:/Users/Mr.chen/Desktop/") + name});
    p.waitForFinished(-1);
    m_btnPull->setEnabled(true);
    m_btnPull->setText(QStringLiteral("拉取到桌面"));
    if (p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0)
        QMessageBox::information(this, QStringLiteral("完成"), QStringLiteral("已保存到桌面: %1").arg(name));
    else
        QMessageBox::warning(this, QStringLiteral("失败"), QStringLiteral("adb pull 失败(检查 adb 端口 5038/连接)"));
}

void MainWindow::onDeleteFile()
{
    auto *it = m_fileList->currentItem();
    if (!it) { QMessageBox::information(this, QStringLiteral("提示"), QStringLiteral("先选中一个文件")); return; }
    QString name = it->data(Qt::UserRole).toString();
    if (QMessageBox::question(this, QStringLiteral("确认"),
            QStringLiteral("删除板端 %1 ?").arg(name)) != QMessageBox::Yes) return;
    QString resp = m_dev->fileDelete(name);
    if (resp.startsWith(QStringLiteral("OK"))) {
        delete it;
        statusBar()->showMessage(QStringLiteral("已删除: ") + name, 3000);
        refreshFileList();
    } else {
        QMessageBox::warning(this, QStringLiteral("失败"), resp);
    }
}

void MainWindow::onStartClicked()
{
    if (m_startAction->isChecked()) {
        if (m_url.isEmpty()) {
            m_startAction->setChecked(false);
            return;
        }
        m_worker->start(m_url);
        m_detTimer->start();
        m_startAction->setText(QStringLiteral("断开"));
    } else {
        m_worker->stop();
        m_detTimer->stop();
        m_video->setDetections(QList<DetOverlayBox>());
        m_startAction->setText(QStringLiteral("连接"));
        m_video->setOverlayText(QStringLiteral("no signal"));
    }
}

// 预览页右侧信息栏:检测统计 + 录像/连接状态 + 设备信息,常驻可见
void MainWindow::updateInfoPanel()
{
    if (!m_infoPanel) return;
    QString html = QStringLiteral("<style>h3{color:#3d8bff;margin:2px 0 4px;} "
                                  "td{padding:1px 6px 1px 0;color:#d8dee6;} .k{color:#9aa5b1;}</style>");

    // -- NPU 检测(统计业务在 core/DeviceClient) --
    html += QStringLiteral("<h3>NPU 检测</h3><table>");
    if (m_dev && m_dev->statsActive()) {
        html += QStringLiteral("<tr><td colspan='2'>%1</td></tr>")
                    .arg(m_dev->statsSummary().toHtmlEscaped());
    } else {
        html += QStringLiteral("<tr><td class='k'>模式</td><td>预览叠加(未在统计)</td></tr>");
    }
    html += QStringLiteral("</table>");

    // -- 状态 --
    html += QStringLiteral("<h3>状态</h3><table>");
    html += QStringLiteral("<tr><td class='k'>连接</td><td>%1</td></tr>")
                .arg(m_startAction->isChecked() ? m_url : QStringLiteral("未连接"));
    html += QStringLiteral("<tr><td class='k'>录像</td><td>%1</td></tr>").arg(m_recState);
    html += QStringLiteral("</table>");

    // -- 设备(每 8s 查一次文件统计,其余走缓存) --
    static QElapsedTimer diskT;
    static QString diskCache = QStringLiteral("--");
    if (!diskT.isValid() || diskT.elapsed() > 8000) {
        diskT.restart();
        QString r = m_dev->fileList();
        if (r.startsWith(QStringLiteral("ERR"))) diskCache = QStringLiteral("离线");
        else {
            // "FILES n size_bytes ..." 或逐行列表——取段数与总大小
            const QStringList lines = r.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
            qint64 total = 0; int cnt = 0;
            for (const QString &l : lines)
                if (l.contains(QStringLiteral(".mp4"))) { cnt++; total += l.section(QLatin1Char(' '), -1).toLongLong(); }
            diskCache = QStringLiteral("%1 段 / %2 MB").arg(cnt).arg(total / (1024 * 1024));
        }
    }
    html += QStringLiteral("<h3>设备</h3><table>");
    html += QStringLiteral("<tr><td class='k'>录像</td><td>%1</td></tr>").arg(diskCache);
    html += QStringLiteral("<tr><td class='k'>证据链</td><td>文件页「哈希校验」</td></tr>");
    html += QStringLiteral("</table>");

    m_infoPanel->setHtml(html);
}

// NPU 检测框轮询:DET_GET(连接期间 200ms;断开即停,画面保持最后一帧的框)
// 测试台开启期间,同时累计准确性统计(轮询数/命中/类别计数/均置信度)
void MainWindow::onDetPoll()
{
    QString resp = m_dev->detections();
    QList<DetOverlayBox> boxes;
    if (!resp.startsWith(QStringLiteral("ERR"))) {
        QJsonDocument doc = QJsonDocument::fromJson(resp.toUtf8());
        const QJsonArray arr = doc.array();
        for (const QJsonValue &v : arr) {
            const QJsonObject o = v.toObject();
            const QJsonArray b = o[QStringLiteral("b")].toArray();
            if (b.size() != 4) continue;
            DetOverlayBox db;
            db.r = QRectF(b[0].toDouble(), b[1].toDouble(),
                          b[2].toDouble() - b[0].toDouble(),
                          b[3].toDouble() - b[1].toDouble());
            db.label = QStringLiteral("%1 %.2f").arg(o[QStringLiteral("n")].toString())
                                             .arg(o[QStringLiteral("p")].toDouble());
            boxes.append(db);
        }
    }
    m_video->setDetections(boxes);
    updateInfoPanel();   // 信息栏随轮询刷新(200ms)

    // ---- 测试台统计(业务在 core/DeviceClient,UI 只接信号刷新) ----
    if (m_yoloTestAction && m_yoloTestAction->isChecked()) {
        m_dev->accumulateStats(resp);   // core 内部判 ERR/空数组并 emit statsUpdated
    }
}

// YOLO 测试台开关:选本地视频 → 推流线程把视频喂给板端 8888(替代摄像头),
// 同时 onDetPoll 的统计开始累计。前提:已连接(预览在播)。
void MainWindow::onYoloTestToggled(bool on)
{
    if (on) {
        // 先选文件:任何前置校验失败都弹窗明示(状态栏 5s 消息易被忽略)
        const QString v = QFileDialog::getOpenFileName(
            this, QStringLiteral("选择测试视频"), QString(),
            QStringLiteral("视频 (*.mp4 *.avi *.mkv *.mov *.webm);;所有文件 (*)"));
        if (v.isEmpty()) { m_yoloTestAction->setChecked(false); return; }

        if (!m_yoloWorker->running() && !m_startAction->isChecked()) {
            QMessageBox::warning(this, QStringLiteral("YOLO测试"),
                QStringLiteral("请先点「连接」建立预览链路,再开测试。"));
            m_yoloTestAction->setChecked(false);
            return;
        }

        m_dev->beginStats();   // core 清零并开始累计(emit statsUpdated → UI 刷新)
        // 视频测试时关掉镜像:画框坐标是物理画面,镜像会让框左右反
        m_video->setMirror(false);
        if (!m_yoloWorker->start(v, QStringLiteral("192.168.137.250")))
            { m_yoloTestAction->setChecked(false); return; }
        m_yoloTestAction->setText(QStringLiteral("⏹ 测试中"));
    } else {
        if (m_yoloWorker->running())
            m_yoloWorker->stop();
        m_yoloTestAction->setText(QStringLiteral("YOLO测试"));
        m_dev->endStats();
        m_video->setMirror(true);
        statusBar()->showMessage(QStringLiteral("YOLO测试: 已停止"), 4000);
    }
}

void MainWindow::onYoloTestState(const QString &text)
{
    statusBar()->showMessage(text, 6000);
}

void MainWindow::onFrame(const QImage &img)
{
    m_video->presentFrame(img);   // 队列连接下也直接调（同线程幂等），兼容两种接法
}

void MainWindow::onStats(double fps, quint64 lost, quint64 uiDropped)
{
    m_statsLabel->setText(QString::number(fps, 'f', 1)
                          + QStringLiteral(" fps | 丢帧 ")
                          + QString::number(lost)
                          + QStringLiteral(" | UI丢 ")
                          + QString::number(uiDropped));
}

void MainWindow::onState(const QString &text)
{
    m_stateLabel->setText(text);
}

void MainWindow::onShowFiles()
{
    // 拉列表(复用 UDP 控制通道)
    QString resp = m_dev->fileList();
    if (resp.startsWith(QStringLiteral("ERR"))) {
        QMessageBox::warning(this, QStringLiteral("文件列表"), QStringLiteral("获取失败: ") + resp);
        return;
    }
    QStringList rows = resp.split(QLatin1Char('\n'), Qt::SkipEmptyParts);

    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("板端录像文件 (/userdata)"));
    auto *lay = new QVBoxLayout(&dlg);
    auto *lst = new QListWidget(&dlg);
    lay->addWidget(lst, 1);

    // 解析 "文件名 字节" 填充列表,展示为 名称 + MB
    for (const QString &r : rows) {
        QString name = r.section(QLatin1Char(' '), 0, 0);
        quint64 bytes = r.section(QLatin1Char(' '), 1, 1).toULongLong();
        if (name == QStringLiteral("(empty)")) continue;
        QListWidgetItem *it = new QListWidgetItem(
            QStringLiteral("%1  (%2 MB)").arg(name).arg(bytes / 1024 / 1024), lst);
        it->setData(Qt::UserRole, name);
        it->setData(Qt::UserRole + 1, bytes);
    }

    // 空间概览
    QString stat = m_dev->recordStatus(nullptr);
    auto *info = new QLabel(QStringLiteral("板端状态: %1   磁盘剩余见 REC_STAT/df").arg(stat), &dlg);
    lay->addWidget(info);

    auto *rowLay = new QHBoxLayout;
    auto *btnPull = new QPushButton(QStringLiteral("拉取到桌面"), &dlg);
    auto *btnDel = new QPushButton(QStringLiteral("删除"), &dlg);
    auto *btnClose = new QPushButton(QStringLiteral("关闭"), &dlg);
    rowLay->addWidget(btnPull); rowLay->addWidget(btnDel); rowLay->addStretch(1); rowLay->addWidget(btnClose);
    lay->addLayout(rowLay);

    auto selName = [&lst]() -> QString {
        auto *it = lst->currentItem();
        return it ? it->data(Qt::UserRole).toString() : QString();
    };
    connect(btnPull, &QPushButton::clicked, &dlg, [&]() {
        QString name = selName();
        if (name.isEmpty()) { QMessageBox::information(&dlg, QStringLiteral("提示"), QStringLiteral("先选中一个文件")); return; }
        btnPull->setEnabled(false);
        btnPull->setText(QStringLiteral("拉取中…(63MB 约 100s)"));
        QCoreApplication::processEvents();
        // adb pull 走本机 adb(与调试同通道,不占 8888/9999 带宽)
        QProcess p;
        p.setProcessChannelMode(QProcess::ForwardedChannels);
        p.start(QStringLiteral("adb"), {QStringLiteral("-P"), QStringLiteral("5038"), QStringLiteral("pull"),
                     QStringLiteral("/userdata/") + name,
                     QStringLiteral("C:/Users/Mr.chen/Desktop/") + name});
        p.waitForFinished(-1);
        btnPull->setEnabled(true);
        btnPull->setText(QStringLiteral("拉取到桌面"));
        if (p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0)
            QMessageBox::information(&dlg, QStringLiteral("完成"), QStringLiteral("已保存到桌面: %1").arg(name));
        else
            QMessageBox::warning(&dlg, QStringLiteral("失败"), QStringLiteral("adb pull 失败(检查 adb 端口 5038/连接)"));
    });
    connect(btnDel, &QPushButton::clicked, &dlg, [&]() {
        QString name = selName();
        if (name.isEmpty()) { QMessageBox::information(&dlg, QStringLiteral("提示"), QStringLiteral("先选中一个文件")); return; }
        if (QMessageBox::question(&dlg, QStringLiteral("确认"),
                QStringLiteral("删除板端 %1 ?").arg(name)) != QMessageBox::Yes) return;
        QString resp2 = m_dev->fileDelete(name);
        if (resp2.startsWith(QStringLiteral("OK"))) {
            delete lst->currentItem();
            statusBar()->showMessage(QStringLiteral("已删除: ") + name, 3000);
        } else {
            QMessageBox::warning(&dlg, QStringLiteral("失败"), resp2);
        }
    });
    connect(btnClose, &QPushButton::clicked, &dlg, &QDialog::accept);
    dlg.resize(520, 400);
    dlg.exec();
}

// 桌面 rec_*.mp4 → 时间轴录像段(文件名解析当天时刻,ffprobe 取时长)
// + 板端 .meta → 事件标记(▲)。切到"回放"页时调用。
void MainWindow::refreshTimeline()
{
    QVector<TlSegment> segs;
    QVector<TlMark> marks;
    QDir desk(QStringLiteral("C:/Users/Mr.chen/Desktop"));
    static const QRegularExpression re(QStringLiteral("rec_(\\d{8})_(\\d{6})\\.mp4$"));
    const QString ffprobe = QStringLiteral(FFMPEG_SDK_DIR) + QStringLiteral("/bin/ffprobe.exe");

    for (const QFileInfo &fi :
         desk.entryInfoList({QStringLiteral("rec_*.mp4")}, QDir::Files)) {
        auto m = re.match(fi.fileName());
        if (!m.hasMatch()) continue;
        const QString t = m.captured(2);   // HHMMSS
        qint64 startMs = t.left(2).toInt() * 3600000LL + t.mid(2, 2).toInt() * 60000LL
                         + t.mid(4, 2).toInt() * 1000LL;
        // ffprobe 取真实时长(失败则给 1s 占位,点中后由 worker 纠正)
        qint64 durMs = 1000;
        QProcess pp;
        pp.start(ffprobe, {QStringLiteral("-v"), QStringLiteral("error"),
                           QStringLiteral("-show_entries"), QStringLiteral("format=duration"),
                           QStringLiteral("-of"), QStringLiteral("csv=p=0"), fi.absoluteFilePath()});
        if (pp.waitForFinished(4000)) {
            bool ok = false;
            double sec = QString::fromUtf8(pp.readAllStandardOutput().trimmed()).toDouble(&ok);
            if (ok && sec > 0) durMs = qint64(sec * 1000);
        }
        segs.push_back({startMs, durMs, fi.absoluteFilePath()});
    }
    std::sort(segs.begin(), segs.end(),
              [](const TlSegment &a, const TlSegment &b) { return a.startMs < b.startMs; });

    // 事件标记:逐段 adb shell cat 板端 .meta(行格式: 相对ms epoch 文本)
    for (const auto &seg : segs) {
        const QString name = QFileInfo(seg.path).fileName();
        QProcess p;
        p.start(QStringLiteral("adb"), {QStringLiteral("-P"), QStringLiteral("5038"),
                     QStringLiteral("shell"),
                     QStringLiteral("cat /userdata/%1.meta 2>/dev/null").arg(name)});
        if (!p.waitForFinished(3000)) continue;
        const QString out = QString::fromUtf8(p.readAllStandardOutput());
        for (const QString &line : out.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
            const QStringList parts = line.split(QLatin1Char(' '));
            if (parts.size() < 2) continue;
            bool ok = false;
            qint64 rel = parts[0].toLongLong(&ok);
            if (!ok) continue;
            marks.push_back({seg.startMs + rel,
                             parts.mid(2).join(QLatin1Char(' '))});
        }
    }
    m_timeline->setSegments(segs);
    m_timeline->setMarks(marks);
    m_timeline->setPositionMs(0);
}

// 时间轴点中录像段 → 立即起播(文件内偏移 = 点击时刻 - 段起点)
void MainWindow::onSegmentClicked(const QString &path, qint64 offsetMs)
{
    static const QRegularExpression re(QStringLiteral("rec_(\\d{8})_(\\d{6})\\.mp4$"));
    auto m = re.match(QFileInfo(path).fileName());
    if (!m.hasMatch()) return;
    const QString t = m.captured(2);
    m_pbSegStartDayMs = t.left(2).toInt() * 3600000LL + t.mid(2, 2).toInt() * 60000LL
                        + t.mid(4, 2).toInt() * 1000LL;
    m_pbPaused = false;
    m_pbPlayBtn->setText(QStringLiteral("⏸ 暂停"));
    m_pbVideo->setOverlayText(QString());   // 清掉提示文字
    m_pbVideo->setMirror(false);            // 回放不镜像(真实方向)
    m_pbWorker->start(path, offsetMs);
    statusBar()->showMessage(QStringLiteral("回放: %1 @ %2 s")
                                 .arg(QFileInfo(path).fileName()).arg(offsetMs / 1000), 4000);
}

// 播放器报时长 → 记录(时间轴 seek 边界用)+ 显示
void MainWindow::onPbDuration(qint64 totalMs)
{
    m_pbSegDurMs = totalMs;
}

// 播放器报位置 → 时间轴游标 + 文本
void MainWindow::onPbPosition(qint64 fileMs)
{
    m_timeline->setPositionMs(m_pbSegStartDayMs + fileMs);
    auto fmt = [](qint64 ms) {
        return QStringLiteral("%1:%2").arg(ms / 60000).arg(ms / 1000 % 60, 2, 10, QLatin1Char('0'));
    };
    m_pbPosLabel->setText(fmt(fileMs) + QStringLiteral(" / ") + fmt(m_pbSegDurMs));
}

// 证据链校验:HASH_VERIFY → 弹报告窗(逐段 OK/TAMPERED/MISSING + 总结)
void MainWindow::onHashVerify()
{
    m_btnHash->setEnabled(false);
    QString resp = m_dev->hashVerify();
    m_btnHash->setEnabled(true);
    if (resp.startsWith(QStringLiteral("ERR"))) {
        QMessageBox::warning(this, QStringLiteral("哈希校验"), QStringLiteral("校验失败: ") + resp);
        return;
    }

    // 总结行决定标题/结论展示
    const bool intact = resp.contains(QStringLiteral("证据链完整"));
    QDialog dlg(this);
    dlg.setWindowTitle(intact ? QStringLiteral("证据链校验:完整 ✓")
                              : QStringLiteral("证据链校验:被破坏!"));
    auto *lay = new QVBoxLayout(&dlg);
    auto *head = new QLabel(intact
        ? QStringLiteral("所有录像段哈希衔接一致,内容未被篡改")
        : QStringLiteral("检测到篡改/缺失!以下段落不可作为证据使用"), &dlg);
    head->setStyleSheet(intact
        ? QStringLiteral("color: #4caf50; font-weight: bold; font-size: 13px;")
        : QStringLiteral("color: #ff5252; font-weight: bold; font-size: 13px;"));
    lay->addWidget(head);

    auto *txt = new QPlainTextEdit(&dlg);
    txt->setPlainText(resp);
    txt->setReadOnly(true);
    QFont mono(QStringLiteral("Consolas")); mono.setStyleHint(QFont::Monospace);
    txt->setFont(mono);
    lay->addWidget(txt, 1);
    auto *close = new QPushButton(QStringLiteral("关闭"), &dlg);
    connect(close, &QPushButton::clicked, &dlg, &QDialog::accept);
    lay->addWidget(close, 0, Qt::AlignRight);
    dlg.resize(560, 420);
    dlg.exec();
}

MainWindow::~MainWindow()
{
    m_worker->stop();   // 先停解码线程再析构 UI
    delete ui;
}

// UDP 7777 发控制命令给板端 recorder,返回板端响应(超时/失败返回 ERR 前缀)
// 板钟校准:板无 RTC(开机为 1970),把本机 epoch 秒发过去。
// 在录像按钮点击前调用,保证录像文件名/creation_time 是真实时间。
void MainWindow::syncBoardTime()
{
    QString resp = m_dev->timeSync();
    if (resp.startsWith(QStringLiteral("OK")))
        statusBar()->showMessage(QStringLiteral("板端时间已校准"), 3000);
    else
        statusBar()->showMessage(QStringLiteral("板端时间校准失败: ") + resp, 5000);
}

void MainWindow::onRecordToggled(bool on)
{
    if (on)
        syncBoardTime();   // 起录前校准板钟,文件名即真实时间
    QString resp = on ? m_dev->recordStart(640, 480, 15)
                      : m_dev->recordStop();
    if (resp.startsWith(QStringLiteral("ERR"))) {
        // 命令失败:回弹按钮状态,提示错误
        m_recAction->blockSignals(true);
        m_recAction->setChecked(!on);
        m_recAction->blockSignals(false);
        statusBar()->showMessage(QStringLiteral("录像命令失败: ") + resp, 5000);
        return;
    }
    if (on) {
        m_recAction->setText(QStringLiteral("■ 停止"));
        m_recState = QStringLiteral("● 录像中");
        m_recTimer->start();
        statusBar()->showMessage(QStringLiteral("录像中(含预录30s): ") + resp, 5000);
    } else {
        m_recAction->setText(QStringLiteral("● 录像"));
        m_recState = QStringLiteral("未录像");
        m_recTimer->stop();
        statusBar()->showMessage(QStringLiteral("录像已保存: ") + resp, 8000);
    }
    updateInfoPanel();
}
