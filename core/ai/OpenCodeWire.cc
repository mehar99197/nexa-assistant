#include "core/ai/OpenCodeWire.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QRegularExpression>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace nexa {

QJsonObject OpenCodeWire::promptBody(const QString &model, const QString &system, const QString &text,
                                     const QList<ImageAttachment> &images) {
    QJsonArray parts{QJsonObject{{u"type"_s, u"text"_s}, {u"text"_s, text}}};
    for (const ImageAttachment &image : images) {
        parts.append(QJsonObject{
            {u"type"_s, u"file"_s},
            {u"mime"_s, image.mimeType},
            {u"url"_s, u"data:%1;base64,%2"_s.arg(image.mimeType, QString::fromLatin1(image.data.toBase64()))},
        });
    }
    // Every tool of opencode's own off: Nexa's tools travel in the prompt, and only Nexa acts. opencode's
    // free models are served only to requests that carry its read tool ("Free usage exceeded"
    // otherwise, measured 2026-10-29), so they get that one — which the server denies (OpenCodeServer).
    QJsonObject tools{{u"*"_s, false}};
    QString instructions = system;
    if (model.startsWith(u"opencode/"_s)) {
        tools.insert(u"read"_s, true);
        // Offered one native tool, a model takes that list for the whole truth and says Nexa's tools are
        // missing (DeepSeek, measured): tell it otherwise. A native call by name is handed to Nexa too.
        instructions += u"\n\nAbout your tools: the native tool list you were given holds only `read`, and it is "
                        "switched off here — do not use it. The tools described above are Nexa's, and they are real "
                        "and available: call one by writing a <tool_call> line exactly as described (calling it by "
                        "name as a native tool also works)."_s;
    }
    QJsonObject body{{u"tools"_s, tools}, {u"parts"_s, parts}};
    if (const qsizetype slash = model.indexOf(u'/'); slash > 0) {
        body.insert(u"model"_s, QJsonObject{{u"providerID"_s, model.left(slash)}, {u"modelID"_s, model.mid(slash + 1)}});
    }
    if (!instructions.isEmpty()) body.insert(u"system"_s, instructions);
    return body;
}

OpenCodeEvent OpenCodeWire::decodeEvent(const QByteArray &data) {
    OpenCodeEvent event;
    const QJsonObject object = QJsonDocument::fromJson(data).object();
    const QString type = object.value(u"type"_s).toString();
    const QJsonObject properties = object.value(u"properties"_s).toObject();
    event.sessionId = properties.value(u"sessionID"_s).toString();
    if (type == u"message.part.delta") {
        if (properties.value(u"field"_s).toString() == u"text") {
            event.kind = OpenCodeEvent::Kind::TextDelta;
            event.partId = properties.value(u"partID"_s).toString();
            event.text = properties.value(u"delta"_s).toString();
        }
    } else if (type == u"message.part.updated") {
        // Each part is announced before its deltas; only this says which are reasoning.
        const QJsonObject part = properties.value(u"part"_s).toObject();
        event.kind = OpenCodeEvent::Kind::PartType;
        event.partId = part.value(u"id"_s).toString();
        event.partType = part.value(u"type"_s).toString();
        if (event.partType == u"tool") {
            const QJsonObject state = part.value(u"state"_s).toObject();
            event.toolName = part.value(u"tool"_s).toString();
            event.toolStatus = state.value(u"status"_s).toString();
            event.toolInput = QString::fromUtf8(QJsonDocument(state.value(u"input"_s).toObject()).toJson(QJsonDocument::Compact));
        }
        if (event.sessionId.isEmpty()) event.sessionId = part.value(u"sessionID"_s).toString();
    } else if (type == u"session.status") {
        const QJsonObject status = properties.value(u"status"_s).toObject();
        const QString state = status.value(u"type"_s).toString();
        if (state == u"busy") event.kind = OpenCodeEvent::Kind::Busy;
        if (state == u"idle") event.kind = OpenCodeEvent::Kind::Idle;
        if (state == u"retry") {
            event.kind = OpenCodeEvent::Kind::Retry;
            event.message = status.value(u"message"_s).toString();
            event.reason = status.value(u"action"_s).toObject().value(u"reason"_s).toString();
        }
    } else if (type == u"session.idle") {
        event.kind = OpenCodeEvent::Kind::Idle;
    } else if (type == u"session.error") {
        const QJsonObject error = properties.value(u"error"_s).toObject();
        event.kind = OpenCodeEvent::Kind::Error;
        event.authError = error.value(u"name"_s).toString() == u"ProviderAuthError";
        event.aborted = error.value(u"name"_s).toString() == u"MessageAbortedError";
        event.status = error.value(u"data"_s).toObject().value(u"statusCode"_s).toInt();
        event.message = error.value(u"data"_s).toObject().value(u"message"_s).toString();
        if (event.message.isEmpty()) event.message = error.value(u"name"_s).toString();
    }
    return event;
}

