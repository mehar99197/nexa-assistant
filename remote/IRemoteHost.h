#pragma once

#include "core/Result.h"

#include <QByteArray>
#include <QList>
#include <QString>

#include <functional>

namespace nexa {

// A question a task waits on, as a phone shows it.
struct RemoteApproval {
    quint64 id{0};
    QString agent;
    QString tool;
    QString question;
};

// What the web remote needs from the rest of Nexa. app/ implements it over AppController and the
// voice backends, so remote/ depends on neither (dependencies point downward).
class IRemoteHost {
public:
    virtual ~IRemoteHost() = default;

    virtual bool isBusy() const = 0;
    // A request from the phone, handled exactly as one typed on the PC: fast path or agent,
    // policy and audit alike. `source` is "phone", or "phone-voice" when it was spoken.
    virtual void submit(const QString &text, const QString &source) = 0;
    // Submit a request accompanied by already-saved local attachment paths (uploaded over /api/upload).
    virtual void submitWithAttachments(const QString &text, const QString &source, const QStringList &savedPaths) {
        (void)savedPaths;
        submit(text, source);
    }
    virtual QString attachmentDir() const { return {}; }
    virtual void cancel() = 0;
    // A spoken request recorded on the phone: 16 kHz mono 16-bit PCM in, the words out.
    virtual void transcribe(const QByteArray &pcm16k, std::function<void(Result<QString>)> done) = 0;
    virtual bool canTranscribe() const = 0;
    // A reply read aloud, as the bytes of a WAV file for the phone to play.
    virtual void renderSpeech(const QString &text, std::function<void(Result<QByteArray>)> done) = 0;

    // Hardcore mode: the PC's keyboard and mouse are locked (and until when, in a sentence); Unlock PC ends it.
    virtual bool isLocked() const { return false; }
    virtual QString lockText() const { return {}; }
    virtual void unlock() {}
    // The questions tasks wait on, and the phone's answer to one.
    virtual QList<RemoteApproval> approvals() const { return {}; }
    virtual void answer(quint64 id, bool allow) { (void)id, (void)allow; }
};

} // namespace nexa
