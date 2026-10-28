#include "core/ai/AgyEvents.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

using namespace Qt::StringLiterals;

namespace nexa {

AgyEvent AgyEvents::decode(const QByteArray &line) {
    AgyEvent event;
    const QJsonObject object = QJsonDocument::fromJson(line).object();
    const QString kind = object.value(u"event"_s).toString();
    if (kind == u"init") {
        event.kind = AgyEvent::Kind::Init;
    } else if (kind == u"step_update") {
        const QJsonObject step = object.value(u"step_update"_s).toObject();
        const QString type = step.value(u"step_type"_s).toString();
        if (type == u"agent_response" && step.contains(u"text_delta"_s)) {
            event.kind = AgyEvent::Kind::Text;
            event.text = step.value(u"text_delta"_s).toString();
        } else if (type == u"tool") {
            event.kind = AgyEvent::Kind::ToolStep;
            event.toolName = step.value(u"tool_name"_s).toString();
            event.ok = step.value(u"state"_s).toString() != u"ERROR";
        }
    } else if (kind == u"result") {
        const QJsonObject result = object.value(u"result"_s).toObject();
        event.kind = AgyEvent::Kind::Result;
        event.ok = result.value(u"status"_s).toString() == u"SUCCESS";
        event.response = result.value(u"response"_s).toString();
        event.error = result.value(u"error"_s).toString();
        for (const QJsonValue &denied : result.value(u"denied_actions"_s).toArray()) {
            event.deniedActions.append(denied.toObject().value(u"display_name"_s).toString());
        }
    }
    return event;
}

QByteArray AgyEvents::userTurn(const QString &text) {
    const QJsonObject part{{u"type"_s, u"text"_s}, {u"text"_s, text}};
    const QJsonObject turn{{u"event"_s, u"user"_s},
                           {u"message"_s, QJsonObject{{u"content"_s, QJsonArray{part}}}}};
    return QJsonDocument(turn).toJson(QJsonDocument::Compact) + '\n';
}

QStringList AgyEvents::parseModels(const QString &output) {
    QStringList models;
    for (const QString &line : output.split(u'\n')) {
        const qsizetype tab = line.indexOf(u'\t');
        if (tab <= 0) continue;     // "Fetching available models..." and the like
        models.append(line.left(tab).trimmed());
    }
    return models;
}

Error AgyEvents::exitError(int exitCode, const QString &stderrTail) {
    if (stderrTail.contains(u"authenticat"_s, Qt::CaseInsensitive)) {
        return Error{ErrorCode::Unauthorized, u"agy is not signed in — run agy once in a terminal to sign in"_s};
    }
    const QStringList lines = stderrTail.trimmed().split(u'\n', Qt::SkipEmptyParts);
    const QString last = lines.isEmpty() ? u"no message"_s : lines.last().trimmed();
    return Error{ErrorCode::ConnectionRefused, u"agy stopped (exit %1): %2"_s.arg(exitCode).arg(last)};
}

Error AgyEvents::resultError(const QString &error) {
    // A model agy does not have: say which, and the ones it has (its own list, after "Available models:").
    static const QRegularExpression unknown(u"model (\\S+) is not recognized"_s);
    if (const QRegularExpressionMatch match = unknown.match(error); match.hasMatch()) {
        QStringList available;
        const qsizetype list = error.indexOf(u"Available models:"_s);
        if (list >= 0) {
            for (const QString &line : error.mid(list).split(u'\n').mid(1)) {
                if (!line.trimmed().isEmpty()) available.append(line.trimmed());
            }
        }
        return Error{ErrorCode::NotFound,
                     u"agy has no model \"%1\" — choose one in the toolbar%2"_s.arg(
                         match.captured(1), available.isEmpty() ? QString() : u" (%1)"_s.arg(available.join(u", "_s)))};
    }
    const QString lower = error.toLower();
    if (lower.contains(u"quota"_s) || lower.contains(u"exhausted"_s) || lower.contains(u"rate limit"_s)
        || lower.contains(u"429"_s)) {
        return Error{ErrorCode::Http, u"HTTP 429: agy: "_s + error};
    }
    return Error{ErrorCode::Unknown, u"agy: "_s + error};
}

} // namespace nexa
