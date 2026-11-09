#pragma once

#include "app/MissionPanel.h"

class QListView;

namespace nexa {

class TaskListModel;

// The tasks in the other designs: a short list at the top of the Actions panel — agent, state, title, progress — of
// what is at work or just finished; a click opens the task. Hidden while there is none.
class TaskStrip : public MissionPanel {
    Q_OBJECT
public:
    explicit TaskStrip(TaskListModel *tasks, QWidget *parent = nullptr);

signals:
    void taskActivated(quint64 id);

private:
    void fit();

    QListView *list_;
};

} // namespace nexa
