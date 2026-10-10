// Nexa Phone — the composition root. Everything is built here and handed to the window.

#include "PhoneSettings.h"
#include "PhoneWindow.h"
#include "Recorder.h"
#include "SpeechPlayer.h"
#include "remote/RemoteClient.h"

#include <QApplication>

using namespace Qt::StringLiterals;

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(u"Nexa"_s);
    QCoreApplication::setApplicationName(u"Nexa Phone"_s);
    // Finger-sized controls; the rest follows the platform's style.
    app.setStyleSheet(u"QPushButton { min-height: 44px; padding: 0 16px; }"
                      "QLineEdit { min-height: 44px; padding: 0 8px; }"
                      "QToolButton { min-height: 44px; min-width: 44px; }"_s);

    nexa::PhoneSettings settings;
    nexa::RemoteClient client;
    nexa::Recorder recorder;
    nexa::SpeechPlayer player;
    nexa::PhoneWindow window(&settings, &client, &recorder, &player);
#ifdef Q_OS_ANDROID
    window.showMaximized();
#else
    window.resize(420, 800);
    window.show();
#endif
    window.start();
    return app.exec();
}
