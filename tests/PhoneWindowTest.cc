// android/PhoneWindow — the phone app's screens, driven the way a thumb would, against the real
// server on this machine: pairing by code, the conversation both ways, cancel, forgetting the PC,
// and trust on first use over TLS with the fingerprint dialog.

#include "ChatPage.h"
#include "PairPage.h"
#include "PhoneSettings.h"
#include "PhoneWindow.h"
#include "Recorder.h"
#include "SpeechPlayer.h"
#include "remote/IRemoteHost.h"
#include "remote/PairingManager.h"
#include "remote/RemoteClient.h"
#include "remote/RemoteServer.h"
#include "remote/TlsIdentity.h"
#include "ui/NexaTheme.h"

#include "QtStringMakers.h"

#include "remote/RemoteRoutes.h"
#include "ui/ElidedLabel.h"

#include <QAbstractButton>
#include <QApplication>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace Qt::StringLiterals;

namespace {

class FakeHost : public IRemoteHost {
public:
    bool isBusy() const override { return false; }
    void submit(const QString &text, const QString &source) override { submitted.append(source + u": "_s + text); }
    void cancel() override { ++cancels; }
    void transcribe(const QByteArray &, std::function<void(Result<QString>)> done) override {
        done(u"open the calculator"_s);
    }
    bool canTranscribe() const override { return true; }
    void renderSpeech(const QString &, std::function<void(Result<QByteArray>)> done) override {
        done(QByteArrayLiteral("RIFF-fake-wav"));
    }

    void unlock() override { ++unlocks; }
    void answer(quint64 id, bool allow) override { answers.append(u"%1:%2"_s.arg(id).arg(allow ? u"yes"_s : u"no"_s)); }

    QStringList submitted;
    int cancels{0};
    int unlocks{0};
    QStringList answers;
};

int freePortPair() {
    for (int attempt = 0; attempt < 50; ++attempt) {
        QTcpServer probe;
        if (!probe.listen(QHostAddress::LocalHost, 0)) continue;
        const int port = probe.serverPort();
        probe.close();
        QTcpServer first;
        QTcpServer second;
        if (port < 65000 && first.listen(QHostAddress::Any, quint16(port))
            && second.listen(QHostAddress::LocalHost, quint16(port + 1))) {
            return port;
        }
    }
    return 0;
}

struct Fixture {
    Fixture()
        : pairing(directory.filePath(u"devices.json"_s)), tls(directory.filePath(u"tls"_s)),
          server(&pairing, &host, &tls), settings(directory.filePath(u"phone.ini"_s)),
          window(&settings, &client, &recorder, &player) {
        // The look the app runs with, so the screens (and their pictures) are the real ones.
        NexaTheme::apply(qApp, ThemeScheme::Dark);
        window.setScheme(ThemeScheme::Dark);
        pages = window.findChild<QStackedWidget *>();
        pair = window.findChild<PairPage *>();
        chat = window.findChild<ChatPage *>();
        for (QLineEdit *field : pair->findChildren<QLineEdit *>()) {
            if (field->placeholderText().contains(u"7777"_s)) address = field;
            else if (field->placeholderText().contains(u"code"_s, Qt::CaseInsensitive)) code = field;
            else name = field;
        }
        pairButton = pair->findChild<QPushButton *>();
        messages = chat->findChild<QListWidget *>();
        input = chat->findChild<QLineEdit *>();
        status = chat->findChild<QLabel *>(u"statusPill"_s);
        send = chat->findChild<QAbstractButton *>(u"send"_s);
        talk = chat->findChild<QWidget *>(u"talk"_s);
    }

