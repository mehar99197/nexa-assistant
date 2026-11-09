#pragma once

#include "core/mission/Task.h"

#include <QAbstractListModel>
#include <QSortFilterProxyModel>

namespace nexa {

class MissionControl;

// The board's tasks as a list (Adapter over MissionControl), oldest first; one row per task, kept current from the
// board's signals. The views pick their rows with the filters below.
class TaskListModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum Role {
        IdRole = Qt::UserRole + 1,
        AgentIdRole,
        TitleRole,
        StateRole,              // TaskState as int
        StateTextRole,          // "RUNNING"
        SubStatusRole,
        ProgressRole,
        ElapsedRole,            // "04:32"
        RoleRole,               // AgentRole as int
        ProviderRole,           // "opencode · gpt-x"
        ActivityRole,           // QList<int>, the sparkline
        DepthRole,
        QueuePositionRole,      // 1-based; 0 when not waiting
        FastPathRole,
    };

    explicit TaskListModel(MissionControl *mission, QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;
    QModelIndex indexOf(TaskId id) const;
    MissionControl *mission() const { return mission_; }

private:
    MissionControl *mission_;
    QList<TaskId> ids_;
};

// Which of the board's tasks a view shows (Specification over the model's rows).
class TaskFilter : public QSortFilterProxyModel {
    Q_OBJECT
public:
    enum class Kind {
        Board,      // what is or was at work: not waiting; running ones first, then the newest finished
        Queue,      // waiting for a slot, in the queue's order
        Agents,     // holding a slot or waiting for its sub-task: the agents rail
        Strip,      // the other designs' short list: top-level tasks at work or just finished
    };
    TaskFilter(Kind kind, TaskListModel *source, QObject *parent = nullptr);

protected:
    bool filterAcceptsRow(int row, const QModelIndex &parent) const override;
    bool lessThan(const QModelIndex &left, const QModelIndex &right) const override;

private:
    Kind kind_;
};

} // namespace nexa
