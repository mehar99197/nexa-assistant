#include "core/tools/ToolExecutor.h"

#include "core/Logging.h"
#include "core/audit/AuditLog.h"
#include "core/policy/PolicyEngine.h"
#include "core/tools/ToolRegistry.h"

#include <QJsonDocument>
#include <QPointer>

#include <memory>

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

void ToolExecutor::execute(const QString &toolName, const QJsonObject &arguments, const QString &source,
                           ITool::Done done) {
    const quint64 id = nextId_++;
    emit actionStarted(id, toolName, arguments, source);

    ITool *tool = registry_->find(toolName);
    if (!tool) {
        const Error error{ErrorCode::NotFound, u"Unknown tool \"%1\""_s.arg(toolName)};
        emit actionFinished(id, false, error.message, 0);
        done(error);
        return;
    }

    const PolicyDecision decision = policy_->evaluate(tool->spec(), arguments);
    if (!decision.needsConfirmation) {
        invoke(id, tool, arguments, source, decision.destructive, false, std::move(done));
        return;
    }
    if (!confirmer_) {
        const Error error{ErrorCode::PermissionDenied, u"Needs confirmation, but no one can be asked"_s};
        emit actionFinished(id, false, error.message, 0);
        done(error);
        return;
    }

    QString question = u"Allow %1?\n\n%2"_s.arg(
        toolName, QString::fromUtf8(QJsonDocument(arguments).toJson(QJsonDocument::Indented)));
    if (!decision.reason.isEmpty()) question += u"\nRisk: %1"_s.arg(decision.reason);
    const QPointer<ToolExecutor> self(this);
    confirmer_(question, [self, id, tool, arguments, source, decision, done = std::move(done)](bool allowed) mutable {
        if (!self) return;
        if (!allowed) {
            const Error error{ErrorCode::PermissionDenied, u"The user declined this action"_s};
            emit self->actionFinished(id, false, error.message, 0);
            done(error);
            return;
        }
        self->invoke(id, tool, arguments, source, decision.destructive, true, std::move(done));
    });
}

void ToolExecutor::invoke(quint64 id, ITool *tool, const QJsonObject &arguments, const QString &source,
                          bool destructive, bool confirmed, ITool::Done done) {
    QElapsedTimer timer;
    timer.start();
    const QPointer<ToolExecutor> self(this);
    // Guards against tools that report twice (e.g. a late transport callback after cancel()).
    auto reported = std::make_shared<bool>(false);
    const QString name = tool->spec().name;

    tool->invoke(arguments, [self, id, name, arguments, source, destructive, confirmed, timer, reported,
                             done = std::move(done)](Result<ToolResult> result) {
        if (*reported) return;
        *reported = true;
        const qint64 elapsed = timer.elapsed();
        const QString text = result ? result.value().text : result.error().message;
        if (self) {
            const AuditLog::Entry entry{QDateTime::currentDateTime(), source, name, arguments, result.isOk(),
                                        text, elapsed, destructive, confirmed};
            if (const Result<void> logged = self->audit_->append(entry); !logged) {
                qCWarning(lcApp) << "Audit log:" << logged.error().message;
            }
            emit self->actionFinished(id, result.isOk(), summarize(text), elapsed);
        }
        done(std::move(result));
    });
}

void ToolExecutor::cancelAll() {
    registry_->cancelAll();
}

} // namespace nexa
