#include <QApplication>

#include <catch2/catch_session.hpp>

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>

#include <memory>
#include <vector>
#endif

// Network tests need an event loop and the phone app's screens need widgets, so the Catch2 session
// runs inside a QApplication — on the offscreen platform unless one was chosen, so the tests run
// the same on a desktop and in a headless job.
int main(int argc, char *argv[]) {
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
#ifdef Q_OS_WIN
    // Windows CRT decodes argv using the active ANSI codepage, corrupting UTF-8 test names
    // containing characters like "→" or "—". Recover the exact UTF-16 command line as UTF-8.
    struct LocalFreeDeleter {
        void operator()(LPWSTR *ptr) const noexcept {
            if (ptr) ::LocalFree(ptr);
        }
    };
    int wideArgc = 0;
    const std::unique_ptr<LPWSTR[], LocalFreeDeleter> wideArgv(
        ::CommandLineToArgvW(::GetCommandLineW(), &wideArgc));
    std::vector<QByteArray> utf8Storage;
    std::vector<char *> utf8Argv;
    if (wideArgv && wideArgc > 0) {
        utf8Storage.reserve(static_cast<size_t>(wideArgc));
        utf8Argv.reserve(static_cast<size_t>(wideArgc) + 1);
        for (int i = 0; i < wideArgc; ++i) {
            utf8Storage.push_back(QString::fromWCharArray(wideArgv[i]).toUtf8());
            utf8Argv.push_back(utf8Storage.back().data());
        }
        utf8Argv.push_back(nullptr);
        argc = wideArgc;
        argv = utf8Argv.data();
    }
#endif
    QApplication app(argc, argv);
    return Catch::Session().run(argc, argv);
}
