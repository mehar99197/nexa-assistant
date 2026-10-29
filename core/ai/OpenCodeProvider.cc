#include "core/ai/OpenCodeProvider.h"

#include "core/Logging.h"
#include "core/ai/ChatStream.h"
#include "core/ai/OpenCodeServer.h"
#include "core/ai/TranscriptWriter.h"
#include "core/net/HttpClient.h"

#include <QJsonDocument>
#include <QRegularExpression>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

constexpr int kRequestTimeoutMs = 30'000;
constexpr int kRetryGiveUpMs = 30'000;
constexpr qsizetype kHeldChars = 80;        // enough for "Gemini 3 Pro is no longer available."

QByteArray json(const QJsonObject &object) {
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

// opencode retries a provider's refusal by itself, silently and for as long as it takes; a quota or
// a free tier used up will not come back during this request.
bool outOfQuota(const OpenCodeEvent &event) {
    const QString why = (event.reason + u' ' + event.message).toLower();
    return why.contains(u"limit"_s) || why.contains(u"quota"_s) || why.contains(u"credit"_s);
}

} // namespace

// The stream of one opencode turn. The provider drives it, since it holds the session.
class OpenCodeStream : public ChatStream {
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

OpenCodeProvider::OpenCodeProvider(ProviderConfig config, HttpClient *http, std::shared_ptr<OpenCodeServer> server,
                                   QObject *parent)
    : IAiProvider(parent), config_(std::move(config)), http_(http), server_(std::move(server)) {
    retryLimit_.setSingleShot(true);
    connect(&retryLimit_, &QTimer::timeout, this, [this] {
        abortSession();
        finishTurn(Error{ErrorCode::Http, u"HTTP 429: opencode: still retrying after %1 s"_s.arg(kRetryGiveUpMs / 1000)});
    });
    turnTimer_.setSingleShot(true);
    connect(&turnTimer_, &QTimer::timeout, this, [this] {
        abortSession();
        finishTurn(Error{ErrorCode::Timeout, u"opencode stopped answering: nothing for %1 s"_s.arg(
                                                 std::max(1, turnTimeoutMs_ / 1000))});
    });
    connect(server_.get(), &OpenCodeServer::event, this, &OpenCodeProvider::onEvent);
    connect(server_.get(), &OpenCodeServer::stopped, this, [this](const Error &error) {
        sessionId_.clear();
        ledger_.reset();
        finishTurn(error);
    });
}

OpenCodeProvider::~OpenCodeProvider() {
    if (stream_) abortSession();
}

IAiProvider::Capabilities OpenCodeProvider::capabilities() const {
    // Vision depends on the model; a text-only one refuses the image, and the Agent then resends.
    return Capabilities{.streaming = true, .nativeTools = false, .vision = true};
}

void OpenCodeProvider::listModels(std::function<void(Result<QStringList>)> done) {
    server_->ensureStarted([self = QPointer<OpenCodeProvider>(this), done](Result<void> started) {
        if (!self) return;
        if (!started) {
            done(started.error());
            return;
        }
        self->http_->get(self->server_->request(u"/config"_s), kRequestTimeoutMs, self,
                         [self, done](Result<QByteArray> config) {
            const QString preferred = config ? OpenCodeWire::parseDefaultModel(config.value()) : QString();
            self->http_->get(self->server_->request(u"/config/providers"_s), kRequestTimeoutMs, self,
                             [self, done, preferred](Result<QByteArray> body) {
                if (!body) {
                    done(body.error());
                    return;
                }
                self->images_ = OpenCodeWire::parseImageSupport(body.value());
                const QStringList models = OpenCodeWire::parseModels(body.value(), preferred);
                if (models.isEmpty()) {
                    done(Error{ErrorCode::Parse, u"opencode listed no models — sign in to a provider with "
                                                 "`opencode auth login`"_s});
                } else {
                    done(models);
                }
            });
        });
    });
}

std::shared_ptr<ChatStream> OpenCodeProvider::chat(const ChatRequest &request) {
    auto *stream = new OpenCodeStream;
    std::shared_ptr<ChatStream> handle(stream, [](ChatStream *done) { done->deleteLater(); });
    if (stream_) {
        QTimer::singleShot(0, stream, [stream] {
            stream->error(Error{ErrorCode::Unknown, u"opencode is still answering the previous request"_s});
        });
        return handle;
    }
    pending_ = request;
    if (pending_.model.isEmpty()) pending_.model = config_.defaultModel;
    // A new turn owns no busy and no parts yet: a late idle of the last turn must not end it.
    sawBusy_ = false;
    partTypes_.clear();
    held_.clear();
    holding_ = true;
    stream_ = stream;
    stream->onAbort = [self = QPointer<OpenCodeProvider>(this)] {
        if (!self) return;
        self->retryLimit_.stop();
        self->turnTimer_.stop();
        self->stream_ = nullptr;
        self->ledger_.reset();
        self->abortSession();
    };
    // Calls back from the event loop even when the server is ready, after the caller has connected.
    server_->ensureStarted([self = QPointer<OpenCodeProvider>(this), stream = QPointer<OpenCodeStream>(stream)](
                               Result<void> started) {
        if (!self || !stream || self->stream_ != stream) return;
        if (!started) {
            self->finishTurn(started.error());
            return;
        }
        self->sendTurn();
    });
    return handle;
}

std::optional<bool> OpenCodeProvider::takesImages(const QString &model) const {
    const auto known = images_.constFind(model.isEmpty() ? config_.defaultModel : model);
    if (known == images_.constEnd()) return std::nullopt;
    return *known;
}

QStringList OpenCodeProvider::imageModels() const {
    QStringList seeing;
    for (auto it = images_.constBegin(); it != images_.constEnd(); ++it) {
        if (it.value()) seeing.append(it.key());
    }
    std::sort(seeing.begin(), seeing.end());
    return seeing;
}

void OpenCodeProvider::warmUp() {
    server_->ensureStarted([](Result<void> started) {
        if (!started) qCInfo(lcAi) << "opencode could not start:" << started.error().message;
    });
}

void OpenCodeProvider::sendTurn() {
    if (sessionId_.isEmpty()) ledger_.reset();
    const TurnPlan plan = ledger_.plan(pending_);
    QStringList parts;
    if (plan.fresh) {
        if (const QString earlier = TranscriptWriter::history(plan.history); !earlier.isEmpty()) parts.append(earlier);
    }
    parts.append(TranscriptWriter::turnText(plan.turns));
    QList<ImageAttachment> images;
    for (const ChatMessage &turn : plan.turns) images += turn.images;
    const QJsonObject body = OpenCodeWire::promptBody(pending_.model, plan.system, parts.join(u"\n\n"_s), images);
    qCDebug(lcAi) << "opencode" << (plan.fresh ? "fresh" : "continued") << "turn," << pending_.model;
    if (!plan.fresh) {
        prompt(sessionId_, body);
        return;
    }

    ledger_.reset();
    if (!sessionId_.isEmpty()) {
        // The conversation it held is over; it would only crowd opencode's own session list.
        http_->deleteResource(server_->request(u"/session/%1"_s.arg(sessionId_)), kRequestTimeoutMs, this,
                              [](Result<QByteArray>) {});
        sessionId_.clear();
    }
    http_->post(server_->request(u"/session"_s), json(QJsonObject{{u"title"_s, u"Nexa"_s}}), kRequestTimeoutMs, this,
                [this, body, turn = stream_](Result<QByteArray> created) {
        if (!stream_ || stream_ != turn) return;     // that turn was cancelled meanwhile
        const QString session = created ? OpenCodeWire::parseSessionId(created.value()) : QString();
        if (session.isEmpty()) {
            finishTurn(created ? Error{ErrorCode::Parse, u"opencode did not open a session"_s} : created.error());
            return;
        }
        prompt(session, body);
    });
}

void OpenCodeProvider::prompt(const QString &session, const QJsonObject &body) {
    sessionId_ = session;
    sawBusy_ = false;
    partTypes_.clear();
    turnTimer_.start(turnTimeoutMs_);
    http_->post(server_->request(u"/session/%1/prompt_async"_s.arg(session)), json(body), kRequestTimeoutMs, this,
                [this](Result<QByteArray> sent) {
        if (!sent) finishTurn(sent.error());
    });
}

void OpenCodeProvider::onEvent(const OpenCodeEvent &event) {
    if (!stream_ || event.sessionId != sessionId_) return;
    if (turnTimer_.isActive()) turnTimer_.start(turnTimeoutMs_);   // a sign of life, reasoning included
    switch (event.kind) {
    case OpenCodeEvent::Kind::Busy:
        sawBusy_ = true;
        break;
    case OpenCodeEvent::Kind::TextDelta: {
        sawBusy_ = true;
        retryLimit_.stop();
        // A thinking model's reasoning streams as text of a "reasoning" part: it is not the reply.
        const QString type = partTypes_.value(event.partId);
        if (type.isEmpty() || type == u"text") showText(event.text);
        break;
    }
    case OpenCodeEvent::Kind::PartType:
        partTypes_.insert(event.partId, event.partType);
        break;
    case OpenCodeEvent::Kind::Idle:
        if (!sawBusy_) break;       // the end of an earlier turn
        if (failIfRetired()) break;
        if (!held_.isEmpty()) stream_->text(std::exchange(held_, {}));
        ledger_.commit(pending_);
        finishTurn(std::nullopt);
        break;
    case OpenCodeEvent::Kind::Retry:
        if (outOfQuota(event)) {
            abortSession();
            finishTurn(Error{ErrorCode::Http, u"HTTP 429: opencode: %1 is out of its quota (%2) — choose another model "
                                              "in the toolbar"_s.arg(modelName(), event.message)});
        } else if (!retryLimit_.isActive()) {
            retryLimit_.start(kRetryGiveUpMs);
        }
        break;
    case OpenCodeEvent::Kind::Error:
        if (event.authError) {
            finishTurn(Error{ErrorCode::Unauthorized, u"opencode is not signed in for this model — run "
                                                      "`opencode auth login` in a terminal"_s});
        } else if (event.status == 401 || event.status == 403) {
            // No licence, no plan, no access: this provider cannot answer with this model.
            finishTurn(Error{ErrorCode::Unauthorized, u"opencode: %1 refused (HTTP %2): %3 — choose another model in the "
                                                      "toolbar"_s.arg(modelName()).arg(event.status).arg(event.message)});
        } else if (event.status > 0) {
            finishTurn(Error{ErrorCode::Http, u"HTTP %1: opencode: %2: %3 — choose another model in the toolbar"_s.arg(
                                                  event.status).arg(modelName(), event.message)});
        } else {
            finishTurn(Error{ErrorCode::Http, u"opencode: "_s + event.message});
        }
        break;
    case OpenCodeEvent::Kind::Other:
        break;
    }
}

void OpenCodeProvider::showText(const QString &text) {
    if (!holding_) {
        stream_->text(text);
        return;
    }
    held_ += text;
    if (failIfRetired() || held_.size() < kHeldChars) return;
    holding_ = false;
    stream_->text(std::exchange(held_, {}));
}

bool OpenCodeProvider::failIfRetired() {
    static const QRegularExpression notice(u"is no longer available"_s, QRegularExpression::CaseInsensitiveOption);
    if (!holding_ || !notice.match(held_).hasMatch()) return false;
    const QString said = held_.section(u'.', 0, 0).trimmed();
    held_.clear();
    abortSession();
    finishTurn(Error{ErrorCode::NotFound, u"opencode: %1 is no longer available (\"%2\") — choose another model in "
                                          "the toolbar"_s.arg(modelName(), said)});
    return true;
}

QString OpenCodeProvider::modelName() const {
    return pending_.model.isEmpty() ? u"opencode's default model"_s : pending_.model;
}

void OpenCodeProvider::finishTurn(std::optional<Error> error) {
    retryLimit_.stop();
    turnTimer_.stop();
    OpenCodeStream *stream = stream_;
    stream_ = nullptr;
    if (!stream) return;
    if (error) {
        ledger_.reset();
        stream->error(*error);
    } else {
        stream->done();
    }
}

void OpenCodeProvider::abortSession() {
    if (sessionId_.isEmpty()) return;
    http_->post(server_->request(u"/session/%1/abort"_s.arg(sessionId_)), "{}", kRequestTimeoutMs, this,
                [](Result<QByteArray>) {});
}

} // namespace nexa
