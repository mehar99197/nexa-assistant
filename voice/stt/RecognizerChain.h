#pragma once

#include "voice/stt/ISpeechRecognizer.h"

#include <QDeadlineTimer>

#include <memory>
#include <vector>

namespace nexa {

// Tries recognisers in order until one succeeds (Chain of Responsibility): the cloud first,
// local whisper when the cloud fails or is unreachable. A recogniser that failed is skipped for a
// minute, so a stopped server does not delay every request.
class RecognizerChain : public ISpeechRecognizer {
    Q_OBJECT
public:
    explicit RecognizerChain(std::vector<std::unique_ptr<ISpeechRecognizer>> links, QObject *parent = nullptr);
    ~RecognizerChain() override;

    QString name() const override;
    bool isLocal() const override;
    void transcribe(const QByteArray &pcm16k, const QString &prompt, Done done) override;
    void cancel() override;
    void warmUp() override;

    // The recogniser that produced the last transcript (for status messages).
    QString lastUsed() const { return lastUsed_; }

private:
    void attempt(size_t link, const QByteArray &pcm16k, const QString &prompt, quint64 generation);
    size_t firstUsable(size_t from) const;

    std::vector<std::unique_ptr<ISpeechRecognizer>> links_;
    std::vector<QDeadlineTimer> skipUntil_;
    Done done_;
    quint64 generation_{0};
    QString lastUsed_;
};

} // namespace nexa
