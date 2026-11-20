#include "core/tools/ToolExecutor.h"

#include "core/Logging.h"
#include "core/audit/AuditLog.h"
#include "core/policy/PolicyEngine.h"
#include "core/tools/ToolRegistry.h"

#include <QJsonDocument>
#include <QPointer>

#include <algorithm>
#include <memory>
#include <utility>

using namespace Qt::StringLiterals;

namespace nexa {

QString summarize(const QString &text, qsizetype maxLength) {
    QString line = text.section(u'\n', 0, 0).trimmed();
    if (line.isEmpty()) line = text.simplified();
    if (line.size() > maxLength) line = line.left(maxLength - 1) + u"…"_s;
    return line;
}

ToolExecutor::ToolExecutor(ToolRegistry *registry, PolicyEngine *policy, AuditLog *audit, QObject *parent)
    : QObject(parent), registry_(registry), policy_(policy), audit_(audit) {}

void ToolExecutor::execute(const QString &toolName, const QJsonObject &arguments, const ToolContext &context,
                           ITool::Done done) {
    const quint64 id = nextId_++;
    auto action = std::make_shared<Action>();
    action->task = context.task;
    action->tool = toolName;
    action->done = std::move(done);
    actions_.insert(id, action);
    emit actionStarted(id, toolName, arguments, context.source, context.task);

    ITool *tool = registry_->find(toolName);
    if (!tool) {
        report(id, action, Error{ErrorCode::NotFound, u"Unknown tool \"%1\""_s.arg(toolName)}, 0);
        return;
    }

    const PolicyDecision decision = policy_->evaluate(tool->spec(), arguments);
    if (!decision.needsConfirmation) {
        invoke(id, action, tool, arguments, context, decision.destructive, false);
        return;
    }
    if (!confirmer_) {
        report(id, action, Error{ErrorCode::PermissionDenied, u"Needs confirmation, but no one can be asked"_s}, 0);
        return;
    }

    QString question = u"Allow %1?\n\n%2"_s.arg(
        toolName, QString::fromUtf8(QJsonDocument(arguments).toJson(QJsonDocument::Indented)));
    if (!decision.reason.isEmpty()) question += u"\nRisk: %1"_s.arg(decision.reason);
    const ConfirmRequest request{id, toolName, arguments, question, decision.reason, context.source, context.task};
    // The question may stay open for a while: the tool is looked up again by name once it is
    // answered, since a connector that went or listed its tools again has deleted the old one.
    // An answer to a question its task withdrew (cancelTask) is ignored.
    const QPointer<ToolExecutor> self(this);
    confirmer_(request, [self, id, action, toolName, arguments, context, decision](bool allowed) {
        if (!self || action->reported) return;
        if (!allowed) {
            self->report(id, action, Error{ErrorCode::PermissionDenied, u"The user declined this action"_s}, 0);
            return;
        }
        ITool *still = self->registry_->find(toolName);
        if (!still) {
            self->report(id, action, Error{ErrorCode::NotSupported, u"\"%1\" is no longer available"_s.arg(toolName)}, 0);
            return;
        }
        self->invoke(id, action, still, arguments, context, decision.destructive, true);
    });
}

void ToolExecutor::invoke(quint64 id, const std::shared_ptr<Action> &action, ITool *tool, const QJsonObject &arguments,
                          const ToolContext &context, bool destructive, bool confirmed) {
    QElapsedTimer timer;
    timer.start();
    const QPointer<ToolExecutor> self(this);
    const QString name = tool->spec().name;
    action->running = tool;

    tool->invokeFor(arguments, context, [self, id, action, name, arguments, context, destructive, confirmed,
                                         timer](Result<ToolResult> result) {
        // Guards against tools that report twice (e.g. a late transport callback after cancel()).
        if (action->finished) return;
        action->finished = true;
        action->running = nullptr;
        const qint64 elapsed = timer.elapsed();
        if (self) {
            // Audited even when its task was cancelled meanwhile: it ran.
            const QString text = result ? result.value().text : result.error().message;
            const AuditLog::Entry entry{QDateTime::currentDateTime(), context.source, name, arguments, result.isOk(),
                                        text, elapsed, destructive, confirmed, context.task};
            if (const Result<void> logged = self->audit_->append(entry); !logged) {
                qCWarning(lcApp) << "Audit log:" << logged.error().message;
            }
        }
        if (action->reported) return;       // its task was cancelled and told so: the result goes nowhere
        if (self) {
            self->report(id, action, std::move(result), elapsed);
        } else if (ITool::Done done = std::exchange(action->done, nullptr)) {
            action->reported = true;
            done(std::move(result));
        }
    });
}

void ToolExecutor::report(quint64 id, const std::shared_ptr<Action> &action, Result<ToolResult> result,
                          qint64 elapsedMs) {
    if (action->reported) return;
    action->reported = true;
    actions_.remove(id);
    const QString text = result ? result.value().text : result.error().message;
    emit actionFinished(id, result.isOk(), summarize(text), elapsedMs, action->task);
    if (ITool::Done done = std::exchange(action->done, nullptr)) done(std::move(result));
}

void ToolExecutor::cancelAll() {
    // Questions still open are withdrawn as well: nothing of what was asked runs after a stop.
    QList<std::pair<quint64, std::shared_ptr<Action>>> asking;
    for (auto it = actions_.cbegin(); it != actions_.cend(); ++it) {
        if (!it.value()->running && !it.value()->reported) asking.append({it.key(), it.value()});
    }
    for (const auto &[id, action] : asking) report(id, action, Error{ErrorCode::Cancelled, u"Cancelled"_s}, 0);
    registry_->cancelAll();
}

void ToolExecutor::cancelTask(quint64 task) {
    if (task == 0) return;
    QList<std::pair<quint64, std::shared_ptr<Action>>> mine;
    for (auto it = actions_.cbegin(); it != actions_.cend(); ++it) {
        if (it.value()->task == task) mine.append({it.key(), it.value()});
    }
    for (const auto &[id, action] : mine) {
        if (action->reported) continue;
        if (ITool *tool = action->running) {
            const bool shared = std::any_of(actions_.cbegin(), actions_.cend(), [task, tool](const auto &other) {
                return other->task != task && other->running == tool && !other->reported;
            });
            // Cancelled only while it is still the registered tool (a connector may have deleted it) and no other task
            // is in it; it then reports Cancelled through its own callback.
            if (!shared && registry_->find(action->tool) == tool) tool->cancel();
        }
        report(id, action, Error{ErrorCode::Cancelled, u"Cancelled"_s}, 0);
    }
}

} // namespace nexa
