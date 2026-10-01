#pragma once
#include <QDateTime>
#include <QWidget>
#include <optional>

namespace fleet {
struct ChartPoint {
    qint64 time = 0;
    std::optional<double> first;
    std::optional<double> second;
};
class ChartWidget : public QWidget {
  public:
    enum class Kind { Online, Bandwidth, Percent, Latency };
    explicit ChartWidget(Kind kind, QWidget* parent = nullptr);
    void setPoints(QList<ChartPoint> points, QDateTime from, QDateTime to);

  protected:
    void paintEvent(QPaintEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void leaveEvent(QEvent*) override;

  private:
    Kind kind_;
    QList<ChartPoint> points_;
    QDateTime from_, to_;
    QPoint hover_{-1, -1};
};

class HealthBar : public QWidget {
  public:
    explicit HealthBar(QWidget* parent = nullptr);
    void setSamples(QList<QPair<qint64, int>> samples, QDateTime from, QDateTime to);

  protected:
    void paintEvent(QPaintEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;

  private:
    QList<QPair<qint64, int>> samples_;
    QDateTime from_, to_;
};
} // namespace fleet
