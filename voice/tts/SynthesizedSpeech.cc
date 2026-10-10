#include "voice/tts/SynthesizedSpeech.h"

#include "core/Logging.h"

#include <QStringList>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

constexpr int kLookahead = 2;               // sentences synthesised ahead of playback
constexpr int kSkipFailedMs = 60'000;

} // namespace

SynthesizedSpeech::SynthesizedSpeech(std::vector<std::unique_ptr<ISpeechSynthesizer>> chain, QObject *parent)
    : ITextToSpeech(parent), chain_(std::move(chain)), skipUntil_(chain_.size()) {
    connect(&player_, &PcmPlayer::started, this, [this] {
        if (speaking_) emit started();
    });
    connect(&player_, &PcmPlayer::finished, this, &SynthesizedSpeech::checkDone);
    connect(&player_, &PcmPlayer::failed, this, [this](const Error &error) {
        stop();
        emit failed(error);
    });
}

SynthesizedSpeech::~SynthesizedSpeech() {
    ++generation_;
    for (const auto &link : chain_) link->cancel();
}

QString SynthesizedSpeech::name() const {
    QStringList names;
    for (const auto &link : chain_) names.append(link->name());
    return names.join(u" → "_s);
}

void SynthesizedSpeech::warmUp() {
    if (!chain_.empty()) chain_[firstUsableLink(0)]->warmUp();     // only the voice that will speak
}

void SynthesizedSpeech::speak(const QString &text) {
    if (chain_.empty() || text.trimmed().isEmpty()) return;
    sentences_.push_back(Sentence{.id = ++nextId_, .text = text});
    speaking_ = true;
    requestAhead();
}

void SynthesizedSpeech::stop() {
    ++generation_;
    sentences_.clear();
    speaking_ = false;
    for (const auto &link : chain_) link->cancel();
    player_.stop();
}

void SynthesizedSpeech::requestAhead() {
    // Mark first, request after: a synthesizer may answer synchronously and re-enter here.
    std::vector<quint64> due;
    int ahead = 0;
    for (Sentence &sentence : sentences_) {
        if (ahead++ >= kLookahead) break;
        if (sentence.requested) continue;
        sentence.requested = true;
        due.push_back(sentence.id);
    }
    for (const quint64 id : due) request(id, firstUsableLink(0));
}

void SynthesizedSpeech::request(quint64 id, size_t link) {
    const auto found = std::find_if(sentences_.begin(), sentences_.end(),
                                    [id](const Sentence &sentence) { return sentence.id == id; });
    if (found == sentences_.end()) return;
    const quint64 generation = generation_;
    const QString text = found->text;  // the sentence may be gone when a synthesizer answers at once
    chain_[link]->synthesize(text, [this, id, link, generation](Result<PcmAudio> clip) {
        if (generation == generation_) deliver(id, link, std::move(clip));
    });
}

void SynthesizedSpeech::deliver(quint64 id, size_t link, Result<PcmAudio> clip) {
    const auto found = std::find_if(sentences_.begin(), sentences_.end(),
                                    [id](const Sentence &sentence) { return sentence.id == id; });
    if (found == sentences_.end()) return;

    if (!clip && clip.error().code != ErrorCode::Cancelled) {
        const size_t next = link + 1 < chain_.size() ? firstUsableLink(link + 1) : chain_.size();
        if (next < chain_.size()) {
            qCInfo(lcVoice) << chain_[link]->name() << "failed (" << clip.error().message << ") — using"
                            << chain_[next]->name();
            skipUntil_[link] = QDeadlineTimer(kSkipFailedMs);
            request(id, next);
            return;
        }
        emit failed(clip.error());
    }
    found->clip = std::move(clip);
    playReady();
}

void SynthesizedSpeech::playReady() {
    while (!sentences_.empty() && sentences_.front().clip) {
        Result<PcmAudio> clip = std::move(*sentences_.front().clip);
        sentences_.pop_front();
        if (clip) player_.enqueue(std::move(clip).value());
    }
    requestAhead();
    checkDone();
}

void SynthesizedSpeech::checkDone() {
    if (!speaking_ || !sentences_.empty() || player_.isPlaying()) return;
    speaking_ = false;
    emit finished();
}

size_t SynthesizedSpeech::firstUsableLink(size_t from) const {
    for (size_t link = from; link < chain_.size(); ++link) {
        if (skipUntil_[link].hasExpired()) return link;
    }
    // Every remaining link failed recently: try the first of them again anyway.
    return std::min(from, chain_.size() - 1);
}

} // namespace nexa
