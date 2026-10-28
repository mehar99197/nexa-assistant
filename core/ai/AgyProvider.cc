#include "core/ai/AgyProvider.h"

#include "core/Logging.h"
#include "core/ai/AgySession.h"
#include "core/ai/ChatStream.h"
#include "core/ai/ProgramLookup.h"
#include "core/ai/TranscriptWriter.h"

#include <QProcess>
#include <QStandardPaths>
#include <QTimer>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

constexpr int kModelsTimeoutMs = 30'000;

// Sent once when agy's model reached for one of agy's own tools instead of Nexa's.
const QString kReminder = u"Use only Nexa's tools, written as <tool_call> blocks as described in Nexa's instructions. "
                          "Your own tools are switched off."_s;

} // namespace

// The stream of one agy turn. The provider drives it, since it holds the session.
class AgyStream : public ChatStream {
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

AgyProvider::AgyProvider(ProviderConfig config, QString workingDir, QObject *parent)
    : IAiProvider(parent), config_(std::move(config)), workingDir_(std::move(workingDir)) {
    if (workingDir_.isEmpty()) {
        workingDir_ = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + u"/nexa/agy"_s;
    }
    turnTimer_.setSingleShot(true);
    connect(&turnTimer_, &QTimer::timeout, this, [this] {
        AgyStream *stream = stream_;
        if (!stream) return;
        stream_ = nullptr;
        ledger_.reset();
        session_.reset();       // a turn cannot be stopped any other way
        stream->error(Error{ErrorCode::Timeout, u"agy stopped answering: nothing for %1 s"_s.arg(
                                                    std::max(1, turnTimeoutMs_ / 1000))});
    });
}

AgyProvider::~AgyProvider() = default;

IAiProvider::Capabilities AgyProvider::capabilities() const {
    return Capabilities{.streaming = true, .nativeTools = false, .vision = false};
}

QString AgyProvider::program() const {
    return findProgram(u"agy"_s, config_.baseUrl.trimmed());
}

void AgyProvider::listModels(std::function<void(Result<QStringList>)> done) {
    const QString path = program();
    if (path.isEmpty() || !models_.isEmpty()) {
        const Result<QStringList> answer = path.isEmpty()
            ? Result<QStringList>(Error{ErrorCode::NotFound, u"agy is not installed (looked on PATH and in ~/.local/bin)"_s})
            : Result<QStringList>(models_);
        QTimer::singleShot(0, this, [done = std::move(done), answer] { done(answer); });
        return;
    }
    auto *process = new QProcess(this);
    process->setProcessEnvironment(AgySession::prepare(workingDir_));
    process->setProgram(path);
    process->setArguments({u"models"_s});
    process->setWorkingDirectory(AgySession::workspace(workingDir_));
    process->setStandardInputFile(QProcess::nullDevice());
    auto answered = std::make_shared<bool>(false);
    auto finish = [this, process, answered, done](Result<QStringList> result) {
        if (std::exchange(*answered, true)) return;
        if (result) models_ = result.value();
        process->deleteLater();
        done(std::move(result));
    };
    connect(process, &QProcess::finished, this, [process, finish](int exitCode, QProcess::ExitStatus) {
        if (exitCode != 0) {
            finish(AgyEvents::exitError(exitCode, QString::fromUtf8(process->readAllStandardError())));
            return;
        }
        const QStringList models = AgyEvents::parseModels(QString::fromUtf8(process->readAllStandardOutput()));
        if (models.isEmpty()) {
            finish(Error{ErrorCode::Parse, u"agy listed no models"_s});
        } else {
            finish(models);
        }
    });
    connect(process, &QProcess::errorOccurred, this, [process, finish](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            finish(Error{ErrorCode::NotFound, u"Cannot start agy: %1"_s.arg(process->errorString())});
        }
    });
    QTimer::singleShot(kModelsTimeoutMs, process, [process, finish] {
        process->kill();
        finish(Error{ErrorCode::Timeout, u"agy did not list its models within 30 s"_s});
    });
    process->start();
}