    bool start() {
        port = freePortPair();
        if (port == 0) return false;
        bool done = false;
        server.start(port, [&done](Result<QString>) { done = true; });
        return QTest::qWaitFor([&done] { return done; }, 30'000) && server.isRunning();
    }

    bool wired() const {
        return pages && pair && chat && address && code && name && pairButton && messages && input && status && send && talk;
    }
    QString pairError() const { return pair->findChildren<QLabel *>().last()->text(); }
    void clickPair() { QTest::mouseClick(pairButton, Qt::LeftButton); }
    bool onChat() const { return pages->currentWidget() == chat && client.isOnline(); }
    QStringList shown() const {
        QStringList lines;
        for (int i = 0; i < messages->count(); ++i) lines.append(messages->item(i)->text());
        return lines;
    }

    QTemporaryDir directory;
    PairingManager pairing;
    TlsIdentity tls;
    FakeHost host;
    RemoteServer server;
    PhoneSettings settings;
    RemoteClient client;
    Recorder recorder;
    SpeechPlayer player;
    PhoneWindow window;
    int port{0};

    QStackedWidget *pages{nullptr};
    PairPage *pair{nullptr};
    ChatPage *chat{nullptr};
    QLineEdit *address{nullptr};
    QLineEdit *code{nullptr};
    QLineEdit *name{nullptr};
    QPushButton *pairButton{nullptr};
    QListWidget *messages{nullptr};
    QLineEdit *input{nullptr};
    QLabel *status{nullptr};
    QAbstractButton *send{nullptr};
    QWidget *talk{nullptr};             // the Talk button is the Nexa core, not a QPushButton
};

bool haveOpenssl() {
    return !QStandardPaths::findExecutable(u"openssl"_s).isEmpty();
}

// NEXA_TEST_SCREENSHOTS=<dir>: keep a picture of each screen as the test saw it, to look at.
void snapshot(QWidget &widget, const QString &name) {
    const QString directory = qEnvironmentVariable("NEXA_TEST_SCREENSHOTS");
    if (directory.isEmpty()) return;
    QTest::qWait(50);   // a label that just changed is laid out again on the next turn of the loop
    widget.grab().save(directory + u'/' + name + u".png"_s);
}

} // namespace

TEST_CASE("The phone app pairs, chats, cancels and forgets, through adb's plain loopback") {
    if (!haveOpenssl()) SKIP("openssl is needed to make the certificate");
    Fixture f;
    REQUIRE(f.wired());
    REQUIRE(f.start());
    f.window.show();
    f.window.start();
    CHECK(f.pages->currentWidget() == f.pair);
    CHECK(f.name->text().startsWith(u"Nexa app on "_s));

    f.address->setText(u"not a host:port:junk"_s);
    f.code->setText(u"123456"_s);
    f.clickPair();
    CHECK(f.pairError().contains(u"not an address"_s));
    snapshot(f.window, u"phone-pair"_s);

    // No code is open on the PC: Nexa says no, and the phone stays unpaired.
    f.address->setText(u"127.0.0.1:%1"_s.arg(f.port + 1));
    f.clickPair();
    REQUIRE(QTest::qWaitFor([&f] { return !f.pairError().isEmpty(); }, 10'000));
    CHECK(f.pages->currentWidget() == f.pair);
    CHECK_FALSE(f.settings.isPaired());

    // The right code: paired under the name given, connected, and told Nexa can hear.
    f.pairing.startPairing();
    f.code->setText(f.pairing.code());
    f.name->setText(u"Test phone"_s);
    f.clickPair();
    REQUIRE(QTest::qWaitFor([&f] { return f.onChat(); }, 10'000));
    CHECK(f.settings.isPaired());
    CHECK(f.settings.token() == f.client.token());
    // Plain HTTP over adb shows no certificate; the PC's welcome hands over its fingerprint (for Tailscale's HTTPS),
    // taken because adb is the phone's own loopback.
    CHECK(QTest::qWaitFor([&f] { return f.settings.fingerprint() == f.server.fingerprint(); }, 5000));
    CHECK(static_cast<ElidedLabel *>(f.status)->fullText() == u"Your PC · connected"_s);
    REQUIRE(f.pairing.devices().size() == 1);
    CHECK(f.pairing.devices().first().name == u"Test phone"_s);
    CHECK(QTest::qWaitFor([&f] { return f.talk->isVisibleTo(f.chat); }, 5000));

    // Typed: reaches Nexa as the phone's, and the line empties.
    QTest::keyClicks(f.input, u"open firefox"_s);
    QTest::keyClick(f.input, Qt::Key_Return);
    CHECK(QTest::qWaitFor([&f] { return f.host.submitted == QStringList{u"phone: open firefox"_s}; }, 5000));
    CHECK(f.input->text().isEmpty());

    // The conversation, whoever started it, streamed into the list.
    f.server.onUserMessage(u"volume 30"_s);
    f.server.onReplyStarted(u"gui"_s);
    f.server.onReplyDelta(u"Do"_s);
    f.server.onReplyDelta(u"ne."_s);
    f.server.onReplyStep(u"set_volume — 30% · 3 ms"_s, true);
    f.server.onReplyFinished(u"stop"_s);
    REQUIRE(QTest::qWaitFor([&f] { return f.messages->count() >= 3; }, 5000));
    CHECK(f.shown() == QStringList{u"volume 30"_s, u"Done."_s, u"set_volume — 30% · 3 ms"_s});

    // A failure is a line of its own, and does not run into the next reply.
    f.server.onReplyStarted(u"gui"_s);
    f.server.onReplyFailed(u"The model is not answering."_s);
    REQUIRE(QTest::qWaitFor([&f] { return f.messages->count() >= 4; }, 5000));
    CHECK(f.shown().last() == u"The model is not answering."_s);
    f.server.onReplyStarted(u"gui"_s);
    f.server.onReplyDelta(u"Again."_s);
    REQUIRE(QTest::qWaitFor([&f] { return f.messages->count() >= 5; }, 5000));
    CHECK(f.shown().last() == u"Again."_s);
    f.server.onReplyFinished(u"stop"_s);

    // Busy: the send button becomes Stop, and Stop cancels.
    f.server.onBusyChanged(true);
    REQUIRE(QTest::qWaitFor([&f] { return f.send->property("busy").toBool(); }, 5000));
    CHECK(f.send->toolTip() == u"Stop"_s);
    snapshot(f.window, u"phone-chat"_s);
    QTest::mouseClick(f.send, Qt::LeftButton);
    CHECK(QTest::qWaitFor([&f] { return f.host.cancels == 1; }, 5000));
    f.server.onBusyChanged(false);
    CHECK(QTest::qWaitFor([&f] { return !f.send->property("busy").toBool(); }, 5000));

    // The core on the phone shows the PC's state, by name as well as by colour.
    f.server.onCoreChanged(u"act"_s, {u"pc"_s, u"tools"_s});
    QLabel *stateTitle = f.chat->findChild<QLabel *>(u"stateTitle"_s);
    REQUIRE(stateTitle);
    CHECK(QTest::qWaitFor([stateTitle] { return stateTitle->text() == u"Acting"_s; }, 5000));
    f.server.onCoreChanged(u"idle"_s, {});
    CHECK(QTest::qWaitFor([stateTitle] { return stateTitle->text() == u"Ready"_s; }, 5000));

    // Forgetting the PC: unpaired here, gone from Nexa's count, back to the first screen.
    emit f.chat->forgetRequested();
    CHECK(f.pages->currentWidget() == f.pair);
    CHECK_FALSE(f.settings.isPaired());
    CHECK(f.messages->count() == 0);
    CHECK(QTest::qWaitFor([&f] { return f.server.clientCount() == 0; }, 5000));
}

TEST_CASE("Handed the address and the code by the PC, the phone app pairs by itself") {
    if (!haveOpenssl()) SKIP("openssl is needed to make the certificate");
    Fixture f;
    REQUIRE(f.wired());
    REQUIRE(f.start());
    f.window.show();
    f.window.start();
    CHECK(f.pages->currentWidget() == f.pair);

    // What app/PhoneLink puts in the launch intent: the adb address, the code, a name.
    f.pairing.startPairing();
    f.window.pairWith(u"localhost:%1"_s.arg(f.port + 1), f.pairing.code(), u"Nexa app on Test phone"_s);
    REQUIRE(QTest::qWaitFor([&f] { return f.onChat(); }, 10'000));
    CHECK(f.settings.isPaired());
    REQUIRE(f.pairing.devices().size() == 1);
    CHECK(f.pairing.devices().first().name == u"Nexa app on Test phone"_s);
    CHECK(f.pairing.code().isEmpty());       // spent

    // A wrong code leaves it on the pairing page, saying so.
    f.window.pairWith(u"localhost:%1"_s.arg(f.port + 1), u"000000"_s, {});
    REQUIRE(QTest::qWaitFor([&f] { return !f.pairError().isEmpty(); }, 10'000));
    CHECK(f.pages->currentWidget() == f.pair);
}

TEST_CASE("Over the network the phone app shows Nexa's fingerprint and trusts it once") {
    if (!haveOpenssl()) SKIP("openssl is needed to make the certificate");
    const QString lan = RemoteServer::lanAddress();
    if (lan.isEmpty()) SKIP("this machine has no network address to reach itself by");
    Fixture f;
    REQUIRE(f.wired());
    REQUIRE(f.start());
    f.window.show();
    f.window.start();
    f.pairing.startPairing();
    f.address->setText(u"%1:%2"_s.arg(lan).arg(f.port));
    f.code->setText(f.pairing.code());

    // The dialog is modal: a timer plays the user, who compares the fingerprints and trusts.
    QString shown;
    int dialogs = 0;
    QTimer user;
    user.setInterval(50);
    QObject::connect(&user, &QTimer::timeout, [&] {
        auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        if (!box) return;
        ++dialogs;
        shown = box->informativeText();
        for (QAbstractButton *button : box->buttons()) {
            if (box->buttonRole(button) == QMessageBox::AcceptRole) {
                button->click();
                return;
            }
        }
    });
    user.start();
    f.clickPair();
    REQUIRE(QTest::qWaitFor([&f] { return f.onChat(); }, 20'000));
    CHECK(dialogs == 1);
    CHECK(shown.contains(f.server.fingerprint()));
    CHECK(f.settings.fingerprint() == f.server.fingerprint());
    CHECK(f.settings.endpoint().tls);
    CHECK(f.settings.endpoint().host == lan);
    CHECK(f.settings.endpoint().port == f.port);

    // Next time the certificate is known: straight in, no dialog.
    f.client.disconnectFromNexa();
    CHECK(QTest::qWaitFor([&f] { return f.server.clientCount() == 0; }, 5000));
    f.window.start();
    REQUIRE(QTest::qWaitFor([&f] { return f.onChat(); }, 10'000));
    CHECK(dialogs == 1);
    user.stop();
}

TEST_CASE("The phone app shows the PC locked, unlocks it, and answers the question a task waits on") {
    if (!haveOpenssl()) SKIP("openssl is needed to make the certificate");
    Fixture f;
    REQUIRE(f.wired());
    REQUIRE(f.start());
    f.window.show();
    f.window.start();
    f.address->setText(u"127.0.0.1:%1"_s.arg(f.port + 1));
    f.pairing.startPairing();
    f.code->setText(f.pairing.code());
    f.clickPair();
    REQUIRE(QTest::qWaitFor([&f] { return f.onChat(); }, 10'000));

    QFrame *bar = f.chat->findChild<QFrame *>(u"lockBar"_s);
    QPushButton *unlock = f.chat->findChild<QPushButton *>(u"unlockButton"_s);
    REQUIRE(bar);
    REQUIRE(unlock);
    CHECK_FALSE(bar->isVisibleTo(f.chat));
    f.server.onLockChanged(true, u"Keyboard and mouse locked until “sort the photos” is done."_s);
    REQUIRE(QTest::qWaitFor([bar, &f] { return bar->isVisibleTo(f.chat); }, 5000));
    CHECK(f.chat->findChild<QLabel *>(u"lockText"_s)->text().contains(u"sort the photos"_s));

    f.server.onApprovalAsked(7, u"CODE-02"_s, u"run_command"_s, u"Run this command? rm -r ~/Downloads/duplicates"_s);
    REQUIRE(QTest::qWaitFor([&f] { return f.chat->findChild<QPushButton *>(u"deny"_s) != nullptr; }, 5000));
    snapshot(f.window, u"phone-hardcore"_s);

    QTest::mouseClick(f.chat->findChild<QPushButton *>(u"deny"_s), Qt::LeftButton);
    CHECK(QTest::qWaitFor([&f] { return f.host.answers == QStringList{u"7:no"_s}; }, 5000));
    f.server.onApprovalSettled(7, false);
    CHECK(QTest::qWaitFor([&f] { return f.chat->findChild<QPushButton *>(u"deny"_s) == nullptr; }, 5000));

    QTest::mouseClick(unlock, Qt::LeftButton);
    CHECK(QTest::qWaitFor([&f] { return f.host.unlocks == 1; }, 5000));
    f.server.onLockChanged(false, {});
    CHECK(QTest::qWaitFor([bar, &f] { return !bar->isVisibleTo(f.chat); }, 5000));
}

TEST_CASE("On a phone 360 pixels wide every control stays on the screen") {
    if (!haveOpenssl()) SKIP("openssl is needed to make the certificate");
    Fixture f;
    REQUIRE(f.wired());
    REQUIRE(f.start());
    // The OnePlus Nord N100: 720 × 1600 at density 320, so 360 × 800 for the layout.
    f.window.resize(360, 800);
    f.window.show();
    f.window.start();
    f.address->setText(u"127.0.0.1:%1"_s.arg(f.port + 1));
    f.pairing.startPairing();
    f.code->setText(f.pairing.code());
    f.name->setText(u"Nexa app on BE2012"_s);
    f.clickPair();
    REQUIRE(QTest::qWaitFor([&f] { return f.onChat(); }, 10'000));
    QTest::qWait(100);

    CHECK(f.chat->minimumSizeHint().width() <= 360);
    CHECK(f.window.width() == 360);
    const auto onScreen = [&f](QWidget *widget) {
        const QRect box(widget->mapTo(&f.window, QPoint(0, 0)), widget->size());
        INFO(widget->objectName().toStdString() << " at " << box.x() << "," << box.y() << " " << box.width() << "x"
                                                << box.height());
        return QRect(0, 0, f.window.width(), f.window.height()).contains(box);
    };
    CHECK(onScreen(f.send));
    CHECK(onScreen(f.input));
    CHECK(onScreen(f.status));
    CHECK(onScreen(f.chat->findChild<QWidget *>(u"moreButton"_s)));
    snapshot(f.window, u"phone-360"_s);

    // Locked by hardcore mode, with a question: the bar and the card fit too.
    f.server.onLockChanged(true, u"Keyboard and mouse locked until “sort the photos in Downloads by year” is done."_s);
    f.server.onApprovalAsked(7, u"CODE-02"_s, u"run_command"_s, u"Run this command? rm -r ~/Downloads/duplicates"_s);
    REQUIRE(QTest::qWaitFor([&f] { return f.chat->findChild<QPushButton *>(u"deny"_s) != nullptr; }, 5000));
    QTest::qWait(100);
    CHECK(f.chat->minimumSizeHint().width() <= 360);
    CHECK(onScreen(f.chat->findChild<QPushButton *>(u"unlockButton"_s)));
    CHECK(onScreen(f.chat->findChild<QPushButton *>(u"deny"_s)));
    snapshot(f.window, u"phone-360-locked"_s);
}

TEST_CASE("The pairing page fits a phone 360 pixels wide") {
    Fixture f;
    REQUIRE(f.wired());
    f.window.resize(360, 800);
    f.window.show();
    f.window.start();
    QTest::qWait(100);
    CHECK(f.pair->minimumSizeHint().width() <= 360);
    CHECK(f.window.width() == 360);
    const QRect button(f.pairButton->mapTo(&f.window, QPoint(0, 0)), f.pairButton->size());
    CHECK(QRect(0, 0, 360, 800).contains(button));
    snapshot(f.window, u"phone-360-pair"_s);
}

TEST_CASE("With the keyboard up, the page is short and the core moves beside its title instead of over it") {
    if (!haveOpenssl()) SKIP("openssl is needed to make the certificate");
    Fixture f;
    REQUIRE(f.wired());
    REQUIRE(f.start());
    f.window.resize(360, 800);
    f.window.show();
    f.window.start();
    f.address->setText(u"127.0.0.1:%1"_s.arg(f.port + 1));
    f.pairing.startPairing();
    f.code->setText(f.pairing.code());
    f.clickPair();
    REQUIRE(QTest::qWaitFor([&f] { return f.onChat(); }, 10'000));
    // The OnePlus Nord N100 with its keyboard up: about 360 × 450 left for the app. Android fixes the window's size
    // (an offscreen window would grow back to its layout's minimum instead), so the test does too.
    f.window.setFixedSize(360, 450);
    // Laid out again once the resize has gone through (the presence turns into a row on the next turn of the loop).
    const auto sendInside = [&f] {
        return QRect(0, 0, 360, 450).contains(QRect(f.send->mapTo(&f.window, QPoint(0, 0)), f.send->size()));
    };
    const bool settled = QTest::qWaitFor([&f, &sendInside] { return f.window.height() == 450 && sendInside(); }, 5000);
    INFO("window " << f.window.width() << "x" << f.window.height() << ", smallest " << f.window.minimumSizeHint().width()
                   << "x" << f.window.minimumSizeHint().height() << ", chat smallest " << f.chat->minimumSizeHint().height()
                   << ", messages " << f.messages->count());
    REQUIRE(settled);
    auto *core = f.chat->findChild<QWidget *>(u"core"_s);
    auto *title = f.chat->findChild<QLabel *>(u"stateTitle"_s);
    REQUIRE(core);
    REQUIRE(title);
    const QRect coreBox(core->mapTo(f.chat, QPoint(0, 0)), core->size());
    const QRect titleBox(title->mapTo(f.chat, QPoint(0, 0)), title->size());
    CHECK_FALSE(coreBox.intersects(titleBox));
    CHECK(core->height() <= 80);
    CHECK(f.chat->minimumSizeHint().height() <= 450);
    CHECK(sendInside());
    snapshot(f.window, u"phone-360-keyboard"_s);
    // The keyboard goes: the large presence comes back while there is no conversation.
    f.window.setFixedSize(360, 800);
    CHECK(QTest::qWaitFor([core] { return core->height() >= 150; }, 5000));
}

TEST_CASE("Connected over adb, the phone app keeps every way to the PC and its certificate, and says the way it uses") {
    if (!haveOpenssl()) SKIP("openssl is needed to make the certificate");
    Fixture f;
    REQUIRE(f.wired());
    REQUIRE(f.start());
    f.server.setRoutes({{RemoteRoute::Kind::Tailscale, u"https://100.101.102.103:%1"_s.arg(f.port)},
                        {RemoteRoute::Kind::Cloudflare, u"https://nexa.nexacodesolutions.online"_s}});
    f.window.show();
    f.window.start();
    f.address->setText(u"127.0.0.1:%1"_s.arg(f.port + 1));      // over adb: plain, on the phone's own loopback
    f.pairing.startPairing();
    f.code->setText(f.pairing.code());
    f.clickPair();
    REQUIRE(QTest::qWaitFor([&f] { return f.onChat(); }, 10'000));
    REQUIRE(QTest::qWaitFor([&f] { return f.settings.routes().size() == 2; }, 5000));
    const QList<RemoteRoute> kept = f.settings.routes();
    CHECK(std::any_of(kept.cbegin(), kept.cend(), [](const RemoteRoute &r) { return r.kind == RemoteRoute::Kind::Tailscale; }));
    CHECK(std::any_of(kept.cbegin(), kept.cend(), [](const RemoteRoute &r) { return r.kind == RemoteRoute::Kind::Cloudflare; }));
    CHECK(f.settings.fingerprint() == f.server.fingerprint());     // taken over adb, for Tailscale's HTTPS
    CHECK(static_cast<ElidedLabel *>(f.status)->fullText() == u"Your PC · connected"_s);

    // Away, through Tailscale: the pill says so.
    emit f.client.connectedVia(u"Tailscale"_s);
    CHECK(static_cast<ElidedLabel *>(f.status)->fullText() == u"Your PC · connected · via Tailscale"_s);
}
