#include "system/apps/AppLauncher.h"

#include "core/ProcessUtils.h"
#include "system/commands/LocalCommandRunner.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>

#ifdef Q_OS_UNIX
#include <csignal>
#include <unistd.h>
#elif defined(Q_OS_WIN)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <shellapi.h>
#endif

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

#ifdef Q_OS_UNIX
QByteArray readProcFile(qint64 pid, const char *name) {
    QFile file(u"/proc/%1/%2"_s.arg(pid).arg(QLatin1StringView(name)));
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.read(4096);
}
#endif

Result<qint64> startDetached(const QString &program, const QStringList &arguments) {
    QProcess process;
    process.setProgram(program);
    process.setArguments(arguments);
    process.setWorkingDirectory(QDir::homePath());
    process.setProcessEnvironment(LocalCommandRunner::environment());
    process.setStandardInputFile(QProcess::nullDevice());
    process.setStandardOutputFile(QProcess::nullDevice());
    process.setStandardErrorFile(QProcess::nullDevice());
    process::configureChild(process);
    qint64 pid = 0;
    if (!process.startDetached(&pid)) {
        return Error{ErrorCode::NotFound, u"Cannot start \"%1\": %2"_s.arg(program, process.errorString())};
    }
    return pid;
}

#ifdef Q_OS_WIN
struct CloseEnumState {
    DWORD pid{0};
    int sent{0};
};

BOOL CALLBACK closeWindowForPid(HWND hwnd, LPARAM lParam) {
    auto *state = reinterpret_cast<CloseEnumState *>(lParam);
    DWORD windowPid = 0;
    ::GetWindowThreadProcessId(hwnd, &windowPid);
    if (windowPid == state->pid && ::IsWindowVisible(hwnd)) {
        if (::PostMessageW(hwnd, WM_CLOSE, 0, 0)) ++state->sent;
    }
    return TRUE;
}
#endif

} // namespace

Result<QString> AppLauncher::launch(const DesktopEntry &entry) {
#ifdef Q_OS_WIN
    if (entry.path.endsWith(u".lnk"_s, Qt::CaseInsensitive) && QFileInfo::exists(entry.path)) {
        const QString native = QDir::toNativeSeparators(entry.path);
        const auto res = reinterpret_cast<qintptr>(
            ::ShellExecuteW(nullptr, L"open", reinterpret_cast<LPCWSTR>(native.utf16()), nullptr, nullptr, SW_SHOWNORMAL));
        if (res <= 32) {
            return Error{ErrorCode::Io, u"Cannot open shortcut %1 (Win32 error %2)"_s.arg(entry.name).arg(res)};
        }
        return u"Launched %1 (%2)"_s.arg(entry.name, entry.id);
    }
    const QStringList argv = DesktopEntry::execArguments(entry.exec);
    if (argv.isEmpty()) {
        return Error{ErrorCode::InvalidArgument, u"%1 has no usable Exec line"_s.arg(entry.name)};
    }
    const Result<qint64> started = startDetached(argv.first(), argv.mid(1));
    if (!started) return started.error();
    return u"Launched %1 (%2)"_s.arg(entry.name, entry.id);
#else
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
#endif
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
#ifdef Q_OS_WIN
    for (const qint64 pid : pids) {
        CloseEnumState state{static_cast<DWORD>(pid), 0};
        ::EnumWindows(&closeWindowForPid, reinterpret_cast<LPARAM>(&state));
        if (state.sent == 0) {
            HANDLE h = ::OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
            if (h) {
                ::TerminateProcess(h, 0);
                ::CloseHandle(h);
            }
        }
    }
#else
    for (const qint64 pid : pids) ::kill(pid_t(pid), SIGTERM);
#endif
    return u"Asked %1 to quit (%2 process%3)"_s.arg(entry.name).arg(pids.size()).arg(pids.size() == 1 ? u""_s : u"es"_s);
}

QList<qint64> AppLauncher::findProcesses(const QString &executable) {
    QList<qint64> pids;
    const qint64 self = QCoreApplication::applicationPid();
#ifdef Q_OS_UNIX
    const uint uid = ::getuid();
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
        const QString argv0 = QFileInfo(QString::fromLocal8Bit(readProcFile(pid, "cmdline").split('\0').value(0))).fileName();
        if ((!comm.isEmpty() && processName == comm) || exe == executable || argv0 == executable) pids.append(pid);
    }
#elif defined(Q_OS_WIN)
    const QString targetName = executable.endsWith(u".exe"_s, Qt::CaseInsensitive) ? executable : executable + u".exe"_s;
    HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (::Process32FirstW(snap, &entry)) {
            do {
                const qint64 pid = static_cast<qint64>(entry.th32ProcessID);
                if (pid <= 0 || pid == self) continue;
                const QString exe = QString::fromWCharArray(entry.szExeFile);
                if (exe.compare(executable, Qt::CaseInsensitive) == 0
                    || exe.compare(targetName, Qt::CaseInsensitive) == 0) {
                    pids.append(pid);
                }
            } while (::Process32NextW(snap, &entry));
        }
        ::CloseHandle(snap);
    }
#else
    Q_UNUSED(executable);
    Q_UNUSED(self);
#endif
    return pids;
}

} // namespace nexa
