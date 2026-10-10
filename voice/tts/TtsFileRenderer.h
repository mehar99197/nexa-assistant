#pragma once

#include "core/Result.h"
#include "voice/tts/ISpeechSynthesizer.h"

#include <QByteArray>
#include <QObject>
#include <QString>

#include <functional>
#include <memory>
#include <vector>

namespace nexa {

// Renders a reply as the bytes of a WAV file instead of playing it — for the web remote, which plays
// it on the phone while the PC stays quiet. Chain of Responsibility over the same synthesizers the
// PC speaks with: the provider's voice first, Piper when that fails.
class TtsFileRenderer : public QObject {
    Q_OBJECT
public:
    using Done = std::function<void(Result<QByteArray>)>;
    // About a minute of speech. A longer reply is read up to a sentence end; the rest is on screen.
    static constexpr qsizetype kMaxCharacters = 1200;

    explicit TtsFileRenderer(std::vector<std::unique_ptr<ISpeechSynthesizer>> chain, QObject *parent = nullptr);
    ~TtsFileRenderer() override;

    bool isEmpty() const { return chain_.empty(); }
    void render(const QString &text, Done done);
    void cancel();

    // Pure: what of a Markdown reply is worth reading out — code left out, marks gone — as one
    // paragraph, cut at the last sentence end that fits.
    static QString speakable(const QString &markdown, qsizetype maxCharacters = kMaxCharacters);

private:
    void tryLink(size_t link, const QString &text, Done done, QStringList problems);

    std::vector<std::unique_ptr<ISpeechSynthesizer>> chain_;
};

} // namespace nexa
