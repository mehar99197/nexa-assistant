#pragma once

#include "app/MissionPanel.h"

#include <QMetaObject>

class QLabel;
class QPlainTextEdit;
class QPushButton;

namespace nexa {

class ChatView;
class MissionControl;

// One task's transcript, opened from the board: its agent, state and title, the conversation as the chat draws it —
// replayed from what the board kept, then followed live — Pause / Resume and Stop, and a box to ask it more once it
// has finished or paused. "‹ Board" goes back.
class TaskTranscriptDrawer : public MissionPanel {
    Q_OBJECT
public:
    explicit TaskTranscriptDrawer(MissionControl *mission, QWidget *parent = nullptr);

    void showTask(quint64 id);
    quint64 currentTask() const { return task_; }
    ChatView *chat() const { return chat_; }

signals:
    void backRequested();
    void followedUp(quint64 task);          // the owner asked the task something more

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void refreshHeader();
    void send();

    MissionControl *mission_;
    quint64 task_{0};
    QLabel *agent_;
    QLabel *state_;
    QLabel *title_;
    QPushButton *pause_;
    QPushButton *stop_;
    ChatView *chat_;
    QPlainTextEdit *input_;
    QPushButton *send_;
};

} // namespace nexa
