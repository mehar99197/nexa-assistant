// Nexa Phone — the composition root. Everything is built here and handed to the window.

#include "PhoneSettings.h"
#include "PhoneWindow.h"
#include "Recorder.h"
#include "SpeechPlayer.h"
#include "remote/RemoteClient.h"
#include "ui/NexaTheme.h"

#include <QApplication>

using namespace Qt::StringLiterals;

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(u"Nexa"_s);
    QCoreApplication::setApplicationName(u"Nexa Phone"_s);
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
    return app.exec();
}
