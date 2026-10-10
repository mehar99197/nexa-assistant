#pragma once

#include "voice/tts/ISpeechSynthesizer.h"

#include <QProcess>
#include <QTemporaryDir>

#include <deque>

namespace nexa {

// Local neural voice with the Piper binary (rhasspy/piper) (Strategy). One long-running process
// per voice reads a sentence per line and writes a WAV file per sentence into a private temporary
// directory, printing its path; each file is read, deleted and handed back as PCM. The process
// starts at the first request and keeps the voice loaded.
class PiperSynthesizer : public ISpeechSynthesizer {
    Q_OBJECT
public:
    PiperSynthesizer(QString binary, QString model, double rate, QObject *parent = nullptr);
    ~PiperSynthesizer() override;

    QString name() const override;
    void synthesize(const QString &text, Done done) override;
    void cancel() override;
    void warmUp() override { ensureRunning(); }

private:
    bool ensureRunning();
    void readOutput();
    void failAll(const Error &error);

    QString binary_;
    QString model_;
    double rate_;
    QProcess process_;
    QTemporaryDir outputDir_;
    QByteArray stdout_;
    std::deque<Done> pending_;      // sentences sent, in order, waiting for their file
    int discard_{0};                // files still to come for cancelled sentences
};

} // namespace nexa
