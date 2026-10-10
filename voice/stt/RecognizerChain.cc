#include "voice/stt/RecognizerChain.h"

#include "core/Logging.h"

#include <QStringList>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

constexpr int kSkipFailedMs = 60'000;

} // namespace

RecognizerChain::RecognizerChain(std::vector<std::unique_ptr<ISpeechRecognizer>> links, QObject *parent)
    : ISpeechRecognizer(parent), links_(std::move(links)), skipUntil_(links_.size()) {}

RecognizerChain::~RecognizerChain() {
    cancel();
}

QString RecognizerChain::name() const {
    QStringList names;
    for (const auto &link : links_) names.append(link->name());
    return names.join(u" → "_s);
}

bool RecognizerChain::isLocal() const {
    return std::all_of(links_.begin(), links_.end(), [](const auto &link) { return link->isLocal(); });
}

void RecognizerChain::warmUp() {
    for (const auto &link : links_) link->warmUp();
}

void RecognizerChain::transcribe(const QByteArray &pcm16k, const QString &prompt, Done done) {
    cancel();
    if (links_.empty()) {
        done(Error{ErrorCode::NotSupported, u"No speech recognition is set up."_s});
        return;
    }
    done_ = std::move(done);
    attempt(firstUsable(0), pcm16k, prompt, ++generation_);
}

void RecognizerChain::cancel() {
    ++generation_;
    for (const auto &link : links_) link->cancel();
    if (Done done = std::exchange(done_, nullptr)) done(Error{ErrorCode::Cancelled, u"Cancelled"_s});
}

void RecognizerChain::attempt(size_t link, const QByteArray &pcm16k, const QString &prompt, quint64 generation) {
    links_[link]->transcribe(pcm16k, prompt, [this, link, pcm16k, prompt, generation](Result<QString> text) {
        if (generation != generation_ || !done_) return;
        if (!text && text.error().code != ErrorCode::Cancelled && link + 1 < links_.size()) {
            const size_t next = firstUsable(link + 1);
            qCInfo(lcVoice) << links_[link]->name() << "failed (" << text.error().message << ") — trying"
                            << links_[next]->name();
            skipUntil_[link] = QDeadlineTimer(kSkipFailedMs);
            attempt(next, pcm16k, prompt, generation);
            return;
        }
        if (text) lastUsed_ = links_[link]->name();
        std::exchange(done_, nullptr)(std::move(text));
    });
}

size_t RecognizerChain::firstUsable(size_t from) const {
    for (size_t link = from; link < links_.size(); ++link) {
        if (skipUntil_[link].hasExpired()) return link;
    }
    return std::min(from, links_.size() - 1);
}

} // namespace nexa
