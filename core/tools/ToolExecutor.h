#pragma once

#include "core/tools/ITool.h"

#include <QElapsedTimer>
#include <QJsonObject>
#include <QObject>

#include <functional>

namespace nexa {

class AuditLog;
class PolicyEngine;
class ToolRegistry;

// The single entry point for running a tool, from the fast path or the agent:
// policy check → optional user confirmation → invoke → audit log → action-log signals.
class ToolExecutor : public QObject {
    Q_OBJECT
public:
    // Asks the user; must call `answer` exactly once (asynchronously is fine).
    using Confirmer = std::function<void(const QString &question, std::function<void(bool)> answer)>;

    ToolExecutor(ToolRegistry *registry, PolicyEngine *policy, AuditLog *audit, QObject *parent = nullptr);

    void setConfirmer(Confirmer confirmer) { confirmer_ = std::move(confirmer); }

    // `source`: gui | voice | phone | agent. `done` runs exactly once.
    void execute(const QString &toolName, const QJsonObject &arguments, const QString &source, ITool::Done done);
    // Cancels every running tool; their callbacks report ErrorCode::Cancelled.
    void cancelAll();

signals:
    void actionStarted(quint64 id, const QString &tool, const QJsonObject &arguments, const QString &source);
    void actionFinished(quint64 id, bool ok, const QString &summary, qint64 elapsedMs);

private:
    void invoke(quint64 id, ITool *tool, const QJsonObject &arguments, const QString &source, bool destructive,
                bool confirmed, ITool::Done done);

    ToolRegistry *registry_;
    PolicyEngine *policy_;
    AuditLog *audit_;
    Confirmer confirmer_;
    quint64 nextId_{1};
};

// First line of a tool result, shortened for one-line UI summaries.
QString summarize(const QString &text, qsizetype maxLength = 160);

} // namespace nexa
