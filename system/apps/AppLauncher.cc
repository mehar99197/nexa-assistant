#include "system/apps/AppLauncher.h"

#include "system/commands/LocalCommandRunner.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>

#include <csignal>
#include <unistd.h>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

QByteArray readProcFile(qint64 pid, const char *name) {
    QFile file(u"/proc/%1/%2"_s.arg(pid).arg(QLatin1StringView(name)));
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.read(4096);
}

Result<qint64> startDetached(const QString &program, const QStringList &arguments) {
    QProcess process;
    process.setProgram(program);
    process.setArguments(arguments);
    process.setWorkingDirectory(QDir::homePath());
    process.setProcessEnvironment(LocalCommandRunner::environment());
    process.setStandardInputFile(QProcess::nullDevice());
    process.setStandardOutputFile(QProcess::nullDevice());
    process.setStandardErrorFile(QProcess::nullDevice());
    process.setUnixProcessParameters(QProcess::UnixProcessFlag::CreateNewSession
                                     | QProcess::UnixProcessFlag::CloseFileDescriptors);
    qint64 pid = 0;
    if (!process.startDetached(&pid)) {
        return Error{ErrorCode::NotFound, u"Cannot start \"%1\": %2"_s.arg(program, process.errorString())};
    }
    return pid;
}

} // namespace

Result<QString> AppLauncher::launch(const DesktopEntry &entry) {
    Result<qint64> started = Error{};
    if (entry.dbusActivatable || entry.terminal) {
        started = startDetached(u"gio"_s, {u"launch"_s, entry.path});
    } else {
        const QStringList argv = DesktopEntry::execArguments(entry.exec);
        if (argv.isEmpty()) {
            return Error{ErrorCode::InvalidArgument, u"%1 has no usable Exec line"_s.arg(entry.name)};
        }
        started = startDetached(argv.first(), argv.mid(1));
    }
    if (!started) return started.error();
    return u"Launched %1 (%2)"_s.arg(entry.name, entry.id);
}

Result<QString> AppLauncher::close(const DesktopEntry &entry) {
    if (const QString appId = entry.flatpakAppId(); !appId.isEmpty()) {
        const Result<qint64> started = startDetached(u"flatpak"_s, {u"kill"_s, appId});
        if (!started) return started.error();
        return u"Asked %1 to quit"_s.arg(entry.name);
    }
    const QString executable = entry.executableName();
    if (executable.isEmpty()) return Error{ErrorCode::InvalidArgument, u"Cannot tell which program %1 runs"_s.arg(entry.name)};

    const QList<qint64> pids = findProcesses(executable);
    if (pids.isEmpty()) return Error{ErrorCode::NotFound, u"%1 is not running"_s.arg(entry.name)};
    for (const qint64 pid : pids) ::kill(pid_t(pid), SIGTERM);
    return u"Asked %1 to quit (%2 process%3)"_s.arg(entry.name).arg(pids.size()).arg(pids.size() == 1 ? u""_s : u"es"_s);
}

QList<qint64> AppLauncher::findProcesses(const QString &executable) {
    QList<qint64> pids;
    const uint uid = ::getuid();
    const qint64 self = QCoreApplication::applicationPid();
    // The kernel truncates process names (comm) to 15 characters, so for longer names comm is
    // ambiguous ("gnome-calculator" vs "gnome-calculator-search-provider"): match exactly or not at all.
    const QString comm = executable.size() <= 15 ? executable : QString();
    const QDir proc(u"/proc"_s);
    for (const QString &name : proc.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        bool numeric = false;
        const qint64 pid = name.toLongLong(&numeric);
        if (!numeric || pid == self || QFileInfo(proc.filePath(name)).ownerId() != uid) continue;

        const QString processName = QString::fromLocal8Bit(readProcFile(pid, "comm")).trimmed();
        const QString exe = QFileInfo(u"/proc/%1/exe"_s.arg(pid)).symLinkTarget().section(u'/', -1);
        const QString argv0 = QString::fromLocal8Bit(readProcFile(pid, "cmdline").split('\0').value(0)).section(u'/', -1);
        if ((!comm.isEmpty() && processName == comm) || exe == executable || argv0 == executable) pids.append(pid);
    }
    return pids;
}

} // namespace nexa
