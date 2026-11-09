#include "core/ai/CodexSession.h"

#include "core/Logging.h"
#include "core/ai/CodexEvents.h"
#include "core/ai/ProgramLookup.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QPointer>

#include <signal.h>
#include <sys/prctl.h>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

constexpr int kIdleStopMs = 10 * 60 * 1000;
constexpr int kInterruptGraceMs = 3'000;
constexpr qsizetype kStderrKept = 4'096;

constexpr auto kProcessFlags = QProcess::UnixProcessFlag::CreateNewSession
    | QProcess::UnixProcessFlag::ResetSignalHandlers | QProcess::UnixProcessFlag::CloseFileDescriptors;

// The process leads its own session, so -pid reaches Codex and anything it started.
void killGroup(qint64 pid) {
    if (pid <= 0) return;
    if (::kill(pid_t(-pid), SIGKILL) != 0) ::kill(pid_t(pid), SIGKILL);
}

void makePrivateDir(const QString &path) {
    QDir().mkpath(path);
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
}

} // namespace

// ---- CodexConnection ----

QString CodexConnection::workspace(const QString &dir) {
    const QString work = dir + u"/work"_s;
    makePrivateDir(dir);
    makePrivateDir(work);
    return work;
}

CodexConnection::CodexConnection(QString program, QString workingDir, QObject *parent)
    : QObject(parent), program_(std::move(program)), workingDir_(std::move(workingDir)) {
    handshakeTimer_.setSingleShot(true);
    connect(&handshakeTimer_, &QTimer::timeout, this, [this] {
        kill();
        finish(Error{ErrorCode::Timeout, u"Codex did not answer its start-up handshake within %1 s"_s.arg(
                                             std::max(1, handshakeTimeoutMs_ / 1000))});
    });
    process_.setProcessChannelMode(QProcess::SeparateChannels);
    connect(&process_, &QProcess::readyReadStandardOutput, this, &CodexConnection::readOutput);
    connect(&process_, &QProcess::readyReadStandardError, this, [this] {
        stderrTail_ += QString::fromUtf8(process_.readAllStandardError());
        if (stderrTail_.size() > kStderrKept) stderrTail_ = stderrTail_.right(kStderrKept);
    });
    connect(&process_, &QProcess::finished, this, [this](int exitCode, QProcess::ExitStatus) {
        readOutput();
        stderrTail_ += QString::fromUtf8(process_.readAllStandardError());
        if (killed_) {
            finish(Error{ErrorCode::Cancelled, u"Codex was stopped"_s});
        } else if (!sawValid_ && garbageLines_ > 0) {
            finish(Error{ErrorCode::Parse, u"Codex printed something Nexa does not understand"_s});
        } else {
            finish(CodexEvents::exitError(exitCode, stderrTail_));
        }
    });
    connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart) return;
        finish(Error{ErrorCode::NotFound, u"Cannot start Codex (%1): %2"_s.arg(program_, process_.errorString())});
    });
}

