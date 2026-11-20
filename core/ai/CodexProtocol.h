#pragma once

#include "core/Result.h"
#include "core/ai/SignIn.h"
#ifdef Q_OS_WIN
#include "core/tools/ToolSpec.h"
#endif

#include <QByteArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>
#include <QUrl>

namespace nexa {

// One line of `codex app-server`'s newline-delimited JSON-RPC (no "jsonrpc" field), told apart by which
// of id / method / result / error it has.
struct CodexMessage {
    enum class Kind { Invalid, Response, Failure, Notification, ServerRequest };
    Kind kind{Kind::Invalid};
    QJsonValue id;              // a response: the number Nexa sent; a server request: whatever the server chose
    QString method;             // notifications and server requests
    QJsonObject params;         // notifications and server requests
    QJsonObject result;         // responses
    int errorCode{0};           // failures
    QString errorMessage;       // failures
};

// Who is signed in to Codex, as far as Nexa may know: never the email, never a token.
struct CodexAccount {
    enum class Kind { SignedOut, ChatGpt, ApiKey, Other };
    Kind kind{Kind::SignedOut};
    QString plan;               // ChatGPT: the plan's own name ("Free", "Plus", …), empty when unknown
};

struct CodexLogin {
    QString loginId;
    QUrl authUrl;               // opened in a browser by the caller; never logged
};

// `codex app-server`'s wire protocol, as far as Nexa uses it (pure): requests and notifications as lines,
// the parameters of each request, decoders for the replies, and the command line that starts a server with
// Codex's own tools switched off. Protocol of codex-cli 0.163.0 (schemas from `codex app-server
// generate-json-schema`); an unknown reply is an Error, never a guess.
class CodexProtocol {
public:
    // Lines to write (a newline ends each).
    static QByteArray request(qint64 id, const QString &method, const QJsonObject &params = {});
    static QByteArray notification(const QString &method, const QJsonObject &params = {});
    static QByteArray response(const QJsonValue &id, const QJsonObject &result);
    static QByteArray errorResponse(const QJsonValue &id, int code, const QString &message);
    static CodexMessage decode(const QByteArray &line);

    // `codex app-server` and the switches that take Codex's own tools away: every stable tool feature off,
    // web search off. Credentials and the user's config stay where they are (CODEX_HOME is not touched).
    static QStringList serverArguments();
    static const QStringList &disabledFeatures();

    // Requests (the parameters).
    static QJsonObject initializeParams(const QString &version);
    static QJsonObject loginStartParams();
    static QJsonObject loginCancelParams(const QString &loginId);
    // A thread of its own that cannot touch anything: ephemeral, read-only sandbox, never asks. `mcpServers`
    // are the names in the user's config that Codex would start at thread start; each is switched off here.
#ifdef Q_OS_WIN
    static QJsonObject threadStartParams(const QString &model, const QString &cwd, const QString &developerInstructions,
                                         const QStringList &mcpServers, const QList<ToolSpec> &dynamicTools = {});
    static QJsonObject dynamicToolResult(bool success, const QString &text);
#else
    static QJsonObject threadStartParams(const QString &model, const QString &cwd, const QString &developerInstructions,
                                         const QStringList &mcpServers);
#endif
    static QJsonObject turnStartParams(const QString &threadId, const QString &text, const QString &model);
    static QJsonObject interruptParams(const QString &threadId, const QString &turnId);
    static QJsonObject modelListParams(const QString &cursor = {});

    // Replies.
    static Result<CodexAccount> parseAccount(const QJsonObject &result);     // account/read
    static SignInStatus statusOf(const CodexAccount &account);
    static Result<CodexLogin> parseLoginStart(const QJsonObject &result);    // account/login/start
    static QStringList parseModels(const QJsonObject &result);               // model/list: visible ones only
    static QString nextCursor(const QJsonObject &modelList);
    static QStringList parseMcpServerNames(const QJsonObject &configRead);   // config/read
    static Result<QString> parseThreadStart(const QJsonObject &result);      // the thread's id
    static Result<QString> parseTurnStart(const QJsonObject &result);        // the turn's id

    // "Free" for the plan Codex calls "free"; empty for one Nexa does not know.
    static QString planLabel(const QString &plan);
};

} // namespace nexa
