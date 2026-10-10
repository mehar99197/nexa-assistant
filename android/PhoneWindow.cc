#include "PhoneWindow.h"

#include "ChatPage.h"
#include "PairPage.h"
#include "PhoneSettings.h"
#include "Recorder.h"
#include "SpeechPlayer.h"

#include <QApplication>
#include <QMessageBox>
#include <QPermissions>
#include <QPushButton>
#include <QStackedWidget>

using namespace Qt::StringLiterals;

namespace nexa {

PhoneWindow::PhoneWindow(PhoneSettings *settings, RemoteClient *client, Recorder *recorder, SpeechPlayer *player,
                         QWidget *parent)
    : QMainWindow(parent), settings_(settings), client_(client), recorder_(recorder), player_(player) {
    setWindowTitle(tr("Nexa"));
    pages_ = new QStackedWidget(this);
    pair_ = new PairPage(pages_);
    chat_ = new ChatPage(pages_);
    pages_->addWidget(pair_);
    pages_->addWidget(chat_);
    setCentralWidget(pages_);

    connect(pair_, &PairPage::pairRequested, this, &PhoneWindow::onPairRequested);
    connect(chat_, &ChatPage::askRequested, this, &PhoneWindow::onAsk);
    connect(chat_, &ChatPage::cancelRequested, client_, &RemoteClient::cancel);
    connect(chat_, &ChatPage::talkPressed, this, &PhoneWindow::onTalkPressed);
    connect(chat_, &ChatPage::talkReleased, this, &PhoneWindow::onTalkReleased);
    connect(chat_, &ChatPage::forgetRequested, this, &PhoneWindow::forget);

    connect(client_, &RemoteClient::stateChanged, this, &PhoneWindow::onStateChanged);
    connect(client_, &RemoteClient::welcomed, this, &PhoneWindow::onWelcomed);
    connect(client_, &RemoteClient::received, this, &PhoneWindow::onReceived);
    connect(client_, &RemoteClient::unauthorized, this, &PhoneWindow::onUnauthorized);
    connect(client_, &RemoteClient::certificateRejected, this, &PhoneWindow::onCertificateRejected);

    connect(recorder_, &Recorder::recorded, this, &PhoneWindow::onRecorded);
    connect(recorder_, &Recorder::failed, this, [this](const QString &reason) {
        chat_->setRecording(false);
        chat_->addMessage(ChatPage::Kind::Error, reason);
    });
    connect(player_, &SpeechPlayer::failed, this, [this](const QString &reason) {
        chat_->addMessage(ChatPage::Kind::Error, reason);
    });
}

void PhoneWindow::start() {
    pair_->setDeviceName(settings_->deviceName());
    const RemoteEndpoint endpoint = settings_->endpoint();
    if (endpoint.isValid()) {
        pair_->setAddress(endpoint.port == RemoteEndpoint::kDefaultPort ? endpoint.host
                                                                        : u"%1:%2"_s.arg(endpoint.host).arg(endpoint.port));
    }
    if (!settings_->isPaired()) {
        showPairPage();
        return;
    }
    client_->setEndpoint(endpoint);
    client_->setPinnedFingerprint(settings_->fingerprint());
    client_->setToken(settings_->token());
    pages_->setCurrentWidget(chat_);
    client_->connectToNexa();
}

void PhoneWindow::showPairPage(const QString &reason) {
    pair_->setBusy(false);
    pair_->setError(reason);
    pages_->setCurrentWidget(pair_);
}

void PhoneWindow::onPairRequested(const QString &address, const QString &code, const QString &name) {
    const std::optional<RemoteEndpoint> endpoint = RemoteEndpoint::parse(address);
    if (!endpoint) {
        pair_->setError(tr("That is not an address Nexa can be reached at."));
        return;
    }
    pair_->setBusy(true);
    settings_->setDeviceName(name);
    client_->disconnectFromNexa();
    client_->setEndpoint(*endpoint);
    client_->setPinnedFingerprint(settings_->fingerprint());
    pairAfterTrust(*endpoint, code, name);
}

// Nexa's certificate is its own, so no authority vouches for it: the user does, once, by comparing
// the fingerprint with the one Settings → Remote shows. The same fingerprint again needs no asking.
void PhoneWindow::pairAfterTrust(const RemoteEndpoint &endpoint, const QString &code, const QString &name) {
    client_->probe([this, endpoint, code, name](Result<RemoteClient::Certificate> probed) {
        if (!probed) {
            showPairPage(tr("Nexa could not be reached at %1: %2").arg(endpoint.host, probed.error().message));
            return;
        }
        const RemoteClient::Certificate &certificate = probed.value();
        if (certificate.trusted || certificate.fingerprint == settings_->fingerprint()) {
            client_->setPinnedFingerprint(certificate.fingerprint);
            pairNow(endpoint, code, name);
            return;
        }
        QMessageBox ask(this);
        ask.setIcon(QMessageBox::Question);
        ask.setWindowTitle(tr("Is this your Nexa?"));
        ask.setText(tr("Nexa at %1 shows this certificate:").arg(endpoint.host));
        ask.setInformativeText(certificate.fingerprint + u"\n\n"_s
                               + tr("Settings → Remote on the PC shows the fingerprint of the real one. "
                                    "Trust it only if the two are the same."));
        QPushButton *trust = ask.addButton(tr("They match — trust it"), QMessageBox::AcceptRole);
        ask.addButton(QMessageBox::Cancel);
        ask.exec();
        if (ask.clickedButton() != trust) {
            showPairPage();
            return;
        }
        client_->setPinnedFingerprint(certificate.fingerprint);
        pairNow(endpoint, code, name);
    });
}

void PhoneWindow::pairNow(const RemoteEndpoint &endpoint, const QString &code, const QString &name) {
    client_->pair(code, name, [this, endpoint](Result<QString> token) {
        if (!token) {
            showPairPage(token.error().message);
            return;
        }
        settings_->save(endpoint, token.value(), client_->pinnedFingerprint());
        pair_->setBusy(false);
        pair_->setError({});
        chat_->clear();
        pages_->setCurrentWidget(chat_);
        client_->connectToNexa();
    });
}

void PhoneWindow::onStateChanged(RemoteClient::State state, const QString &detail) {
    switch (state) {
    case RemoteClient::State::Online:
        chat_->setStatus(tr("connected · %1").arg(device_.isEmpty() ? detail : device_), ChatPage::Status::Ok);
        break;
    case RemoteClient::State::Connecting:
        chat_->setStatus(tr("connecting…"), ChatPage::Status::Plain);
        break;
    case RemoteClient::State::Waiting:
        chat_->setStatus(detail, ChatPage::Status::Warn);
        break;
    case RemoteClient::State::Offline:
        chat_->setStatus(detail.isEmpty() ? tr("offline") : detail, ChatPage::Status::Warn);
        break;
    }
}

void PhoneWindow::onWelcomed(const QString &device, bool voice, bool busy) {
    device_ = device;
    chat_->setStatus(tr("connected · %1").arg(device), ChatPage::Status::Ok);
    chat_->setVoiceAvailable(voice);
    chat_->setBusy(busy);
}

void PhoneWindow::onReceived(const remote::ServerMessage &message) {
    using Type = remote::ServerMessage::Type;
    switch (message.type) {
    case Type::Accepted:
        chat_->endReply();
        chat_->addMessage(message.source == u"pc"_s ? ChatPage::Kind::FromPc : ChatPage::Kind::User, message.text);
        break;
    case Type::Delta:
        chat_->appendReply(message.text);
        break;
    case Type::Step:
        chat_->addMessage(message.ok ? ChatPage::Kind::Step : ChatPage::Kind::StepFailed, message.text);
        break;
    case Type::Finished:
        chat_->endReply();
        break;
    case Type::Failed:
        chat_->endReply();
        chat_->addMessage(ChatPage::Kind::Error, message.text);
        break;
    case Type::Transcript:
        chat_->addMessage(ChatPage::Kind::Heard, u"“%1”"_s.arg(message.text));
        break;
    case Type::Speech:
        client_->fetch(message.url, [this](Result<QByteArray> wav) {
            if (wav) player_->play(wav.value());
        });
        break;
    case Type::Busy:
        chat_->setBusy(message.busy);
        break;
    case Type::Error:
        chat_->addMessage(ChatPage::Kind::Error, message.text);
        break;
    case Type::Welcome:
    case Type::Pong:
        break;
    }
}

void PhoneWindow::onUnauthorized(const QString &reason) {
    settings_->forgetToken();
    showPairPage(reason.isEmpty() ? tr("This phone is no longer paired. Pair it again.") : reason);
}

void PhoneWindow::onCertificateRejected(const QString &fingerprint) {
    chat_->addMessage(ChatPage::Kind::Error,
                      tr("Nexa's certificate is not the one you trusted (it shows %1). If Nexa was set up "
                         "again on the PC, forget this PC and pair once more; otherwise someone may be in "
                         "between.").arg(fingerprint));
}

void PhoneWindow::onAsk(const QString &text) {
    if (!client_->ask(text)) chat_->addMessage(ChatPage::Kind::Error, tr("Not connected to Nexa."));
}

void PhoneWindow::onTalkPressed() {
    wantTalk_ = true;
    if (recorder_->isRecording()) return;
#if QT_CONFIG(permissions)
    QMicrophonePermission microphone;
    switch (qApp->checkPermission(microphone)) {
    case Qt::PermissionStatus::Undetermined:
        qApp->requestPermission(microphone, this, [this](const QPermission &permission) {
            if (permission.status() == Qt::PermissionStatus::Granted) beginRecording();
            else chat_->addMessage(ChatPage::Kind::Error, tr("Without the microphone, Nexa cannot hear you."));
        });
        return;
    case Qt::PermissionStatus::Denied:
        chat_->addMessage(ChatPage::Kind::Error, tr("Allow the microphone for Nexa in the phone's settings."));
        return;
    case Qt::PermissionStatus::Granted:
        break;
    }
#endif
    beginRecording();
}

void PhoneWindow::beginRecording() {
    // The button was let go while the permission was being asked for.
    if (!wantTalk_) return;
    recorder_->start();
    if (recorder_->isRecording()) {
        chat_->setRecording(true);
        chat_->setStatus(tr("listening…"), ChatPage::Status::Recording);
    }
}

void PhoneWindow::onTalkReleased() {
    wantTalk_ = false;
    if (!recorder_->isRecording()) return;
    chat_->setRecording(false);
    onStateChanged(client_->state(), device_);
    recorder_->stop();
}

void PhoneWindow::onRecorded(const QByteArray &pcm) {
    if (!client_->askAloud(pcm)) chat_->addMessage(ChatPage::Kind::Error, tr("Not connected to Nexa."));
}

void PhoneWindow::forget() {
    client_->disconnectFromNexa();
    client_->setToken({});
    settings_->forgetToken();
    chat_->clear();
    showPairPage();
}

} // namespace nexa
