#include "core/ai/CodexProvider.h"

#include "core/Logging.h"
#include "core/ai/ChatStream.h"
#include "core/ai/CodexEvents.h"
#include "core/ai/ProgramLookup.h"
#include "core/ai/TranscriptWriter.h"

#include <QDir>
#include <QJsonObject>
#include "core/config/Portable.h"
#include <QTimer>
#ifdef Q_OS_WIN
#include <QSet>
#endif

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

constexpr int kModelsTimeoutMs = 30'000;
constexpr int kModelPages = 10;

// Sent once when Codex's model reached for one of Codex's own tools instead of Nexa's.
const QString kReminder = u"Use only Nexa's tools, written as <tool_call> blocks as described in Nexa's instructions. "
                          "Your own tools are switched off."_s;

QString notInstalled() {
    return u"Codex is not installed (%1)"_s.arg(programSearchHint());
}

#ifdef Q_OS_WIN
QStringList toolNamesOf(const QList<ToolSpec> &tools) {
    QStringList names;
    names.reserve(tools.size());
    for (const ToolSpec &spec : tools) names.append(spec.name);
    names.sort();
    return names;
}
#endif

} // namespace

// The stream of one Codex turn. The provider drives it, since it holds the session.
class CodexStream : public ChatStream {
public:
    std::function<void()> onAbort;

    void text(const QString &delta) { emitText(delta); }
#ifdef Q_OS_WIN
    void finishWithToolCalls(QList<ToolCall> calls) { complete(FinishReason::ToolCalls, std::move(calls)); }
#endif
    void done() { complete(FinishReason::Stop, {}); }
    void error(const Error &error) { fail(error); }

protected:
    void abortTransport() override {
        if (onAbort) onAbort();
    }
};

CodexProvider::CodexProvider(ProviderConfig config, QString workingDir, QObject *parent)
    : IAiProvider(parent), config_(std::move(config)), workingDir_(std::move(workingDir)) {
    if (workingDir_.isEmpty()) {
        workingDir_ = portable::cacheDir() + u"/codex"_s;
    }
    turnTimer_.setSingleShot(true);
    connect(&turnTimer_, &QTimer::timeout, this, [this] {
        CodexStream *stream = stream_;
        if (!stream) return;
        stream_ = nullptr;
        ledger_.reset();
        session_.reset();       // a server that says nothing is ended, not asked
        stream->error(Error{ErrorCode::Timeout, u"Codex stopped answering: nothing for %1 s"_s.arg(
                                                    std::max(1, turnTimeoutMs_ / 1000))});
    });
}

CodexProvider::~CodexProvider() = default;

IAiProvider::Capabilities CodexProvider::capabilities() const {
#ifdef Q_OS_WIN
    return Capabilities{.streaming = true, .nativeTools = true, .vision = false};
#else
    return Capabilities{.streaming = true, .nativeTools = false, .vision = false};
#endif
}

QString CodexProvider::locate(const QString &configured) {
    return findProgram(u"codex"_s, configured.trimmed());
}

QString CodexProvider::program() const {
    return locate(config_.baseUrl);
}

void CodexProvider::listModels(std::function<void(Result<QStringList>)> done) {
    const QString path = program();
    if (path.isEmpty() || !models_.isEmpty()) {
        const Result<QStringList> answer = path.isEmpty() ? Result<QStringList>(Error{ErrorCode::NotFound, notInstalled()})
                                                          : Result<QStringList>(models_);
        QTimer::singleShot(0, this, [done = std::move(done), answer] { done(answer); });
        return;
    }
    auto *connection = new CodexConnection(path, workingDir_, this);
    auto answered = std::make_shared<bool>(false);
    auto finish = [this, connection, answered, done = std::move(done)](Result<QStringList> result) {
        if (std::exchange(*answered, true)) return;
        if (result) models_ = result.value();
        connection->deleteLater();
        done(std::move(result));
    };
    connect(connection, &CodexConnection::closed, this, [finish](const Error &error) { finish(error); });
    QTimer::singleShot(kModelsTimeoutMs, connection, [finish] {
        finish(Error{ErrorCode::Timeout, u"Codex did not list its models within 30 s"_s});
    });

    // The account first: model/list answers without a sign-in too, with a catalogue the account cannot use.
    auto collected = std::make_shared<QStringList>();
    using Page = std::function<void(const QString &, int)>;
    auto page = std::make_shared<Page>();
    *page = [connection, finish, collected, weak = std::weak_ptr<Page>(page)](const QString &cursor, int pages) {
        connection->call(u"model/list"_s, CodexProtocol::modelListParams(cursor),
                         [finish, collected, next = weak.lock(), pages](Result<QJsonObject> result) {
                             if (!result) return finish(result.error());
                             *collected += CodexProtocol::parseModels(result.value());
                             const QString cursor = CodexProtocol::nextCursor(result.value());
                             if (!cursor.isEmpty() && pages + 1 < kModelPages && next) return (*next)(cursor, pages + 1);
                             collected->removeDuplicates();
                             if (collected->isEmpty()) return finish(Error{ErrorCode::Parse, u"Codex listed no models"_s});
                             finish(*collected);
                         });
    };
    connection->call(u"account/read"_s, {}, [finish, page](Result<QJsonObject> result) {
        if (!result) return finish(result.error());
        const Result<CodexAccount> account = CodexProtocol::parseAccount(result.value());
        if (!account) return finish(account.error());
        if (account.value().kind == CodexAccount::Kind::SignedOut) {
            return finish(Error{ErrorCode::Unauthorized, CodexEvents::notSignedInMessage()});
        }
        (*page)(QString(), 0);
    });
}

