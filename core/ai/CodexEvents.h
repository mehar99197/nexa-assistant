#pragma once

#include "core/Result.h"
#include "core/ai/CodexProtocol.h"

#include <QJsonValue>
#include <QString>

namespace nexa {

enum class CodexTurnStatus { Completed, Interrupted, Failed, InProgress, Unknown };

// One notification of `codex app-server`, as far as Nexa cares.
struct CodexEvent {
    enum class Kind {
        Ignored,
        TurnStarted,        // turnId
        Text,               // itemId, text: a piece of an agent message
        ItemText,           // itemId, text: a whole agent message, when it ends (the deltas may not have come)
        Activity,           // the model is working (reasoning, an item started or ended)
        OwnTool,            // itemType: Codex used one of its own tools
        TurnDone,           // turnId, status, errorMessage / errorInfo
        ErrorNotice,        // errorMessage / errorInfo, willRetry
        AccountUpdated,
        LoginCompleted,     // loginId, ok, errorMessage
    };
    Kind kind{Kind::Ignored};
    QString threadId;
    QString turnId;
    QString itemId;
    QString itemType;
    QString text;
    QString loginId;
    CodexTurnStatus status{CodexTurnStatus::Unknown};
    QString errorMessage;
    QJsonValue errorInfo;       // codexErrorInfo: a name, or an object with one key
    bool willRetry{false};
    bool ok{false};
};

// What the notifications mean and what the failures are called (pure).
class CodexEvents {
public:
    static CodexEvent decode(const CodexMessage &message);
    // Items that are Codex using one of its own tools (shell, patches, MCP servers, web search, …): Nexa
    // switches them all off, so one starting is a mistake to stop. Everything but plain text and thinking.
    static bool isOwnToolItem(const QString &type);
    // A turn that ended with an error: not signed in -> Unauthorized; a usage or rate limit, or an
    // overloaded server -> "HTTP 429: …" (so the chain asks another provider); a model Codex does not
    // have -> NotFound; a connection that failed -> Network.
    static Error turnError(const QString &message, const QJsonValue &errorInfo);
    // A request that came back as a JSON-RPC error.
    static Error rpcError(int code, const QString &message);
    // The process ended before it answered: what it said last on stderr (never more than a line).
    static Error exitError(int exitCode, const QString &stderrTail);
    static QString notSignedInMessage();
    // What Codex said, cut to one short line, without colours and with any web address (a sign-in link
    // carries a secret), email address and key-like string replaced: safe to show and to log.
    static QString tidy(const QString &text);
};

} // namespace nexa
