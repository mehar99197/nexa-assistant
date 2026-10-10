#pragma once

#include "remote/RemoteClient.h"

#include <QMainWindow>
#include <QString>

class QStackedWidget;

namespace nexa {

class ChatPage;
class PairPage;
class PhoneSettings;
class Recorder;
class SpeechPlayer;

// The app's one window (Mediator): it decides which page shows, turns what the client receives
// into lines of the conversation, and what the user does into requests. Trust on first use lives
// here, because only the user can compare the certificate's fingerprint with the PC's.
class PhoneWindow : public QMainWindow {
    Q_OBJECT
public:
    PhoneWindow(PhoneSettings *settings, RemoteClient *client, Recorder *recorder, SpeechPlayer *player,
                QWidget *parent = nullptr);

    // Paired: connect. Otherwise the pairing page.
    void start();

private:
    void showPairPage(const QString &reason = {});
    void onPairRequested(const QString &address, const QString &code, const QString &name);
    void pairAfterTrust(const RemoteEndpoint &endpoint, const QString &code, const QString &name);
    void pairNow(const RemoteEndpoint &endpoint, const QString &code, const QString &name);
    void onStateChanged(RemoteClient::State state, const QString &detail);
    void onWelcomed(const QString &device, bool voice, bool busy);
    void onReceived(const remote::ServerMessage &message);
    void onUnauthorized(const QString &reason);
    void onCertificateRejected(const QString &fingerprint);
    void onAsk(const QString &text);
    void onTalkPressed();
    void onTalkReleased();
    void beginRecording();
    void onRecorded(const QByteArray &pcm);
    void forget();

    PhoneSettings *settings_;
    RemoteClient *client_;
    Recorder *recorder_;
    SpeechPlayer *player_;
    QStackedWidget *pages_{nullptr};
    PairPage *pair_{nullptr};
    ChatPage *chat_{nullptr};
    QString device_;
    bool wantTalk_{false};
};

} // namespace nexa
