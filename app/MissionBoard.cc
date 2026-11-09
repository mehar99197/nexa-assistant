#include "app/MissionBoard.h"

#include "app/TaskListModel.h"
#include "core/mission/MissionControl.h"
#include "ui/NexaIcons.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QListView>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QStyledItemDelegate>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

constexpr int kRowHeight = 66;
constexpr int kQueueRowHeight = 44;
constexpr int kButton = 34;

QRectF buttonBox(const QRectF &row) {
    return {row.right() - kButton - 10, row.center().y() - kButton / 2.0, kButton, kButton};
}

// A task's row: agent and state, title and last step, progress, time, and its button.
class TaskRowDelegate : public QStyledItemDelegate {
public:
    TaskRowDelegate(const MissionPanel *panel, std::function<void(quint64, TaskState)> pressed)
        : QStyledItemDelegate(const_cast<MissionPanel *>(panel)), panel_(panel), pressed_(std::move(pressed)) {}

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &) const override {
        return {option.rect.width(), kRowHeight + 8};
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        const auto state = TaskState(index.data(TaskListModel::StateRole).toInt());
        const auto role = AgentRole(index.data(TaskListModel::RoleRole).toInt());
        const QColor roleColour = panel_->roleColor(role);
        const int depth = index.data(TaskListModel::DepthRole).toInt();
        const QRectF row = QRectF(option.rect).adjusted(1 + depth * 18, 1, -1, -8);
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        const bool needsYou = state == TaskState::AwaitingYou || state == TaskState::Paused;
        painter->setPen(QPen(needsYou ? panel_->warnColor() : panel_->lineColor(), needsYou ? 1.3 : 1));
        painter->setBrush(panel_->raisedColor());
        painter->drawRoundedRect(row, 8, 8);

        // The agent and its state, in a column of their own.
        painter->setFont(panel_->mono(9, true));
        painter->setPen(roleColour);
        painter->drawText(QRectF(row.left() + 14, row.top() + 14, 100, 16), Qt::AlignLeft | Qt::AlignVCenter,
                          index.data(TaskListModel::AgentIdRole).toString());
        painter->setFont(panel_->mono(7.5, true));
        painter->setPen(panel_->stateColor(state));
        painter->drawText(QRectF(row.left() + 14, row.top() + 34, 100, 14), Qt::AlignLeft | Qt::AlignVCenter,
                          index.data(TaskListModel::StateTextRole).toString());

        const QRectF button = buttonBox(row);
        const double right = button.left() - 14;
        const double left = row.left() + 122;
        const double numbers = 64;      // the percent and the time, right-aligned
        QFont titleFont = option.font;
        titleFont.setBold(true);
        painter->setFont(titleFont);
        painter->setPen(panel_->textColor());
        const QFontMetrics titleMetrics(titleFont);
        painter->drawText(QRectF(left, row.top() + 10, right - numbers - left, 18), Qt::AlignLeft | Qt::AlignVCenter,
                          titleMetrics.elidedText(index.data(TaskListModel::TitleRole).toString(), Qt::ElideRight,
                                                  int(right - numbers - left)));
        painter->setFont(panel_->mono(8));
        painter->setPen(panel_->mutedColor());
        const QFontMetrics statusMetrics(panel_->mono(8));
        const QString status = u"▸ "_s + index.data(TaskListModel::SubStatusRole).toString();
        painter->drawText(QRectF(left, row.top() + 29, right - numbers - left, 14), Qt::AlignLeft | Qt::AlignVCenter,
                          statusMetrics.elidedText(status, Qt::ElideRight, int(right - numbers - left)));

        const int progress = index.data(TaskListModel::ProgressRole).toInt();
        const QRectF track(left, row.bottom() - 14, right - numbers - left - 8, 4);
        painter->setPen(Qt::NoPen);
        painter->setBrush(panel_->lineColor());
        painter->drawRoundedRect(track, 2, 2);
        painter->setBrush(state == TaskState::Failed ? panel_->errorColor() : roleColour);
        painter->drawRoundedRect(QRectF(track.left(), track.top(), track.width() * std::clamp(progress, 0, 100) / 100.0, 4), 2, 2);

        painter->setFont(panel_->mono(9, true));
        painter->setPen(panel_->textColor());
        painter->drawText(QRectF(right - numbers, row.top() + 12, numbers, 16), Qt::AlignRight | Qt::AlignVCenter,
                          u"%1%"_s.arg(progress));
        painter->setFont(panel_->mono(8));
        painter->setPen(panel_->mutedColor());
        painter->drawText(QRectF(right - numbers, row.top() + 32, numbers, 14), Qt::AlignRight | Qt::AlignVCenter,
                          index.data(TaskListModel::ElapsedRole).toString());

        painter->setPen(QPen(panel_->lineColor(), 1));
        painter->setBrush(Qt::NoBrush);
        painter->drawRoundedRect(button, 6, 6);
        NexaIcons::paint(*painter, iconFor(state), button.adjusted(10, 10, -10, -10), panel_->textColor());
        painter->restore();
    }

    bool editorEvent(QEvent *event, QAbstractItemModel *, const QStyleOptionViewItem &option,
                     const QModelIndex &index) override {
        if (event->type() != QEvent::MouseButtonRelease) return false;
        const auto *mouse = static_cast<QMouseEvent *>(event);
        const int depth = index.data(TaskListModel::DepthRole).toInt();
        const QRectF row = QRectF(option.rect).adjusted(1 + depth * 18, 1, -1, -8);
        if (!buttonBox(row).contains(mouse->position())) return false;
        pressed_(index.data(TaskListModel::IdRole).toULongLong(), TaskState(index.data(TaskListModel::StateRole).toInt()));
        return true;
    }

    // Pause while it works, resume while paused, clear once finished.
    static NexaIcons::Kind iconFor(TaskState state) {
        if (state == TaskState::Paused) return NexaIcons::Kind::Play;
        if (isFinished(state)) return NexaIcons::Kind::Cross;
        return NexaIcons::Kind::Pause;
    }

