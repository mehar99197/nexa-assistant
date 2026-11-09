#pragma once

#include "app/MissionPanel.h"

class QLabel;
class QListView;

namespace nexa {

class MissionControl;
class TaskListModel;

// Mission Control's board: "MISSION BOARD · 3 running · 1 needs you · 2 queued · 12 done today", Pause all and
// Add tasks, one row per task at work or done — its agent and state, its title, its last step, its progress, how long,
// and a button that pauses, resumes or clears it — and the queue below: what starts when a slot frees, with the agent
// it will get, each removable.
class MissionBoard : public MissionPanel {
    Q_OBJECT
public:
    MissionBoard(TaskListModel *tasks, QWidget *parent = nullptr);

signals:
    void taskActivated(quint64 id);
    void pauseRequested(quint64 id);
    void resumeRequested(quint64 id);
    void stopRequested(quint64 id);
    void removeRequested(quint64 id);
    void pauseAllRequested();
    void addTasksRequested();

private:
    void refreshCounts();
    void fitQueue();

    MissionControl *mission_;
    QLabel *counts_;
    QListView *tasks_;
    QListView *queue_;
    QLabel *queueTitle_;
    QLabel *queueLimit_;
    QLabel *queueEmpty_;
};

} // namespace nexa
