#pragma once

#include "app/MissionPanel.h"

class QLabel;

namespace nexa {

class MetricsTiles;
class NexaCoreWidget;
class SparklineWidget;

// Mission Control's centre card: the Nexa core with the state's name beside it ("Wake word on"), the microphone in a
// line ("MIC OK · −21 dB · 0 % clipped") and a meter of its level, what it heard last, and the metric tiles below.
class CoreCard : public MissionPanel {
    Q_OBJECT
public:
    explicit CoreCard(QWidget *parent = nullptr);

    NexaCoreWidget *core() const { return core_; }
    MetricsTiles *tiles() const { return tiles_; }
    void setState(const QString &title, const QString &hint);
    void setMicLine(const QString &line, bool trouble);
    void pushLevel(double level);           // 0 … 1, while the microphone is open
    void setLastHeard(const QString &text);

private:
    NexaCoreWidget *core_;
    QLabel *title_;
    QLabel *hint_;
    QLabel *mic_;
    SparklineWidget *wave_;
    QLabel *heard_;
    MetricsTiles *tiles_;
};

} // namespace nexa
