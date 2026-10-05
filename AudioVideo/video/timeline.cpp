// ============================================================================
// 回放时间轴实现:纯 QPainter 自绘,无第三方依赖
// 配色与全局深色 QSS 一致(录像段=蓝,标记=琥珀,游标=青)
// ============================================================================
#include "timeline.h"
#include <QPainter>
#include <QMouseEvent>
#include <QWheelEvent>
#include <cmath>

Timeline::Timeline(QWidget *parent) : QWidget(parent)
{
    setMouseTracking(true);          // 悬停也收 mouseMove(拖动顺滑)
    setMinimumHeight(84);
}

void Timeline::setSegments(const QVector<TlSegment> &segs) { m_segs = segs; update(); }
void Timeline::setMarks(const QVector<TlMark> &marks)     { m_marks = marks; update(); }
void Timeline::setPositionMs(qint64 dayMs)                 { m_posMs = dayMs; update(); }

// 毫秒 → x 像素(当前可见窗口内线性映射)
int Timeline::dayMsToX(qint64 dayMs) const
{
    const int w = width();
    if (m_viewSpan <= 0) return 0;
    double f = double(dayMs - m_viewStart) / double(m_viewSpan);
    return int(f * w);
}

qint64 Timeline::xToDayMs(int x) const
{
    const int w = width();
    if (w <= 0) return m_viewStart;
    return m_viewStart + qint64(double(x) / w * m_viewSpan);
}

void Timeline::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);

    const int W = width(), H = height();
    const int rulerH = 20;           // 顶部时间标尺
    const int barY = rulerH + 6;     // 录像段条带 y
    const int barH = 22;

    // 背景
    p.fillRect(rect(), QColor(0x12, 0x15, 0x1a));

    // ---- 顶部标尺:按窗口跨度自适应刻度(24h→3h/6h,1h→10/15min) ----
    p.fillRect(0, 0, W, rulerH, QColor(0x23, 0x28, 0x30));
    p.setPen(QColor(0x6b, 0x76, 0x84));
    QFont f = font(); f.setPixelSize(10); p.setFont(f);
    const qint64 day0 = 0, dayEnd = 24 * 3600 * 1000LL;
    qint64 tick;
    qint64 step = m_viewSpan / 6;                    // 约 6 根主刻度
    // 取整到好看的步长(1/5/10/15/30min,1/2/3/6h)
    static const qint64 kSteps[] = {
        60 * 1000LL, 5 * 60 * 1000LL, 10 * 60 * 1000LL, 15 * 60 * 1000LL,
        30 * 60 * 1000LL, 3600 * 1000LL, 3 * 3600 * 1000LL, 6 * 3600 * 1000LL };
    for (int i = 8 - 1; i >= 0; i--)
        if (kSteps[i] <= step) { step = kSteps[i]; break; }
    if (step < 60 * 1000LL) step = 60 * 1000LL;
    const int h = int(m_viewStart / 3600000), mi = int(m_viewStart % 3600000 / 60000);
    (void)h; (void)mi;
    tick = (m_viewStart / step) * step;
    for (; tick <= dayEnd; tick += step) {
        if (tick < day0) continue;
        int x = dayMsToX(tick);
        if (x < -40 || x > W + 40) continue;
        bool major = (tick % (step * (m_viewSpan > 6 * 3600 * 1000LL ? 2 : 1))) == 0;
        p.setPen(QColor(0x34, 0x3b, 0x46));
        p.drawLine(x, rulerH - 6, x, rulerH);
        if (major) {
            p.setPen(QColor(0x9a, 0xa5, 0xb1));
            int hh = int(tick / 3600000), mm = int(tick % 3600000 / 60000);
            p.drawText(x + 3, rulerH - 8, QString("%1:%2")
                           .arg(hh, 2, 10, QLatin1Char('0'))
                           .arg(mm, 2, 10, QLatin1Char('0')));
        }
    }

    // ---- 录像段色块(蓝,点中发亮) ----
    for (const auto &s : m_segs) {
        int x0 = dayMsToX(s.startMs), x1 = dayMsToX(s.startMs + s.durationMs);
        if (x1 < 0 || x0 > W) continue;
        p.fillRect(std::max(x0, 0), barY, std::min(x1, W) - std::max(x0, 0), barH,
                   QColor(0x2f, 0x6f, 0xd0));
    }
    // 条带外框
    p.setPen(QColor(0x34, 0x3b, 0x46));
    p.drawRect(0, barY, W - 1, barH);

    // ---- 事件标记:琥珀色 ▲(画在条带下沿) ----
    p.setRenderHint(QPainter::Antialiasing, true);
    for (const auto &m : m_marks) {
        int x = dayMsToX(m.atMs);
        if (x < -8 || x > W + 8) continue;
        QPolygon tri;
        tri << QPoint(x - 5, barY + barH) << QPoint(x + 5, barY + barH)
            << QPoint(x, barY + barH + 8);
        p.setPen(QColor(0xff, 0xb0, 0x20));
        p.setBrush(QColor(0xff, 0xb0, 0x20));
        p.drawPolygon(tri);
    }
    p.setRenderHint(QPainter::Antialiasing, false);

    // ---- 播放游标(青色竖线贯穿) ----
    int px = dayMsToX(m_posMs);
    if (px >= 0 && px <= W) {
        p.setPen(QColor(0x30, 0xd5, 0xc8));
        p.drawLine(px, 0, px, H);
        p.fillRect(px - 3, 0, 7, rulerH, QColor(0x30, 0xd5, 0xc8));
    }
}

void Timeline::mousePressEvent(QMouseEvent *e)
{
    if (e->button() != Qt::LeftButton) return;
    m_dragging = true;
    qint64 t = xToDayMs(int(e->x()));
    setPositionMs(t);
    // 命中录像段 → 通知(点击即跳转该段)
    const int barY = 26, barH = 22;
    if (e->y() >= barY && e->y() <= barY + barH) {
        for (const auto &s : m_segs) {
            qint64 end = s.startMs + s.durationMs;
            if (t >= s.startMs && t <= end) {
                emit segmentClicked(s.path, t - s.startMs);
                break;
            }
        }
    }
    emit seekRequested(t);
}

void Timeline::mouseMoveEvent(QMouseEvent *e)
{
    if (!m_dragging) return;
    qint64 t = xToDayMs(int(e->x()));
    setPositionMs(t);
    emit seekRequested(t);
}

void Timeline::mouseReleaseEvent(QMouseEvent *)
{ m_dragging = false; }

// 滚轮缩放:以鼠标位置为中心放大/缩小
void Timeline::wheelEvent(QWheelEvent *e)
{
    qint64 anchor = xToDayMs(int(e->x()));
    double k = (e->angleDelta().y() > 0) ? 0.7 : 1.4;
    qint64 newSpan = qint64(m_viewSpan * k);
    if (newSpan < 5 * 60 * 1000LL) newSpan = 5 * 60 * 1000LL;         // 最小 5min
    if (newSpan > 24 * 3600 * 1000LL) newSpan = 24 * 3600 * 1000LL;   // 最大 24h
    double f = double(anchor - m_viewStart) / double(m_viewSpan);
    m_viewSpan = newSpan;
    m_viewStart = anchor - qint64(f * newSpan);
    if (m_viewStart < 0) m_viewStart = 0;
    if (m_viewStart + m_viewSpan > 24 * 3600 * 1000LL)
        m_viewStart = 24 * 3600 * 1000LL - m_viewSpan;
    update();
}

void Timeline::pickSegmentAt(int, int) {}   // 保留:未来 hover 高亮
