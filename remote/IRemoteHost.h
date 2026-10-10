#pragma once

#include "core/Result.h"

#include <QByteArray>
#include <QString>

#include <functional>

namespace nexa {

// What the web remote needs from the rest of Nexa. app/ implements it over AppController and the
// voice backends, so remote/ depends on neither (dependencies point downward).
class IRemoteHost {
public:
    virtual ~IRemoteHost() = default;

    virtual bool isBusy() const = 0;
    // A request from the phone, handled exactly as one typed on the PC: fast path or agent,
    // policy and audit alike. `source` is "phone", or "phone-voice" when it was spoken.
    virtual void submit(const QString &text, const QString &source) = 0;
    virtual void cancel() = 0;
    // A spoken request recorded on the phone: 16 kHz mono 16-bit PCM in, the words out.
    virtual void transcribe(const QByteArray &pcm16k, std::function<void(Result<QString>)> done) = 0;
    virtual bool canTranscribe() const = 0;
    // A reply read aloud, as the bytes of a WAV file for the phone to play.
    virtual void renderSpeech(const QString &text, std::function<void(Result<QByteArray>)> done) = 0;
};

} // namespace nexa