QStringList OpenCodeWire::parseModels(const QByteArray &configProviders, const QString &preferred) {
    QStringList models;
    const QJsonArray providers = QJsonDocument::fromJson(configProviders).object().value(u"providers"_s).toArray();
    for (const QJsonValue &value : providers) {
        const QJsonObject provider = value.toObject();
        const QString id = provider.value(u"id"_s).toString();
        const QJsonObject list = provider.value(u"models"_s).toObject();
        for (auto it = list.begin(); it != list.end(); ++it) models.append(id + u'/' + it.key());
    }
    std::sort(models.begin(), models.end());
    if (const qsizetype at = models.indexOf(preferred); at > 0) models.move(at, 0);
    return models;
}

QHash<QString, bool> OpenCodeWire::parseImageSupport(const QByteArray &configProviders) {
    QHash<QString, bool> images;
    const QJsonArray providers = QJsonDocument::fromJson(configProviders).object().value(u"providers"_s).toArray();
    for (const QJsonValue &value : providers) {
        const QJsonObject provider = value.toObject();
        const QString id = provider.value(u"id"_s).toString();
        const QJsonObject list = provider.value(u"models"_s).toObject();
        for (auto it = list.begin(); it != list.end(); ++it) {
            const QJsonValue image =
                it.value().toObject().value(u"capabilities"_s).toObject().value(u"input"_s).toObject().value(u"image"_s);
            if (image.isBool()) images.insert(id + u'/' + it.key(), image.toBool());
        }
    }
    return images;
}

QString OpenCodeWire::parseDefaultModel(const QByteArray &config) {
    return QJsonDocument::fromJson(config).object().value(u"model"_s).toString();
}

QUrl OpenCodeWire::parseListening(const QByteArray &output) {
#ifdef Q_OS_WIN
    static const QRegularExpression line(u"(?:opencode\\s+)?server listening on (https?://\\S+)"_s);
#else
    static const QRegularExpression line(u"opencode server listening on (https?://\\S+)"_s);
#endif
    const QRegularExpressionMatch match = line.match(QString::fromUtf8(output));
    return match.hasMatch() ? QUrl(match.captured(1)) : QUrl();
}

QString OpenCodeWire::parseSessionId(const QByteArray &body) {
    return QJsonDocument::fromJson(body).object().value(u"id"_s).toString();
}

bool OpenCodeWire::isOwnTool(const QString &name) {
    static const QStringList own{u"bash"_s, u"read"_s, u"glob"_s, u"grep"_s, u"list"_s, u"edit"_s, u"write"_s,
                                 u"apply_patch"_s, u"task"_s, u"webfetch"_s, u"websearch"_s, u"google_search"_s,
                                 u"todowrite"_s, u"todoread"_s, u"skill"_s, u"question"_s, u"invalid"_s,
                                 u"list_mcp_resources"_s, u"read_mcp_resource"_s, u"list_mcp_resource_templates"_s};
    return own.contains(name);
}

