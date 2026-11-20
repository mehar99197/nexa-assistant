#pragma once

#include "core/net/MultipartForm.h"
#include "voice/stt/ISpeechRecognizer.h"

#include <QPointer>

#include <memory>

class QNetworkReply;

namespace nexa {

class HttpClient;

// Speech recognition through the active provider, e.g. OpenAI (Strategy). Transcription models
// (whisper, *transcribe*, nova, …) use the OpenAI /audio/transcriptions endpoint; any other model
// is taken for an audio-capable chat model (Gemini, gpt-audio) and gets the recording as
// input_audio with an instruction to write down what was said.
class CloudRecognizer : public ISpeechRecognizer {
    Q_OBJECT
public:
    struct Settings {
        QString providerName;   // "OpenAI"
        QString baseUrl;        // …/v1
        QString apiKey;
        QString model;          // e.g. agy/gemini-3.1-flash-lite or whisper-1
        QString language;       // ISO 639-1, or "auto"
    };

    CloudRecognizer(HttpClient *http, Settings settings, QObject *parent = nullptr);
    ~CloudRecognizer() override;

    QString name() const override;
    bool isLocal() const override { return false; }
    void transcribe(const QByteArray &pcm16k, const QString &prompt, Done done) override;
    void cancel() override;

    // Pure request/response mapping (unit-tested).
    static bool usesTranscriptionEndpoint(const QString &model);
    static QByteArray chatBody(const Settings &settings, const QByteArray &wav, const QString &prompt);
    static MultipartForm transcriptionForm(const Settings &settings, const QByteArray &wav, const QString &prompt);
    static Result<QString> parseTranscription(const QByteArray &body);
    static Result<QString> parseChatReply(const QByteArray &body);

private:
    HttpClient *http_;
    Settings settings_;
    QPointer<QNetworkReply> reply_;
    std::shared_ptr<Done> pending_;
};

} // namespace nexa
