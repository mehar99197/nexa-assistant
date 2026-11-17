#include "system/android/AdbClient.h"

#include "core/Logging.h"

#include <QProcess>
#include <QStandardPaths>
#include <QTimer>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

QString adbProgram() {
    static const QString program = [] {
        if (const QString found = QStandardPaths::findExecutable(u"adb"_s); !found.isEmpty()) return found;
#ifdef Q_OS_WIN
        QStringList dirs;
        if (const QString local = qEnvironmentVariable("LOCALAPPDATA"); !local.isEmpty()) {
            dirs.append(local + u"/Android/Sdk/platform-tools"_s);
        }
        dirs.append(QDir::homePath() + u"/AppData/Local/Android/Sdk/platform-tools"_s);
        dirs.append(u"C:/Android/platform-tools"_s);
        return QStandardPaths::findExecutable(u"adb"_s, dirs);
#else
        return QString();
#endif
    }();
    return program;
}

QString withoutTrailingNewline(const QByteArray &bytes) {
    QString text = QString::fromUtf8(bytes);
    while (text.endsWith(u'\n') || text.endsWith(u'\r')) text.chop(1);
    return text;
}

} // namespace

struct AdbClient::Job {
    Done done;
    // Owned by the process, not the job: the job can end inside the timer's own timeout signal,
    // and a QTimer must not be destroyed while it is emitting.
    QTimer *timer{nullptr};
};

QString AdbClient::Output::text() const {
    return withoutTrailingNewline(standardOutput);
}

QString AdbClient::Output::errorText() const {
    return withoutTrailingNewline(standardError);
}

AdbClient::AdbClient(QObject *parent) : QObject(parent) {}

AdbClient::~AdbClient() {
    for (QProcess *process : jobs_.keys()) {
        process->disconnect(this);
        process->kill();
        process->waitForFinished(500);
        process->deleteLater();
    }
}

bool AdbClient::isInstalled() {
    return !adbProgram().isEmpty();
}

void AdbClient::runOn(const QString &serial, const QStringList &arguments, int timeoutMs, Done done) {
    run(QStringList{u"-s"_s, serial} + arguments, timeoutMs, std::move(done));
}

QString AdbClient::program() const {
    return program_.isEmpty() ? adbProgram() : program_;
}

void AdbClient::run(const QStringList &arguments, int timeoutMs, Done done) {
    if (program().isEmpty()) {
#ifdef Q_OS_WIN
        done(Error{ErrorCode::NotSupported, u"adb is not installed. Install Android Platform Tools and add adb to PATH."_s});
#else
        done(Error{ErrorCode::NotSupported, u"adb is not installed. Install it with: sudo apt install adb"_s});
#endif
        return;
    }
    auto *process = new QProcess(this);
    auto job = std::make_shared<Job>();
    job->done = std::move(done);
    job->timer = new QTimer(process);
    jobs_.insert(process, job);

    job->timer->setSingleShot(true);
    connect(job->timer, &QTimer::timeout, this, [this, process, timeoutMs] {
        process->kill();
        finish(process, Error{ErrorCode::Timeout, u"The phone did not answer within %1 s."_s.arg(timeoutMs / 1000.0)});
    });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart) return;      // the others end in finished()
        finish(process, Error{ErrorCode::NotSupported, u"adb could not be started: %1"_s.arg(process->errorString())});
    });
    connect(process, &QProcess::finished, this, [this, process](int exitCode, QProcess::ExitStatus status) {
        Output output{process->readAllStandardOutput(), process->readAllStandardError(), exitCode};
        if (status == QProcess::CrashExit) {
            finish(process, Error{ErrorCode::Io, u"adb crashed: %1"_s.arg(output.errorText())});
            return;
        }
        finish(process, std::move(output));
    });

    // The pairing code is a secret, if a short-lived one: it stays out of the log.
    QStringList logged = arguments;
    if (logged.size() >= 3 && logged.first() == u"pair") logged.last() = u"******"_s;
    qCDebug(lcApp) << "adb" << logged;
    job->timer->start(timeoutMs);
    process->start(program(), arguments);
}

void AdbClient::cancelAll() {
    const QList<QProcess *> running = jobs_.keys();
    for (QProcess *process : running) {
        process->kill();
        finish(process, Error{ErrorCode::Cancelled, u"Cancelled"_s});
    }
}

void AdbClient::finish(QProcess *process, Result<Output> result) {
    const std::shared_ptr<Job> job = jobs_.take(process);
    if (!job) return;           // already finished: a timeout raced the exit
    job->timer->stop();
    job->timer->disconnect(this);
    process->disconnect(this);
    process->deleteLater();
    job->done(std::move(result));
}

std::optional<Error> AdbClient::errorFrom(const Output &output) {
    if (output.exitCode == 0) return std::nullopt;
    const QString message = output.errorText().isEmpty() ? output.text() : output.errorText();
    const QString lower = message.toLower();
    // Only adb's own messages; a command that failed on the phone speaks for itself.
    if (!lower.startsWith(u"adb:"_s) && !lower.startsWith(u"error:"_s)) return std::nullopt;

    if (lower.contains(u"no devices/emulators found"_s) || lower.contains(u"no devices found"_s)) {
        // NotSupported rather than NotFound: this is a dead end for the user to fix, not a name the
        // model should try again with.
        return Error{ErrorCode::NotSupported,
                     u"No phone is connected. Pair it in Settings → Android (Developer options → Wireless debugging), "
                     "or plug it in with USB debugging on."_s};
    }
    if (lower.contains(u"unauthorized"_s)) {
        return Error{ErrorCode::PermissionDenied,
                     u"The phone has not allowed this computer yet: unlock it and accept the debugging prompt."_s};
    }
    if (lower.contains(u"offline"_s)) {
        return Error{ErrorCode::Io, u"The phone is connected but offline — reconnect it (toggle Wireless debugging)."_s};
    }
    if (lower.contains(u"not found"_s)) {
        return Error{ErrorCode::NotSupported, u"The phone is no longer connected. %1"_s.arg(message)};
    }
    return Error{ErrorCode::Io, message};
}

} // namespace nexa
