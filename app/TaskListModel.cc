#include "app/TaskListModel.h"

#include "app/MissionPanel.h"
#include "core/mission/MissionControl.h"

using namespace Qt::StringLiterals;

namespace nexa {

TaskListModel::TaskListModel(MissionControl *mission, QObject *parent) : QAbstractListModel(parent), mission_(mission) {
    for (const Task &task : mission_->tasks()) ids_.append(task.id);
    connect(mission_, &MissionControl::taskAdded, this, [this](quint64 id) {
        beginInsertRows({}, int(ids_.size()), int(ids_.size()));
        ids_.append(id);
        endInsertRows();
    });
    connect(mission_, &MissionControl::taskRemoved, this, [this](quint64 id) {
        const qsizetype row = ids_.indexOf(id);
        if (row < 0) return;
        beginRemoveRows({}, int(row), int(row));
        ids_.removeAt(row);
        endRemoveRows();
    });
    connect(mission_, &MissionControl::taskChanged, this, [this](quint64 id) {
        const QModelIndex at = indexOf(id);
        if (at.isValid()) emit dataChanged(at, at);
    });
    connect(mission_, &MissionControl::queueChanged, this, [this] {
        if (!ids_.isEmpty()) emit dataChanged(index(0), index(int(ids_.size()) - 1), {QueuePositionRole, StateRole});
    });
}

int TaskListModel::rowCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : int(ids_.size());
}

QModelIndex TaskListModel::indexOf(TaskId id) const {
    const qsizetype row = ids_.indexOf(id);
    return row < 0 ? QModelIndex() : index(int(row));
}

QVariant TaskListModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() >= ids_.size()) return {};
    const Task *task = mission_->task(ids_.at(index.row()));
    if (!task) return {};
    switch (role) {
    case Qt::DisplayRole:
    case TitleRole: return task->title;
    case IdRole: return QVariant::fromValue<quint64>(task->id);
    case AgentIdRole: return task->agentId;
    case StateRole: return int(task->state);
    case StateTextRole: return taskStateName(task->state);
    case SubStatusRole: return task->subStatus;
    case ProgressRole: return task->progress;
    case ElapsedRole: return MissionPanel::elapsed(*task);
    case RoleRole: return int(task->role);
    case ProviderRole: return task->providerName;
    case ActivityRole: return QVariant::fromValue(task->activity);
    case DepthRole: return task->depth;
    case QueuePositionRole: return int(mission_->queue().indexOf(task->id)) + 1;
    case FastPathRole: return task->fastPath;
    case Qt::ToolTipRole: return u"%1 · %2\n%3"_s.arg(task->agentId, task->providerName, task->text);
    default: return {};
    }
}

QHash<int, QByteArray> TaskListModel::roleNames() const {
    return {{IdRole, "taskId"}, {AgentIdRole, "agentId"}, {TitleRole, "title"}, {StateRole, "state"},
            {SubStatusRole, "subStatus"}, {ProgressRole, "progress"}, {ElapsedRole, "elapsed"}};
}

TaskFilter::TaskFilter(Kind kind, TaskListModel *source, QObject *parent) : QSortFilterProxyModel(parent), kind_(kind) {
    setSourceModel(source);
    setDynamicSortFilter(true);
    sort(0);
}

bool TaskFilter::filterAcceptsRow(int row, const QModelIndex &parent) const {
    const QModelIndex at = sourceModel()->index(row, 0, parent);
    const auto state = TaskState(at.data(TaskListModel::StateRole).toInt());
    const int depth = at.data(TaskListModel::DepthRole).toInt();
    switch (kind_) {
    case Kind::Board: return state != TaskState::Queued;
    case Kind::Queue: return state == TaskState::Queued;
    case Kind::Agents: return holdsSlot(state) || state == TaskState::Delegating;
    case Kind::Strip: return depth == 0 && state != TaskState::Queued;
    }
    return false;
}

bool TaskFilter::lessThan(const QModelIndex &left, const QModelIndex &right) const {
    const quint64 a = left.data(TaskListModel::IdRole).toULongLong();
    const quint64 b = right.data(TaskListModel::IdRole).toULongLong();
    if (kind_ == Kind::Queue) {
        return left.data(TaskListModel::QueuePositionRole).toInt() < right.data(TaskListModel::QueuePositionRole).toInt();
    }
    if (kind_ == Kind::Board || kind_ == Kind::Strip) {
        // What works first (oldest at the top, as the mock-up lists them), then the finished, newest first.
        const bool aDone = isFinished(TaskState(left.data(TaskListModel::StateRole).toInt()));
        const bool bDone = isFinished(TaskState(right.data(TaskListModel::StateRole).toInt()));
        if (aDone != bDone) return !aDone;
        return aDone ? a > b : a < b;
    }
    return a < b;
}

} // namespace nexa
