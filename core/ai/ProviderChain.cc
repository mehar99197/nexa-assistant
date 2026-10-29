#include "core/ai/ProviderChain.h"

#include "core/Logging.h"
#include "core/ai/ChatStream.h"
#include "core/tools/TextToolParser.h"

#include <QPointer>
#include <QRegularExpression>
#include <QStringList>
#include <QTimer>

using namespace Qt::StringLiterals;

namespace nexa {

// The stream of one request through the chain: the current link's stream passed through, and the
// next link tried when that one cannot answer before it said anything.
class ChainStream : public ChatStream {
public:
    ChainStream(ProviderChain *chain, ChatRequest request) : chain_(chain), request_(std::move(request)) {
        // A picture goes to a provider that can see it, when one of them can.
        for (const ChatMessage &message : request_.messages) pictures_ = pictures_ || !message.images.isEmpty();
        pictures_ = pictures_ && chain_->takesImages(request_.model) == std::optional<bool>(true);
    }

    void attempt(size_t from) {
        if (isDone() || !chain_) return;
        size_t index = chain_->firstUsable(from);
        while (pictures_ && index < chain_->links_.size()
               && !chain_->canSee(index, chain_->modelFor(index, request_))) {
            unseeing_.append(chain_->links_[index].provider->displayName() + u" cannot see images"_s);
            index = chain_->firstUsable(index + 1);
        }
        if (index >= chain_->links_.size()) {
            fail(Error{lastCode_, (failures_ + unseeing_).join(u"; "_s)});
            return;
        }
        const ProviderChain::Link &link = chain_->links_[index];
        ChatRequest request = request_;
        request.model = chain_->modelFor(index, request_);
        if (!link.provider->capabilities().nativeTools && !request.tools.isEmpty()) {
            request = TextToolParser::asTextRequest(request);
        }
        if (index > 0) {
            const QString route = request.model.isEmpty() ? link.provider->displayName()
                                                          : link.provider->displayName() + u" · "_s + request.model;
            const QStringList why = failures_ + unseeing_;
            const QString reason = why.isEmpty() ? chain_->lastFailure_[0] : why.join(u"; "_s);
            qCInfo(lcAi) << "answering through" << route << "—" << reason;
            setRoute(route, reason);
        }

        inner_ = link.provider->chat(request);
        ChatStream *inner = inner_.get();
        connect(inner, &ChatStream::textDelta, this, [this](const QString &delta) {
            if (!delta.isEmpty()) output_ = true;
            emitText(delta);
        });
        connect(inner, &ChatStream::toolCallDelta, this, [this](int at, const QString &name, const QString &arguments) {
            output_ = true;
            emitToolCallDelta(at, name, arguments);
        });
        connect(inner, &ChatStream::finished, this, [this, inner, index](FinishReason reason) {
            if (inner != inner_.get()) return;
            if (chain_ && reason != FinishReason::Cancelled) chain_->answered(index);
            complete(reason, inner->toolCalls());
        });
        connect(inner, &ChatStream::failed, this, [this, inner, index](const Error &error) {
            if (inner != inner_.get()) return;
            if (output_ || !chain_ || !ProviderChain::meansUnavailable(error)) {
                fail(error);
                return;
            }
            const QString name = chain_->links_[index].provider->displayName();
            failures_.append(name + u": "_s + error.message);
            lastCode_ = error.code;
            chain_->sitOut(index);
            chain_->lastFailure_[index] = failures_.last();
            attempt(index + 1);
        });
    }

protected:
    void abortTransport() override {
        if (inner_) inner_->cancel();
    }

private:
    QPointer<ProviderChain> chain_;
    ChatRequest request_;
    std::shared_ptr<ChatStream> inner_;
    QStringList failures_;
    QStringList unseeing_;      // links passed over because the request has a picture they cannot see
    ErrorCode lastCode_{ErrorCode::Unknown};
    bool output_{false};
    bool pictures_{false};      // the request has a picture, and some link can see it
};

ProviderChain::ProviderChain(std::vector<Link> links, QObject *parent)
    : IAiProvider(parent), links_(std::move(links)), skipUntil_(links_.size()), lastFailure_(links_.size()) {}

ProviderChain::~ProviderChain() = default;

QString ProviderChain::id() const {
    return links_.empty() ? QString() : links_.front().provider->id();
}

QString ProviderChain::displayName() const {
    return links_.empty() ? QString() : links_.front().provider->displayName();
}

IAiProvider::Capabilities ProviderChain::capabilities() const {
    const size_t index = firstUsable(0);
    if (index < links_.size()) return links_[index].provider->capabilities();
    return links_.empty() ? Capabilities{} : links_.front().provider->capabilities();
}

void ProviderChain::listModels(std::function<void(Result<QStringList>)> done) {
    if (links_.empty()) {
        done(Error{ErrorCode::NotFound, u"No AI provider is configured"_s});
        return;
    }
    links_.front().provider->listModels(std::move(done));
}

std::shared_ptr<ChatStream> ProviderChain::chat(const ChatRequest &request) {
    auto *stream = new ChainStream(this, request);
    std::shared_ptr<ChatStream> handle(stream, [](ChatStream *done) { done->deleteLater(); });
    // From the event loop: the caller connects first, and hears even an immediate reroute.
    QTimer::singleShot(0, stream, [stream] { stream->attempt(0); });
    return handle;
}

std::optional<bool> ProviderChain::takesImages(const QString &model) const {
    ChatRequest asked;
    asked.model = model;
    for (size_t i = 0; i < links_.size(); ++i) {
        if (canSee(i, modelFor(i, asked))) return true;
    }
    return false;
}

QStringList ProviderChain::imageModels() const {
    return links_.empty() ? QStringList() : links_.front().provider->imageModels();
}

QString ProviderChain::modelFor(size_t link, const ChatRequest &request) const {
    return link == 0 && !request.model.isEmpty() ? request.model : links_[link].model;
}

// A provider that does not know about the model counts as seeing when it has vision at all: the
// request goes, and a refusal is handled as for any model.
bool ProviderChain::canSee(size_t link, const QString &model) const {
    const IAiProvider *provider = links_[link].provider.get();
    const std::optional<bool> known = provider->takesImages(model);
    return known ? *known : provider->capabilities().vision;
}

void ProviderChain::warmUp() {
    if (!links_.empty()) links_.front().provider->warmUp();
}

bool ProviderChain::meansUnavailable(const Error &error) {
    switch (error.code) {
    case ErrorCode::ConnectionRefused:
    case ErrorCode::NotFound:
    case ErrorCode::Unauthorized:
    case ErrorCode::Timeout:
    case ErrorCode::Network:
        return true;
    case ErrorCode::Http: {
        // "HTTP 429: …" from a reply, "[503]: …" inside a gateway's stream.
        static const QRegularExpression status(u"^\\[?(?:HTTP )?(\\d{3})\\]?"_s);
        const QRegularExpressionMatch match = status.match(error.message);
        if (!match.hasMatch()) return false;
        const int code = match.captured(1).toInt();
        return code == 402 || code == 429 || code >= 500;     // out of credits, rate limited, down
    }
    default:
        return false;
    }
}

size_t ProviderChain::firstUsable(size_t from) const {
    for (size_t i = from; i < links_.size(); ++i) {
        if (skipUntil_[i].hasExpired()) return i;
    }
    // Every link sitting out: try them all again rather than refuse without asking anyone.
    return from == 0 && !links_.empty() ? 0 : links_.size();
}

void ProviderChain::sitOut(size_t link) {
    skipUntil_[link] = QDeadlineTimer(skipWindowMs_);
}

void ProviderChain::answered(size_t link) {
    skipUntil_[link] = QDeadlineTimer();
}

} // namespace nexa
