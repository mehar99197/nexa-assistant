#pragma once

#include "core/Result.h"
#include "voice/Wav.h"

#include <QDateTime>
#include <QString>

namespace nexa {

// A recording of the owner reading a short passage, kept so Nexa can speak in their voice (value, pure): the
// sound (16 kHz mono, the quiet cut off both ends), the words read, and when. It is the owner's own voice, so it
// stays on this computer — ~/.local/share/nexa/voice-sample.wav and voice-sample.json, 0600 — is never sent
// anywhere (ZipVoice makes the voice locally) and never logged; Forget removes it.
struct VoiceSample {
    PcmAudio audio;
    QString text;
    QDateTime recorded;

    static constexpr int kMinMs = 3'000;        // of sound, for a voice worth copying
    static constexpr int kMaxMs = 12'000;       // what the model is given of a longer recording
    static constexpr int kRecordMs = 14'000;    // how long the microphone stays open for the passage

    static QString defaultDir();                // ~/.local/share/nexa
    static QString wavPath(const QString &dir = defaultDir());
    static QString infoPath(const QString &dir = defaultDir());
    static QString passage();                   // what the owner reads aloud: about ten seconds of English
    static Result<VoiceSample> load(const QString &dir = defaultDir());
    Result<void> save(const QString &dir = defaultDir()) const;
    static Result<void> forget(const QString &dir = defaultDir());

    // The recording with the quiet cut off both ends (a short breath kept), at most kMaxMs long.
    static PcmAudio trimmed(const PcmAudio &audio);
    // Whether a trimmed recording is a sample worth keeping: long enough and not silence. The error says what to do.
    static Result<void> usable(const PcmAudio &audio);
    int durationMs() const { return int(audio.durationMs()); }
};

} // namespace nexa
