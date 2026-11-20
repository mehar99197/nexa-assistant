#include "voice/listen/VoiceProfile.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QSaveFile>
#include "core/config/Portable.h"

#include <cmath>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

constexpr QFileDevice::Permissions kPrivate = QFileDevice::ReadOwner | QFileDevice::WriteOwner;

QList<float> unit(QList<float> vector) {
    double length = 0;
    for (const float value : vector) length += double(value) * double(value);
    length = std::sqrt(length);
    if (length <= 0) return vector;
    for (float &value : vector) value = float(double(value) / length);
    return vector;
}

} // namespace

QString VoiceProfile::defaultPath() {
    return portable::dataDir() + u"/voice-profile.json"_s;
}

double VoiceProfile::cosine(const QList<float> &a, const QList<float> &b) {
    if (a.isEmpty() || a.size() != b.size()) return -1.0;
    double dot = 0;
    double aa = 0;
    double bb = 0;
    for (qsizetype i = 0; i < a.size(); ++i) {
        dot += double(a[i]) * double(b[i]);
        aa += double(a[i]) * double(a[i]);
        bb += double(b[i]) * double(b[i]);
    }
    if (aa <= 0 || bb <= 0) return -1.0;
    return dot / (std::sqrt(aa) * std::sqrt(bb));
}

VoiceProfile VoiceProfile::fromRecordings(const QString &model, const QList<QList<float>> &embeddings) {
    VoiceProfile profile;
    profile.model = model;
    profile.enrolled = QDateTime::currentDateTimeUtc();
    for (const QList<float> &embedding : embeddings) {
        if (embedding.isEmpty()) continue;
        const QList<float> direction = unit(embedding);
        if (profile.embedding.isEmpty()) profile.embedding = QList<float>(direction.size(), 0.0f);
        if (direction.size() != profile.embedding.size()) continue;
        for (qsizetype i = 0; i < direction.size(); ++i) profile.embedding[i] += direction[i];
        ++profile.samples;
    }
    profile.embedding = unit(profile.embedding);
    return profile;
}

QJsonObject VoiceProfile::toJson() const {
    QJsonArray values;
    for (const float value : embedding) values.append(double(value));
    return QJsonObject{{u"model"_s, model}, {u"samples"_s, samples}, {u"enrolled"_s, enrolled.toString(Qt::ISODate)},
                       {u"embedding"_s, values}};
}

Result<VoiceProfile> VoiceProfile::fromJson(const QJsonObject &json) {
    VoiceProfile profile;
    profile.model = json.value(u"model"_s).toString();
    profile.samples = json.value(u"samples"_s).toInt();
    profile.enrolled = QDateTime::fromString(json.value(u"enrolled"_s).toString(), Qt::ISODate);
    for (const QJsonValue &value : json.value(u"embedding"_s).toArray()) {
        if (!value.isDouble()) return Error{ErrorCode::Parse, u"The voice profile is damaged."_s};
        profile.embedding.append(float(value.toDouble()));
    }
    if (profile.model.isEmpty() || profile.embedding.isEmpty()) return Error{ErrorCode::Parse, u"The voice profile is damaged."_s};
    return profile;
}

Result<VoiceProfile> VoiceProfile::load(const QString &path) {
    QFile file(path);
    if (!file.exists()) return Error{ErrorCode::NotFound, u"No voice is enrolled."_s};
    if (!file.open(QIODevice::ReadOnly)) return Error{ErrorCode::Io, u"Cannot read the voice profile: %1"_s.arg(file.errorString())};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject()) return Error{ErrorCode::Parse, u"The voice profile is damaged."_s};
    return fromJson(document.object());
}

Result<void> VoiceProfile::save(const QString &path) const {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return Error{ErrorCode::Io, u"Cannot save the voice profile: %1"_s.arg(file.errorString())};
    file.write(QJsonDocument(toJson()).toJson(QJsonDocument::Compact));
    if (!file.commit()) return Error{ErrorCode::Io, u"Cannot save the voice profile: %1"_s.arg(file.errorString())};
    QFile::setPermissions(path, kPrivate);
    return {};
}

Result<void> VoiceProfile::forget(const QString &path) {
    if (QFile::exists(path) && !QFile::remove(path)) return Error{ErrorCode::Io, u"Cannot remove the voice profile."_s};
    return {};
}

} // namespace nexa
