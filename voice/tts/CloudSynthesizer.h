#pragma once

#include "voice/tts/AudioFileDecoder.h"
#include "voice/tts/ISpeechSynthesizer.h"

#include <QPointer>

#include <map>

class QNetworkReply;

namespace nexa {

class HttpClient;

// Cloud voice through an OpenAI-compatible /audio/speech endpoint, e.g. OpenAI (Strategy).
// Asks for WAV; MP3 and other formats (a gateway's gtts/default answers with MP3) are decoded.
class CloudSynthesizer : public ISpeechSynthesizer {
    Q_OBJECT
public:
    struct Settings {
        QString providerName;   // shown in the UI: "OpenAI"
        QString baseUrl;        // …/v1
        QString apiKey;
        QString model;          // e.g. gtts/default, tts-1
        QString voice;          // e.g. alloy; a language code for gtts/*
        double rate{0.0};       // -1 … 1
    };

    CloudSynthesizer(HttpClient *http, Settings settings, QObject *parent = nullptr);
    ~CloudSynthesizer() override;

    QString name() const override;
    void synthesize(const QString &text, Done done) override;
    void cancel() override;

    static QByteArray requestBody(const Settings &settings, const QString &text);

private:
    void complete(quint64 id, Result<PcmAudio> result);

    HttpClient *http_;
    Settings settings_;
    AudioFileDecoder decoder_;
    quint64 nextId_{0};
    std::map<quint64, Done> pending_;
    std::map<quint64, QPointer<QNetworkReply>> replies_;
};

} // namespace nexa