std::shared_ptr<ChatStream> CodexProvider::chat(const ChatRequest &request) {
    auto *stream = new CodexStream;
    std::shared_ptr<ChatStream> handle(stream, [](ChatStream *done) { done->deleteLater(); });
    // Failures go out from the event loop, once the caller has connected to the stream.
    const auto failSoon = [stream](const Error &error) {
        QTimer::singleShot(0, stream, [stream, error] { stream->error(error); });
    };
    const QString path = program();
    if (path.isEmpty()) {
        failSoon(Error{ErrorCode::NotFound, notInstalled()});
        return handle;
    }
#ifdef Q_OS_WIN
    // Continuation of an open Codex turn paused on native dynamic item/tool/call request(s).
    if (session_ && session_->isUsable() && session_->hasPendingToolCalls() && !stream_) {
        const QStringList wanted = session_->pendingToolCallIds();
        QList<ChatMessage> toolResults;
        QSet<QString> foundIds;
        for (qsizetype i = request.messages.size() - 1; i >= 0; --i) {
            const ChatMessage &msg = request.messages.at(i);
            if (msg.role == Role::Assistant) break;
            if (msg.role == Role::Tool && wanted.contains(msg.toolCallId)) {
                toolResults.append(msg);
                foundIds.insert(msg.toolCallId);
            }
        }
        if (foundIds.size() == wanted.size()) {
            pending_ = request;
            if (pending_.model.isEmpty()) pending_.model = config_.defaultModel;
            stream_ = stream;
            activeStream_ = stream;
            turnTimer_.start(turnTimeoutMs_);
            stream->onAbort = [self = QPointer<CodexProvider>(this)] {
                if (!self) return;
                self->turnTimer_.stop();
                self->stream_ = nullptr;
                self->ledger_.reset();
                if (self->session_) self->session_->interrupt();
            };
            connect(stream, &QObject::destroyed, this, [this](QObject *gone) {
                if (gone != activeStream_ || !session_ || !session_->isBusy() || session_->isInterrupting()) return;
                turnTimer_.stop();
                ledger_.reset();
                session_->interrupt();
            });
            QTimer::singleShot(0, stream, [stream, session = QPointer<CodexSession>(session_.get()),
                                           results = std::move(toolResults)] {
                if (session && !stream->isDone()) session->submitToolResults(results);
            });
            return handle;
        }
        session_->interrupt();
        retireSession();
        ledger_.reset();
    }
#endif
    if (session_ && session_->isBusy()) {
        // Only a turn nobody waits for any more (cancelled, dropped) is left to wind down; one that is being
        // stopped for using Codex's own tool still has a stream waiting, which must not be orphaned.
        if (!session_->isInterrupting() || stream_) {
            failSoon(Error{ErrorCode::Unknown, u"Codex is still answering the previous request"_s});
            return handle;
        }
        retireSession();        // a cancelled turn still winding down: leave it to finish, start another
    }

    pending_ = request;
    if (pending_.model.isEmpty()) pending_.model = config_.defaultModel;
    if (!session_ || !session_->isUsable()) ledger_.reset();
#ifdef Q_OS_WIN
    const QStringList offeredTools = toolNamesOf(pending_.tools);
    if (session_ && offeredTools != threadToolNames_) ledger_.reset();
#endif
    const TurnPlan plan = ledger_.plan(pending_);
    QStringList parts;
    bool newThread = false;
    if (plan.fresh) {
        ledger_.reset();
        if (!session_ || !session_->isUsable()) replaceSession(path);
        newThread = true;
#ifdef Q_OS_WIN
        threadToolNames_ = offeredTools;
        session_->setDynamicTools(pending_.tools);
        if (pending_.tools.isEmpty() && !plan.system.isEmpty()) {
            parts.append(TranscriptWriter::instructions(plan.system));
        }
#else
        if (!plan.system.isEmpty()) parts.append(TranscriptWriter::instructions(plan.system));
#endif
        if (const QString earlier = TranscriptWriter::history(plan.history); !earlier.isEmpty()) parts.append(earlier);
    } else if (plan.instructionsChanged) {
        parts.append(TranscriptWriter::instructionsUpdate(plan.system));
    }
    parts.append(TranscriptWriter::turnText(plan.turns));
    qCDebug(lcAi) << "codex" << (plan.fresh ? "fresh" : "continued") << "turn," << pending_.model;

    reminded_ = false;
    stream_ = stream;
    turnTimer_.start(turnTimeoutMs_);
    stream->onAbort = [self = QPointer<CodexProvider>(this)] {
        if (!self) return;
        self->turnTimer_.stop();
        self->stream_ = nullptr;
        self->ledger_.reset();      // its thread holds a half answer Nexa's conversation does not
        if (self->session_) self->session_->interrupt();
    };
    activeStream_ = stream;
    // A turn nobody waits for any more (the stream was dropped without a cancel) is stopped, not left to run.
    connect(stream, &QObject::destroyed, this, [this](QObject *gone) {
        if (gone != activeStream_ || !session_ || !session_->isBusy() || session_->isInterrupting()) return;
        turnTimer_.stop();
        ledger_.reset();
        session_->interrupt();
    });
#ifdef Q_OS_WIN
    const QString devExtra = plan.fresh ? plan.system : QString();
    QTimer::singleShot(0, stream, [stream, session = QPointer<CodexSession>(session_.get()),
                                   text = parts.join(u"\n\n"_s), model = pending_.model, newThread, devExtra] {
        if (session && !stream->isDone()) session->send(text, model, newThread, devExtra); // cancelled before it began: not sent
    });
#else
    QTimer::singleShot(0, stream, [stream, session = QPointer<CodexSession>(session_.get()),
                                   text = parts.join(u"\n\n"_s), model = pending_.model, newThread] {
        if (session && !stream->isDone()) session->send(text, model, newThread);     // cancelled before it began: not sent
    });
#endif
    return handle;
}