private:
    const MissionPanel *panel_;
    std::function<void(quint64, TaskState)> pressed_;
};

// A waiting task's row: its place, its title, the agent it will get, and × to take it off.
class QueueRowDelegate : public QStyledItemDelegate {
public:
    QueueRowDelegate(const MissionPanel *panel, std::function<void(quint64)> removed)
        : QStyledItemDelegate(const_cast<MissionPanel *>(panel)), panel_(panel), removed_(std::move(removed)) {}

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &) const override {
        return {option.rect.width(), kQueueRowHeight + 6};
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        const QRectF row = QRectF(option.rect).adjusted(1, 1, -1, -6);
        const auto role = AgentRole(index.data(TaskListModel::RoleRole).toInt());
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        painter->setPen(QPen(panel_->lineColor(), 1, Qt::DashLine));
        painter->setBrush(Qt::NoBrush);
        painter->drawRoundedRect(row, 8, 8);
        painter->setFont(panel_->mono(8));
        painter->setPen(panel_->mutedColor());
        const int place = index.data(TaskListModel::QueuePositionRole).toInt();
        painter->drawText(QRectF(row.left() + 14, row.top(), 100, row.height()), Qt::AlignLeft | Qt::AlignVCenter,
                          u"#%1 · QUEUED"_s.arg(place));
        const QRectF button(row.right() - 30 - 8, row.center().y() - 15, 30, 30);
        const QString next = u"%1 → %2%3"_s.arg(index.data(TaskListModel::ProviderRole).toString().section(u" · "_s, 0, 0).toLower(),
                                               index.data(TaskListModel::AgentIdRole).toString(),
                                               place == 1 ? u" · next"_s : QString());
        const QFontMetrics nextMetrics(panel_->mono(8));
        const double nextWidth = std::min<double>(nextMetrics.horizontalAdvance(next) + 4, row.width() * 0.4);
        painter->setPen(panel_->roleColor(role));
        painter->drawText(QRectF(button.left() - 12 - nextWidth, row.top(), nextWidth, row.height()),
                          Qt::AlignRight | Qt::AlignVCenter, nextMetrics.elidedText(next, Qt::ElideLeft, int(nextWidth)));
        painter->setFont(option.font);
        painter->setPen(panel_->textColor());
        const double titleLeft = row.left() + 122;
        const double titleWidth = button.left() - 24 - nextWidth - titleLeft;
        painter->drawText(QRectF(titleLeft, row.top(), titleWidth, row.height()), Qt::AlignLeft | Qt::AlignVCenter,
                          QFontMetrics(option.font).elidedText(index.data(TaskListModel::TitleRole).toString(),
                                                               Qt::ElideRight, int(titleWidth)));
        painter->setPen(QPen(panel_->lineColor(), 1));
        painter->drawRoundedRect(button, 6, 6);
        NexaIcons::paint(*painter, NexaIcons::Kind::Cross, button.adjusted(9, 9, -9, -9), panel_->mutedColor());
        painter->restore();
    }

    bool editorEvent(QEvent *event, QAbstractItemModel *, const QStyleOptionViewItem &option,
                     const QModelIndex &index) override {
        if (event->type() != QEvent::MouseButtonRelease) return false;
        const QRectF row = QRectF(option.rect).adjusted(1, 1, -1, -6);
        const QRectF button(row.right() - 30 - 8, row.center().y() - 15, 30, 30);
        if (!button.contains(static_cast<QMouseEvent *>(event)->position())) return false;
        removed_(index.data(TaskListModel::IdRole).toULongLong());
        return true;
    }

private:
    const MissionPanel *panel_;
    std::function<void(quint64)> removed_;
};

} // namespace

