#pragma once

#include "core/tools/ITool.h"

#include <QElapsedTimer>
#include <QHash>
#include <QJsonObject>
#include <QObject>

#include <functional>
#include <memory>

namespace nexa {

class AuditLog;
class PolicyEngine;
class ToolRegistry;

// What the user is asked before an action the policy wants approved.
struct ConfirmRequest {
    quint64 id{0};              // the action's id, as actionStarted gave it
    QString tool;
    QJsonObject arguments;
    QString question;           // in words: "Allow run_command?\n\n{…}\nRisk: …"
    QString risk;               // the policy's reason, empty when it gave none
    QString source;
    quint64 task{0};            // the task that asks (0: none)
};

// The single entry point for running a tool, from the fast path or the agent:
// policy check → optional user confirmation → invoke → audit log → action-log signals.
// Every action belongs to a task (ToolContext::task) when the mission board runs it: the signals, the question and the
// audit line say which, and cancelTask() stops one task's actions without touching another's.
class ToolExecutor : public QObject {
    Q_OBJECT
public:
    // Asks the user; must call `answer` exactly once (asynchronously is fine). An answer that comes after the task
    // was cancelled is ignored.
    using Confirmer = std::function<void(const ConfirmRequest &request, std::function<void(bool)> answer)>;

    ToolExecutor(ToolRegistry *registry, PolicyEngine *policy, AuditLog *audit, QObject *parent = nullptr);

    void setConfirmer(Confirmer confirmer) { confirmer_ = std::move(confirmer); }

    // `done` runs exactly once.
    void execute(const QString &toolName, const QJsonObject &arguments, const ToolContext &context, ITool::Done done);
    // `source`: gui | voice | phone | agent; no task.
    void execute(const QString &toolName, const QJsonObject &arguments, const QString &source, ITool::Done done) {
        execute(toolName, arguments, ToolContext{source, 0}, std::move(done));
    }
    // Cancels every running tool; their callbacks report ErrorCode::Cancelled.
    void cancelAll();
    // Cancels the actions of one task (not 0): a question still open is withdrawn, and a running tool is cancelled —
    // unless another task is in the same tool (ITool::cancel() is per tool, not per invocation): then this task is told
    // Cancelled at once and the tool's late result, still audited, goes nowhere else. Every one reports Cancelled.
    void cancelTask(quint64 task);

signals:
    void actionStarted(quint64 id, const QString &tool, const QJsonObject &arguments, const QString &source,
                       quint64 task);
    void actionFinished(quint64 id, bool ok, const QString &summary, qint64 elapsedMs, quint64 task);

private:
    // One action between its start and its report: what cancelTask needs to stop it, and the guard that lets a report
    // through once.
    struct Action {
        quint64 task{0};
        QString tool;
        ITool *running{nullptr};        // set while invoked; null while the question is open, and once it reported
        bool finished{false};           // the tool reported
        bool reported{false};           // the caller was told (actionFinished, then `done`)
        ITool::Done done;
    };

    void invoke(quint64 id, const std::shared_ptr<Action> &action, ITool *tool, const QJsonObject &arguments,
                const ToolContext &context, bool destructive, bool confirmed);
    // Reports the end of an action once: the signal, then the caller's callback.
    void report(quint64 id, const std::shared_ptr<Action> &action, Result<ToolResult> result, qint64 elapsedMs);

    ToolRegistry *registry_;
    PolicyEngine *policy_;
    AuditLog *audit_;
    Confirmer confirmer_;
    quint64 nextId_{1};
    QHash<quint64, std::shared_ptr<Action>> actions_;     // started and not yet reported
};

// First line of a tool result, shortened for one-line UI summaries.
QString summarize(const QString &text, qsizetype maxLength = 160);

} // namespace nexa
