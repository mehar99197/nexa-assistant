#pragma once

#include <QList>
#include <QString>

namespace nexa {

// One thing a task said or did, as the chat shows it.
struct TranscriptEvent {
    enum class Kind { User, ReplyStart, Route, Via, Delta, Action, Notice, Error, Finished };
    Kind kind{Kind::Notice};
    QString text;       // the words; an action's tool
    QString extra;      // an action's summary
    bool ok{true};
    qint64 ms{0};
};

// What a task said and did, kept so its transcript can be shown again when the user opens it (Memento of the chat).
// Words streamed one after another are kept as one piece. Pure.
class TaskTranscript {
public:
    void append(TranscriptEvent event);
    const QList<TranscriptEvent> &events() const { return events_; }
    // The words of the last reply (what a parent task gets back from its sub-task).
    QString lastReply() const;

private:
    QList<TranscriptEvent> events_;
};

} // namespace nexa
