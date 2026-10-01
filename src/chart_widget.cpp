#include "chart_widget.h"
#include "domain.h"
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QToolTip>
#include <algorithm>
#include <cmath>

namespace fleet {
namespace {
QString number(double v, ChartWidget::Kind kind) {
    if (kind == ChartWidget::Kind::Percent)
        return QString::number(v, 'f', 0) + " %";
    if (kind == ChartWidget::Kind::Latency)
        return QString::number(v, 'f', 0) + " мс";
    if (kind != ChartWidget::Kind::Bandwidth)
        return QLocale(QLocale::Russian).toString(v, 'f', 0);
    if (v >= 1024 * 1024 * 1024)
        return QString::number(v / (1024 * 1024 * 1024), 'f', 1) + " ГБ/с";
    if (v >= 1024 * 1024)
        return QString::number(v / (1024 * 1024), 'f', 1) + " МБ/с";
    if (v >= 1024)
        return QString::number(v / 1024, 'f', 0) + " КБ/с";
    return QString::number(v, 'f', 0) + " Б/с";
}
QColor healthColor(int h) {
    switch (static_cast<Health>(h)) {
    case Health::Online:
        return QColor("#35b47d");
    case Health::Warning:
        return QColor("#dfb24b");
    case Health::Offline:
        return QColor("#ed6b7c");
    default:
        return QColor("#2b3544");
    }
}
} // namespace
ChartWidget::ChartWidget(Kind kind, QWidget* parent) : QWidget(parent), kind_(kind) {
    setMinimumHeight(200);
    setMinimumWidth(240);
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setAccessibleName(kind == Kind::Online ? "График клиентов онлайн" : "График сетевого трафика");
}
void ChartWidget::setPoints(QList<ChartPoint> points, QDateTime from, QDateTime to) {
    points_ = std::move(points);
    from_ = from;
    to_ = to;
    update();
}
void ChartWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF plot(58, 18, qMax(20, width() - 78), qMax(20, height() - 55));
    const qint64 start = from_.toMSecsSinceEpoch(), end = to_.toMSecsSinceEpoch();
    double maximum = 1;
    bool hasData = false;
    for (const auto& v : points_) {
        if (v.first) {
            maximum = std::max(maximum, *v.first);
            hasData = true;
        }
        if (v.second) {
            maximum = std::max(maximum, *v.second);
            hasData = true;
        }
    }
    maximum *= 1.2;
    if (kind_ == Kind::Percent)
        maximum = 100;
    if (kind_ == Kind::Online)
        maximum = std::max(5.0, std::ceil(maximum / 5) * 5);
    p.setFont(QFont("Segoe UI", 9));
    for (int i = 0; i <= 4; ++i) {
        const double y = plot.bottom() - plot.height() * i / 4;
        p.setPen(QPen(QColor("#27303d"), 1, Qt::DotLine));
        p.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
        p.setPen(QColor("#8b98a9"));
        QString label = number(maximum * i / 4, kind_);
        if (kind_ == Kind::Bandwidth)
            label.remove("/с");
        p.drawText(QRectF(0, y - 9, plot.left() - 9, 18), Qt::AlignRight | Qt::AlignVCenter, label);
    }
    for (int i = 0; i <= 4; ++i) {
        const qint64 time = start + (end - start) * i / 4;
        const double x = plot.left() + plot.width() * i / 4;
        p.setPen(QColor("#8b98a9"));
        const QString text = QDateTime::fromMSecsSinceEpoch(time).toString(
            end - start > 2LL * 24 * 3600000 ? "dd.MM" : "HH:mm");
        p.drawText(QRectF(x - 30, plot.bottom() + 12, 60, 20), Qt::AlignCenter, text);
    }
    if (!hasData || end <= start) {
        p.setPen(QColor("#8b98a9"));
        p.setFont(QFont("Segoe UI", 11));
        p.drawText(plot, Qt::AlignCenter, "Нет измерений за выбранный период");
        return;
    }
    auto coordinate = [&](const ChartPoint& v, double value) {
        return QPointF(plot.left() + plot.width() * double(v.time - start) / double(end - start),
                       plot.bottom() - plot.height() * value / maximum);
    };
    auto series = [&](bool second, QColor color) {
        QPainterPath line;
        QPointF first, last;
        bool begun = false;
        qint64 previous = 0;
        const qint64 gap = qMax<qint64>(120000, (end - start) / 240);
        auto finish = [&] {
            if (!begun)
                return;
            QPainterPath area = line;
            area.lineTo(last.x(), plot.bottom());
            area.lineTo(first.x(), plot.bottom());
            area.closeSubpath();
            QLinearGradient gradient(0, plot.top(), 0, plot.bottom());
            QColor upper = color;
            upper.setAlpha(75);
            QColor lower = color;
            lower.setAlpha(3);
            gradient.setColorAt(0, upper);
            gradient.setColorAt(1, lower);
            p.fillPath(area, gradient);
            p.setPen(QPen(color, 2));
            p.drawPath(line);
            p.setBrush(color);
            p.setPen(Qt::NoPen);
            p.drawEllipse(last, 2.5, 2.5);
            line = QPainterPath();
            begun = false;
        };
        p.save();
        p.setClipRect(plot.adjusted(-3, -3, 3, 3));
        for (const auto& v : points_) {
            const auto value = second ? v.second : v.first;
            if (!value || v.time < start || v.time > end) {
                finish();
                previous = 0;
                continue;
            }
            if (begun && v.time - previous > gap)
                finish();
            const QPointF xy = coordinate(v, *value);
            if (!begun) {
                line.moveTo(xy);
                first = xy;
                begun = true;
            } else
                line.lineTo(xy);
            last = xy;
            previous = v.time;
        }
        finish();
        p.restore();
    };
    series(false, QColor("#4c9dff"));
    if (kind_ == Kind::Bandwidth || kind_ == Kind::Percent)
        series(true, QColor("#46d8a9"));
    if (plot.contains(hover_) && !points_.isEmpty()) {
        const qint64 time =
            start + qint64((hover_.x() - plot.left()) / plot.width() * (end - start));
        const auto closest =
            std::min_element(points_.begin(), points_.end(), [&](const auto& a, const auto& b) {
                return std::abs(a.time - time) < std::abs(b.time - time);
            });
        const double x = coordinate(*closest, 0).x();
        p.setPen(QPen(QColor("#6e8098"), 1, Qt::DashLine));
        p.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
        QString tip =
            QDateTime::fromMSecsSinceEpoch(closest->time).toString("dd.MM HH:mm") + "  ·  ";
        tip += closest->first ? number(*closest->first, kind_) : "нет данных";
        if (kind_ == Kind::Bandwidth || kind_ == Kind::Percent)
            tip += " / " + (closest->second ? number(*closest->second, kind_) : "нет данных");
        const QFontMetrics fm(p.font());
        const int tw = qMin(width() - 16, fm.horizontalAdvance(tip) + 22);
        const QRectF box(qBound(8, hover_.x() - tw / 2, width() - tw - 8), 4, tw, 28);
        p.setPen(QColor("#38465a"));
        p.setBrush(QColor("#1d2837"));
        p.drawRoundedRect(box, 5, 5);
        p.setPen(QColor("#e4edf8"));
        p.drawText(box, Qt::AlignCenter, tip);
    }
}
void ChartWidget::mouseMoveEvent(QMouseEvent* e) {
    hover_ = e->position().toPoint();
    update();
}
void ChartWidget::leaveEvent(QEvent*) {
    hover_ = {-1, -1};
    update();
}

