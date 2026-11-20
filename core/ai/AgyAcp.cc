#include "core/ai/AgyAcp.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

constexpr qsizetype kSafeTextLimit = 200;

QByteArray asLine(const QJsonObject &object) {
    return QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
}

QUrl webUrl(const QJsonValue &value) {
    if (!value.isString()) return {};
    const QUrl url(value.toString(), QUrl::StrictMode);
    const bool web = url.scheme() == u"https" || url.scheme() == u"http";
    return url.isValid() && web && !url.host().isEmpty() ? url : QUrl();
}

} // namespace

QByteArray AgyAcp::request(int id, const QString &method, const QJsonObject &params) {
    return asLine({{u"jsonrpc"_s, u"2.0"_s}, {u"id"_s, id}, {u"method"_s, method}, {u"params"_s, params}});
}

QByteArray AgyAcp::initializeRequest(int id) {
    return request(id, u"initialize"_s,
                   {{u"protocolVersion"_s, 1},
                    {u"clientCapabilities"_s, QJsonObject{}},
                    {u"clientInfo"_s, QJsonObject{{u"name"_s, u"nexa"_s},
                                                  {u"version"_s, QStringLiteral(NEXA_VERSION)}}}});
}

QByteArray AgyAcp::newSessionRequest(int id, const QString &workspace) {
    return request(id, u"session/new"_s, {{u"cwd"_s, workspace}, {u"mcpServers"_s, QJsonArray{}}});
}

QByteArray AgyAcp::closeSessionRequest(int id, const QString &sessionId) {
    return request(id, u"session/close"_s, {{u"sessionId"_s, sessionId}});
}

QByteArray AgyAcp::authenticateRequest(int id, const QString &methodId) {
    return request(id, u"authenticate"_s, {{u"methodId"_s, methodId}});
}

QByteArray AgyAcp::logoutRequest(int id) {
    return request(id, u"logout"_s);
}

QByteArray AgyAcp::errorReply(const QJsonValue &id, int code, const QString &message) {
    return asLine({{u"jsonrpc"_s, u"2.0"_s},
                   {u"id"_s, id},
                   {u"error"_s, QJsonObject{{u"code"_s, code}, {u"message"_s, message}}}});
}

