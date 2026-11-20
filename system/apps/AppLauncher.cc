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
#include <objbase.h>
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
#ifndef Q_OS_WIN
    process::configureChild(process);
#endif
    qint64 pid = 0;
    if (!process.startDetached(&pid)) {
        return Error{ErrorCode::NotFound, u"Cannot start \"%1\": %2"_s.arg(program, process.errorString())};
    }
    return pid;
}

#ifdef Q_OS_WIN
struct WinComScope {
    HRESULT hr{E_FAIL};
    WinComScope() : hr(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)) {}
    ~WinComScope() {
        if (hr == S_OK || hr == S_FALSE) ::CoUninitialize();
    }
};

struct CloseEnumState {
    DWORD pid{0};
    int sent{0};
    bool explorerOnly{false};
};

BOOL CALLBACK closeWindowForPid(HWND hwnd, LPARAM lParam) {
    auto *state = reinterpret_cast<CloseEnumState *>(lParam);
    DWORD windowPid = 0;
    ::GetWindowThreadProcessId(hwnd, &windowPid);
    if (windowPid == state->pid && ::IsWindowVisible(hwnd)) {
        if (state->explorerOnly) {
            wchar_t cls[64]{};
            ::GetClassNameW(hwnd, cls, 64);
            if (QString::fromWCharArray(cls).compare(u"CabinetWClass"_s, Qt::CaseInsensitive) != 0) {
                return TRUE;
            }
        }
        if (::PostMessageW(hwnd, WM_CLOSE, 0, 0)) ++state->sent;
    }
    return TRUE;
}

struct CloseTitleState {
    QString title;
    DWORD selfPid{0};
    int sent{0};
};

BOOL CALLBACK closeWindowByTitle(HWND hwnd, LPARAM lParam) {
    auto *state = reinterpret_cast<CloseTitleState *>(lParam);
    if (!::IsWindowVisible(hwnd)) return TRUE;
    DWORD pid = 0;
    ::GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0 || pid == state->selfPid) return TRUE;
    wchar_t cls[64]{};
    ::GetClassNameW(hwnd, cls, 64);
    const QString className = QString::fromWCharArray(cls);
    if (className == u"Progman"_s || className == u"WorkerW"_s || className == u"Shell_TrayWnd"_s
        || className == u"Shell_SecondaryTrayWnd"_s) {
        return TRUE;
    }
    const int len = ::GetWindowTextLengthW(hwnd);
    if (len <= 0 || len > 512) return TRUE;
    QString text(len, Qt::Uninitialized);
    const int copied = ::GetWindowTextW(hwnd, reinterpret_cast<LPWSTR>(text.data()), len + 1);
    text.truncate(copied);
    const QString trimmed = text.trimmed();
    if (trimmed.compare(state->title, Qt::CaseInsensitive) == 0
        || trimmed.endsWith(u" - "_s + state->title, Qt::CaseInsensitive)
        || trimmed.endsWith(u" \u2014 "_s + state->title, Qt::CaseInsensitive)) {
        if (::PostMessageW(hwnd, WM_CLOSE, 0, 0)) ++state->sent;
    }
    return TRUE;
}
#endif

} // namespace

Result<QString> AppLauncher::launch(const DesktopEntry &entry) {
#ifdef Q_OS_WIN
    ::AllowSetForegroundWindow(ASFW_ANY);
    WinComScope com;
    if (entry.path.startsWith(u"shell:AppsFolder\\"_s, Qt::CaseInsensitive)) {
        qintptr res = reinterpret_cast<qintptr>(::ShellExecuteW(
            nullptr, L"open", reinterpret_cast<LPCWSTR>(entry.path.utf16()), nullptr, nullptr, SW_SHOWNORMAL));
        if (res <= 32) {
            res = reinterpret_cast<qintptr>(::ShellExecuteW(
                nullptr, L"open", L"explorer.exe", reinterpret_cast<LPCWSTR>(entry.path.utf16()), nullptr, SW_SHOWNORMAL));
        }
        if (res <= 32) {
            return Error{ErrorCode::Io, u"Cannot open %1 (Win32 error %2)"_s.arg(entry.name).arg(res)};
        }
        return u"Launched %1 (%2)"_s.arg(entry.name, entry.id);
    }
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
    if (started) return u"Launched %1 (%2)"_s.arg(entry.name, entry.id);
    const QString native = QDir::toNativeSeparators(argv.first());
    const QString params = argv.mid(1).join(u' ');
    const auto res = reinterpret_cast<qintptr>(::ShellExecuteW(
        nullptr, L"open", reinterpret_cast<LPCWSTR>(native.utf16()),
        params.isEmpty() ? nullptr : reinterpret_cast<LPCWSTR>(params.utf16()), nullptr, SW_SHOWNORMAL));
    if (res <= 32) return started.error();
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
#ifdef Q_OS_WIN
    const QList<qint64> pids = executable.isEmpty() ? QList<qint64>{} : findProcesses(executable);
    const bool isExplorer = executable.compare(u"explorer.exe"_s, Qt::CaseInsensitive) == 0
                            || executable.compare(u"explorer"_s, Qt::CaseInsensitive) == 0;
    int totalSent = 0;
    for (const qint64 pid : pids) {
        CloseEnumState state{static_cast<DWORD>(pid), 0, isExplorer};
        ::EnumWindows(&closeWindowForPid, reinterpret_cast<LPARAM>(&state));
        totalSent += state.sent;
    }
    if (totalSent == 0 && !isExplorer && !entry.name.trimmed().isEmpty()) {
        CloseTitleState titleState{entry.name.trimmed(), ::GetCurrentProcessId(), 0};
        ::EnumWindows(&closeWindowByTitle, reinterpret_cast<LPARAM>(&titleState));
        totalSent += titleState.sent;
    }
    if (totalSent > 0) {
        return u"Asked %1 to quit (%2 window%3)"_s.arg(entry.name).arg(totalSent).arg(totalSent == 1 ? u""_s : u"s"_s);
    }
    if (pids.isEmpty() || isExplorer) {
        return Error{ErrorCode::NotFound, u"%1 is not running"_s.arg(entry.name)};
    }
    for (const qint64 pid : pids) {
        HANDLE h = ::OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
        if (h) {
            ::TerminateProcess(h, 0);
            ::CloseHandle(h);
        }
    }
    return u"Asked %1 to quit (%2 process%3)"_s.arg(entry.name).arg(pids.size()).arg(pids.size() == 1 ? u""_s : u"es"_s);
#else
    if (executable.isEmpty()) return Error{ErrorCode::InvalidArgument, u"Cannot tell which program %1 runs"_s.arg(entry.name)};

    const QList<qint64> pids = findProcesses(executable);
    if (pids.isEmpty()) return Error{ErrorCode::NotFound, u"%1 is not running"_s.arg(entry.name)};
    for (const qint64 pid : pids) ::kill(pid_t(pid), SIGTERM);
    return u"Asked %1 to quit (%2 process%3)"_s.arg(entry.name).arg(pids.size()).arg(pids.size() == 1 ? u""_s : u"es"_s);
#endif
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
