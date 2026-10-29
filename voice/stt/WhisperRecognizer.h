#pragma once

#include "voice/stt/ISpeechRecognizer.h"

#include <QThread>
#include <QTimer>

#include <atomic>
#include <memory>

namespace nexa {

class WhisperWorker;

// Local speech recognition with whisper.cpp (Strategy); audio never leaves the machine. The model
// is loaded on a worker thread at the first request (or warmUp()) and freed after ten idle
// minutes; inference never runs on the GUI thread. cancel() stops a running transcription through
// whisper's abort callback. Built only with NEXA_BUILD_WHISPER.
class WhisperRecognizer : public ISpeechRecognizer {
    Q_OBJECT
public:
    WhisperRecognizer(QString modelPath, QString language, QObject *parent = nullptr);
    ~WhisperRecognizer() override;

    QString name() const override;
    bool isLocal() const override { return true; }
    void transcribe(const QByteArray &pcm16k, const QString &prompt, Done done) override;
    void transcribeWords(const QByteArray &pcm16k, const QString &prompt, DoneWords done) override;
    void cancel() override;
    void warmUp() override;

private:
    void run(const QByteArray &pcm16k, const QString &prompt, bool words, DoneWords done);

    QString modelPath_;
    QThread thread_;
    WhisperWorker *worker_{nullptr};            // lives in thread_
    std::shared_ptr<std::atomic_bool> abort_;   // of the running job
    std::shared_ptr<DoneWords> pending_;
    QTimer unloadTimer_;
};

} // namespace nexa