QNetworkRequest OpenCodeWire::request(const QUrl &base, const QString &path, const QString &password) {
    QString root = base.toString();
    while (root.endsWith(u'/')) root.chop(1);
    QNetworkRequest request(QUrl(root + path));
    request.setHeader(QNetworkRequest::ContentTypeHeader, u"application/json"_s);
    request.setRawHeader("Authorization", "Basic " + (u"opencode:"_s + password).toUtf8().toBase64());
    return request;
}

namespace {

// opencode's own words, made safe to show: its links (their query holds state and codes) are cut out, the
// text is one line and short.
QString scrubbed(QString text) {
    static const QRegularExpression link(u"[A-Za-z][A-Za-z0-9+.-]*://\\S*"_s);
    text.replace(link, u"…"_s);
    text = text.simplified();
    constexpr qsizetype kLongest = 160;
    if (text.size() > kLongest) text = text.left(kLongest - 1) + u"…"_s;
    return text;
}

// A name that is only an identifier: safe to show as it is.
QString identifier(const QString &text) {
    static const QRegularExpression plain(u"\\A[A-Za-z0-9_.-]{1,60}\\z"_s);
    return plain.match(text).hasMatch() ? text : QString();
}

} // namespace

Result<QList<OpenCodeAuthMethod>> OpenCodeWire::parseAuthMethods(const QByteArray &body) {
    const QJsonDocument document = QJsonDocument::fromJson(body);
    if (!document.isObject()) return Error{ErrorCode::Parse, u"opencode's list of sign-ins is not what Nexa expects"_s};
    QList<OpenCodeAuthMethod> methods;
    const QJsonObject providers = document.object();
    for (auto it = providers.begin(); it != providers.end(); ++it) {
        const QJsonArray entries = it.value().toArray();
        for (int index = 0; index < entries.size(); ++index) {
            const QJsonObject entry = entries.at(index).toObject();
            const QJsonValue type = entry.value(u"type"_s);
            if (!type.isString()) continue;
            methods.append(OpenCodeAuthMethod{.providerId = it.key(),
                                              .index = index,
                                              .oauth = type.toString() == u"oauth",
                                              .label = entry.value(u"label"_s).toString(),
                                              .hasPrompts = !entry.value(u"prompts"_s).toArray().isEmpty()});
        }
    }
    return methods;
}

QByteArray OpenCodeWire::authorizeBody(int method) {
    return QJsonDocument(QJsonObject{{u"method"_s, method}}).toJson(QJsonDocument::Compact);
}

QByteArray OpenCodeWire::callbackBody(int method, const QString &code) {
    QJsonObject body{{u"method"_s, method}};
    if (!code.isEmpty()) body.insert(u"code"_s, code);
    return QJsonDocument(body).toJson(QJsonDocument::Compact);
}

Result<OpenCodeAuthorization> OpenCodeWire::parseAuthorization(const QByteArray &body) {
    // An index that is no oauth method answers 200 with `null`.
    if (body.trimmed() == "null") {
        return Error{ErrorCode::NotSupported, u"opencode has no browser sign-in for that method"_s};
    }
    const QJsonDocument document = QJsonDocument::fromJson(body);
    const Error unexpected{ErrorCode::Parse, u"opencode's sign-in reply is not what Nexa expects"_s};
    if (!document.isObject()) return unexpected;
    const QJsonObject reply = document.object();
    const QString method = reply.value(u"method"_s).toString();
    if (method != u"auto" && method != u"code") return unexpected;
    // The link is opened for the person: a web page only, never a file or a script.
    const QUrl url(reply.value(u"url"_s).toString(), QUrl::TolerantMode);
    if (!url.isValid() || url.host().isEmpty() || (url.scheme() != u"https" && url.scheme() != u"http")) {
        return unexpected;
    }
    return OpenCodeAuthorization{.url = url,
                                 .needsCode = method == u"code",
                                 .instructions = reply.value(u"instructions"_s).toString()};
}

bool OpenCodeWire::isTrue(const QByteArray &body) {
    return body.trimmed() == "true";
}

