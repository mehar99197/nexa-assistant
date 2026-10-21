// Nexa Phone — the composition root. Everything is built here and handed to the window.

#include "LaunchIntent.h"
#include "PhoneSettings.h"
#include "PhoneWindow.h"
#include "Recorder.h"
#include "SpeechPlayer.h"
#include "core/Logging.h"
#include "remote/RemoteClient.h"
#include "ui/NexaTheme.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QSslSocket>

using namespace Qt::StringLiterals;

// Started behind the lock screen (the PC's --pair-phone while the phone sleeps), the activity is
// paused at once and Qt for Android holds this function still, inside QApplication, until the user
// unlocks: nothing here has hung, it is waiting for the screen.
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(u"Nexa"_s);
    QCoreApplication::setApplicationName(u"Nexa Phone"_s);
    // One line in logcat that says whether OpenSSL is inside this build: Qt for Android has no TLS
    // without it, and then only http://localhost through adb can reach Nexa (plain qWarning: the
    // default category always reaches logcat, whatever the rules).
    qWarning().noquote() << "nexa tls:"
                         << (QSslSocket::supportsSsl() ? QSslSocket::sslLibraryVersionString()
                                                       : u"none — OpenSSL is missing from this build"_s);

    // On the desktop the PC's hand-over can be played by hand: --pair localhost:7777 --code 123456.
    // The phone has no command line; parse() never exits the process, which process() would do on
    // a stray argument — and on Android an exit() from Qt's thread hangs the app instead of ending it.
    QCommandLineParser parser;
    parser.addHelpOption();
    const QCommandLineOption pairOption(u"pair"_s, u"Pair with Nexa at <address> on start."_s, u"address"_s);
    const QCommandLineOption codeOption(u"code"_s, u"The six-digit pairing code Settings → Remote shows."_s, u"code"_s);
    const QCommandLineOption nameOption(u"name"_s, u"What the PC lists this phone as."_s, u"name"_s);
    parser.addOptions({pairOption, codeOption, nameOption});
#ifdef Q_OS_ANDROID
    parser.parse(app.arguments());
#else
    parser.process(app);
#endif

    // Nexa's own look, dark or light as the phone is set; then finger-sized controls.
    const nexa::ThemeScheme scheme = nexa::NexaTheme::schemeFor(u"system"_s);
    nexa::NexaTheme::apply(&app, scheme);
    app.setStyleSheet(app.styleSheet()
                      + u"\nQPushButton { min-height: 44px; padding: 0 16px; }"
                        "QLineEdit { min-height: 40px; }"
                        "QToolButton#moreButton { min-height: 44px; min-width: 44px; }"_s);

    nexa::PhoneSettings settings;
    nexa::RemoteClient client;
    nexa::Recorder recorder;
    nexa::SpeechPlayer player;
    nexa::PhoneWindow window(&settings, &client, &recorder, &player);
    window.setScheme(scheme);
#ifdef Q_OS_ANDROID
    window.showMaximized();
#else
    window.resize(420, 820);
    window.show();
#endif
    window.start();

    // Started by the PC over adb with the address and the code: pair now, no typing.
    nexa::LaunchIntent intent = nexa::LaunchIntent::fromActivity();
    if (parser.isSet(pairOption)) intent.address = parser.value(pairOption);
    if (parser.isSet(codeOption)) intent.code = parser.value(codeOption);
    if (parser.isSet(nameOption)) intent.name = parser.value(nameOption);
    // Plain qWarning: the default category always reaches logcat, whatever the rules.
    if (intent.isPairing()) {
        qWarning().noquote() << "nexa launch: pairing with" << intent.address << "as" << intent.name;
        window.pairWith(intent.address, intent.code, intent.name);
    } else {
        qWarning().noquote() << "nexa launch: nothing handed over" << (intent.address.isEmpty() ? "(no address)" : "(no code)");
    }
    return app.exec();
}
