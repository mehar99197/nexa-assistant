#include "voice/tts/TtsFileRenderer.h"

#include "voice/Wav.h"
#include "voice/tts/SentenceChunker.h"

using namespace Qt::StringLiterals;

namespace nexa {

TtsFileRenderer::TtsFileRenderer(std::vector<std::unique_ptr<ISpeechSynthesizer>> chain, QObject *parent)
    : QObject(parent), chain_(std::move(chain)) {}

TtsFileRenderer::~TtsFileRenderer() {
    cancel();
}

QString TtsFileRenderer::speakable(const QString &markdown, qsizetype maxCharacters) {
    SentenceChunker chunker;
    QStringList sentences = chunker.feed(markdown);
    sentences += chunker.finish();
    QString text;
    for (const QString &sentence : std::as_const(sentences)) {
        const QString words = sentence.simplified();
        if (words.isEmpty()) continue;
        if (!text.isEmpty() && text.size() + 1 + words.size() > maxCharacters) break;
        if (!text.isEmpty()) text += u' ';
        text += words;
    }
    // One sentence longer than everything allowed: cut it at a word.
    if (text.size() > maxCharacters) {
        text.truncate(maxCharacters);
        if (const qsizetype space = text.lastIndexOf(u' '); space > maxCharacters / 2) text.truncate(space);
        text += u'…';
    }
    return text;
}

void TtsFileRenderer::render(const QString &text, Done done) {
    const QString words = speakable(text);
    if (words.isEmpty()) {
        done(Error{ErrorCode::InvalidArgument, u"There is nothing in the reply to read out."_s});
        return;
    }
    if (chain_.empty()) {
        done(Error{ErrorCode::NotSupported, u"No voice is set up (Settings → Voice)."_s});
        return;
    }
    tryLink(0, words, std::move(done), {});
}

void TtsFileRenderer::tryLink(size_t link, const QString &text, Done done, QStringList problems) {
    if (link >= chain_.size()) {
        done(Error{ErrorCode::Unknown, problems.join(u"; "_s)});
        return;
    }
    ISpeechSynthesizer *synthesizer = chain_[link].get();
    synthesizer->synthesize(text, [this, link, text, done, problems](Result<PcmAudio> clip) mutable {
        if (clip) {
            done(wav::encode(clip.value()));
            return;
        }
        if (clip.error().code == ErrorCode::Cancelled) {
            done(clip.error());
            return;
        }
        problems.append(u"%1: %2"_s.arg(chain_[link]->name(), clip.error().message));
        tryLink(link + 1, text, done, problems);
    });
}

void TtsFileRenderer::cancel() {
    for (const std::unique_ptr<ISpeechSynthesizer> &synthesizer : chain_) synthesizer->cancel();
}

} // namespace nexa
