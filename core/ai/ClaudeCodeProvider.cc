#include "core/ai/ClaudeCodeProvider.h"

#include "core/Logging.h"
#include "core/ai/ChatStream.h"
#include "core/ai/ClaudeCodeSession.h"
#include "core/ai/ProgramLookup.h"
#include "core/ai/TranscriptWriter.h"

#include "core/config/Portable.h"
#include <QTimer>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

QString notInstalled() {
    return u"Claude Code is not installed (%1)"_s.arg(programSearchHint());
}

} // namespace

// The stream of one Claude Code turn. The provider drives it, since it holds the session.
class ClaudeCodeStream : public ChatStream {
public:
    std::function<void()> onAbort;

    void text(const QString &delta) { emitText(delta); }
    void done() { complete(FinishReason::Stop, {}); }
    void error(const Error &error) { fail(error); }

protected:
    void abortTransport() override {
        if (onAbort) onAbort();
    }
};

ClaudeCodeProvider::ClaudeCodeProvider(ProviderConfig config, QString workingDir, QObject *parent)
    : IAiProvider(parent), config_(std::move(config)), workingDir_(std::move(workingDir)) {
    if (workingDir_.isEmpty()) {
        workingDir_ = portable::cacheDir() + u"/claude"_s;
    }
    turnTimer_.setSingleShot(true);
    connect(&turnTimer_, &QTimer::timeout, this, [this] {
        ClaudeCodeStream *stream = stream_;
        if (!stream) return;
        stream_ = nullptr;
        ledger_.reset();
        session_.reset();       // a turn cannot be stopped any other way
        stream->error(Error{ErrorCode::Timeout, u"Claude Code stopped answering: nothing for %1 s"_s.arg(
                                                    std::max(1, turnTimeoutMs_ / 1000))});
    });
}

ClaudeCodeProvider::~ClaudeCodeProvider() = default;

IAiProvider::Capabilities ClaudeCodeProvider::capabilities() const {
    return Capabilities{.streaming = true, .nativeTools = false, .vision = true};
}

QString ClaudeCodeProvider::locate(const QString &configured) {
    return findProgram(u"claude"_s, configured.trimmed());
}

QString ClaudeCodeProvider::program() const {
    return locate(config_.baseUrl);
}

void ClaudeCodeProvider::listModels(std::function<void(Result<QStringList>)> done) {
    const Result<QStringList> answer = program().isEmpty() ? Result<QStringList>(Error{ErrorCode::NotFound, notInstalled()})
                                                           : Result<QStringList>(ClaudeCodeEvents::knownModels());
    QTimer::singleShot(0, this, [done = std::move(done), answer] { done(answer); });
}

std::shared_ptr<ChatStream> ClaudeCodeProvider::chat(const ChatRequest &request) {
    auto *stream = new ClaudeCodeStream;
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
    if (session_ && session_->isBusy()) {
        failSoon(Error{ErrorCode::Unknown, u"Claude Code is still answering the previous request"_s});
        return handle;
    }

    pending_ = request;
    if (pending_.model.isEmpty()) pending_.model = config_.defaultModel;
    if (!session_ || !session_->isUsable()) ledger_.reset();
    TurnPlan plan = ledger_.plan(pending_);
    // The system prompt is a start flag of this program: a running session cannot be told a new one, so a changed
    // prompt is a fresh session given the whole conversation again.
    if (!plan.fresh && plan.instructionsChanged) plan = TurnLedger().plan(pending_);
    QStringList parts;
    if (plan.fresh) {
        ledger_.reset();
        const bool reusable = session_ && session_->isUsable() && session_->turnsTaken() == 0
            && session_->model() == pending_.model && session_->systemPrompt() == plan.system;
        if (!reusable) replaceSession(path, pending_.model, plan.system);
        if (const QString earlier = TranscriptWriter::history(plan.history); !earlier.isEmpty()) parts.append(earlier);
    }
    parts.append(TranscriptWriter::turnText(plan.turns));
    QList<ImageAttachment> images;
    for (const ChatMessage &turn : plan.turns) images += turn.images;
    qCDebug(lcAi) << "claude" << (plan.fresh ? "fresh" : "continued") << "turn," << pending_.model << "," << images.size()
                  << "image(s)";

    stream_ = stream;
    turnTimer_.start(turnTimeoutMs_);
    stream->onAbort = [self = QPointer<ClaudeCodeProvider>(this)] {
        if (!self) return;
        self->turnTimer_.stop();
        self->stream_ = nullptr;
        self->ledger_.reset();
        self->session_.reset();     // kills Claude Code: a turn cannot be stopped any other way
    };
    QTimer::singleShot(0, stream, [session = QPointer<ClaudeCodeSession>(session_.get()), text = parts.join(u"\n\n"_s),
                                   images] {
        if (session) session->send(text, images);
    });
    return handle;
}

void ClaudeCodeProvider::warmUp() {
    // The first request replaces this session (its system prompt is a start flag and not known yet), but the start
    // brings the program's files into the page cache, which is most of a cold start.
    if (session_) return;
    const QString path = program();
    if (path.isEmpty()) return;
    replaceSession(path, config_.defaultModel, {});
    session_->start();
}

void ClaudeCodeProvider::replaceSession(const QString &program, const QString &model, const QString &systemPrompt) {
    session_ = std::make_unique<ClaudeCodeSession>(program, model, systemPrompt, workingDir_);
    ClaudeCodeSession *session = session_.get();
    connect(session, &ClaudeCodeSession::activity, this, [this] {
        if (stream_) turnTimer_.start(turnTimeoutMs_);
    });
    connect(session, &ClaudeCodeSession::text, this, [this](const QString &delta) {
        if (stream_) stream_->text(delta);
    });
    connect(session, &ClaudeCodeSession::turnFinished, this, &ClaudeCodeProvider::onTurnFinished);
    connect(session, &ClaudeCodeSession::turnFailed, this, [this](const Error &error) {
        turnTimer_.stop();
        ledger_.reset();
        if (ClaudeCodeStream *stream = stream_) {
            stream_ = nullptr;
            stream->error(error);
        }
    });
}

void ClaudeCodeProvider::onTurnFinished(const ClaudeCodeEvent &result) {
    ClaudeCodeStream *stream = stream_;
    if (!stream) return;
    turnTimer_.stop();
    if (!result.ok) {
        ledger_.reset();
        stream_ = nullptr;
        stream->error(ClaudeCodeEvents::resultError(result));
        return;
    }
    ledger_.commit(pending_);
    stream_ = nullptr;
    stream->done();
}

} // namespace nexa
