#include "voice/stt/CloudRecognizer.h"

#include "core/net/HttpClient.h"
#include "voice/Wav.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QUrl>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

constexpr int kRequestTimeoutMs = 45'000;

QString instruction(const CloudRecognizer::Settings &settings, const QString &prompt) {
    QString text = u"You are a speech-to-text engine. Write down exactly what the speaker says, word for word, "
                   "in the language they speak. Output only the transcript: no quotes, labels, notes, answers or "
                   "translation. If nothing intelligible is said, output nothing."_s;
    if (!settings.language.isEmpty() && settings.language != u"auto"_s) {
        text += u" The speaker's language is most likely \"%1\"."_s.arg(settings.language);
    }
    if (!prompt.isEmpty()) text += u" Context, for the spelling of names: %1"_s.arg(prompt);
    return text;
}

// Models sometimes wrap the transcript in quotes or label it.
QString unwrap(QString text) {
    static const QRegularExpression label(u"^\\s*(?:transcript(?:ion)?|text)\\s*:\\s*"_s,
                                          QRegularExpression::CaseInsensitiveOption);
    text = text.trimmed();
    text.remove(label);
    for (const QStringView pair : {u"\"\""_sv, u"“”"_sv, u"``"_sv, u"''"_sv}) {
        if (text.size() >= 2 && text.front() == pair[0] && text.back() == pair[1]) text = text.mid(1, text.size() - 2);
    }
    return text.trimmed();
}

} // namespace

CloudRecognizer::CloudRecognizer(HttpClient *http, Settings settings, QObject *parent)
    : ISpeechRecognizer(parent), http_(http), settings_(std::move(settings)) {}

CloudRecognizer::~CloudRecognizer() {
    cancel();
}

QString CloudRecognizer::name() const {
    return u"%1 %2"_s.arg(settings_.providerName, settings_.model);
}

bool CloudRecognizer::usesTranscriptionEndpoint(const QString &model) {
    static const QRegularExpression transcription(
        u"whisper|transcri|(?:^|[/_-])asr|nova-|parakeet|voxtral|chirp|universal-|solaria|speech-to-text|stt-"_s,
        QRegularExpression::CaseInsensitiveOption);
    return transcription.match(model).hasMatch();
}

QByteArray CloudRecognizer::chatBody(const Settings &settings, const QByteArray &wav, const QString &prompt) {
    const QJsonArray content{
        QJsonObject{{u"type"_s, u"text"_s}, {u"text"_s, u"Transcribe this recording."_s}},
        QJsonObject{{u"type"_s, u"input_audio"_s},
                    {u"input_audio"_s, QJsonObject{{u"data"_s, QString::fromLatin1(wav.toBase64())},
                                                   {u"format"_s, u"wav"_s}}}},
    };
    const QJsonObject body{
        {u"model"_s, settings.model},
        {u"stream"_s, false},
        {u"temperature"_s, 0},
        {u"messages"_s, QJsonArray{
             QJsonObject{{u"role"_s, u"system"_s}, {u"content"_s, instruction(settings, prompt)}},
             QJsonObject{{u"role"_s, u"user"_s}, {u"content"_s, content}},
         }},
    };
    return QJsonDocument(body).toJson(QJsonDocument::Compact);
}

MultipartForm CloudRecognizer::transcriptionForm(const Settings &settings, const QByteArray &wav,
                                                 const QString &prompt) {
    MultipartForm form;
    form.addField(u"model"_s, settings.model);
    form.addField(u"response_format"_s, u"json"_s);
    if (!settings.language.isEmpty() && settings.language != u"auto"_s) form.addField(u"language"_s, settings.language);
    if (!prompt.isEmpty()) form.addField(u"prompt"_s, prompt);
    form.addFile(u"file"_s, u"speech.wav"_s, "audio/wav", wav);
    return form;
}

Result<QString> CloudRecognizer::parseTranscription(const QByteArray &body) {
    const QJsonDocument doc = QJsonDocument::fromJson(body);
    if (doc.isObject()) {
        const QJsonValue text = doc.object().value(u"text"_s);
        if (text.isString()) return text.toString().trimmed();
        return Error{ErrorCode::Parse, u"The transcription reply has no text."_s};
    }
    // response_format=text servers answer with the bare transcript.
    if (!body.trimmed().startsWith('{') && !body.trimmed().startsWith('[')) return QString::fromUtf8(body).trimmed();
    return Error{ErrorCode::Parse, u"Unexpected transcription reply."_s};
}

Result<QString> CloudRecognizer::parseChatReply(const QByteArray &body) {
    const QJsonObject root = QJsonDocument::fromJson(body).object();
    const QJsonArray choices = root.value(u"choices"_s).toArray();
    if (choices.isEmpty()) {
        const QString message = HttpClient::serverMessage(body);
        return Error{ErrorCode::Parse, message.isEmpty() ? u"The model returned no transcript."_s : message};
    }
    const QJsonValue content = choices.first().toObject().value(u"message"_s).toObject().value(u"content"_s);
    if (content.isString()) return unwrap(content.toString());
    QString text;
    for (const QJsonValue &part : content.toArray()) text += part.toObject().value(u"text"_s).toString();
    return unwrap(text);
}

void CloudRecognizer::transcribe(const QByteArray &pcm16k, const QString &prompt, Done done) {
    cancel();
    // Shared with the reply callback: whoever runs first (reply or cancel()) takes the callback.
    auto call = std::make_shared<Done>(std::move(done));
    pending_ = call;
    const QByteArray wav = wav::encode(PcmAudio{.samples = pcm16k, .sampleRate = 16'000, .channels = 1});
    const bool transcription = usesTranscriptionEndpoint(settings_.model);

    QNetworkRequest request;
    QByteArray body;
    if (transcription) {
        const MultipartForm form = transcriptionForm(settings_, wav, prompt);
        request = HttpClient::jsonRequest(QUrl(settings_.baseUrl + u"/audio/transcriptions"_s), settings_.apiKey);
        request.setHeader(QNetworkRequest::ContentTypeHeader, form.contentType());
        body = form.body();
    } else {
        request = HttpClient::jsonRequest(QUrl(settings_.baseUrl + u"/chat/completions"_s), settings_.apiKey);
        body = chatBody(settings_, wav, prompt);
    }
    reply_ = http_->post(request, body, kRequestTimeoutMs, this, [call, transcription](Result<QByteArray> reply) {
        if (!*call) return;     // cancelled
        const Done done = std::exchange(*call, nullptr);
        if (!reply) {
            done(reply.error());
            return;
        }
        done(transcription ? parseTranscription(reply.value()) : parseChatReply(reply.value()));
    });
}

void CloudRecognizer::cancel() {
    if (const std::shared_ptr<Done> call = std::exchange(pending_, nullptr); call && *call) {
        std::exchange(*call, nullptr)(Error{ErrorCode::Cancelled, u"Cancelled"_s});
    }
    if (reply_) reply_->abort();
    reply_.clear();
}

} // namespace nexa
