#include "app/TaskTranscriptDrawer.h"

#include "app/ChatView.h"
#include "core/mission/MissionControl.h"

#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

using namespace Qt::StringLiterals;

namespace nexa {

TaskTranscriptDrawer::TaskTranscriptDrawer(MissionControl *mission, QWidget *parent) : MissionPanel(parent), mission_(mission) {
    setObjectName(u"taskDrawer"_s);
    auto *column = new QVBoxLayout(this);
    column->setContentsMargins(16, 12, 16, 12);
    column->setSpacing(8);

    auto *header = new QHBoxLayout;
    auto *back = new QPushButton(tr("‹ Board"), this);
    back->setObjectName(u"ghost"_s);
    back->setCursor(Qt::PointingHandCursor);
    connect(back, &QPushButton::clicked, this, &TaskTranscriptDrawer::backRequested);
    agent_ = new QLabel(this);
    agent_->setObjectName(u"drawerAgent"_s);
    state_ = new QLabel(this);
    state_->setObjectName(u"drawerState"_s);
    title_ = new QLabel(this);
    title_->setObjectName(u"drawerTitle"_s);
    pause_ = new QPushButton(this);
    pause_->setObjectName(u"ghost"_s);
    stop_ = new QPushButton(tr("Stop"), this);
    stop_->setObjectName(u"ghost"_s);
    connect(pause_, &QPushButton::clicked, this, [this] {
        const Task *task = mission_->task(task_);
        if (!task) return;
        if (task->state == TaskState::Paused) {
            mission_->resume(task_);
        } else {
            mission_->pause(task_);
        }
    });
    connect(stop_, &QPushButton::clicked, this, [this] { mission_->stop(task_); });
    header->addWidget(back);
    header->addWidget(agent_);
    header->addWidget(state_);
    header->addWidget(title_, 1);
    header->addWidget(pause_);
    header->addWidget(stop_);
    column->addLayout(header);

    chat_ = new ChatView(this);
    column->addWidget(chat_, 1);

    auto *row = new QHBoxLayout;
    input_ = new QPlainTextEdit(this);
    input_->setObjectName(u"input"_s);
    input_->setFixedHeight(48);
    input_->setTabChangesFocus(true);
    input_->installEventFilter(this);
    send_ = new QPushButton(tr("Ask it"), this);
    send_->setObjectName(u"send"_s);
    send_->setFixedHeight(48);
    connect(send_, &QPushButton::clicked, this, &TaskTranscriptDrawer::send);
    row->addWidget(input_, 1);
    row->addWidget(send_);
    column->addLayout(row);

    // The task followed live, as the chat follows the conversation.
    const auto mine = [this](quint64 id) { return id != 0 && id == task_; };
    connect(mission_, &MissionControl::taskReplyStarted, this, [this, mine](quint64 id) {
        if (mine(id)) chat_->beginAssistantMessage();
    });
    connect(mission_, &MissionControl::taskReplyRoute, this, [this, mine](quint64 id, const QString &route) {
        if (mine(id)) chat_->setReplyRoute(route);
    });
    connect(mission_, &MissionControl::taskReplyVia, this, [this, mine](quint64 id, const QString &route) {
        if (mine(id)) chat_->setReplyVia(route);
    });
    connect(mission_, &MissionControl::taskReplyDelta, this, [this, mine](quint64 id, const QString &delta) {
        if (mine(id)) chat_->appendAssistantDelta(delta);
    });
    connect(mission_, &MissionControl::taskReplyNotice, this, [this, mine](quint64 id, const QString &message) {
        if (mine(id)) chat_->addNotice(message);
    });
    connect(mission_, &MissionControl::taskReplyAction, this,
            [this, mine](quint64 id, const QString &tool, bool ok, const QString &summary, qint64 ms) {
        if (mine(id)) chat_->addAction(tool, ok, summary, ms);
    });
    connect(mission_, &MissionControl::taskReplyFinished, this, [this, mine](quint64 id, FinishReason reason) {
        if (!mine(id)) return;
        chat_->endAssistantMessage();
        const Task *task = mission_->task(id);
        if (task && task->state == TaskState::Paused) {
            chat_->addNotice(tr("Paused."));
        } else if (reason == FinishReason::Cancelled) {
            chat_->addNotice(tr("Stopped."));
        }
    });
    connect(mission_, &MissionControl::taskReplyFailed, this, [this, mine](quint64 id, const Error &error) {
        if (!mine(id)) return;
        chat_->endAssistantMessage();
        chat_->addError(error.message);
    });
    connect(mission_, &MissionControl::taskChanged, this, [this, mine](quint64 id) {
        if (mine(id)) refreshHeader();
    });
    connect(mission_, &MissionControl::taskRemoved, this, [this, mine](quint64 id) {
        if (!mine(id)) return;
        task_ = 0;
        emit backRequested();
    });
    connect(this, &MissionPanel::coloursChanged, this, &TaskTranscriptDrawer::refreshHeader);
}

void TaskTranscriptDrawer::showTask(quint64 id) {
    task_ = id;
    chat_->clearTranscript();
    const Task *task = mission_->task(id);
    if (const TaskTranscript *transcript = mission_->transcript(id); transcript && task) {
        using Kind = TranscriptEvent::Kind;
        bool open = false;
        for (const TranscriptEvent &event : transcript->events()) {
            switch (event.kind) {
            case Kind::User: chat_->addUserMessage(event.text, task->source, event.extra); break;
            case Kind::ReplyStart:
                chat_->beginAssistantMessage();
                open = true;
                break;
            case Kind::Route: chat_->setReplyRoute(event.text); break;
            case Kind::Via: chat_->setReplyVia(event.text); break;
            case Kind::Delta: chat_->appendAssistantDelta(event.text); break;
            case Kind::Action: chat_->addAction(event.text, event.ok, event.extra, event.ms); break;
            case Kind::Notice: chat_->addNotice(event.text); break;
            case Kind::Error:
                chat_->endAssistantMessage();
                chat_->addError(event.text);
                open = false;
                break;
            case Kind::Finished:
                chat_->endAssistantMessage();
                open = false;
                break;
            }
        }
        Q_UNUSED(open);     // a reply still running stays open, and goes on live
    }
    refreshHeader();
    input_->setFocus();
}

void TaskTranscriptDrawer::refreshHeader() {
    const Task *task = mission_->task(task_);
    if (!task) return;
    agent_->setText(task->agentId);
    agent_->setStyleSheet(u"color: %1;"_s.arg(roleColor(task->role).name()));
    state_->setText(taskStateName(task->state));
    state_->setStyleSheet(u"color: %1;"_s.arg(stateColor(task->state).name()));
    title_->setText(task->title);
    title_->setToolTip(u"%1\n%2"_s.arg(task->providerName, task->text));
    const bool finished = isFinished(task->state);
    pause_->setText(task->state == TaskState::Paused ? tr("Resume") : tr("Pause"));
    pause_->setEnabled(!finished);
    stop_->setEnabled(!finished);
    const bool canAsk = finished || task->state == TaskState::Paused;
    send_->setEnabled(canAsk);
    input_->setPlaceholderText(canAsk ? tr("Ask %1 more, in its own conversation…").arg(task->agentId)
                                      : tr("%1 is at work — pause it, or wait until it is done, to ask it more.")
                                            .arg(task->agentId));
}

bool TaskTranscriptDrawer::eventFilter(QObject *watched, QEvent *event) {
    if (watched == input_ && event->type() == QEvent::KeyPress) {
        const auto *key = static_cast<QKeyEvent *>(event);
        if ((key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) && !(key->modifiers() & Qt::ShiftModifier)) {
            send();
            return true;
        }
    }
    return MissionPanel::eventFilter(watched, event);
}

void TaskTranscriptDrawer::send() {
    const QString text = input_->toPlainText().trimmed();
    const Task *task = mission_->task(task_);
    if (text.isEmpty() || !task || !(isFinished(task->state) || task->state == TaskState::Paused)) return;
    chat_->addUserMessage(text, u"gui"_s);
    input_->clear();
    mission_->followUp(task_, text, u"gui"_s);
    emit followedUp(task_);
}

} // namespace nexa
