#include "app/TaskStrip.h"

#include "app/TaskListModel.h"

#include <QListView>
#include <QPainter>
#include <QStyledItemDelegate>
#include <QVBoxLayout>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

constexpr int kRow = 30;
constexpr int kShown = 5;

class StripDelegate : public QStyledItemDelegate {
public:
    explicit StripDelegate(const MissionPanel *panel) : QStyledItemDelegate(const_cast<MissionPanel *>(panel)), panel_(panel) {}

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &) const override { return {option.rect.width(), kRow}; }

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        const QRectF row = QRectF(option.rect).adjusted(8, 0, -8, 0);
        const auto state = TaskState(index.data(TaskListModel::StateRole).toInt());
        const auto role = AgentRole(index.data(TaskListModel::RoleRole).toInt());
        painter->save();
        painter->setFont(panel_->mono(8, true));
        painter->setPen(panel_->roleColor(role));
        painter->drawText(QRectF(row.left(), row.top(), 82, row.height()), Qt::AlignLeft | Qt::AlignVCenter,
                          index.data(TaskListModel::AgentIdRole).toString());
        painter->setFont(panel_->mono(7.5, true));
        painter->setPen(panel_->stateColor(state));
        painter->drawText(QRectF(row.left() + 84, row.top(), 74, row.height()), Qt::AlignLeft | Qt::AlignVCenter,
                          index.data(TaskListModel::StateTextRole).toString());
        painter->setFont(panel_->mono(8));
        painter->setPen(panel_->mutedColor());
        painter->drawText(QRectF(row.right() - 40, row.top(), 40, row.height()), Qt::AlignRight | Qt::AlignVCenter,
                          u"%1%"_s.arg(index.data(TaskListModel::ProgressRole).toInt()));
        painter->setFont(option.font);
        painter->setPen(panel_->textColor());
        const QRectF title(row.left() + 160, row.top(), row.width() - 204, row.height());
        painter->drawText(title, Qt::AlignLeft | Qt::AlignVCenter,
                          QFontMetrics(option.font).elidedText(index.data(TaskListModel::TitleRole).toString(),
                                                               Qt::ElideRight, int(title.width())));
        painter->restore();
    }

private:
    const MissionPanel *panel_;
};

} // namespace

TaskStrip::TaskStrip(TaskListModel *tasks, QWidget *parent) : MissionPanel(parent) {
    setObjectName(u"taskStripPanel"_s);
    auto *column = new QVBoxLayout(this);
    column->setContentsMargins(0, 0, 0, 6);
    list_ = new QListView(this);
    list_->setObjectName(u"taskStrip"_s);
    list_->setModel(new TaskFilter(TaskFilter::Kind::Strip, tasks, this));
    list_->setItemDelegate(new StripDelegate(this));
    list_->setUniformItemSizes(true);
    list_->setSelectionMode(QAbstractItemView::NoSelection);
    list_->setCursor(Qt::PointingHandCursor);
    list_->setToolTip(tr("Tasks: click one to see it, pause it or stop it"));
    connect(list_, &QListView::clicked, this, [this](const QModelIndex &index) {
        emit taskActivated(index.data(TaskListModel::IdRole).toULongLong());
    });
    column->addWidget(list_);
    for (const auto signal : {&QAbstractItemModel::rowsInserted, &QAbstractItemModel::rowsRemoved}) {
        connect(list_->model(), signal, this, &TaskStrip::fit);
    }
    connect(list_->model(), &QAbstractItemModel::layoutChanged, this, &TaskStrip::fit);
    connect(this, &MissionPanel::coloursChanged, list_->viewport(), [this] { list_->viewport()->update(); });
    fit();
}

void TaskStrip::fit() {
    const int rows = list_->model()->rowCount();
    setVisible(rows > 0);
    list_->setFixedHeight(std::min(rows, kShown) * kRow + 4);
}

} // namespace nexa
