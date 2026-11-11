#pragma once

#include "core/config/Config.h"
#include "remote/IRemoteHost.h"

#include <QObject>

#include <memory>

namespace nexa {

class AppController;
class HttpClient;
class SherpaRuntime;
class ISpeechRecognizer;
class TtsFileRenderer;

// Adapter: the web remote's view of Nexa (IRemoteHost) over AppController and the voice backends.
// It keeps a recogniser and a voice of its own, built from the same Settings → Voice, so a phone's
// request never touches the PC's microphone or speakers. They are built the first time a phone
// needs them, so a web remote that is switched off costs nothing.
class RemoteBridge : public QObject, public IRemoteHost {
    Q_OBJECT
public:
    // `sherpa`: so the phone hears the owner's own voice too, when "Speak in my voice" is on.
    RemoteBridge(AppController *controller, HttpClient *http, SherpaRuntime *sherpa = nullptr, QObject *parent = nullptr);
    ~RemoteBridge() override;

    // Settings changed: the next phone request builds its voice backends afresh.
    void applyConfig(const Config &config);

    bool isBusy() const override;
    void submit(const QString &text, const QString &source) override;
    void cancel() override;
    void transcribe(const QByteArray &pcm16k, std::function<void(Result<QString>)> done) override;
    bool canTranscribe() const override;
    void renderSpeech(const QString &text, std::function<void(Result<QByteArray>)> done) override;
    bool isLocked() const override;         // hardcore mode counts down to the lock, or holds it
    QString lockText() const override;
    void unlock() override;
    QList<RemoteApproval> approvals() const override;
    void answer(quint64 id, bool allow) override;

private:
    void build() const;

    AppController *controller_;
    HttpClient *http_;
    SherpaRuntime *sherpa_;
    Config config_;
    mutable bool built_{false};
    mutable std::unique_ptr<ISpeechRecognizer> recognizer_;
    mutable std::unique_ptr<TtsFileRenderer> renderer_;
    bool transcribing_{false};
};

} // namespace nexa