HealthBar::HealthBar(QWidget* parent) : QWidget(parent) {
    setMinimumWidth(170);
    setFixedHeight(22);
    setMouseTracking(true);
    setAccessibleName("История доступности сервера");
}
void HealthBar::setSamples(QList<QPair<qint64, int>> samples, QDateTime from, QDateTime to) {
    samples_ = std::move(samples);
    from_ = from;
    to_ = to;
    update();
}
void HealthBar::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    constexpr int count = 48;
    const double cell = double(width()) / count;
    const qint64 begin = from_.toMSecsSinceEpoch(),
                 duration = qMax<qint64>(1, to_.toMSecsSinceEpoch() - begin);
    for (int i = 0; i < count; ++i) {
        int h = int(Health::Unknown);
        bool found = false;
        for (const auto& s : samples_)
            if (s.first >= begin + duration * i / count &&
                s.first < begin + duration * (i + 1) / count) {
                if (!found || s.second > h)
                    h = s.second;
                found = true;
            }
        QColor color = healthColor(h);
        p.setBrush(color);
        p.setPen(Qt::NoPen);
        p.drawRoundedRect(QRectF(i * cell + 1, 6, qMax(1.0, cell - 2), 10), 2, 2);
    }
}
void HealthBar::mouseMoveEvent(QMouseEvent* e) {
    constexpr int count = 48;
    const int cell = qBound(0, int(e->position().x() * count / qMax(1, width())), count - 1);
    const qint64 begin = from_.toMSecsSinceEpoch(),
                 duration = qMax<qint64>(1, to_.toMSecsSinceEpoch() - begin);
    int h = int(Health::Unknown);
    for (const auto& s : samples_)
        if (s.first >= begin + duration * cell / count &&
            s.first < begin + duration * (cell + 1) / count) {
            h = qMax(h, s.second);
        }
    const QStringList labels = {"Нет измерений", "Работает", "Требует внимания", "Недоступен"};
    QToolTip::showText(
        e->globalPosition().toPoint(),
        QDateTime::fromMSecsSinceEpoch(begin + duration * cell / count).toString("dd.MM HH:mm") +
            " · " + labels.value(h),
        this);
}
} // namespace fleet
