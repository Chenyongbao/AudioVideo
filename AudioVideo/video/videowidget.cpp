#include "videowidget.h"

#include <QPainter>
#include <QTimer>
#include <QDateTime>
#include <cstdio>

// 顶点着色器：全屏四边形 + 纹理坐标
static const char *kVsh =
    "attribute vec2 aPos;\n"
    "attribute vec2 aTexCoord;\n"
    "varying vec2 vTexCoord;\n"
    "void main() {\n"
    "    gl_Position = vec4(aPos, 0.0, 1.0);\n"
    "    vTexCoord = aTexCoord;\n"
    "}\n";

// 片元着色器：采样纹理 + 左上角叠加文字alpha混合（文字预渲染进 QImage）
static const char *kFsh =
    "uniform sampler2D uTex;\n"
    "varying vec2 vTexCoord;\n"
    "void main() {\n"
    "    gl_FragColor = texture2D(uTex, vTexCoord);\n"
    "}\n";

VideoWidget::VideoWidget(QWidget *parent)
    : QOpenGLWidget(parent)
{
    // Qt5 默认兼容 profile，直接用固定管线+着色器混合即可
    setUpdateBehavior(QOpenGLWidget::NoPartialUpdate);
}

VideoWidget::~VideoWidget()
{
    makeCurrent();
    if (m_texture)
        glDeleteTextures(1, &m_texture);
    doneCurrent();
}

void VideoWidget::setMirror(bool on)
{
    if (m_mirror == on)
        return;
    m_mirror = on;
    update();
}

void VideoWidget::setDetections(const QList<DetOverlayBox> &d)
{
    m_dets = d;   // UI 线程读写(paintGL 同线程),直接赋值
    update();
}

void VideoWidget::setLatencyTest(bool on)
{
    if (m_latencyTest == on)
        return;
    m_latencyTest = on;
    if (on && !m_latTimer.isValid())
        m_latTimer.start();
    // 30ms 定时重绘:让色块/边框跳变平滑刷新(仅 test 模式跑)
    if (on && !m_latTick) {
        m_latTick = new QTimer(this);
        connect(m_latTick, &QTimer::timeout, this, QOverload<>::of(&QWidget::update));
        m_latTick->start(30);
    }
    if (m_latTick)
        on ? m_latTick->start(30) : m_latTick->stop();
    update();
}

void VideoWidget::setOverlayText(const QString &text)
{
    m_overlay = text;
    update();
}

void VideoWidget::presentFrame(const QImage &img)
{
    {
        QMutexLocker lk(&m_pendingMutex);
        m_pending = img;   // 直接覆盖：只保留最新帧（天然丢帧策略）
    }
    update();              // 请求重绘，paintGL 消费
}

