#include "app/AgentsRail.h"

#include "app/TaskListModel.h"
#include "core/mission/MissionControl.h"
#include "ui/NexaIcons.h"
#include "ui/Sparkline.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QListView>
#include <QPainter>
#include <QPushButton>
#include <QStyledItemDelegate>
#include <QVBoxLayout>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

constexpr int kCardHeight = 96;

NexaIcons::Kind iconFor(AgentRole role) {
    switch (role) {
    case AgentRole::Web: return NexaIcons::Kind::Globe;
    case AgentRole::Code: return NexaIcons::Kind::Code;
    case AgentRole::Screen: return NexaIcons::Kind::Screen;
    case AgentRole::Phone: return NexaIcons::Kind::Phone;
    case AgentRole::General: return NexaIcons::Kind::Tools;
    }
    return NexaIcons::Kind::Tools;
}

// One agent's card (the rail's rows).
class AgentCardDelegate : public QStyledItemDelegate {
public:
    explicit AgentCardDelegate(const MissionPanel *panel) : QStyledItemDelegate(const_cast<MissionPanel *>(panel)), panel_(panel) {}

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &) const override {
        return {option.rect.width(), kCardHeight + 8};
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        const auto state = TaskState(index.data(TaskListModel::StateRole).toInt());
        const auto role = AgentRole(index.data(TaskListModel::RoleRole).toInt());
        const QColor roleColour = panel_->roleColor(role);
        const QRectF card = QRectF(option.rect).adjusted(1, 1, -1, -8);
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        const bool needsYou = state == TaskState::AwaitingYou;
        painter->setPen(QPen(needsYou ? panel_->warnColor() : panel_->lineColor(), needsYou ? 1.4 : 1));
        painter->setBrush(panel_->raisedColor());
        painter->drawRoundedRect(card, 8, 8);

        const QRectF iconBox(card.left() + 10, card.top() + 10, 28, 28);
        QColor iconFill = roleColour;
        iconFill.setAlphaF(0.12f);
        painter->setPen(Qt::NoPen);
        painter->setBrush(iconFill);
        painter->drawRoundedRect(iconBox, 6, 6);
        NexaIcons::paint(*painter, iconFor(role), iconBox.adjusted(5, 5, -5, -5), roleColour);

        const QString chip = state == TaskState::AwaitingYou ? u"AWAITING YOU"_s : taskStateName(state);
        const QFontMetricsF chipMetrics(panel_->mono(7.5, true));
        const double chipWidth = chipMetrics.horizontalAdvance(chip) + 14;
        const QRectF chipBox(card.right() - 10 - chipWidth, card.top() + 12, chipWidth, 18);
        panel_->paintChip(*painter, chipBox, chip, panel_->stateColor(state));

        const double textLeft = iconBox.right() + 10;
        const double textRight = chipBox.left() - 6;
        painter->setFont(panel_->mono(9, true));
        painter->setPen(roleColour);
        painter->drawText(QRectF(textLeft, card.top() + 8, textRight - textLeft, 16), Qt::AlignLeft | Qt::AlignVCenter,
                          index.data(TaskListModel::AgentIdRole).toString());
        QFont small = option.font;
        small.setPointSizeF(std::max(7.0, small.pointSizeF() - 1.5));
        painter->setFont(small);
        painter->setPen(panel_->mutedColor());
        const QFontMetrics smallMetrics(small);
        painter->drawText(QRectF(textLeft, card.top() + 24, textRight - textLeft, 14), Qt::AlignLeft | Qt::AlignVCenter,
                          smallMetrics.elidedText(index.data(TaskListModel::ProviderRole).toString(), Qt::ElideRight,
                                                  int(textRight - textLeft)));

        painter->setFont(option.font);
        painter->setPen(panel_->textColor());
        const QFontMetrics metrics(option.font);
        painter->drawText(QRectF(card.left() + 10, card.top() + 44, card.width() - 20, 18), Qt::AlignLeft | Qt::AlignVCenter,
                          metrics.elidedText(index.data(TaskListModel::TitleRole).toString(), Qt::ElideRight,
                                             int(card.width() - 20)));

        const QList<int> activity = index.data(TaskListModel::ActivityRole).value<QList<int>>();
        Sparkline::paint(*painter, QRectF(card.left() + 10, card.bottom() - 26, 100, 16), activity, roleColour, true, 4);
        painter->setFont(panel_->mono(8));
        painter->setPen(panel_->mutedColor());
        painter->drawText(QRectF(card.right() - 90, card.bottom() - 26, 80, 16), Qt::AlignRight | Qt::AlignVCenter,
                          index.data(TaskListModel::ElapsedRole).toString());
        painter->restore();
    }

