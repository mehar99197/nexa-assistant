#include "app/UnixSignals.h"

#include "core/Logging.h"

#include <QSocketNotifier>

#ifdef Q_OS_UNIX
#include <array>
#include <csignal>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace nexa {

#ifdef Q_OS_UNIX
namespace {

constexpr std::array kSignals{SIGTERM, SIGINT, SIGHUP};

} // namespace

UnixSignals::UnixSignals(QObject *parent) : QObject(parent) {
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds_) != 0) {
        qCWarning(lcApp) << "Cannot watch Unix signals: socketpair failed";
        return;
    }
    notifier_ = new QSocketNotifier(fds_[1], QSocketNotifier::Read, this);
    connect(notifier_, &QSocketNotifier::activated, this, [this] {
        unsigned char signal = 0;
        if (::read(fds_[1], &signal, 1) == 1) emit received(signal);
    });

    struct sigaction action {};
    action.sa_handler = &UnixSignals::handle;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    for (const int signal : kSignals) ::sigaction(signal, &action, nullptr);
}

UnixSignals::~UnixSignals() {
    for (const int signal : kSignals) ::signal(signal, SIG_DFL);
    for (int &fd : fds_) {
        if (fd >= 0) ::close(fd);
        fd = -1;
    }
}

void UnixSignals::handle(int signal) {
    // Async-signal-safe: only write(2) here; the notifier does the rest on the GUI thread.
    const unsigned char byte = static_cast<unsigned char>(signal);
    [[maybe_unused]] const ssize_t written = ::write(fds_[0], &byte, 1);
}
#else
UnixSignals::UnixSignals(QObject *parent) : QObject(parent) {}
UnixSignals::~UnixSignals() = default;
void UnixSignals::handle(int) {}
#endif

} // namespace nexa