void VideoWidget::initializeGL()
{
    initializeOpenGLFunctions();

    m_program.addShaderFromSourceCode(QOpenGLShader::Vertex, kVsh);
    m_program.addShaderFromSourceCode(QOpenGLShader::Fragment, kFsh);
    m_program.bindAttributeLocation("aPos", 0);
    m_program.bindAttributeLocation("aTexCoord", 1);
    m_program.link();

    glGenTextures(1, &m_texture);
    glBindTexture(GL_TEXTURE_2D, m_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    m_texW = m_texH = 0;
}

void VideoWidget::resizeGL(int w, int h)
{
    Q_UNUSED(w);
    Q_UNUSED(h);
}

void VideoWidget::ensureTextureSize(int w, int h)
{
    if (m_texW == w && m_texH == h)
        return;
    glBindTexture(GL_TEXTURE_2D, m_texture);
    // 首次/尺寸变化时分配存储；glTexSubImage2D 更新更快
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    m_texW = w;
    m_texH = h;
}

void VideoWidget::paintGL()
{
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    {
        QMutexLocker lk(&m_pendingMutex);
        if (!m_pending.isNull()) {
            m_frame = m_pending;   // 取最新帧
            m_pending = QImage();  // 清空，避免重复上传
        }
    }

    if (m_frame.isNull())
        return;

    const int fw = m_frame.width(), fh = m_frame.height();

    m_program.bind();
    ensureTextureSize(fw, fh);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_texture);

    // QImage -> GL 纹理（RGBA8888 与 GL_RGBA/UNSIGNED_BYTE 直接对齐）
    QImage rgb = m_frame.convertToFormat(QImage::Format_RGBA8888);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, fw, fh, GL_RGBA, GL_UNSIGNED_BYTE,
                    rgb.constBits());

    // letterbox：保持宽高比居中显示
    const int ww = width(), wh = height();
    float sx = float(ww) / fw, sy = float(wh) / fh;
    float scale = (sx < sy) ? sx : sy;
    float vw = fw * scale / ww * 2.0f;   // NDC 半宽
    float vh = fh * scale / wh * 2.0f;   // NDC 半高

    // V 坐标上下翻转：OpenGL 纹理原点在左下角，而 QImage 第一行是图像顶部。
    // U 坐标水平翻转：自拍预览镜像（m_mirror=on 时左右对调，产生"照镜子"方向感）。
    // 同时不能加 static —— kVerts 依赖每次变化的 vw/vh（letterbox）。
    const float u0 = m_mirror ? 1.0f : 0.0f;   // 左列纹理 U
    const float u1 = m_mirror ? 0.0f : 1.0f;   // 右列纹理 U
    const float kVerts[] = {
        // x      y      u     v
        -vw,  -vh,  u0, 1.0f,
         vw,  -vh,  u1, 1.0f,
         vw,   vh,  u1, 0.0f,
        -vw,   vh,  u0, 0.0f,
    };
    // 两个三角形的索引
    static const unsigned short kIdx[] = {0, 1, 2, 0, 2, 3};

    m_program.enableAttributeArray(0);
    m_program.setAttributeArray(0, GL_FLOAT, kVerts, 2, 4 * sizeof(float));
    m_program.enableAttributeArray(1);
    m_program.setAttributeArray(1, GL_FLOAT, kVerts + 2, 2, 4 * sizeof(float));
    m_program.setUniformValue("uTex", 0);

    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, kIdx);

    m_program.disableAttributeArray(0);
    m_program.disableAttributeArray(1);
    m_program.release();

    // 叠加信息（连接状态/帧率/检测框/延迟测量）用 QPainter 画在 GL 之上
    if (!m_overlay.isEmpty() || m_latencyTest || !m_dets.isEmpty()) {
        QPainter p(this);
        if (!m_overlay.isEmpty()) {
            p.setPen(Qt::green);
            p.setFont(QFont("Consolas", 10));
            p.drawText(12, 20, m_overlay);
        }

        // NPU 检测框:帧坐标 → 控件坐标(letterbox 居中 + 可选镜像)
        // 仅叠加在预览 widget 上,绝不进入编码帧(取证画面保持原始)
        if (!m_dets.isEmpty() && !m_frame.isNull()) {
            const int fw = m_frame.width(), fh = m_frame.height();
            const int ww = width(), wh = height();
            float scale = std::min(float(ww) / fw, float(wh) / fh);
            float ox = (ww - fw * scale) / 2.0f, oy = (wh - fh * scale) / 2.0f;
            QPen pen(QColor(0x30, 0xd5, 0xc8), 2);   // 青色框(与时间轴游标同色系)
            p.setPen(pen);
            p.setFont(QFont("Consolas", 9));
            for (const auto &d : m_dets) {
                float x1, x2;                        // 镜像:水平翻转
                if (m_mirror) { x1 = ox + (fw - d.r.right()) * scale; x2 = ox + (fw - d.r.left()) * scale; }
                else          { x1 = ox + d.r.left() * scale;        x2 = ox + d.r.right() * scale; }
                float y1 = oy + d.r.top() * scale, y2 = oy + d.r.bottom() * scale;
                p.drawRect(QRectF(x1, y1, x2 - x1, y2 - y1));
                p.drawText(QPointF(x1, y1 - 4), d.label);
            }
        }

        // 延迟测量元素:色块(远端,随视频来)与边框(本机)都以 epoch 毫秒 %3000 驱动,
        // 同相基准(WSL2 与 Windows 共享系统时钟)→ 相位差 = 纯链路延迟
        if (m_latencyTest) {
            const qint64 t = QDateTime::currentMSecsSinceEpoch() % 3000;
            const bool white = t < 2000;   // 0-2s 白,2-3s 黑
            const QColor c = white ? Qt::white : Qt::black;

            // 左上角 48px 色块(避开 overlay 文字区,放其下方)
            p.fillRect(12, 30, 48, 48, c);
            p.setPen(white ? Qt::black : Qt::white);
            p.drawRect(12, 30, 48, 48);

            // 窗口边框 6px(与色块同相同色)
            p.setPen(QPen(c, 6));
            p.drawRect(rect().adjusted(3, 3, -3, -3));
        }
    }
}
