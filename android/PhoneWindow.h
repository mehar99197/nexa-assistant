#pragma once

#include "remote/RemoteClient.h"
#include "ui/NexaCoreSpec.h"
#include "ui/NexaTheme.h"

#include <QElapsedTimer>
#include <QMainWindow>
#include <QString>

#include <optional>

class QStackedWidget;

namespace nexa {

class ChatPage;
class PairPage;
class PhoneSettings;
class Recorder;
class SpeechPlayer;

// The app's one window (Mediator): it decides which page shows, turns what the client receives
// into lines of the conversation and into the state of the core, and what the user does into
// requests. Trust on first use lives here, because only the user can compare the certificate's
// fingerprint with the PC's. The core shows the PC's state, except while this phone itself is
// listening, waiting for its words to be recognised, or reading a reply aloud.
class PhoneWindow : public QMainWindow {
    Q_OBJECT
public:
    static constexpr int kHoldMs = 300;     // a press this long is push-to-talk; shorter is a tap

    PhoneWindow(PhoneSettings *settings, RemoteClient *client, Recorder *recorder, SpeechPlayer *player,
                QWidget *parent = nullptr);

    void setScheme(ThemeScheme scheme);
    // Paired: connect. Otherwise the pairing page.
    void start();
    // The PC handed over where it is and the code (over adb): pair as if they had been typed.
    void pairWith(const QString &address, const QString &code, const QString &name);

private:
    void showPairPage(const QString &reason = {});
    void onPairRequested(const QString &address, const QString &code, const QString &name);
    void pairAfterTrust(const RemoteEndpoint &endpoint, const QString &code, const QString &name);
    void pairNow(const RemoteEndpoint &endpoint, const QString &code, const QString &name);
    void onStateChanged(RemoteClient::State state, const QString &detail);
    void onWelcomed(const QString &device, bool voice, bool busy, const QString &mode);
    void onReceived(const remote::ServerMessage &message);
    void onUnauthorized(const QString &reason);
    void onCertificateRejected(const QString &fingerprint);
    // What the PC said of the ways to it: kept, tried in turn; its certificate taken while connected over adb.
    void onRoutesLearned(const QList<nexa::RemoteRoute> &routes, const QString &fingerprint);
    void useLearnedRoutes();
    void onAsk(const QString &text);
    void onAskWithFiles(const QString &text, const QStringList &filePaths);
    void onTalkPressed();
    void onTalkReleased();
    void beginRecording();
    void onRecorded(const QByteArray &pcm);
    void onRecordingStopped();
    void forget();
    void setLocalMode(std::optional<CoreMode> mode);
    void showCore();

    PhoneSettings *settings_;
    RemoteClient *client_;
    Recorder *recorder_;
    SpeechPlayer *player_;
    QStackedWidget *pages_{nullptr};
    PairPage *pair_{nullptr};
    ChatPage *chat_{nullptr};
    QString device_;
    bool wantTalk_{false};
    bool tapRecording_{false};          // a tap started it: it ends at the pause, or at the next tap
    QElapsedTimer pressClock_;
    CoreMode pcMode_{CoreMode::Idle};
    CoreNodes pcNodes_{NoNode};
    std::optional<CoreMode> localMode_;
};

} // namespace nexa
