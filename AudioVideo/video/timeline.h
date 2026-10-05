#ifndef TIMELINE_H
#define TIMELINE_H

// ============================================================================
// 回放时间轴控件(自绘 QWidget,对标专业 NVR 的底部 24h 时间轴):
//   - 录像段:蓝色色块(有录像的时间范围)
//   - 事件标记:琥珀色 ▲ 刻度(录制中 REC_MARK 打的点)
//   - 播放位置:青色竖线
//   - 交互:鼠标按下/拖动 → seek 信号;滚轮 → 缩放;时间标尺自适应刻度
// 数据模型:以"当天 0 点"为原点的毫秒时刻;多段录像/标记通过 setSegments 载入
// ============================================================================
#include <QWidget>
#include <QVector>
#include <QElapsedTimer>

struct TlSegment {                       // 一段录像
    qint64 startMs;                      // 当天 0 点起算的毫秒
    qint64 durationMs;
    QString path;                        // 对应的本地 MP4 路径
};
struct TlMark {                          // 一个事件标记
    qint64 atMs;                         // 当天 0 点起算的毫秒
    QString text;
};

class Timeline : public QWidget
{
    Q_OBJECT
public:
    explicit Timeline(QWidget *parent = nullptr);

    void setSegments(const QVector<TlSegment> &segs);
    void setMarks(const QVector<TlMark> &marks);
    void setPositionMs(qint64 dayMs);    // 播放位置(外部播放器驱动)
    qint64 positionMs() const { return m_posMs; }

    // 当前窗口可见范围(毫秒)
    qint64 viewStartMs() const { return m_viewStart; }
    qint64 viewSpanMs() const { return m_viewSpan; }

signals:
    void seekRequested(qint64 dayMs);    // 用户点/拖时间轴
    void segmentClicked(const QString &path, qint64 offsetMs);  // 点中某段录像

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void wheelEvent(QWheelEvent *) override;
    QSize minimumSizeHint() const override { return QSize(200, 84); }

private:
    qint64 xToDayMs(int x) const;        // 窗口 x 像素 → 当天毫秒
    int dayMsToX(qint64 dayMs) const;    // 当天毫秒 → 窗口 x 像素
    void drawRuler(QPainter &p);
    void pickSegmentAt(int x, int y);     // 命中检测:录像段条带

    QVector<TlSegment> m_segs;
    QVector<TlMark> m_marks;
    qint64 m_posMs = 0;
    qint64 m_viewStart = 0;              // 可见窗口起点(天毫秒)
    qint64 m_viewSpan = 24 * 3600 * 1000LL;   // 初始 = 整天
    bool m_dragging = false;
};

#endif // TIMELINE_H
