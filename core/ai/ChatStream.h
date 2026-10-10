#pragma once

#include "core/Result.h"
#include "core/ai/ChatRequest.h"

#include <QList>
#include <QObject>
#include <QString>

namespace nexa {

enum class FinishReason { Stop, Length, ToolCalls, ContentFilter, Cancelled, Other };

// One streamed model reply: textDelta/toolCallDelta while it arrives, then exactly one of
// finished() or failed(). Providers subclass it; consumers connect to the signals and may cancel().
class ChatStream : public QObject {
    Q_OBJECT
public:
    explicit ChatStream(QObject *parent = nullptr);

    // Stops the transfer and emits finished(FinishReason::Cancelled) unless already done.
    void cancel();
    bool isDone() const { return done_; }
    const QString &text() const { return text_; }
    const QList<ToolCall> &toolCalls() const { return toolCalls_; }    // complete once finished

signals:
    void textDelta(const QString &delta);
    void toolCallDelta(int index, const QString &name, const QString &argumentsDelta);
    void finished(nexa::FinishReason reason);
    void failed(const nexa::Error &error);

protected:
    virtual void abortTransport() = 0;

    void emitText(const QString &delta);
    void emitToolCallDelta(int index, const QString &name, const QString &argumentsDelta);
    void complete(FinishReason reason, QList<ToolCall> toolCalls);
    void fail(const Error &error);

private:
    QString text_;
    QList<ToolCall> toolCalls_;
    bool done_{false};
};

} // namespace nexa