std::shared_ptr<ChatStream> AgyProvider::chat(const ChatRequest &request) {
    auto *stream = new AgyStream;
    std::shared_ptr<ChatStream> handle(stream, [](ChatStream *done) { done->deleteLater(); });
    // Failures go out from the event loop, once the caller has connected to the stream.
    const auto failSoon = [stream](const Error &error) {
        QTimer::singleShot(0, stream, [stream, error] { stream->error(error); });
    };
    const QString path = program();
    if (path.isEmpty()) {
        failSoon(Error{ErrorCode::NotFound, u"agy is not installed (looked on PATH and in ~/.local/bin)"_s});
        return handle;
    }
    if (session_ && session_->isBusy()) {
        failSoon(Error{ErrorCode::Unknown, u"agy is still answering the previous request"_s});
        return handle;
    }

    pending_ = request;
    if (pending_.model.isEmpty()) pending_.model = config_.defaultModel;
    if (!session_ || !session_->isUsable()) ledger_.reset();
    const TurnPlan plan = ledger_.plan(pending_);
    QStringList parts;
    if (plan.fresh) {
        ledger_.reset();
        const bool reusable = session_ && session_->isUsable() && session_->turnsTaken() == 0
            && session_->model() == pending_.model;
        if (!reusable) replaceSession(path, pending_.model);
        if (!plan.system.isEmpty()) parts.append(TranscriptWriter::instructions(plan.system));
        if (const QString earlier = TranscriptWriter::history(plan.history); !earlier.isEmpty()) parts.append(earlier);
    } else if (plan.instructionsChanged) {
        parts.append(TranscriptWriter::instructionsUpdate(plan.system));
    }
    parts.append(TranscriptWriter::turnText(plan.turns));
    qCDebug(lcAi) << "agy" << (plan.fresh ? "fresh" : "continued") << "turn," << pending_.model;

    reminded_ = false;
    stream_ = stream;
    turnTimer_.start(turnTimeoutMs_);
    stream->onAbort = [self = QPointer<AgyProvider>(this)] {
        if (!self) return;
        self->turnTimer_.stop();
        self->stream_ = nullptr;
        self->ledger_.reset();
        self->session_.reset();     // kills agy: a turn cannot be stopped any other way
    };
    QTimer::singleShot(0, stream, [session = QPointer<AgySession>(session_.get()), text = parts.join(u"\n\n"_s)] {
        if (session) session->send(text);
    });
    return handle;
}

void AgyProvider::warmUp() {
    if (session_) return;
    const QString path = program();
    if (path.isEmpty()) return;
    replaceSession(path, config_.defaultModel);
    session_->start();
}

void AgyProvider::replaceSession(const QString &program, const QString &model) {
    session_ = std::make_unique<AgySession>(program, model, workingDir_);
    AgySession *session = session_.get();
    connect(session, &AgySession::activity, this, [this] {
        if (stream_) turnTimer_.start(turnTimeoutMs_);
    });
    connect(session, &AgySession::text, this, [this](const QString &delta) {
        if (stream_) stream_->text(delta);
    });
    connect(session, &AgySession::turnFinished, this, &AgyProvider::onTurnFinished);
    connect(session, &AgySession::turnFailed, this, [this](const Error &error) {
        turnTimer_.stop();
        ledger_.reset();
        if (AgyStream *stream = stream_) {
            stream_ = nullptr;
            stream->error(error);
        }
    });
}

void AgyProvider::onTurnFinished(const AgyEvent &result) {
    AgyStream *stream = stream_;
    if (!stream) return;
    turnTimer_.stop();
    if (!result.ok) {
        ledger_.reset();
        stream_ = nullptr;
        stream->error(AgyEvents::resultError(result.error));
        return;
    }
    if (result.response.trimmed().isEmpty() && !result.deniedActions.isEmpty()) {
        if (!std::exchange(reminded_, true)) {
            turnTimer_.start(turnTimeoutMs_);
            session_->send(kReminder);
            return;
        }
        ledger_.reset();
        stream_ = nullptr;
        stream->error(Error{ErrorCode::NotSupported, u"agy tried to use its own %1 tool, which Nexa does not allow"_s.arg(
                                                         result.deniedActions.first())});
        return;
    }
    ledger_.commit(pending_);
    stream_ = nullptr;
    stream->done();
}

} // namespace nexa
