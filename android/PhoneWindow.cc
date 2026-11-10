#include "PhoneWindow.h"

#include "ChatPage.h"
#include "PairPage.h"
#include "PhoneSettings.h"
#include "Recorder.h"
#include "SpeechPlayer.h"
#include "core/Logging.h"

#include <QApplication>
#include <QMessageBox>
#include <QPermissions>
#include <QPushButton>
#include <QStackedWidget>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

CoreNodes nodesFromNames(const QStringList &names) {
    CoreNodes nodes = NoNode;
    for (const QString &name : names) {
        for (const CoreNode node : {VoiceNode, PcNode, ToolsNode, PhoneNode}) {
            if (coreNodeName(node).compare(name, Qt::CaseInsensitive) == 0) nodes |= node;
        }
    }
    return nodes;
}

} // namespace

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
    connect(chat_, &ChatPage::unlockRequested, client_, &RemoteClient::unlock);
    connect(chat_, &ChatPage::approvalAnswered, client_, &RemoteClient::answer);
    connect(chat_, &ChatPage::talkPressed, this, &PhoneWindow::onTalkPressed);
    connect(chat_, &ChatPage::talkReleased, this, &PhoneWindow::onTalkReleased);
    connect(chat_, &ChatPage::forgetRequested, this, &PhoneWindow::forget);

    connect(client_, &RemoteClient::stateChanged, this, &PhoneWindow::onStateChanged);
    connect(client_, &RemoteClient::welcomed, this, &PhoneWindow::onWelcomed);
    connect(client_, &RemoteClient::received, this, &PhoneWindow::onReceived);
    connect(client_, &RemoteClient::unauthorized, this, &PhoneWindow::onUnauthorized);
    connect(client_, &RemoteClient::certificateRejected, this, &PhoneWindow::onCertificateRejected);
    connect(client_, &RemoteClient::routesLearned, this, &PhoneWindow::onRoutesLearned);
    connect(client_, &RemoteClient::connectedVia, this, [this](const QString &via) {
        // At home (adb, the Wi-Fi) it is simply connected; away, it says how.
        const bool away = !via.isEmpty() && via != u"adb"_s && via != u"Wi-Fi"_s;
        chat_->setStatus(away ? tr("Your PC · connected · via %1").arg(via) : tr("Your PC · connected"), ChatPage::Status::Ok);
    });

    connect(recorder_, &Recorder::recorded, this, &PhoneWindow::onRecorded);
    connect(recorder_, &Recorder::stopped, this, &PhoneWindow::onRecordingStopped);
    connect(recorder_, &Recorder::level, chat_, &ChatPage::setLevel);
    connect(recorder_, &Recorder::failed, this, [this](const QString &reason) {
        tapRecording_ = false;
        chat_->setRecording(false);
        setLocalMode(std::nullopt);
        chat_->addMessage(ChatPage::Kind::Error, reason);
    });
    connect(player_, &SpeechPlayer::started, this, [this] { setLocalMode(CoreMode::Speaking); });
    connect(player_, &SpeechPlayer::finished, this, [this] {
        if (localMode_ == CoreMode::Speaking) setLocalMode(std::nullopt);
    });
    connect(player_, &SpeechPlayer::failed, this, [this](const QString &reason) {
        chat_->addMessage(ChatPage::Kind::Error, reason);
    });
}

void PhoneWindow::setScheme(ThemeScheme scheme) {
    chat_->setScheme(scheme);
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
    useLearnedRoutes();
    pages_->setCurrentWidget(chat_);
    client_->connectToNexa();
}

void PhoneWindow::useLearnedRoutes() {
    QList<RemoteEndpoint> ways;
    for (const RemoteRoute &route : settings_->routes()) {
        if (const std::optional<RemoteEndpoint> way = RemoteEndpoint::fromRoute(route)) ways.append(*way);
    }
    client_->setAlternates(ways);
}

void PhoneWindow::onRoutesLearned(const QList<RemoteRoute> &routes, const QString &fingerprint) {
    if (!routes.isEmpty()) settings_->setRoutes(routes);
    // Nexa's own certificate, for the direct ways (Tailscale, the Wi-Fi): taken only while connected over adb — the
    // phone's own loopback, which no one on a network or at a tunnel stands in — and only if none is trusted yet.
    if (!fingerprint.isEmpty() && settings_->fingerprint().isEmpty() && !client_->endpointInUse().tls) {
        settings_->setFingerprint(fingerprint);
        client_->setPinnedFingerprint(fingerprint);
        qWarning().noquote() << "nexa routes: took the PC's certificate over adb";
    }
    useLearnedRoutes();
}

void PhoneWindow::pairWith(const QString &address, const QString &code, const QString &name) {
    showPairPage();
    pair_->setAddress(address);
    if (!name.isEmpty()) pair_->setDeviceName(name);
    onPairRequested(address, code, pair_->deviceName());
}

