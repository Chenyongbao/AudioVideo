#ifndef VIDEOWIDGET_H
#define VIDEOWIDGET_H

// 视频渲染控件：QOpenGLWidget 纹理上传渲染。
// UI 线程内渲染；解码线程通过信号槽（队列连接）把 QImage 投递过来。
// 设计要点：
//  - presentFrame() 只存 QImage + update()，真正的纹理上传在 paintGL()（持有 GL 上下文的线程）；
//  - 支持保持宽高比的 letterbox 显示（黑边填充）；
//  - 丢帧策略：若上一帧还没渲染完，直接覆盖 m_pending（保留最新帧，渲染旧的没意义）。

#include <QOpenGLWidget>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QImage>
#include <QMutex>
#include <QElapsedTimer>
#include <QList>
#include <QRectF>
#include <QString>

// 检测框叠加(帧坐标系,如 640x480;paintGL 内按 letterbox+mirror 映射到控件)
struct DetOverlayBox {
    QRectF r;
    QString label;
};

class VideoWidget : public QOpenGLWidget, protected QOpenGLFunctions
{
    Q_OBJECT
public:
    explicit VideoWidget(QWidget *parent = nullptr);
    ~VideoWidget() override;

    // 显示叠加信息（如连接状态/帧率），画在左上角
    void setOverlayText(const QString &text);

    // 水平镜像（自拍预览用）：翻转纹理 U 坐标,画面左右对调
    void setMirror(bool on);

    // 延迟测量开关:左上角 48px 色块随本机时钟 3s 周期跳变(0-2s 白/2-3s 黑),
    // 与窗口边框同一 timer 驱动。手机拍屏后对比"视频里色块跳变"与"边框跳变"的相位差。
    void setLatencyTest(bool on);

    // NPU 检测框叠加:仅画在预览上,永不进入编码帧(取证画面保持原始)
    void setDetections(const QList<DetOverlayBox> &d);

public slots:
    // 解码线程 -> UI 线程的帧入口（Qt::QueuedConnection）
    void presentFrame(const QImage &img);

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;

private:
    void ensureTextureSize(int w, int h);

    // 双缓冲：m_pending 由信号槽投递写入（UI 线程），paintGL 消费
    QImage m_pending;
    QImage m_frame;          // 当前正在渲染的帧
    QMutex m_pendingMutex;   // presentFrame 与 paintGL 同在 UI 线程，理论可省；
                             // 保留以允许未来"渲染独立线程"演进，成本可忽略

    QOpenGLShaderProgram m_program;
    GLuint m_texture = 0;
    int m_texW = 0, m_texH = 0;
    QString m_overlay;
    bool m_mirror = false;   // 水平镜像标志

    // 延迟测量:test 模式下用同一本机时钟驱动色块与边框,3s 周期跳变
    bool m_latencyTest = false;
    QElapsedTimer m_latTimer;
    class QTimer *m_latTick = nullptr;

    QList<DetOverlayBox> m_dets;   // 最新检测框(帧坐标,UI 线程读写)
};

#endif // VIDEOWIDGET_H
