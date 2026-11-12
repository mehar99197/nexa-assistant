#include "core/ai/ClaudeCodeEvents.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

#include <initializer_list>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

bool anyOf(const QString &text, std::initializer_list<QStringView> needles) {
    for (QStringView needle : needles) {
        if (text.contains(needle)) return true;
    }
    return false;
}

// The plan as a word for the status line. A plan is an enumeration value of Claude Code's ("max", "pro"), never an
// account: an unknown one is kept to letters, digits, spaces and hyphens, and short.
QString planName(const QString &subscription) {
    const QString plan = subscription.trimmed().toLower();
    if (plan.isEmpty()) return {};
    static const QHash<QString, QString> known{{u"max"_s, u"Max"_s},
                                               {u"pro"_s, u"Pro"_s},
                                               {u"team"_s, u"Team"_s},
                                               {u"enterprise"_s, u"Enterprise"_s},
                                               {u"free"_s, u"Free"_s}};
    if (const auto found = known.constFind(plan); found != known.constEnd()) return *found;
    QString tidy;
    for (const QChar character : plan) {
        if (character.isLetterOrNumber() || character == u' ' || character == u'-') tidy.append(character);
        if (character == u'_') tidy.append(u' ');
    }
    tidy = tidy.simplified().left(32);
    if (tidy.isEmpty()) return {};
    tidy[0] = tidy[0].toUpper();
    return tidy;
}

const QString kParseMessage = u"Claude Code's sign-in status is not the JSON Nexa expects: is this a newer Claude Code?"_s;

} // namespace

ClaudeCodeEvent ClaudeCodeEvents::decode(const QByteArray &line) {
    ClaudeCodeEvent event;
    const QJsonObject object = QJsonDocument::fromJson(line).object();
    const QString type = object.value(u"type"_s).toString();
    if (type == u"system") {
        if (object.value(u"subtype"_s).toString() == u"init") {
            event.kind = ClaudeCodeEvent::Kind::Init;
            event.model = object.value(u"model"_s).toString();
            event.sessionId = object.value(u"session_id"_s).toString();
        }
    } else if (type == u"stream_event") {
        // An Anthropic Messages stream event: of its deltas, only text is the reply (thinking and signatures are not).
        const QJsonObject inner = object.value(u"event"_s).toObject();
        if (inner.value(u"type"_s).toString() == u"content_block_delta") {
            const QJsonObject delta = inner.value(u"delta"_s).toObject();
            if (delta.value(u"type"_s).toString() == u"text_delta") {
                event.kind = ClaudeCodeEvent::Kind::Text;
                event.text = delta.value(u"text"_s).toString();
            }
        }
    } else if (type == u"assistant") {
        // The whole message so far; of interest only when the API refused the turn ("authentication_failed"), which
        // comes with words for the user in its content, not as a text delta.
        const QString error = object.value(u"error"_s).toString();
        if (!error.isEmpty()) {
            event.kind = ClaudeCodeEvent::Kind::ApiError;
            event.apiError = error;
        }
    } else if (type == u"result") {
        event.kind = ClaudeCodeEvent::Kind::Result;
        event.subtype = object.value(u"subtype"_s).toString();
        event.sessionId = object.value(u"session_id"_s).toString();
        // Measured: a turn the API refused ends with subtype "success" and is_error true — the flag is what counts.
        event.ok = !object.value(u"is_error"_s).toBool(false) && !event.subtype.startsWith(u"error"_s);
        const QString result = object.value(u"result"_s).toString();
        if (event.ok) {
            event.response = result;
        } else {
            QStringList words;
            if (!result.trimmed().isEmpty()) words.append(result.trimmed());
            for (const QJsonValue &item : object.value(u"errors"_s).toArray()) {
                const QString said = item.isString() ? item.toString() : item.toObject().value(u"message"_s).toString();
                if (!said.trimmed().isEmpty()) words.append(said.trimmed());
            }
            event.error = words.isEmpty() ? event.subtype : words.join(u"; "_s);
        }
        if (const QJsonValue status = object.value(u"api_error_status"_s); status.isDouble()) {
            event.apiErrorStatus = status.toInt();
        }
    }
    return event;
}