MissionBoard::MissionBoard(TaskListModel *tasks, QWidget *parent) : MissionPanel(parent), mission_(tasks->mission()) {
    setObjectName(u"missionBoard"_s);
    auto *column = new QVBoxLayout(this);
    column->setContentsMargins(16, 14, 16, 14);
    column->setSpacing(10);

    auto *header = new QHBoxLayout;
    auto *titles = new QVBoxLayout;
    titles->setSpacing(2);
    auto *title = new QLabel(tr("// MISSION BOARD"), this);
    title->setObjectName(u"sectionTitle"_s);
    counts_ = new QLabel(this);
    counts_->setObjectName(u"boardCounts"_s);
    counts_->setTextFormat(Qt::RichText);
    titles->addWidget(title);
    titles->addWidget(counts_);
    header->addLayout(titles);
    header->addStretch();
    auto *pauseAll = new QPushButton(tr("Pause all"), this);
    pauseAll->setObjectName(u"ghost"_s);
    pauseAll->setCursor(Qt::PointingHandCursor);
    pauseAll->setToolTip(tr("Pause every task at work; each keeps what it has done · Ctrl+Shift+Space"));
    connect(pauseAll, &QPushButton::clicked, this, &MissionBoard::pauseAllRequested);
    auto *add = new QPushButton(tr("+  Add tasks"), this);
    add->setObjectName(u"addTasksButton"_s);
    add->setCursor(Qt::PointingHandCursor);
    add->setToolTip(tr("Several tasks at once, one per line · Ctrl+T"));
    connect(add, &QPushButton::clicked, this, &MissionBoard::addTasksRequested);
    header->addWidget(pauseAll);
    header->addWidget(add);
    column->addLayout(header);

    tasks_ = new QListView(this);
    tasks_->setObjectName(u"taskList"_s);
    tasks_->setModel(new TaskFilter(TaskFilter::Kind::Board, tasks, this));
    tasks_->setItemDelegate(new TaskRowDelegate(this, [this](quint64 id, TaskState state) {
        if (state == TaskState::Paused) {
            emit resumeRequested(id);
        } else if (isFinished(state)) {
            emit removeRequested(id);
        } else {
            emit pauseRequested(id);
        }
    }));
    tasks_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    tasks_->setSelectionMode(QAbstractItemView::NoSelection);
    tasks_->setCursor(Qt::PointingHandCursor);
    connect(tasks_, &QListView::clicked, this, [this](const QModelIndex &index) {
        emit taskActivated(index.data(TaskListModel::IdRole).toULongLong());
    });
    column->addWidget(tasks_, 1);

    auto *queueHeader = new QHBoxLayout;
    queueTitle_ = new QLabel(tr("QUEUE · STARTS WHEN A SLOT FREES"), this);
    queueTitle_->setObjectName(u"sectionTitle"_s);
    queueLimit_ = new QLabel(this);
    queueLimit_->setObjectName(u"sectionNote"_s);
    queueHeader->addWidget(queueTitle_);
    queueHeader->addStretch();
    queueHeader->addWidget(queueLimit_);
    column->addLayout(queueHeader);
    queue_ = new QListView(this);
    queue_->setObjectName(u"queueList"_s);
    queue_->setModel(new TaskFilter(TaskFilter::Kind::Queue, tasks, this));
    queue_->setItemDelegate(new QueueRowDelegate(this, [this](quint64 id) { emit stopRequested(id); }));
    queue_->setSelectionMode(QAbstractItemView::NoSelection);
    queue_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    column->addWidget(queue_);
    queueEmpty_ = new QLabel(tr("Nothing waiting."), this);
    queueEmpty_->setObjectName(u"sectionNote"_s);
    column->addWidget(queueEmpty_);

    connect(mission_, &MissionControl::countsChanged, this, &MissionBoard::refreshCounts);
    connect(queue_->model(), &QAbstractItemModel::rowsInserted, this, &MissionBoard::fitQueue);
    connect(queue_->model(), &QAbstractItemModel::rowsRemoved, this, &MissionBoard::fitQueue);
    connect(queue_->model(), &QAbstractItemModel::modelReset, this, &MissionBoard::fitQueue);
    connect(queue_->model(), &QAbstractItemModel::layoutChanged, this, &MissionBoard::fitQueue);
    connect(this, &MissionPanel::coloursChanged, this, [this] {
        tasks_->viewport()->update();
        queue_->viewport()->update();
        refreshCounts();
    });
    refreshCounts();
    fitQueue();
}

void MissionBoard::refreshCounts() {
    const int running = mission_->running();
    const int needsYou = mission_->awaiting();
    const int queued = int(mission_->queue().size());
    QString words = tr("%1 running").arg(running);
    if (needsYou > 0) {
        words += u" · <span style=\"color:%1\">%2</span>"_s.arg(warnColor().name(), tr("%1 needs you").arg(needsYou));
    }
    words += tr(" · %1 queued · %2 done today").arg(queued).arg(mission_->doneToday());
    counts_->setText(words);
    queueLimit_->setText(tr("limit %1 concurrent").arg(mission_->slotCount()));
}

void MissionBoard::fitQueue() {
    const int rows = queue_->model()->rowCount();
    queue_->setVisible(rows > 0);
    queueEmpty_->setVisible(rows == 0);
    queue_->setFixedHeight(std::min(rows, 3) * (kQueueRowHeight + 6) + 4);
}

} // namespace nexa