private:
    const MissionPanel *panel_;
};

} // namespace

AgentsRail::AgentsRail(TaskListModel *tasks, QWidget *parent) : MissionPanel(parent), mission_(tasks->mission()) {
    setObjectName(u"missionRail"_s);
    auto *column = new QVBoxLayout(this);
    column->setContentsMargins(14, 14, 14, 14);
    column->setSpacing(10);

    auto *header = new QHBoxLayout;
    auto *title = new QLabel(tr("// AGENTS"), this);
    title->setObjectName(u"sectionTitle"_s);
    count_ = new QLabel(this);
    count_->setObjectName(u"sectionNote"_s);
    header->addWidget(title);
    header->addStretch();
    header->addWidget(count_);
    column->addLayout(header);

    // The voice is an agent of its own: it does not take tasks, it hears them.
    auto *voice = new QFrame(this);
    voice->setObjectName(u"voiceCard"_s);
    auto *voiceColumn = new QVBoxLayout(voice);
    voiceColumn->setContentsMargins(10, 8, 10, 8);
    voiceColumn->setSpacing(4);
    auto *voiceTop = new QHBoxLayout;
    auto *voiceName = new QLabel(u"VOICE"_s, voice);
    voiceName->setObjectName(u"voiceName"_s);
    voiceChip_ = new QLabel(voice);
    voiceChip_->setObjectName(u"voiceChip"_s);
    voiceTop->addWidget(voiceName);
    voiceTop->addStretch();
    voiceTop->addWidget(voiceChip_);
    voiceColumn->addLayout(voiceTop);
    voiceEngines_ = new QLabel(voice);
    voiceEngines_->setObjectName(u"sectionNote"_s);
    voiceLine_ = new QLabel(voice);
    voiceLine_->setObjectName(u"voiceLine"_s);
    voiceLine_->setWordWrap(true);
    voiceLevel_ = new SparklineWidget(voice);
    voiceLevel_->setObjectName(u"voiceLevel"_s);
    voiceLevel_->setFixedHeight(18);
    voiceColumn->addWidget(voiceEngines_);
    voiceColumn->addWidget(voiceLine_);
    voiceColumn->addWidget(voiceLevel_);
    column->addWidget(voice);

    list_ = new QListView(this);
    list_->setObjectName(u"agentList"_s);
    list_->setModel(new TaskFilter(TaskFilter::Kind::Agents, tasks, this));
    list_->setItemDelegate(new AgentCardDelegate(this));
    list_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    list_->setSelectionMode(QAbstractItemView::NoSelection);
    list_->setMouseTracking(true);
    list_->setCursor(Qt::PointingHandCursor);
    connect(list_, &QListView::clicked, this, [this](const QModelIndex &index) {
        emit taskActivated(index.data(TaskListModel::IdRole).toULongLong());
    });
    column->addWidget(list_, 1);

    auto *spawn = new QPushButton(tr("+  Spawn agent"), this);
    spawn->setObjectName(u"spawn"_s);
    spawn->setCursor(Qt::PointingHandCursor);
    spawn->setToolTip(tr("Add tasks: each runs as an agent of its own · Ctrl+T"));
    connect(spawn, &QPushButton::clicked, this, &AgentsRail::spawnRequested);
    column->addWidget(spawn);

    connect(mission_, &MissionControl::countsChanged, this, &AgentsRail::refreshCount);
    connect(this, &MissionPanel::coloursChanged, list_->viewport(), [this] { list_->viewport()->update(); });
    setVoice(tr("OFF"), tr("Hold Talk, or switch the wake word on in Settings → Voice."), {});
    refreshCount();
}

void AgentsRail::refreshCount() {
    const QList<Task> tasks = mission_->tasks();
    const auto active = std::count_if(tasks.cbegin(), tasks.cend(), [](const Task &task) {
        return holdsSlot(task.state) || task.state == TaskState::Delegating;
    });
    count_->setText(tr("%1 active · %2 / %3 slots").arg(active).arg(mission_->running()).arg(mission_->slotCount()));
}

void AgentsRail::setVoice(const QString &chip, const QString &line, const QString &engines) {
    voiceChip_->setText(chip);
    voiceLine_->setText(line);
    voiceEngines_->setText(engines);
    voiceEngines_->setVisible(!engines.isEmpty());
}

void AgentsRail::pushVoiceLevel(int percent) {
    voiceLevel_->push(std::clamp(percent, 0, 100));
}

} // namespace nexa