AcpMessage AgyAcp::decode(const QByteArray &line) {
    AcpMessage message;
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(line.trimmed(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) return message;
    const QJsonObject object = document.object();

    const QJsonValue id = object.value(u"id"_s);
    const bool hasId = !id.isUndefined() && !id.isNull();
    if (hasId) {
        message.rawId = id;
        if (id.isDouble()) message.id = qint64(id.toInteger(-1));
    }

    if (object.value(u"method"_s).isString() && !object.value(u"method"_s).toString().isEmpty()) {
        message.method = object.value(u"method"_s).toString();      // a notification, or a request of agy's
        message.params = object.value(u"params"_s);
        message.valid = true;
        return message;
    }

    const bool hasError = object.value(u"error"_s).isObject();
    if (!hasId || (!hasError && !object.contains(u"result"_s))) return message;    // not JSON-RPC
    if (hasError) {
        const QJsonObject error = object.value(u"error"_s).toObject();
        message.isError = true;
        message.code = error.value(u"code"_s).toInt();
        message.message = error.value(u"message"_s).toString();
    } else {
        message.result = object.value(u"result"_s);
    }
    message.valid = true;
    return message;
}

std::optional<AcpInitialize> AgyAcp::parseInitialize(const QJsonValue &result) {
    if (!result.isObject()) return std::nullopt;
    const QJsonObject object = result.toObject();
    if (!object.value(u"agentInfo"_s).isObject() && !object.value(u"agentCapabilities"_s).isObject()
        && !object.value(u"authMethods"_s).isArray()) {
        return std::nullopt;
    }
    AcpInitialize info;
    info.version = object.value(u"agentInfo"_s).toObject().value(u"version"_s).toString();
    const QJsonValue logout = object.value(u"agentCapabilities"_s).toObject().value(u"auth"_s).toObject()
                                  .value(u"logout"_s);
    info.canLogout = !logout.isUndefined() && !logout.isNull() && !(logout.isBool() && !logout.toBool());
    for (const QJsonValue &entry : object.value(u"authMethods"_s).toArray()) {
        const QJsonObject method = entry.toObject();
        const QString id = method.value(u"id"_s).toString();
        if (id.isEmpty()) continue;
        info.authMethods.append({id, method.value(u"name"_s).toString()});
    }
    return info;
}

std::optional<AcpSession> AgyAcp::parseSession(const QJsonValue &result) {
    if (!result.isObject()) return std::nullopt;
    const QJsonObject object = result.toObject();
    AcpSession session;
    session.sessionId = object.value(u"sessionId"_s).toString();
    if (session.sessionId.isEmpty()) return std::nullopt;
    const QJsonObject tier = object.value(u"_meta"_s).toObject().value(u"antigravity"_s).toObject()
                                 .value(u"tier"_s).toObject();
    session.plan = tier.value(u"name"_s).toString();
    session.planId = tier.value(u"id"_s).toString();
    return session;
}

bool AgyAcp::isAuthRequired(const AcpMessage &message) {
    // -32000 is ACP's "authentication required"; it is also a generic server error, so the words must agree.
    return message.valid && message.isError && message.code == kAuthRequiredCode
        && message.message.contains(u"auth"_s, Qt::CaseInsensitive);
}

QList<SignInMethod> AgyAcp::oauthMethods(const AcpInitialize &initialize) {
    QList<SignInMethod> methods;
    for (const AcpInitialize::AuthMethod &method : initialize.authMethods) {
        if (!method.id.startsWith(u"oauth-"_s)) continue;
        QString label;
        if (method.id == u"oauth-personal") {
            label = u"Google account"_s;
        } else if (method.id == u"oauth-business") {
            label = u"Gemini Enterprise"_s;
        } else {
            label = method.name.isEmpty() ? method.id : method.name;
        }
        methods.append(SignInMethod{.id = method.id, .label = label, .needsCode = false});
    }
    return methods;
}

QUrl AgyAcp::authLink(const AcpMessage &message) {
    if (!message.isNotification() || !message.method.contains(u"auth"_s, Qt::CaseInsensitive)) return {};
    const QJsonObject params = message.params.toObject();
    for (const QString &key : {u"url"_s, u"authUrl"_s, u"auth_url"_s, u"authorizationUrl"_s, u"uri"_s,
                               u"verificationUri"_s}) {
        if (const QUrl url = webUrl(params.value(key)); !url.isEmpty()) return url;
    }
    return {};
}

QString AgyAcp::safeText(const QString &text) {
    // A link carries the OAuth state, and a parameter named like a secret its value; an address names the account;
    // a long run of token characters is a token; a run of digits or a mixed run of letters and digits is a code.
    static const QRegularExpression link(u"[A-Za-z][A-Za-z0-9+.-]*://\\S*"_s);
    static const QRegularExpression secretParameter(
        u"\\b(state|code|token|access_token|refresh_token|id_token|key|api_key|secret|client_secret|password|"
        "passcode|nonce|verifier|code_verifier)\\s*[=:]\\s*[^\\s&,;\"']+"_s,
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression address(u"[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+"_s);
    static const QRegularExpression tokenLike(u"[A-Za-z0-9_\\-./~+=]{24,}"_s);
    static const QRegularExpression mixedCode(
        u"(?=[A-Za-z0-9_-]*[0-9])(?=[A-Za-z0-9_-]*[A-Za-z])[A-Za-z0-9_-]{12,}"_s);
    static const QRegularExpression digitsCode(u"\\b[0-9]{6,}\\b"_s);
    QString safe = text;
    safe.replace(link, u"[link]"_s);
    safe.replace(secretParameter, u"\\1 [removed]"_s);
    safe.replace(address, u"[address]"_s);
    safe.replace(tokenLike, u"[removed]"_s);
    safe.replace(mixedCode, u"[removed]"_s);
    safe.replace(digitsCode, u"[removed]"_s);
    safe = safe.simplified();
    if (safe.size() > kSafeTextLimit) safe = safe.left(kSafeTextLimit) + u"…"_s;
    return safe;
}

} // namespace nexa
