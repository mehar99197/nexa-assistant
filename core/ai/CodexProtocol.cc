#include "core/ai/CodexProtocol.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QHash>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

QByteArray line(const QJsonObject &object) {
    return QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
}

Error parseError(const QString &what) {
    return Error{ErrorCode::Parse, u"Codex answered in a way Nexa does not understand (%1)"_s.arg(what)};
}

} // namespace

QByteArray CodexProtocol::request(qint64 id, const QString &method, const QJsonObject &params) {
    return line(QJsonObject{{u"id"_s, id}, {u"method"_s, method}, {u"params"_s, params}});
}

QByteArray CodexProtocol::notification(const QString &method, const QJsonObject &params) {
    QJsonObject object{{u"method"_s, method}};
    if (!params.isEmpty()) object.insert(u"params"_s, params);
    return line(object);
}

QByteArray CodexProtocol::response(const QJsonValue &id, const QJsonObject &result) {
    return line(QJsonObject{{u"id"_s, id}, {u"result"_s, result}});
}

QByteArray CodexProtocol::errorResponse(const QJsonValue &id, int code, const QString &message) {
    return line(QJsonObject{{u"id"_s, id},
                            {u"error"_s, QJsonObject{{u"code"_s, code}, {u"message"_s, message}}}});
}

CodexMessage CodexProtocol::decode(const QByteArray &data) {
    CodexMessage message;
    const QJsonDocument document = QJsonDocument::fromJson(data);
    if (!document.isObject()) return message;
    const QJsonObject object = document.object();
    const bool hasId = object.contains(u"id"_s) && !object.value(u"id"_s).isNull();
    const bool hasMethod = object.value(u"method"_s).isString();
    if (hasMethod) {
        message.method = object.value(u"method"_s).toString();
        message.params = object.value(u"params"_s).toObject();
        message.kind = hasId ? CodexMessage::Kind::ServerRequest : CodexMessage::Kind::Notification;
        message.id = object.value(u"id"_s);
    } else if (hasId && object.contains(u"error"_s)) {
        const QJsonObject error = object.value(u"error"_s).toObject();
        message.kind = CodexMessage::Kind::Failure;
        message.id = object.value(u"id"_s);
        message.errorCode = error.value(u"code"_s).toInt();
        message.errorMessage = error.value(u"message"_s).toString();
    } else if (hasId && object.contains(u"result"_s)) {
        message.kind = CodexMessage::Kind::Response;
        message.id = object.value(u"id"_s);
        message.result = object.value(u"result"_s).toObject();
    }
    return message;
}

const QStringList &CodexProtocol::disabledFeatures() {
    // Codex's stable features that are tools of its own (codex-cli 0.163.0, `codex features list`).
    static const QStringList features{u"shell_tool"_s,   u"unified_exec"_s, u"view_image"_s,   u"sleep_tool"_s,
                                      u"image_generation"_s, u"apps"_s,     u"plugins"_s,      u"multi_agent"_s,
                                      u"browser_use"_s,  u"computer_use"_s, u"in_app_browser"_s, u"hooks"_s,
                                      u"goals"_s,        u"memories"_s,     u"skill_search"_s, u"tool_suggest"_s};
    return features;
}

QStringList CodexProtocol::serverArguments() {
    QStringList arguments{u"app-server"_s};
    for (const QString &feature : disabledFeatures()) arguments += {u"--disable"_s, feature};
    arguments += {u"-c"_s, u"web_search=\"disabled\""_s};
    return arguments;
}

QJsonObject CodexProtocol::initializeParams(const QString &version) {
    return QJsonObject{
        {u"clientInfo"_s, QJsonObject{{u"name"_s, u"nexa"_s}, {u"title"_s, u"Nexa"_s}, {u"version"_s, version}}},
        {u"capabilities"_s, QJsonObject{{u"experimentalApi"_s, false}, {u"requestAttestation"_s, false}}}};
}

QJsonObject CodexProtocol::loginStartParams() {
    return QJsonObject{{u"type"_s, u"chatgpt"_s}};
}

QJsonObject CodexProtocol::loginCancelParams(const QString &loginId) {
    return QJsonObject{{u"loginId"_s, loginId}};
}

QJsonObject CodexProtocol::threadStartParams(const QString &model, const QString &cwd,
                                             const QString &developerInstructions, const QStringList &mcpServers) {
    QJsonObject params{{u"cwd"_s, cwd},
                       {u"approvalPolicy"_s, u"never"_s},
                       {u"sandbox"_s, u"read-only"_s},
                       {u"ephemeral"_s, true}};
    if (!model.isEmpty()) params.insert(u"model"_s, model);
    if (!developerInstructions.isEmpty()) params.insert(u"developerInstructions"_s, developerInstructions);
    QJsonObject config;
    if (!mcpServers.isEmpty()) {
        // Merged over the user's own entries, so a server is switched off without its definition.
        QJsonObject servers;
        for (const QString &name : mcpServers) servers.insert(name, QJsonObject{{u"enabled"_s, false}});
        config.insert(u"mcp_servers"_s, servers);
    }
    params.insert(u"config"_s, config);
    return params;
}

QJsonObject CodexProtocol::turnStartParams(const QString &threadId, const QString &text, const QString &model) {
    const QJsonObject input{{u"type"_s, u"text"_s}, {u"text"_s, text}, {u"text_elements"_s, QJsonArray()}};
    QJsonObject params{{u"threadId"_s, threadId}, {u"input"_s, QJsonArray{input}}};
    if (!model.isEmpty()) params.insert(u"model"_s, model);
    return params;
}

