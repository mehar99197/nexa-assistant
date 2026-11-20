#include "app/ActivityStream.h"

#include "core/mission/MissionControl.h"

#include <QButtonGroup>
#include <QHBoxLayout>
#include <QLabel>
#include <QListView>
#include <QPainter>
#include <QPushButton>
#include <QScrollBar>
#include <QStyledItemDelegate>
#include <QVBoxLayout>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

constexpr int kLineHeight = 22;
constexpr int kMaxRows = 500;

// One line: the time, the lane in its colour, what happened.
class ActivityLineDelegate : public QStyledItemDelegate {
public:
    explicit ActivityLineDelegate(const MissionPanel *panel) : QStyledItemDelegate(const_cast<MissionPanel *>(panel)), panel_(panel) {}

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &) const override {
        return {option.rect.width(), kLineHeight};
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        const QRectF row = option.rect;
        const auto lane = Lane(index.data(ActivityModel::LaneRole).toInt());
        painter->save();
        painter->setFont(panel_->mono(8));
        painter->setPen(panel_->mutedColor());
        painter->drawText(QRectF(row.left() + 2, row.top(), 64, row.height()), Qt::AlignLeft | Qt::AlignVCenter,
                          index.data(ActivityModel::TimeRole).toString());
        painter->setFont(panel_->mono(8, true));
        painter->setPen(panel_->laneColor(lane));
        painter->drawText(QRectF(row.left() + 68, row.top(), 62, row.height()), Qt::AlignLeft | Qt::AlignVCenter,
                          laneName(lane));
        painter->setFont(panel_->mono(8));
        painter->setPen(index.data(ActivityModel::OkRole).toBool() ? panel_->textColor() : panel_->errorColor());
        const QRectF text(row.left() + 132, row.top(), row.width() - 134, row.height());
        painter->drawText(text, Qt::AlignLeft | Qt::AlignVCenter,
                          QFontMetrics(panel_->mono(8)).elidedText(index.data(Qt::DisplayRole).toString(), Qt::ElideRight,
                                                                   int(text.width())));
        painter->restore();
    }

private:
    const MissionPanel *panel_;
};

} // namespace

ActivityModel::ActivityModel(MissionControl *mission, QObject *parent) : QAbstractListModel(parent), mission_(mission) {
    rows_ = mission_->feed().events();
    connect(mission_, &MissionControl::activity, this, [this](const ActivityEvent &event) {
        if (lane_ && event.lane != *lane_) return;
        if (rows_.size() >= kMaxRows) {
            beginRemoveRows({}, 0, 0);
            rows_.removeFirst();
            endRemoveRows();
        }
        beginInsertRows({}, int(rows_.size()), int(rows_.size()));
        rows_.append(event);
        endInsertRows();
    });
}

void ActivityModel::setLane(std::optional<Lane> lane) {
    beginResetModel();
    lane_ = lane;
    rows_ = mission_->feed().events(lane);
    endResetModel();
}

int ActivityModel::rowCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : int(rows_.size());
}

QVariant ActivityModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() >= rows_.size()) return {};
    const ActivityEvent &event = rows_.at(index.row());
    switch (role) {
    case Qt::DisplayRole: return event.agentId.isEmpty() ? event.text : u"%1 %2"_s.arg(event.agentId, event.text);
    case Qt::ToolTipRole: return event.text;
    case LaneRole: return int(event.lane);
    case TimeRole: return event.time.toString(u"HH:mm:ss"_s);
    case OkRole: return event.ok;
    default: return {};
    }
}

