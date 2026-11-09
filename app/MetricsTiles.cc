#include "app/MetricsTiles.h"

#include <QFrame>
#include <QGridLayout>
#include <QLabel>
#include <QVBoxLayout>

#include <cmath>

using namespace Qt::StringLiterals;

namespace nexa {

MetricsTiles::MetricsTiles(QWidget *parent) : QWidget(parent) {
    setObjectName(u"metricsTiles"_s);
    auto *grid = new QGridLayout(this);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(8);
    latency_ = tile(tr("LATENCY"), u"web"_s);
    tokens_ = tile(tr("TOKENS/S"), u"code"_s);
    cpu_ = tile(tr("CPU"), u"screen"_s);
    memory_ = tile(tr("MEMORY"), u"text"_s);
    grid->addWidget(latency_->parentWidget(), 0, 0);
    grid->addWidget(tokens_->parentWidget(), 0, 1);
    grid->addWidget(cpu_->parentWidget(), 1, 0);
    grid->addWidget(memory_->parentWidget(), 1, 1);
    for (QLabel *value : {latency_, tokens_, cpu_, memory_}) value->setText(u"—"_s);
}

QLabel *MetricsTiles::tile(const QString &label, const QString &tone) {
    auto *frame = new QFrame(this);
    frame->setObjectName(u"metricTile"_s);
    auto *column = new QVBoxLayout(frame);
    column->setContentsMargins(12, 8, 12, 8);
    column->setSpacing(2);
    auto *name = new QLabel(label, frame);
    name->setObjectName(u"tileLabel"_s);
    auto *value = new QLabel(frame);
    value->setObjectName(u"tileValue"_s);
    value->setProperty("tone", tone);
    column->addWidget(name);
    column->addWidget(value);
    return value;
}

void MetricsTiles::setLatency(qint64 ms) {
    latency_->setText(ms < 0 ? u"—"_s : ms < 10'000 ? u"%1 ms"_s.arg(ms) : u"%1 s"_s.arg(ms / 1000.0, 0, 'f', 1));
}

void MetricsTiles::setTokensPerSecond(double rate) {
    tokens_->setText(rate <= 0 ? u"—"_s : QString::number(int(std::lround(rate))));
}

void MetricsTiles::setProcess(double cpuPercent, qint64 residentBytes) {
    cpu_->setText(cpuPercent < 0 ? u"—"_s : u"%1%"_s.arg(int(std::lround(cpuPercent))));
    if (residentBytes <= 0) {
        memory_->setText(u"—"_s);
    } else if (residentBytes >= qint64(1) << 30) {
        memory_->setText(u"%1 GB"_s.arg(double(residentBytes) / double(qint64(1) << 30), 0, 'f', 1));
    } else {
        memory_->setText(u"%1 MB"_s.arg(residentBytes >> 20));
    }
}

QString MetricsTiles::latencyText() const {
    return latency_->text();
}

QString MetricsTiles::tokensText() const {
    return tokens_->text();
}

} // namespace nexa