QJsonObject CodexProtocol::interruptParams(const QString &threadId, const QString &turnId) {
    return QJsonObject{{u"threadId"_s, threadId}, {u"turnId"_s, turnId}};
}

QJsonObject CodexProtocol::modelListParams(const QString &cursor) {
    QJsonObject params;
    if (!cursor.isEmpty()) params.insert(u"cursor"_s, cursor);
    return params;
}

QString CodexProtocol::planLabel(const QString &plan) {
    static const QHash<QString, QString> labels{
        {u"free"_s, u"Free"_s},           {u"go"_s, u"Go"_s},
        {u"plus"_s, u"Plus"_s},           {u"pro"_s, u"Pro"_s},
        {u"prolite"_s, u"Pro Lite"_s},    {u"promax"_s, u"Pro Max"_s},
        {u"team"_s, u"Team"_s},           {u"business"_s, u"Business"_s},
        {u"self_serve_business_prolite"_s, u"Business"_s},
        {u"self_serve_business_usage_based"_s, u"Business"_s},
        {u"ent26"_s, u"Enterprise"_s},    {u"enterprise"_s, u"Enterprise"_s},
        {u"enterprise_cbp_automation"_s, u"Enterprise"_s},
        {u"enterprise_cbp_usage_based"_s, u"Enterprise"_s},
        {u"edu"_s, u"Edu"_s},             {u"edu_plus"_s, u"Edu Plus"_s},
        {u"edu_pro"_s, u"Edu Pro"_s}};
    return labels.value(plan);
}

Result<CodexAccount> CodexProtocol::parseAccount(const QJsonObject &result) {
    if (!result.contains(u"account"_s)) return parseError(u"account/read without an account"_s);
    CodexAccount account;
    const QJsonValue value = result.value(u"account"_s);
    if (value.isNull()) return account;         // signed out
    if (!value.isObject()) return parseError(u"account/read"_s);
    const QString type = value.toObject().value(u"type"_s).toString();
    if (type == u"chatgpt") {
        account.kind = CodexAccount::Kind::ChatGpt;
        account.plan = planLabel(value.toObject().value(u"planType"_s).toString());     // never the email
    } else if (type == u"apiKey") {
        account.kind = CodexAccount::Kind::ApiKey;
    } else if (!type.isEmpty()) {
        account.kind = CodexAccount::Kind::Other;
    } else {
        return parseError(u"account/read: an account without a type"_s);
    }
    return account;
}

SignInStatus CodexProtocol::statusOf(const CodexAccount &account) {
    SignInStatus status;
    switch (account.kind) {
    case CodexAccount::Kind::SignedOut:
        status.state = SignInStatus::State::SignedOut;
        break;
    case CodexAccount::Kind::ChatGpt:
        status.state = SignInStatus::State::SignedIn;
        status.detail = account.plan.isEmpty() ? u"ChatGPT"_s : u"ChatGPT · %1"_s.arg(account.plan);
        break;
    case CodexAccount::Kind::ApiKey:
        status.state = SignInStatus::State::SignedIn;
        status.detail = u"API key"_s;
        break;
    case CodexAccount::Kind::Other:
        status.state = SignInStatus::State::SignedIn;
        break;
    }
    return status;
}

Result<CodexLogin> CodexProtocol::parseLoginStart(const QJsonObject &result) {
    if (result.value(u"type"_s).toString() != u"chatgpt") return parseError(u"account/login/start: not a ChatGPT login"_s);
    CodexLogin login;
    login.loginId = result.value(u"loginId"_s).toString();
    login.authUrl = QUrl(result.value(u"authUrl"_s).toString(), QUrl::StrictMode);
    if (login.loginId.isEmpty()) return parseError(u"account/login/start: no login id"_s);
    const QString scheme = login.authUrl.scheme();
    if (!login.authUrl.isValid() || (scheme != u"https" && scheme != u"http") || login.authUrl.host().isEmpty()) {
        return parseError(u"account/login/start: no web address to sign in at"_s);
    }
    return login;
}

QStringList CodexProtocol::parseModels(const QJsonObject &result) {
    QStringList models;
    for (const QJsonValue &entry : result.value(u"data"_s).toArray()) {
        const QJsonObject model = entry.toObject();
        if (model.value(u"hidden"_s).toBool(false)) continue;
        QString name = model.value(u"model"_s).toString();
        if (name.isEmpty()) name = model.value(u"id"_s).toString();
        if (!name.isEmpty() && !models.contains(name)) models.append(name);
    }
    return models;
}

QString CodexProtocol::nextCursor(const QJsonObject &modelList) {
    return modelList.value(u"nextCursor"_s).toString();
}

QStringList CodexProtocol::parseMcpServerNames(const QJsonObject &configRead) {
    return configRead.value(u"config"_s).toObject().value(u"mcp_servers"_s).toObject().keys();
}

Result<QString> CodexProtocol::parseThreadStart(const QJsonObject &result) {
    const QString id = result.value(u"thread"_s).toObject().value(u"id"_s).toString();
    if (id.isEmpty()) return parseError(u"thread/start: no thread id"_s);
    return id;
}

Result<QString> CodexProtocol::parseTurnStart(const QJsonObject &result) {
    const QString id = result.value(u"turn"_s).toObject().value(u"id"_s).toString();
    if (id.isEmpty()) return parseError(u"turn/start: no turn id"_s);
    return id;
}

} // namespace nexa
