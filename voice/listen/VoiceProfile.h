#pragma once

#include "core/Result.h"

#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QString>

namespace nexa {

// The owner's voice as Nexa knows it (value, pure): the mean of the speaker embeddings of a few sentences they read,
// and the model that made them (another model's numbers mean nothing). Kept in ~/.local/share/nexa/voice-profile.json,
// 0600 — it describes a person's voice, so it never leaves the machine and is never logged.
struct VoiceProfile {
    QString model;
    QList<float> embedding;     // unit length
    int samples{0};             // how many recordings it was made from
    QDateTime enrolled;

    static QString defaultPath();
    static Result<VoiceProfile> load(const QString &path = defaultPath());
    Result<void> save(const QString &path = defaultPath()) const;
    static Result<void> forget(const QString &path = defaultPath());

    // The direction the recordings share: their unit vectors averaged, then made unit length again.
    static VoiceProfile fromRecordings(const QString &model, const QList<QList<float>> &embeddings);
    // −1 … 1; the same voice scores high, another low. Vectors of different lengths score −1.
    static double cosine(const QList<float> &a, const QList<float> &b);
    double score(const QList<float> &embedding) const { return cosine(embedding, this->embedding); }

    QJsonObject toJson() const;
    static Result<VoiceProfile> fromJson(const QJsonObject &json);
};

} // namespace nexa
