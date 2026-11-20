#pragma once

#include <QWidget>

class QLabel;

namespace nexa {

// Four tiles under the core: LATENCY (the newest task's time to its first word), TOKENS/S (its pace), CPU and MEMORY
// (Nexa and the programs it runs). "—" while there is nothing to tell.
class MetricsTiles : public QWidget {
    Q_OBJECT
public:
    explicit MetricsTiles(QWidget *parent = nullptr);

    void setLatency(qint64 ms);                 // < 0: none
    void setTokensPerSecond(double rate);       // <= 0: none
    void setProcess(double cpuPercent, qint64 residentBytes);

    QString latencyText() const;
    QString tokensText() const;

private:
    QLabel *tile(const QString &label, const QString &tone);

    QLabel *latency_;
    QLabel *tokens_;
    QLabel *cpu_;
    QLabel *memory_;
};

} // namespace nexa
