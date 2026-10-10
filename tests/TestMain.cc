#include <QApplication>

#include <catch2/catch_session.hpp>

// Network tests need an event loop and the phone app's screens need widgets, so the Catch2 session
// runs inside a QApplication — on the offscreen platform unless one was chosen, so the tests run
// the same on a desktop and in a headless job.
int main(int argc, char *argv[]) {
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    return Catch::Session().run(argc, argv);
}
