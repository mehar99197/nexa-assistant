#pragma once

#include "core/Result.h"
#include "core/ai/CodexEvents.h"
#include "core/ai/CodexProtocol.h"

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QObject>
#include <QProcess>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <functional>
#include <memory>

namespace nexa {

// One `codex app-server` process and its JSON-RPC (Facade): starts it in its own process group with Codex's
// own tools off, does the handshake (`initialize`, then `initialized`), matches replies to requests, hands
// out the notifications, and answers every request the server makes with a refusal, so a turn can never
// wait for Nexa. Calls made before the handshake is done wait for it. The user's CODEX_HOME is the
// program's own: its sign-in is the owner's, and Nexa never reads it. `closed` comes once, when the
// process ended or never started; every call still waiting then fails with the same error.
class CodexConnection : public QObject {
    Q_OBJECT
public:
    using Reply = std::function<void(Result<QJsonObject>)>;

    // `workingDir`: Nexa's own folder for Codex; the process runs in its empty `work` folder (see workspace()).
    CodexConnection(QString program, QString workingDir, QObject *parent = nullptr);
    ~CodexConnection() override;

    void start();                                   // idempotent
    void call(const QString &method, const QJsonObject &params, Reply reply);
    void kill();                                    // the process group, now
    bool isBroken() const { return broken_; }
    bool isReady() const { return ready_; }
    // A server that has not finished the handshake by then is not Codex (or is not working): 20 s.
    void setHandshakeTimeoutMs(int ms) { handshakeTimeoutMs_ = ms; }

    // `dir/work`, created for the owner alone: an empty folder for the server (and its threads) to stand in.
    static QString workspace(const QString &dir);

signals:
    void notification(const nexa::CodexMessage &message);
    void closed(const nexa::Error &error);

private:
    struct Waiting {
        QString method;
        QJsonObject params;
        Reply reply;
    };

    void send(const QString &method, const QJsonObject &params, Reply reply);
    void readOutput();
    void handle(const CodexMessage &message);
    void finish(const Error &error);

    QString program_;
    QString workingDir_;
    QProcess process_;
    QTimer handshakeTimer_;
    QByteArray buffer_;
    QString stderrTail_;
    QHash<qint64, Reply> pending_;
    QList<Waiting> queue_;
    qint64 nextId_{1};
    int handshakeTimeoutMs_{20'000};
    int garbageLines_{0};
    bool started_{false};
    bool ready_{false};
    bool broken_{false};
    bool killed_{false};
    bool sawValid_{false};
    Error brokenWith_;
};

// What a turn came to when Codex used one of its own tools and Nexa stopped it.
struct CodexTurnResult {
    QString response;           // everything streamed
    QStringList ownTools;       // the kinds of item Codex used that are its own tools
};

// One warm Codex server holding a conversation (Facade over CodexConnection): checks the account once,
// asks the user's config which MCP servers it would start and switches them off, starts a thread — ephemeral,
// read-only, never asking — and runs one turn at a time on it, streaming the agent's messages (a break
// between one message and the next). Codex's own tools are off at the command line; an item that is one
// anyway is stopped at once (turn/interrupt) and reported. A cancel is a turn/interrupt too, and the process
// group goes if Codex has not ended the turn in 3 s. The process lives in its own session, goes after
// ten idle minutes, and a session ends with its process.
class CodexSession : public QObject {
    Q_OBJECT
public:
    CodexSession(QString program, QString workingDir, QObject *parent = nullptr);
    ~CodexSession() override;

    void start();                                   // idempotent: the process and the handshake
    // One turn. `newThread`: begin a new thread first (the first turn, or after one that cannot be trusted);
    // `model` empty: the first model Codex lists as visible.
    void send(const QString &text, const QString &model, bool newThread);
    void interrupt();                               // the turn in progress; `settled` follows
    void stop();                                    // kills the process
    bool isUsable() const { return !broken_; }
    bool isBusy() const { return busy_; }
    bool isInterrupting() const { return interrupting_; }
    int turnsTaken() const { return turns_; }

    static QString workspace(const QString &dir) { return CodexConnection::workspace(dir); }
    static const QString &developerInstructions();

signals:
    void activity();                                // Codex did something during a turn (thinking counts)
    void text(const QString &delta);
    void ownTool(const QString &type);
    void turnFinished(const nexa::CodexTurnResult &result);
    void turnFailed(const nexa::Error &error);
    void settled();                                 // an interrupt took effect (or the process ended doing it)

private:
    void advance();
    void failTurn(const Error &error);
    void onNotification(const CodexMessage &message);
    void onClosed(const Error &error);
    void emitText(const QString &itemId, const QString &delta);
    void finishTurn(const CodexEvent &event);
    void sendInterrupt();
    void settleInterrupt();
    // A request of the turn on its way: the answer is dropped when that turn is over by then.
    void ask(const QString &method, const QJsonObject &params, CodexConnection::Reply done);

    QString program_;
    QString workingDir_;
    std::unique_ptr<CodexConnection> connection_;
    QTimer idle_;
    QTimer killTimer_;
    QString text_;                      // the turn on its way
    QString requestedModel_;
    QString defaultModel_;              // the first visible model, once listed
    QString threadId_;
    QString turnId_;
    QString lastItem_;
    QSet<QString> deltaItems_;
    QString reply_;
    QStringList ownTools_;
    QString lastNoticeMessage_;         // the last error Codex said it would not retry
    QJsonValue lastNoticeInfo_;
    QStringList mcpServers_;
    quint64 serial_{0};
    int turns_{0};
    bool mcpKnown_{false};
    bool accountOk_{false};
    bool broken_{false};
    bool busy_{false};
    bool turnRequested_{false};
    bool interrupting_{false};
    bool ownToolInterrupt_{false};
    bool killedForInterrupt_{false};
};

} // namespace nexa