Result<QStringList> OpenCodeWire::parseConnected(const QByteArray &body) {
    const QJsonObject object = QJsonDocument::fromJson(body).object();
    QStringList connected;
    if (object.value(u"connected"_s).isArray()) {
        for (const QJsonValue &id : object.value(u"connected"_s).toArray()) {
            if (id.isString()) connected.append(id.toString());
        }
        return connected;
    }
    if (object.value(u"providers"_s).isArray()) {
        for (const QJsonValue &provider : object.value(u"providers"_s).toArray()) {
            const QString id = provider.toObject().value(u"id"_s).toString();
            if (!id.isEmpty()) connected.append(id);
        }
        return connected;
    }
    return Error{ErrorCode::Parse, u"opencode's list of connected providers is not what Nexa expects"_s};
}

Error OpenCodeWire::authFailure(int httpStatus, const QByteArray &body, OpenCodeAuthStep step) {
    // opencode's errors: {name, data: {…}} — Effect's own: {_tag, message}.
    const QJsonObject object = QJsonDocument::fromJson(body).object();
    const QJsonObject data = object.value(u"data"_s).toObject();
    QString name = object.value(u"name"_s).toString();
    if (name.isEmpty()) name = object.value(u"_tag"_s).toString();
    QString message = data.value(u"message"_s).toString();
    if (message.isEmpty()) message = object.value(u"message"_s).toString();
    message = scrubbed(message);

    if (name == u"ProviderAuthOauthMissing") {
        return Error{ErrorCode::NotFound, u"opencode no longer has that sign-in pending (it timed out, or opencode "
                                          "restarted). Start it again."_s};
    }
    if (name == u"ProviderAuthOauthCodeMissing") {
        return Error{ErrorCode::InvalidArgument, u"opencode needs the code the browser page shows. Paste it and "
                                                 "try again."_s};
    }
    if (name == u"ProviderAuthOauthCallbackFailed") {
        if (step == OpenCodeAuthStep::SubmitCode) {
            return Error{ErrorCode::Unauthorized, u"opencode did not accept that code. Check it, or start the "
                                                  "sign-in again."_s};
        }
        return Error{ErrorCode::Unauthorized, u"The sign-in did not complete: it was refused, failed or timed out. "
                                              "Try again."_s};
    }
    if (name == u"ProviderAuthValidationFailed") {
        const QString field = identifier(data.value(u"field"_s).toString());
        return Error{ErrorCode::InvalidArgument,
                     field.isEmpty() ? u"opencode did not accept the sign-in details."_s
                                     : u"opencode did not accept the value for %1."_s.arg(field)};
    }
    if (name == u"ProviderAuthError") {
        return Error{ErrorCode::Unauthorized, message.isEmpty() ? u"opencode could not sign in."_s
                                                                 : u"opencode could not sign in: %1"_s.arg(message)};
    }
    if (name == u"BadRequest" || name == u"InvalidRequestError") {
        return Error{ErrorCode::InvalidArgument,
                     message.isEmpty() ? u"opencode did not understand the sign-in request."_s
                                       : u"opencode did not understand the sign-in request: %1"_s.arg(message)};
    }
    if (httpStatus == 401 || httpStatus == 403) {
        return Error{ErrorCode::Unauthorized, u"opencode's server refused Nexa's request (HTTP %1)."_s.arg(httpStatus)};
    }
    if (step == OpenCodeAuthStep::Authorize && (httpStatus >= 500 || identifier(name).isEmpty())) {
        // opencode starts a listener for some sign-ins (ChatGPT's, on port 1455): a program that holds it fails them.
        return Error{ErrorCode::Http, u"opencode could not start that sign-in (HTTP %1). If another program is "
                                      "already signing in — a terminal opencode, the Codex CLI — finish or close it "
                                      "and try again."_s.arg(httpStatus)};
    }
    const QString known = identifier(name);
    return Error{ErrorCode::Http, known.isEmpty() ? u"opencode refused the sign-in (HTTP %1)."_s.arg(httpStatus)
                                                  : u"opencode refused the sign-in (%1, HTTP %2)."_s.arg(known).arg(httpStatus)};
}

} // namespace nexa
