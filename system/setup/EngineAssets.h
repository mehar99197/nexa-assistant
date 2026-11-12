#pragma once

#include <QList>
#include <QString>
#include <QStringList>

namespace nexa::engineassets {

// One download the voice needs (pure data): a model file, or an archive whose listed members are installed. The
// same URLs and SHA-256 pins as scripts/download-sherpa.sh, download-piper.sh and download-whisper-model.sh, which a
// test reads to prove it.
struct Asset {
    QString id;             // "whisper-base.en", "piper-en_US-lessac-medium", "silero", "gtcrn", "speaker", "kws", "zipvoice", "vocos"
    QString group;          // "listening" | "speaker" | "voice" | "own-voice" | "engine"
    QString url;
    qint64 bytes{0};        // of the download — the archive's, for one
    QString sha256;         // of the download
    QString installDir;     // relative to the data folder: "models", "piper/voices", "sherpa", "sherpa/speaker", …
    QStringList files;      // the installed names; an archive's members under its top folder; one name for a plain file
    QString archiveTop;     // the archive's top folder; empty for a plain file

    bool isArchive() const { return !archiveTop.isEmpty(); }
};

// The models, in the order they are fetched (small first, the biggest last).
QList<Asset> all();
// The engines a packaged build ships and a source install downloads (not models; scripts/build-deb.sh's pins).
QList<Asset> engines();
// Of `all()`, those with any installed file missing under `dataDir`.
QList<Asset> missing(const QString &dataDir);
qint64 totalBytes(const QList<Asset> &assets);
// The groups, in order, and their titles for Settings.
QStringList groups();
QString groupTitle(const QString &group);
// "153 MB", "1.2 GB".
QString sizeText(qint64 bytes);

} // namespace nexa::engineassets
