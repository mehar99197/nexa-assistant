#include "voice/tts/CloudSynthesizer.h"

#include "core/net/HttpClient.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QUrl>

#include <cmath>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

constexpr int kRequestTimeoutMs = 30'000;

} // namespace

CloudSynthesizer::CloudSynthesizer(HttpClient *http, Settings settings, QObject *parent)
    : ISpeechSynthesizer(parent), http_(http), settings_(std::move(settings)) {}

CloudSynthesizer::~CloudSynthesizer() {
    cancel();
}

QString CloudSynthesizer::name() const {
    return u"%1 %2"_s.arg(settings_.providerName, settings_.model);
}

QByteArray CloudSynthesizer::requestBody(const Settings &settings, const QString &text) {
    QJsonObject body{
        {u"model"_s, settings.model},
        {u"input"_s, text},
        {u"response_format"_s, u"wav"_s},
    };
    if (!settings.voice.isEmpty()) body.insert(u"voice"_s, settings.voice);
    // OpenAI's speed runs 0.25 … 4; the slider's ±1 maps to half … double speed.
    if (settings.rate != 0.0) body.insert(u"speed"_s, std::round(std::pow(2.0, settings.rate) * 100.0) / 100.0);
    return QJsonDocument(body).toJson(QJsonDocument::Compact);
}

void CloudSynthesizer::synthesize(const QString &text, Done done) {
    const quint64 id = ++nextId_;
    pending_.emplace(id, std::move(done));
    const QUrl url(settings_.baseUrl + u"/audio/speech"_s);
    QNetworkRequest request = HttpClient::jsonRequest(url, settings_.apiKey);
    request.setRawHeader("Accept", "audio/wav, audio/*");
    QNetworkReply *reply = http_->post(request, requestBody(settings_, text), kRequestTimeoutMs, this,
                                       [this, id](Result<QByteArray> body) {
        replies_.erase(id);
        if (!pending_.contains(id)) return;
        if (!body) {
            complete(id, body.error());
            return;
        }
        decoder_.decode(body.value(), [this, id](Result<PcmAudio> audio) { complete(id, std::move(audio)); });
    });
    replies_.emplace(id, reply);
}

void CloudSynthesizer::cancel() {
    auto pending = std::exchange(pending_, {});
    auto replies = std::exchange(replies_, {});
    for (auto &[id, reply] : replies) {
        if (reply) reply->abort();
    }
    for (auto &[id, done] : pending) done(Error{ErrorCode::Cancelled, u"Cancelled"_s});
}

void CloudSynthesizer::complete(quint64 id, Result<PcmAudio> result) {
    const auto found = pending_.find(id);
    if (found == pending_.end()) return;    // cancelled meanwhile
    Done done = std::move(found->second);
    pending_.erase(found);
    done(std::move(result));
}

} // namespace nexa
