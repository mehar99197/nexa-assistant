#include "system/net/TailscaleWatcher.h"

#include "core/Logging.h"
#include "core/ai/ProgramLookup.h"

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

QString findTailscale() {
    if (const QString found = findProgram(u"tailscale"_s); !found.isEmpty()) return found;
#ifdef Q_OS_WIN
    QStringList dirs;
    for (const QString &root : {qEnvironmentVariable("ProgramFiles"), qEnvironmentVariable("ProgramFiles(x86)")}) {
        if (!root.isEmpty()) dirs.append(root + u"/Tailscale"_s);
    }
    return findProgramIn(u"tailscale"_s, dirs);
#else
    return {};
#endif
}

} // namespace

TailscaleWatcher::TailscaleWatcher(QObject *parent) : QObject(parent), program_(findTailscale()) {
    timer_.setInterval(kRefreshMs);
    connect(&timer_, &QTimer::timeout, this, &TailscaleWatcher::refresh);
}

TailscaleWatcher::~TailscaleWatcher() {
    if (login_) login_->kill();
}

void TailscaleWatcher::setWatched(bool watched) {
    if (watched == timer_.isActive()) return;
    if (watched) {
        timer_.start();
        refresh();
    } else {
        timer_.stop();
    }
}

void TailscaleWatcher::refresh() {
    if (!installed() || status_) return;
    auto *process = new QProcess(this);
    status_ = process;
    connect(process, &QProcess::finished, this, [this, process] {
        process->deleteLater();
        const tunnellog::TailscaleState read = tunnellog::parseTailscaleStatus(process->readAllStandardOutput());
        const bool different = read.signedIn != state_.signedIn || read.ipv4 != state_.ipv4
                               || read.dnsName != state_.dnsName || read.backend != state_.backend;
        state_ = read;
        if (different) {
            qCInfo(lcApp).noquote() << "tailscale:" << (state_.signedIn ? u"on the network as "_s + state_.dnsName
                                                                         : u"not signed in ("_s + state_.backend + u')');
            emit changed();
        }
    });
    QTimer::singleShot(5'000, process, [process] { process->kill(); });
    process->start(program_, {u"status"_s, u"--json"_s});
}

void TailscaleWatcher::signIn(const QString &hostname, std::function<void(Result<QString>)> done) {
    if (!installed()) {
#ifdef Q_OS_WIN
        done(Error{ErrorCode::NotFound, tr("Tailscale is not installed: https://tailscale.com/download/windows")});
#else
        done(Error{ErrorCode::NotFound, tr("Tailscale is not installed: https://tailscale.com/download/linux")});
#endif
        return;
    }
    if (state_.signedIn) {
        done(QString());
        return;
    }
    if (login_) login_->kill();
    auto *process = new QProcess(this);
    login_ = process;
    process->setProcessChannelMode(QProcess::MergedChannels);
    auto answered = std::make_shared<bool>(false);
    auto output = std::make_shared<QString>();
    connect(process, &QProcess::readyRead, this, [process, done, answered, output] {
        *output += QString::fromUtf8(process->readAll());
        if (*answered) return;
        if (const QString link = tunnellog::tailscaleLoginUrl(*output); !link.isEmpty()) {
            *answered = true;
            done(link);
        }
    });
    connect(process, &QProcess::finished, this, [this, process, done, answered, output](int code) {
        process->deleteLater();
        refresh();
        if (*answered) return;
        *answered = true;
        if (code == 0) {
            done(QString());
            return;
        }
        if (output->contains(u"Access denied"_s) || output->contains(u"prefs write access denied"_s)) {
#ifdef Q_OS_WIN
            done(Error{ErrorCode::PermissionDenied,
                       tr("Tailscale denied access; make sure the Tailscale Windows service is running and you are allowed to operate it.")});
#else
            done(Error{ErrorCode::PermissionDenied,
                       tr("Tailscale needs to be told once that you may run it: sudo tailscale set --operator=$USER")});
#endif
            return;
        }
        done(Error{ErrorCode::Unknown, output->trimmed().section(u'\n', 0, 0)});
    });
    // The owner has ten minutes to sign in; `tailscale up` waits until they have.
    QTimer::singleShot(600'000, process, [process] { process->kill(); });
    QStringList arguments{u"up"_s, u"--hostname=%1"_s.arg(hostname)};
#ifndef Q_OS_WIN
    // `tailscale up` resets every setting it is not given: the operator (the user allowed to run it) is given again,
    // or the next sign-in would need sudo once more.
    const QString user = qEnvironmentVariable("USER");
    if (!user.isEmpty()) arguments.append(u"--operator=%1"_s.arg(user));
#endif
    process->start(program_, arguments);
}

} // namespace nexa
