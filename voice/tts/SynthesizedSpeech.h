#pragma once

#include "voice/tts/ISpeechSynthesizer.h"
#include "voice/tts/ITextToSpeech.h"
#include "voice/tts/PcmPlayer.h"

#include <QDeadlineTimer>

#include <deque>
#include <memory>
#include <optional>
#include <vector>

namespace nexa {

// Speaks sentences in order through a chain of synthesizers (Chain of Responsibility: the cloud
// voice first, Piper when the cloud fails) and plays the clips back to back. The next sentences
// are synthesised while the current one plays, so there are no gaps; a synthesizer that failed is
// skipped for a minute, so a stopped server does not delay every sentence.
class SynthesizedSpeech : public ITextToSpeech {
    Q_OBJECT
public:
    explicit SynthesizedSpeech(std::vector<std::unique_ptr<ISpeechSynthesizer>> chain, QObject *parent = nullptr);
    ~SynthesizedSpeech() override;

    QString name() const override;
    void speak(const QString &text) override;
    void stop() override;
    bool isSpeaking() const override { return speaking_; }
    void warmUp() override;

    void setVolume(double volume) { player_.setVolume(volume); }

private:
    struct Sentence {
        quint64 id{0};
        QString text;
        std::optional<Result<PcmAudio>> clip;
        bool requested{false};
    };

    void requestAhead();
    void request(quint64 id, size_t link);
    void deliver(quint64 id, size_t link, Result<PcmAudio> clip);
    void playReady();
    void checkDone();
    size_t firstUsableLink(size_t from) const;

    std::vector<std::unique_ptr<ISpeechSynthesizer>> chain_;
    std::vector<QDeadlineTimer> skipUntil_;
    std::deque<Sentence> sentences_;    // not yet handed to the player
    PcmPlayer player_;
    quint64 nextId_{0};
    quint64 generation_{0};
    bool speaking_{false};
};

} // namespace nexa