ActivityStream::ActivityStream(MissionControl *mission, QWidget *parent) : MissionPanel(parent), mission_(mission) {
    setObjectName(u"missionStream"_s);
    auto *column = new QVBoxLayout(this);
    column->setContentsMargins(14, 14, 14, 14);
    column->setSpacing(10);

    auto *header = new QHBoxLayout;
    auto *title = new QLabel(tr("// LIVE ACTIVITY"), this);
    title->setObjectName(u"sectionTitle"_s);
    auto *streaming = new QLabel(tr("● STREAMING"), this);
    streaming->setObjectName(u"streaming"_s);
    header->addWidget(title);
    header->addStretch();
    header->addWidget(streaming);
    column->addLayout(header);

    model_ = new ActivityModel(mission_, this);
    auto *chipRow = new QHBoxLayout;
    chipRow->setSpacing(4);
    chips_ = new QButtonGroup(this);
    chips_->setExclusive(true);
    const QList<std::pair<QString, std::optional<Lane>>> lanes{
        {tr("ALL"), std::nullopt},     {u"WEB"_s, Lane::Web},     {u"CODE"_s, Lane::Code},
        {u"SCREEN"_s, Lane::Screen},   {u"PHONE"_s, Lane::Phone}, {u"VOICE"_s, Lane::Voice}};
    for (const auto &[name, lane] : lanes) {
        auto *chip = new QPushButton(name, this);
        chip->setObjectName(u"laneChip"_s);
        chip->setCheckable(true);
        chip->setCursor(Qt::PointingHandCursor);
        chip->setChecked(!lane.has_value());
        chips_->addButton(chip);
        chipRow->addWidget(chip);
        connect(chip, &QPushButton::toggled, this, [this, lane](bool on) {
            if (on) model_->setLane(lane);
        });
    }
    column->addLayout(chipRow);

    approvals_ = new QVBoxLayout;
    approvals_->setSpacing(8);
    column->addLayout(approvals_);

    lines_ = new QListView(this);
    lines_->setObjectName(u"activityList"_s);
    lines_->setModel(model_);
    lines_->setItemDelegate(new ActivityLineDelegate(this));
    lines_->setUniformItemSizes(true);
    lines_->setSelectionMode(QAbstractItemView::NoSelection);
    column->addWidget(lines_, 1);
    // Newest last, followed as they come unless the user scrolled up to read.
    connect(model_, &QAbstractItemModel::rowsInserted, this, [this] {
        QScrollBar *bar = lines_->verticalScrollBar();
        if (bar->value() >= bar->maximum() - kLineHeight * 2) lines_->scrollToBottom();
    });
    connect(model_, &QAbstractItemModel::modelReset, lines_, &QListView::scrollToBottom);
    connect(this, &MissionPanel::coloursChanged, lines_->viewport(), [this] { lines_->viewport()->update(); });

    connect(mission_, &MissionControl::approvalRequested, this, &ActivityStream::addApproval);
    connect(mission_, &MissionControl::approvalSettled, this,
            [this](quint64, quint64 actionId, bool) { dropApproval(actionId); });
    for (const Approval &waiting : mission_->approvals()) addApproval(waiting.task, waiting.actionId);
}

void ActivityStream::addApproval(quint64 task, quint64 actionId) {
    if (cards_.contains(actionId)) return;
    const QList<Approval> waiting = mission_->approvals();
    const auto found = std::find_if(waiting.cbegin(), waiting.cend(), [actionId](const Approval &a) { return a.actionId == actionId; });
    if (found == waiting.cend()) return;
    const Task *asking = mission_->task(task);

    auto *card = new QFrame(this);
    card->setObjectName(u"approvalCard"_s);
    auto *inside = new QVBoxLayout(card);
    inside->setContentsMargins(12, 10, 12, 12);
    inside->setSpacing(6);
    auto *heading = new QLabel(tr("● APPROVAL NEEDED · %1").arg(found->agentId), card);
    heading->setObjectName(u"approvalTitle"_s);
    // The question's first line in words, and what it would run with; never more than a few lines here.
    const QString what = found->question.section(u'\n', 0, 0);
    QString detail = found->question.section(u'\n', 1).simplified();
    if (detail.size() > 180) detail = detail.left(179) + u"…"_s;
    auto *question = new QLabel(asking ? u"%1 — %2"_s.arg(what, asking->title) : what, card);
    question->setObjectName(u"approvalQuestion"_s);
    question->setWordWrap(true);
    auto *args = new QLabel(detail, card);
    args->setObjectName(u"approvalDetail"_s);
    args->setWordWrap(true);
    args->setTextInteractionFlags(Qt::TextSelectableByMouse);
    inside->addWidget(heading);
    inside->addWidget(question);
    inside->addWidget(args);
    if (!found->risk.isEmpty()) {
        auto *risk = new QLabel(tr("Risk: %1").arg(found->risk), card);
        risk->setObjectName(u"approvalDetail"_s);
        risk->setWordWrap(true);
        inside->addWidget(risk);
    }
    auto *buttons = new QHBoxLayout;
    auto *approve = new QPushButton(tr("Approve"), card);
    approve->setObjectName(u"approve"_s);
    approve->setCursor(Qt::PointingHandCursor);
    auto *deny = new QPushButton(tr("Deny"), card);
    deny->setObjectName(u"deny"_s);
    deny->setCursor(Qt::PointingHandCursor);
    connect(approve, &QPushButton::clicked, this, [this, actionId] { emit approved(actionId); });
    connect(deny, &QPushButton::clicked, this, [this, actionId] { emit denied(actionId); });
    buttons->addWidget(approve, 1);
    buttons->addWidget(deny, 1);
    inside->addLayout(buttons);
    approvals_->addWidget(card);
    cards_.insert(actionId, card);
}

void ActivityStream::dropApproval(quint64 actionId) {
    if (QWidget *card = cards_.take(actionId)) card->deleteLater();
}

} // namespace nexa
