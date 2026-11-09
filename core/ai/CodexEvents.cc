#include "core/ai/CodexEvents.h"

#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

constexpr qsizetype kMessageKept = 200;

CodexTurnStatus statusOf(const QString &status) {
    if (status == u"completed") return CodexTurnStatus::Completed;
    if (status == u"interrupted") return CodexTurnStatus::Interrupted;
    if (status == u"failed") return CodexTurnStatus::Failed;
    if (status == u"inProgress") return CodexTurnStatus::InProgress;
    return CodexTurnStatus::Unknown;
}

// codexErrorInfo is a name ("usageLimitExceeded") or an object with one key and details
// ({"httpConnectionFailed":{"httpStatusCode":503}}).
struct ErrorInfo {
    QString name;
    int httpStatus{0};
};

ErrorInfo infoOf(const QJsonValue &value) {
    ErrorInfo info;
    if (value.isString()) {
        info.name = value.toString();
    } else if (value.isObject() && !value.toObject().isEmpty()) {
        const QJsonObject object = value.toObject();
        info.name = object.keys().first();
        info.httpStatus = object.value(info.name).toObject().value(u"httpStatusCode"_s).toInt();
    }
    return info;
}

bool anyOf(const QString &lower, std::initializer_list<const char16_t *> words) {
    for (const char16_t *word : words) {
        if (lower.contains(QStringView(word))) return true;
    }
    return false;
}

QString itemTypeOf(const QJsonObject &params) {
    return params.value(u"item"_s).toObject().value(u"type"_s).toString();
}

} // namespace

QString CodexEvents::tidy(const QString &text) {
    static const QRegularExpression colours(u"\x1b\\[[0-9;]*m"_s);
    static const QRegularExpression link(u"https?://\\S+"_s);
    static const QRegularExpression email(u"[\\w.+\\-]+@[\\w\\-]+(?:\\.[\\w\\-]+)+"_s);
    static const QRegularExpression secret(u"[A-Za-z0-9_\\-]{32,}"_s);      // a key, a JWT segment, a long id
    QString line = text;
    line.remove(colours);
    line.replace(link, u"<link>"_s);
    line.replace(email, u"<email>"_s);
    line.replace(secret, u"<token>"_s);
    line = line.simplified();
    if (line.size() > kMessageKept) line = line.left(kMessageKept) + u"…"_s;
    return line;
}

QString CodexEvents::notSignedInMessage() {
    return u"Codex is not signed in — sign in under Settings → Providers"_s;
}

bool CodexEvents::isOwnToolItem(const QString &type) {
    static const QSet<QString> harmless{u"agentMessage"_s, u"userMessage"_s, u"reasoning"_s,
                                        u"plan"_s,         u"contextCompaction"_s, u"hookPrompt"_s};
    return !type.isEmpty() && !harmless.contains(type);
}

