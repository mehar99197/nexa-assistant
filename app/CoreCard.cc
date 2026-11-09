#include "app/CoreCard.h"

#include "app/MetricsTiles.h"
#include "ui/NexaCoreWidget.h"
#include "ui/Sparkline.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QStyle>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

using namespace Qt::StringLiterals;

namespace nexa {

CoreCard::CoreCard(QWidget *parent) : MissionPanel(parent) {
    setObjectName(u"coreCard"_s);
    auto *column = new QVBoxLayout(this);
    column->setContentsMargins(16, 14, 16, 14);
    column->setSpacing(12);

    auto *top = new QHBoxLayout;
    top->setSpacing(18);
    core_ = new NexaCoreWidget(this);
    core_->setDiameter(104);
    core_->setShowNodes(true);
    top->addWidget(core_, 0, Qt::AlignVCenter);
    auto *words = new QVBoxLayout;
    words->setSpacing(4);
    auto *titleRow = new QHBoxLayout;
    titleRow->setSpacing(10);
    title_ = new QLabel(this);
    title_->setObjectName(u"coreTitle"_s);
    mic_ = new QLabel(this);
    mic_->setObjectName(u"micLine"_s);
    titleRow->addWidget(title_);
    titleRow->addWidget(mic_, 0, Qt::AlignBottom);
    titleRow->addStretch();
    words->addStretch();
    words->addLayout(titleRow);
    hint_ = new QLabel(this);
    hint_->setObjectName(u"stateHint"_s);
    hint_->setWordWrap(true);
    words->addWidget(hint_);
    wave_ = new SparklineWidget(this);
    wave_->setObjectName(u"waveform"_s);
    wave_->setFixedHeight(26);
    words->addWidget(wave_);
    heard_ = new QLabel(this);
    heard_->setObjectName(u"lastHeard"_s);
    heard_->setWordWrap(true);
    words->addWidget(heard_);
    words->addStretch();
    top->addLayout(words, 1);
    column->addLayout(top);

    tiles_ = new MetricsTiles(this);
    column->addWidget(tiles_);
    setLastHeard({});
}

void CoreCard::setState(const QString &title, const QString &hint) {
    title_->setText(title);
    hint_->setText(hint);
}

void CoreCard::setMicLine(const QString &line, bool trouble) {
    mic_->setText(line);
    mic_->setProperty("trouble", trouble);
    mic_->style()->unpolish(mic_);
    mic_->style()->polish(mic_);
}

void CoreCard::pushLevel(double level) {
    wave_->push(int(std::lround(std::clamp(level, 0.0, 1.0) * 100)));
}

void CoreCard::setLastHeard(const QString &text) {
    heard_->setText(text.isEmpty() ? tr("last heard › nothing yet") : tr("last heard › “%1”").arg(text));
}

} // namespace nexa