void CodexProvider::warmUp() {
    if (session_) return;
    const QString path = program();
    if (path.isEmpty()) return;
    replaceSession(path);
    session_->start();
}

void CodexProvider::replaceSession(const QString &program) {
    session_ = std::make_unique<CodexSession>(program, workingDir_);
    CodexSession *session = session_.get();
    connect(session, &CodexSession::activity, this, [this] {
        if (stream_) turnTimer_.start(turnTimeoutMs_);
    });
    connect(session, &CodexSession::text, this, [this](const QString &delta) {
        if (stream_) stream_->text(delta);
    });
#ifdef Q_OS_WIN
    connect(session, &CodexSession::toolCallsReady, this, [this](const QList<ToolCall> &calls) {
        CodexStream *stream = stream_;
        if (!stream) return;
        turnTimer_.stop();
        stream_ = nullptr;
        stream->finishWithToolCalls(calls);
    });
#endif
    connect(session, &CodexSession::turnFinished, this, &CodexProvider::onTurnFinished);
    connect(session, &CodexSession::turnFailed, this, [this](const Error &error) {
        turnTimer_.stop();
        ledger_.reset();
        if (CodexStream *stream = stream_) {
            stream_ = nullptr;
            stream->error(error);
        }
    });
}

// The session of a turn that was cancelled stays alive until Codex has ended the turn (or the 3 s are up).
void CodexProvider::retireSession() {
    CodexSession *old = session_.release();
    old->disconnect(this);
    old->setParent(this);
    connect(old, &CodexSession::settled, old, &QObject::deleteLater);
}

void CodexProvider::onTurnFinished(const CodexTurnResult &result) {
    CodexStream *stream = stream_;
    if (!stream) return;
    turnTimer_.stop();
    if (!result.ownTools.isEmpty() && result.response.trimmed().isEmpty()) {
        if (!std::exchange(reminded_, true)) {
            turnTimer_.start(turnTimeoutMs_);
            session_->send(kReminder, pending_.model, false);
            return;
        }
        ledger_.reset();
        stream_ = nullptr;
        stream->error(Error{ErrorCode::NotSupported, u"Codex tried to use its own %1 tool, which Nexa does not allow"_s.arg(
                                                         result.ownTools.first())});
        return;
    }
    ledger_.commit(pending_);
    stream_ = nullptr;
    stream->done();
}

} // namespace nexa