CodexEvent CodexEvents::decode(const CodexMessage &message) {
    CodexEvent event;
    if (message.kind != CodexMessage::Kind::Notification) return event;
    const QString &method = message.method;
    const QJsonObject &params = message.params;
    event.threadId = params.value(u"threadId"_s).toString();
    event.turnId = params.value(u"turnId"_s).toString();

    if (method == u"item/agentMessage/delta") {
        event.kind = CodexEvent::Kind::Text;
        event.itemId = params.value(u"itemId"_s).toString();
        event.text = params.value(u"delta"_s).toString();
    } else if (method == u"item/started" || method == u"item/completed") {
        const QJsonObject item = params.value(u"item"_s).toObject();
        const QString type = itemTypeOf(params);
        event.itemId = item.value(u"id"_s).toString();
        event.itemType = type;
        if (isOwnToolItem(type)) {
            event.kind = CodexEvent::Kind::OwnTool;
        } else if (method == u"item/completed" && type == u"agentMessage" && item.contains(u"text"_s)) {
            event.kind = CodexEvent::Kind::ItemText;
            event.text = item.value(u"text"_s).toString();
        } else {
            event.kind = CodexEvent::Kind::Activity;
        }
    } else if (method == u"turn/started") {
        event.kind = CodexEvent::Kind::TurnStarted;
        event.turnId = params.value(u"turn"_s).toObject().value(u"id"_s).toString();
    } else if (method == u"turn/completed") {
        const QJsonObject turn = params.value(u"turn"_s).toObject();
        const QJsonObject error = turn.value(u"error"_s).toObject();
        event.kind = CodexEvent::Kind::TurnDone;
        event.turnId = turn.value(u"id"_s).toString();
        event.status = statusOf(turn.value(u"status"_s).toString());
        event.errorMessage = error.value(u"message"_s).toString();
        event.errorInfo = error.value(u"codexErrorInfo"_s);
    } else if (method == u"error") {
        const QJsonObject error = params.value(u"error"_s).toObject();
        event.kind = CodexEvent::Kind::ErrorNotice;
        event.willRetry = params.value(u"willRetry"_s).toBool();
        event.errorMessage = error.value(u"message"_s).toString();
        event.errorInfo = error.value(u"codexErrorInfo"_s);
    } else if (method == u"account/login/completed") {
        event.kind = CodexEvent::Kind::LoginCompleted;
        event.loginId = params.value(u"loginId"_s).toString();
        event.ok = params.value(u"success"_s).toBool();
        event.errorMessage = params.value(u"error"_s).toString();
    } else if (method == u"account/updated") {
        event.kind = CodexEvent::Kind::AccountUpdated;
    } else if (method.startsWith(u"item/")) {
        event.kind = CodexEvent::Kind::Activity;        // reasoning deltas, plan updates, progress
    }
    return event;
}

Error CodexEvents::turnError(const QString &message, const QJsonValue &errorInfo) {
    const ErrorInfo info = infoOf(errorInfo);
    const QString said = tidy(message);
    const QString lower = message.toLower();
    const QString text = said.isEmpty() ? u"the turn failed"_s : said;

    if (info.name == u"unauthorized"
        || anyOf(lower, {u"not logged in", u"please log in", u"log in again", u"sign in again"})) {
        return Error{ErrorCode::Unauthorized, notSignedInMessage()};
    }
    if (info.name == u"usageLimitExceeded" || info.name == u"rateLimitExceeded" || info.name == u"serverOverloaded"
        || anyOf(lower, {u"usage limit", u"rate limit", u"too many requests"})) {
        return Error{ErrorCode::Http, u"HTTP 429: Codex: %1"_s.arg(text)};
    }
    if (info.name == u"httpConnectionFailed" || info.name == u"responseStreamConnectionFailed"
        || info.name == u"responseStreamDisconnected" || info.name == u"responseTooManyFailedAttempts") {
        if (info.httpStatus == 402 || info.httpStatus == 429 || info.httpStatus >= 500) {
            return Error{ErrorCode::Http, u"HTTP %1: Codex: %2"_s.arg(info.httpStatus).arg(text)};
        }
        return Error{ErrorCode::Network, u"Codex could not reach its servers: %1"_s.arg(text)};
    }
    if (lower.contains(u"model"_s)
        && anyOf(lower, {u"does not exist", u"not found", u"not supported", u"unknown model", u"do not have access"})) {
        return Error{ErrorCode::NotFound, u"Codex: %1"_s.arg(text)};
    }
    return Error{ErrorCode::Unknown, u"Codex: %1"_s.arg(text)};
}

Error CodexEvents::rpcError(int code, const QString &message) {
    constexpr int kServerOverloaded = -32001;       // "Server overloaded; retry later."
    if (code == kServerOverloaded) return Error{ErrorCode::Http, u"HTTP 429: Codex: %1"_s.arg(tidy(message))};
    return turnError(message, QJsonValue());
}

Error CodexEvents::exitError(int exitCode, const QString &stderrTail) {
    const QString lower = stderrTail.toLower();
    if (anyOf(lower, {u"not logged in", u"authenticat", u"codex login"})) {
        return Error{ErrorCode::Unauthorized, notSignedInMessage()};
    }
    const QStringList lines = stderrTail.trimmed().split(u'\n', Qt::SkipEmptyParts);
    const QString last = lines.isEmpty() ? u"no message"_s : tidy(lines.last());
    return Error{ErrorCode::ConnectionRefused,
                 u"Codex stopped (exit %1): %2"_s.arg(exitCode).arg(last.isEmpty() ? u"no message"_s : last)};
}

} // namespace nexa