void PhoneWindow::showPairPage(const QString &reason) {
    if (!reason.isEmpty()) qWarning().noquote() << "nexa pairing:" << reason;
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
    qWarning().noquote() << "nexa pairing: asking" << endpoint.url(u"/api/pair"_s);
    client_->pair(code, name, [this, endpoint](Result<QString> token) {
        if (!token) {
            showPairPage(token.error().message);
            return;
        }
        qWarning().noquote() << "nexa pairing: paired with" << endpoint.host;
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
        chat_->setStatus(tr("Your PC · connected"), ChatPage::Status::Ok);
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
    showCore();
}

void PhoneWindow::onWelcomed(const QString &device, bool voice, bool busy, const QString &mode) {
    device_ = device;
    // The phone's own name (as it was paired) says nothing here; the pill keeps to what the phone is connected to.
    chat_->setStatus(tr("Your PC · connected"), ChatPage::Status::Ok);
    chat_->setVoiceAvailable(voice);
    // The PC sends its lock and what waits right after the welcome: whatever was shown before is out of date.
    chat_->setLocked(false, {});
    chat_->clearApprovals();
    chat_->setBusy(busy);
    pcMode_ = coreModeFromKey(mode).value_or(busy ? CoreMode::Thinking : CoreMode::Idle);
    pcNodes_ = NoNode;
    showCore();
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
        if (localMode_ == CoreMode::Transcribing) setLocalMode(std::nullopt);
        break;
    case Type::Failed:
        chat_->endReply();
        chat_->addMessage(ChatPage::Kind::Error, message.text);
        if (localMode_ == CoreMode::Transcribing) setLocalMode(std::nullopt);
        break;
    case Type::Transcript:
        chat_->addMessage(ChatPage::Kind::Heard, u"“%1”"_s.arg(message.text));
        if (localMode_ == CoreMode::Transcribing) setLocalMode(std::nullopt);
        break;
    case Type::Speech:
        client_->fetch(message.url, [this](Result<QByteArray> wav) {
            if (wav) player_->play(wav.value());
        });
        break;
    case Type::Busy:
        chat_->setBusy(message.busy);
        break;
    case Type::State:
        pcMode_ = coreModeFromKey(message.mode).value_or(CoreMode::Idle);
        pcNodes_ = nodesFromNames(message.nodes);
        showCore();
        break;
    case Type::Error:
        chat_->addMessage(ChatPage::Kind::Error, message.text);
        if (localMode_ == CoreMode::Transcribing) setLocalMode(std::nullopt);
        break;
    case Type::Lock:
        chat_->setLocked(message.locked, message.text);
        break;
    case Type::Approval:
        chat_->addApproval(message.id, message.agent, message.tool, message.text);
        break;
    case Type::Settled:
        chat_->settleApproval(message.id);
        break;
    case Type::Welcome:
    case Type::Pong:
    case Type::Routes:      // the client says them (routesLearned)
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
    // A tap while a tapped recording runs sends it.
    if (recorder_->isRecording() && tapRecording_) {
        recorder_->stop();
        pressClock_.invalidate();
        return;
    }
    pressClock_.start();
    wantTalk_ = true;
    tapRecording_ = false;
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
    // The button was let go while the permission was being asked for — unless that was a tap.
    if (!wantTalk_ && !tapRecording_) return;
    player_->stop();        // talking over a reply cuts it, as on the PC
    recorder_->start();
    if (!recorder_->isRecording()) return;
    if (tapRecording_) recorder_->setStopOnSilence(true);
    chat_->setRecording(true);
    chat_->setStatus(tr("listening…"), ChatPage::Status::Recording);
    setLocalMode(CoreMode::Listening);
}

void PhoneWindow::onTalkReleased() {
    wantTalk_ = false;
    if (!pressClock_.isValid()) return;
    const bool tap = pressClock_.elapsed() < kHoldMs;
    pressClock_.invalidate();
    if (tap) {
        // Keep listening until the pause after the request.
        tapRecording_ = true;
        if (recorder_->isRecording()) recorder_->setStopOnSilence(true);
        showCore();
        return;
    }
    if (recorder_->isRecording()) recorder_->stop();
}

void PhoneWindow::onRecorded(const QByteArray &pcm) {
    if (!client_->askAloud(pcm)) {
        chat_->addMessage(ChatPage::Kind::Error, tr("Not connected to Nexa."));
        return;
    }
    setLocalMode(CoreMode::Transcribing);
}

void PhoneWindow::onRecordingStopped() {
    tapRecording_ = false;
    chat_->setRecording(false);
    if (localMode_ == CoreMode::Listening) setLocalMode(std::nullopt);
    onStateChanged(client_->state(), device_);
}

void PhoneWindow::forget() {
    client_->disconnectFromNexa();
    client_->setToken({});
    settings_->forgetToken();
    chat_->clear();
    showPairPage();
}

void PhoneWindow::setLocalMode(std::optional<CoreMode> mode) {
    localMode_ = mode;
    showCore();
}

void PhoneWindow::showCore() {
    const bool online = client_->isOnline();
    const CoreMode mode = !online ? CoreMode::Offline : localMode_.value_or(pcMode_);
    const CoreNodes nodes = online && !localMode_ ? pcNodes_ : NoNode;
    // The phone's own listening, recognising and reading aloud say what to do here; the PC's say it is the PC.
    const bool mine = localMode_.has_value();
    QString hint;
    switch (mode) {
    case CoreMode::Idle: hint = tr("Say a request, or type one."); break;
    case CoreMode::WakeWord: hint = tr("The PC is listening for its name."); break;
    case CoreMode::Listening:
        hint = !mine ? tr("The PC is listening.") : tapRecording_ ? tr("Tap again, or pause, to send.") : tr("Let go to send.");
        break;
    case CoreMode::Transcribing: hint = mine ? tr("Recognising what you said…") : tr("The PC is recognising what it heard…"); break;
    case CoreMode::Thinking: hint = tr("Nexa is writing a reply…"); break;
    case CoreMode::Acting: hint = tr("Running tools on the PC…"); break;
    case CoreMode::Speaking: hint = mine ? tr("Reading the reply aloud.") : tr("The PC is reading the reply aloud."); break;
    case CoreMode::FastPath: hint = tr("Done directly, without the model."); break;
    case CoreMode::Offline: hint = online ? tr("The PC cannot reach its AI provider.") : tr("Not connected to the PC."); break;
    }
    chat_->setCore(mode, nodes, hint);
}

} // namespace nexa