CodexConnection::~CodexConnection() {
    disconnect(&process_, nullptr, this, nullptr);
    if (process_.state() == QProcess::NotRunning) return;
    killGroup(process_.processId());
    process_.waitForFinished(1'000);
}

void CodexConnection::start() {
    if (started_) return;
    started_ = true;
    process_.setProgram(program_);
    process_.setProcessEnvironment(environmentFor(program_));     // an npm install's node beside it (nvm)
    process_.setArguments(CodexProtocol::serverArguments());
    process_.setWorkingDirectory(workspace(workingDir_));
    process_.setUnixProcessParameters(kProcessFlags);
    // Its own session takes it out of reach of the terminal's hangup: tie it to Nexa instead, so a crash of
    // Nexa does not leave Codex running.
    process_.setChildProcessModifier([] { ::prctl(PR_SET_PDEATHSIG, SIGKILL); });
    qCInfo(lcAi) << "starting codex app-server";
    handshakeTimer_.start(handshakeTimeoutMs_);
    process_.start();
    if (broken_) return;                // it could not even start
    QString version = QCoreApplication::applicationVersion();
    if (version.isEmpty()) version = u"0"_s;
    send(u"initialize"_s, CodexProtocol::initializeParams(version), [this](Result<QJsonObject> result) {
        if (!result) {
            kill();
            finish(result.error());
            return;
        }
        handshakeTimer_.stop();
        process_.write(CodexProtocol::notification(u"initialized"_s));
        ready_ = true;
        const QList<Waiting> waiting = std::exchange(queue_, {});
        for (const Waiting &call : waiting) send(call.method, call.params, call.reply);
    });
}

void CodexConnection::call(const QString &method, const QJsonObject &params, Reply reply) {
    if (broken_) {
        QTimer::singleShot(0, this, [reply = std::move(reply), error = brokenWith_] { reply(error); });
        return;
    }
    if (!ready_) {
        queue_.append(Waiting{method, params, std::move(reply)});
        start();
        return;
    }
    send(method, params, std::move(reply));
}

void CodexConnection::send(const QString &method, const QJsonObject &params, Reply reply) {
    const qint64 id = nextId_++;
    pending_.insert(id, std::move(reply));
    process_.write(CodexProtocol::request(id, method, params));
}

void CodexConnection::kill() {
    killed_ = true;
    if (process_.state() == QProcess::NotRunning) return;
    killGroup(process_.processId());
}

void CodexConnection::readOutput() {
    buffer_ += process_.readAllStandardOutput();
    QPointer<CodexConnection> self(this);
    qsizetype newline = 0;
    while (self && (newline = buffer_.indexOf('\n')) >= 0) {
        const QByteArray line = buffer_.left(newline);
        buffer_.remove(0, newline + 1);
        if (line.trimmed().isEmpty()) continue;
        const CodexMessage message = CodexProtocol::decode(line);
        if (message.kind == CodexMessage::Kind::Invalid) {
            ++garbageLines_;            // counted, never logged: it could be anything
            continue;
        }
        sawValid_ = true;
        handle(message);
    }
}

void CodexConnection::handle(const CodexMessage &message) {
    switch (message.kind) {
    case CodexMessage::Kind::Response:
    case CodexMessage::Kind::Failure: {
        const qint64 id = qint64(message.id.toDouble(-1));
        const auto found = pending_.find(id);
        if (found == pending_.end()) return;
        Reply reply = std::move(found.value());
        pending_.erase(found);
        if (message.kind == CodexMessage::Kind::Response) {
            reply(message.result);
        } else {
            reply(CodexEvents::rpcError(message.errorCode, message.errorMessage));
        }
        break;
    }
    case CodexMessage::Kind::Notification:
        emit notification(message);
        break;
    case CodexMessage::Kind::ServerRequest:
        // An approval, an elicitation, a tool call: Nexa answers none of them, but never leaves one hanging.
        qCInfo(lcAi) << "codex asked for" << message.method << "- refused";
        process_.write(CodexProtocol::errorResponse(message.id, -32601, u"Nexa does not handle "_s + message.method));
        break;
    case CodexMessage::Kind::Invalid:
        break;
    }
}

void CodexConnection::finish(const Error &error) {
    if (broken_) return;
    broken_ = true;
    brokenWith_ = error;
    handshakeTimer_.stop();
    QPointer<CodexConnection> self(this);
    const QHash<qint64, Reply> pending = std::exchange(pending_, {});
    const QList<Waiting> waiting = std::exchange(queue_, {});
    for (const Reply &reply : pending) {
        reply(error);
        if (!self) return;
    }
    for (const Waiting &call : waiting) {
        call.reply(error);
        if (!self) return;
    }
    emit closed(error);
}

// ---- CodexSession ----

const QString &CodexSession::developerInstructions() {
    static const QString text =
        u"You are the model behind Nexa, a desktop assistant. Your own tools (shell, file edits, web search, MCP "
        "servers) are switched off. Use only Nexa's tools, written as <tool_call> blocks as described in Nexa's "
        "instructions at the start of the conversation, and answer the user's request directly."_s;
    return text;
}

CodexSession::CodexSession(QString program, QString workingDir, QObject *parent)
    : QObject(parent), program_(std::move(program)), workingDir_(std::move(workingDir)) {
    idle_.setSingleShot(true);
    idle_.setInterval(kIdleStopMs);
    connect(&idle_, &QTimer::timeout, this, &CodexSession::stop);
    killTimer_.setSingleShot(true);
    killTimer_.setInterval(kInterruptGraceMs);
    connect(&killTimer_, &QTimer::timeout, this, [this] {
        if (!interrupting_ || !connection_) return;
        qCInfo(lcAi) << "codex did not end the turn after the interrupt - ending the process";
        killedForInterrupt_ = true;
        connection_->kill();
    });
}

CodexSession::~CodexSession() = default;

void CodexSession::start() {
    if (connection_) return;
    connection_ = std::make_unique<CodexConnection>(program_, workingDir_);
    connect(connection_.get(), &CodexConnection::notification, this, &CodexSession::onNotification);
    connect(connection_.get(), &CodexConnection::closed, this, &CodexSession::onClosed);
    connection_->start();
}

void CodexSession::stop() {
    idle_.stop();
    broken_ = true;
    if (connection_) connection_->kill();
}

void CodexSession::send(const QString &text, const QString &model, bool newThread) {
    busy_ = true;           // before start(): a program that cannot start fails this turn
    ++turns_;
    ++serial_;
    idle_.stop();
    text_ = text;
    requestedModel_ = model;
    turnId_.clear();
    lastItem_.clear();
    deltaItems_.clear();
    reply_.clear();
    ownTools_.clear();
    lastNoticeMessage_.clear();
    lastNoticeInfo_ = QJsonValue();
    turnRequested_ = false;
    interrupting_ = false;
    ownToolInterrupt_ = false;
    if (newThread) threadId_.clear();
    start();
    if (broken_) {
        const quint64 serial = serial_;
        QTimer::singleShot(0, this, [this, serial] {
            if (serial == serial_ && busy_) failTurn(Error{ErrorCode::ConnectionRefused, u"Codex is no longer running"_s});
        });
        return;
    }
    advance();
}

void CodexSession::ask(const QString &method, const QJsonObject &params, CodexConnection::Reply done) {
    const quint64 serial = serial_;
    connection_->call(method, params, [self = QPointer<CodexSession>(this), serial, done = std::move(done)](
                                          Result<QJsonObject> result) {
        if (!self || self->serial_ != serial || !self->busy_) return;
        done(std::move(result));
    });
}

// The next thing the turn on its way is waiting for: the account, the user's MCP servers, a model, a thread,
// and then the turn itself. Each answer comes back here.
void CodexSession::advance() {
    if (!busy_) return;
    if (interrupting_ && !turnRequested_) {
        settleInterrupt();
        return;
    }
    if (!accountOk_) {
        ask(u"account/read"_s, {}, [this](Result<QJsonObject> result) {
            if (!result) return failTurn(result.error());
            const Result<CodexAccount> account = CodexProtocol::parseAccount(result.value());
            if (!account) return failTurn(account.error());
            if (account.value().kind == CodexAccount::Kind::SignedOut) {
                return failTurn(Error{ErrorCode::Unauthorized, CodexEvents::notSignedInMessage()});
            }
            accountOk_ = true;
            advance();
        });
        return;
    }
    if (!mcpKnown_) {
        // Codex starts the servers of the user's config.toml when a thread starts; a thread of Nexa's gets
        // none of them, so their names come first (and without them the turn does not start).
        ask(u"config/read"_s, QJsonObject{{u"includeLayers"_s, false}}, [this](Result<QJsonObject> result) {
            if (!result) return failTurn(result.error());
            mcpServers_ = CodexProtocol::parseMcpServerNames(result.value());
            mcpKnown_ = true;
            advance();
        });
        return;
    }
    if (requestedModel_.isEmpty() && defaultModel_.isEmpty()) {
        ask(u"model/list"_s, CodexProtocol::modelListParams(), [this](Result<QJsonObject> result) {
            if (!result) return failTurn(result.error());
            const QStringList models = CodexProtocol::parseModels(result.value());
            if (models.isEmpty()) return failTurn(Error{ErrorCode::Parse, u"Codex listed no models"_s});
            defaultModel_ = models.first();
            advance();
        });
        return;
    }
    const QString model = requestedModel_.isEmpty() ? defaultModel_ : requestedModel_;
    if (threadId_.isEmpty()) {
        ask(u"thread/start"_s,
            CodexProtocol::threadStartParams(model, workspace(workingDir_), developerInstructions(), mcpServers_),
            [this](Result<QJsonObject> result) {
                if (!result) return failTurn(result.error());
                const Result<QString> thread = CodexProtocol::parseThreadStart(result.value());
                if (!thread) return failTurn(thread.error());
                threadId_ = thread.value();
                advance();
            });
        return;
    }
    turnRequested_ = true;
    ask(u"turn/start"_s, CodexProtocol::turnStartParams(threadId_, text_, model), [this](Result<QJsonObject> result) {
        if (!result) return failTurn(result.error());
        const Result<QString> turn = CodexProtocol::parseTurnStart(result.value());
        if (!turn) return failTurn(turn.error());
        if (turnId_.isEmpty()) turnId_ = turn.value();
        if (interrupting_) sendInterrupt();
    });
}

void CodexSession::failTurn(const Error &error) {
    const bool wasInterrupting = interrupting_ && !ownToolInterrupt_;
    busy_ = false;
    interrupting_ = false;
    ownToolInterrupt_ = false;
    killTimer_.stop();
    idle_.start();
    if (wasInterrupting) {
        emit settled();
        return;
    }
    emit turnFailed(error);
    if (error.code == ErrorCode::Unauthorized) stop();      // the next try starts a server that reads the sign-in afresh
}

void CodexSession::onNotification(const CodexMessage &message) {
    if (!busy_ || !turnRequested_) return;
    const CodexEvent event = CodexEvents::decode(message);
    if (event.kind == CodexEvent::Kind::Ignored) return;
    if (event.threadId != threadId_) return;       // every notification of a turn names its thread
    if (!event.turnId.isEmpty() && !turnId_.isEmpty() && event.turnId != turnId_) return;
    if (turnId_.isEmpty() && !event.turnId.isEmpty()) {
        turnId_ = event.turnId;
        if (interrupting_) sendInterrupt();
    }

    switch (event.kind) {
    case CodexEvent::Kind::Text:
        emit activity();
        emitText(event.itemId, event.text);
        break;
    case CodexEvent::Kind::ItemText:
        emit activity();
        if (!deltaItems_.contains(event.itemId)) emitText(event.itemId, event.text);
        break;
    case CodexEvent::Kind::TurnStarted:
    case CodexEvent::Kind::Activity:
        emit activity();
        break;
    case CodexEvent::Kind::OwnTool:
        emit activity();
        if (!ownTools_.contains(event.itemType)) {
            ownTools_.append(event.itemType);
            qCInfo(lcAi) << "codex started one of its own tools:" << event.itemType << "- stopping the turn";
            emit ownTool(event.itemType);
        }
        if (!interrupting_) {
            ownToolInterrupt_ = true;
            interrupt();
        }
        break;
    case CodexEvent::Kind::ErrorNotice:
        emit activity();
        if (!event.willRetry) {
            lastNoticeMessage_ = event.errorMessage;
            lastNoticeInfo_ = event.errorInfo;
        }
        break;
    case CodexEvent::Kind::TurnDone:
        finishTurn(event);
        break;
    case CodexEvent::Kind::AccountUpdated:
    case CodexEvent::Kind::LoginCompleted:
    case CodexEvent::Kind::Ignored:
        break;
    }
}

void CodexSession::emitText(const QString &itemId, const QString &delta) {
    if (delta.isEmpty() || interrupting_) return;
    QString piece = delta;
    if (itemId != lastItem_) {
        // A new message of the agent: its own paragraph.
        if (!lastItem_.isEmpty() && !reply_.trimmed().isEmpty()) piece = u"\n\n"_s + piece;
        lastItem_ = itemId;
    }
    deltaItems_.insert(itemId);
    reply_ += piece;
    emit text(piece);
}

void CodexSession::finishTurn(const CodexEvent &event) {
    const bool wasInterrupting = interrupting_;
    const bool forTool = ownToolInterrupt_;
    busy_ = false;
    interrupting_ = false;
    ownToolInterrupt_ = false;
    killTimer_.stop();
    idle_.start();
    if (wasInterrupting && !forTool) {
        emit settled();
        return;
    }
    if (event.status == CodexTurnStatus::Completed || (forTool && event.status == CodexTurnStatus::Interrupted)) {
        emit turnFinished(CodexTurnResult{reply_, ownTools_});
        return;
    }
    if (event.status == CodexTurnStatus::Failed) {
        // The turn's own error, or else the last one Codex announced it would not retry.
        const bool said = !event.errorMessage.isEmpty() || event.errorInfo.isString() || event.errorInfo.isObject();
        const Error error = said ? CodexEvents::turnError(event.errorMessage, event.errorInfo)
                                 : CodexEvents::turnError(lastNoticeMessage_, lastNoticeInfo_);
        emit turnFailed(error);
        if (error.code == ErrorCode::Unauthorized) stop();
        return;
    }
    if (event.status == CodexTurnStatus::Interrupted) {
        emit turnFailed(Error{ErrorCode::Unknown, u"Codex stopped the turn before it answered"_s});
        return;
    }
    emit turnFailed(Error{ErrorCode::Parse, u"Codex ended a turn in a way Nexa does not understand"_s});
}

void CodexSession::interrupt() {
    if (!busy_) return;
    if (interrupting_) {
        // Already being stopped (for Codex's own tool); now somebody has cancelled the turn as well, so nobody
        // wants its result any more, and its end must be a `settled`, not a result to hand on.
        ownToolInterrupt_ = false;
        return;
    }
    interrupting_ = true;
    killTimer_.start();
    if (!turnId_.isEmpty()) sendInterrupt();
    // else: the turn id is not known yet; it is sent when it arrives, or the turn is dropped before it starts.
}

void CodexSession::sendInterrupt() {
    if (!connection_ || threadId_.isEmpty() || turnId_.isEmpty()) return;
    connection_->call(u"turn/interrupt"_s, CodexProtocol::interruptParams(threadId_, turnId_), [](Result<QJsonObject>) {
        // Nothing to do with the answer: turn/completed, or the kill timer, ends it.
    });
}

void CodexSession::settleInterrupt() {
    busy_ = false;
    interrupting_ = false;
    ownToolInterrupt_ = false;
    killTimer_.stop();
    idle_.start();
    emit settled();
}

void CodexSession::onClosed(const Error &error) {
    broken_ = true;
    idle_.stop();
    killTimer_.stop();
    const bool wasBusy = std::exchange(busy_, false);
    const bool userInterrupt = interrupting_ && !ownToolInterrupt_;
    interrupting_ = false;
    ownToolInterrupt_ = false;
    if (!wasBusy) return;
    if (userInterrupt) {
        emit settled();
        return;
    }
    emit turnFailed(std::exchange(killedForInterrupt_, false)
                        ? Error{ErrorCode::Timeout, u"Codex did not stop when asked, so it was ended"_s}
                        : error);
}

} // namespace nexa
