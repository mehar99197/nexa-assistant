#pragma once

#include <QString>
#include <QStringList>

namespace nexa {

// Where the voice engines live (Strategy by place): the user's own copies first — what scripts/download-sherpa.sh
// and scripts/download-piper.sh put in ~/.local/share/nexa — then the package's, NEXA_ENGINE_DIR
// (/usr/lib/nexa-assistant in the .deb). Models and voices are the user's always: the package ships the engines, and
// Settings → Voice downloads the models into the user's folder.
class EngineLocator {
public:
    explicit EngineLocator(QString userDataDir = defaultUserDataDir(), QString packageDir = packageEngineDir());

    static QString defaultUserDataDir();    // ~/.local/share/nexa
    static QString packageEngineDir();      // NEXA_ENGINE_DIR, or empty for a source build

    QString userDataDir() const { return user_; }
    QString packageDir() const { return package_; }

    // sherpa-onnx: the first of the user's and the package's sherpa folders holding lib/libsherpa-onnx-c-api.so;
    // the user's when neither does (so the message names the right place).
    QString sherpaLibraryDir() const;
    QString sherpaModelsDir() const;        // <user>/sherpa — the spotter, Silero, GTCRN, the speaker model, ZipVoice
    bool sherpaFromPackage() const;

    // Piper: the first of the user's and the package's piper folders with an executable `piper`.
    QString piperDir() const;
    QString piperVoicesDir() const;         // <user>/piper/voices
    bool piperFromPackage() const;
    // Where voices are looked for, in order: the user's voices folder, then the engine folder's `voices`.
    QStringList piperVoiceSearchDirs() const;

    QString modelsDir() const;              // <user>/models — whisper's ggml files

private:
    QString user_;
    QString package_;
};

} // namespace nexa