QByteArray ClaudeCodeEvents::userTurn(const QString &text, const QList<ImageAttachment> &images) {
    QJsonArray content;
    for (const ImageAttachment &image : images) {
        content.append(QJsonObject{{u"type"_s, u"image"_s},
                                   {u"source"_s, QJsonObject{{u"type"_s, u"base64"_s},
                                                             {u"media_type"_s, image.mimeType},
                                                             {u"data"_s, QString::fromLatin1(image.data.toBase64())}}}});
    }
    content.append(QJsonObject{{u"type"_s, u"text"_s}, {u"text"_s, text}});
    const QJsonObject turn{{u"type"_s, u"user"_s},
                           {u"message"_s, QJsonObject{{u"role"_s, u"user"_s}, {u"content"_s, content}}}};
    return QJsonDocument(turn).toJson(QJsonDocument::Compact) + '\n';
}

QStringList ClaudeCodeEvents::knownModels() {
    return {u"fable"_s, u"opus"_s, u"sonnet"_s, u"haiku"_s};
}

Result<SignInStatus> ClaudeCodeEvents::parseStatus(const QByteArray &json) {
    const QJsonDocument document = QJsonDocument::fromJson(json);
    if (!document.isObject()) return Error{ErrorCode::Parse, kParseMessage};
    const QJsonObject object = document.object();
    const QJsonValue loggedIn = object.value(u"loggedIn"_s);
    if (!loggedIn.isBool()) return Error{ErrorCode::Parse, kParseMessage};
    if (!loggedIn.toBool()) return SignInStatus{SignInStatus::State::SignedOut, {}, {}};

    // Only these two keys are read. The object also carries the email and the organisation: they stay in it.
    const QString method = object.value(u"authMethod"_s).toString().trimmed().toLower();
    const QString plan = planName(object.value(u"subscriptionType"_s).toString());
    QString detail;
    if (method == u"console" || method.contains(u"api"_s)) {
        detail = u"Anthropic Console (API billing)"_s;
    } else {
        detail = plan.isEmpty() ? u"Claude"_s : u"Claude · "_s + plan;
    }
    return SignInStatus{SignInStatus::State::SignedIn, detail, {}};
}

QString ClaudeCodeEvents::notSignedInMessage() {
    return u"Claude Code is not signed in — open Settings → Providers → Claude Code and press Sign in"_s;
}

Error ClaudeCodeEvents::exitError(int exitCode, const QString &stderrTail) {
    const QString lower = stderrTail.toLower();
    if (anyOf(lower, {u"not logged in", u"/login", u"authenticat", u"invalid api key", u"unauthorized"})) {
        return Error{ErrorCode::Unauthorized, notSignedInMessage()};
    }
    const QStringList lines = stderrTail.trimmed().split(u'\n', Qt::SkipEmptyParts);
    const QString last = lines.isEmpty() ? u"no message"_s : lines.last().trimmed();
    return Error{ErrorCode::ConnectionRefused, u"Claude Code stopped (exit %1): %2"_s.arg(exitCode).arg(last)};
}

Error ClaudeCodeEvents::resultError(const ClaudeCodeEvent &result) {
    const QString text = result.error.trimmed().isEmpty() ? result.subtype : result.error.trimmed();
    const QString lower = text.toLower();
    const QString api = result.apiError.toLower();
    const int status = result.apiErrorStatus;

    if (api.contains(u"auth"_s) || status == 401 || status == 403
        || anyOf(lower, {u"not logged in", u"/login", u"authentication", u"invalid api key", u"unauthorized",
                         u"oauth token", u"please log in"})) {
        return Error{ErrorCode::Unauthorized, notSignedInMessage()};
    }
    const bool limitStatus = status == 402 || status == 429 || status >= 500;
    if (limitStatus || api.contains(u"rate"_s) || api.contains(u"limit"_s) || api.contains(u"overload"_s)
        || anyOf(lower, {u"rate limit", u"rate_limit", u"usage limit", u"hit your limit", u"limit reached",
                         u"too many requests", u"overloaded", u"quota", u"out of extra usage", u"429", u"529"})) {
        return Error{ErrorCode::Http, u"HTTP %1: Claude Code: %2"_s.arg(limitStatus ? status : 429).arg(text)};
    }
    if (status == 404 || api.contains(u"not_found"_s)
        || anyOf(lower, {u"not_found", u"not found", u"no such model", u"unknown model", u"invalid model",
                         u"unsupported model", u"model does not exist"})) {
        return Error{ErrorCode::NotFound,
                     u"Claude Code has no such model (%1) — choose fable, opus, sonnet or haiku in the toolbar"_s.arg(text)};
    }
    return Error{ErrorCode::Unknown, u"Claude Code: "_s + text};
}

} // namespace nexa
