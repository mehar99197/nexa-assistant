#include "core/ProcessUtils.h"

#include <QProcess>

#ifdef Q_OS_UNIX
#include <cerrno>
#include <csignal>
#include <sys/prctl.h>
#elif defined(Q_OS_WIN)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#endif

namespace nexa::process {

void configureChild(QProcess &process) {
#ifdef Q_OS_UNIX
    static constexpr QProcess::UnixProcessParameters kProcessFlags{
        QProcess::UnixProcessFlag::CreateNewSession
        | QProcess::UnixProcessFlag::ResetSignalHandlers
        | QProcess::UnixProcessFlag::CloseFileDescriptors,
    };
    process.setUnixProcessParameters(kProcessFlags);
    process.setChildProcessModifier([] { ::prctl(PR_SET_PDEATHSIG, SIGKILL); });
#elif defined(Q_OS_WIN)
    process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) {
        if ((args->flags & CREATE_NEW_CONSOLE) == 0) {
            args->flags |= CREATE_NO_WINDOW;
        }
    });
#else
    Q_UNUSED(process);
#endif
}

void killTree(qint64 pid, bool force, bool includeRoot) {
    if (pid <= 0) return;
#ifdef Q_OS_UNIX
    const int sig = force ? SIGKILL : SIGTERM;
    if (::kill(pid_t(-pid), sig) != 0 && includeRoot) ::kill(pid_t(pid), sig);
#elif defined(Q_OS_WIN)
    Q_UNUSED(force);
    HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    std::vector<DWORD> order;
    if (snap != INVALID_HANDLE_VALUE) {
        std::unordered_map<DWORD, std::vector<DWORD>> children;
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (::Process32FirstW(snap, &entry)) {
            do {
                children[entry.th32ParentProcessID].push_back(entry.th32ProcessID);
            } while (::Process32NextW(snap, &entry));
        }
        ::CloseHandle(snap);

        const DWORD root = static_cast<DWORD>(pid);
        std::queue<DWORD> pending;
        std::unordered_set<DWORD> visited;
        visited.insert(root);
        if (includeRoot) order.push_back(root);
        if (const auto it = children.find(root); it != children.end()) {
            for (const DWORD child : it->second) {
                if (visited.insert(child).second) {
                    pending.push(child);
                    order.push_back(child);
                }
            }
        }
        while (!pending.empty()) {
            const DWORD cur = pending.front();
            pending.pop();
            const auto it = children.find(cur);
            if (it == children.end()) continue;
            for (const DWORD child : it->second) {
                if (visited.insert(child).second) {
                    pending.push(child);
                    order.push_back(child);
                }
            }
        }
    } else if (includeRoot) {
        order.push_back(static_cast<DWORD>(pid));
    }

    // Terminate parents first so supervisors cannot respawn children mid-pass.
    for (const DWORD target : order) {
        HANDLE handle = ::OpenProcess(PROCESS_TERMINATE, FALSE, target);
        if (handle) {
            ::TerminateProcess(handle, 1);
            ::CloseHandle(handle);
        }
    }
#else
    Q_UNUSED(force);
    Q_UNUSED(includeRoot);
#endif
}

bool isAlive(qint64 pid) {
    if (pid <= 0) return false;
#ifdef Q_OS_UNIX
    return ::kill(pid_t(pid), 0) == 0 || errno == EPERM;
#elif defined(Q_OS_WIN)
    HANDLE handle = ::OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (!handle) return false;
    const DWORD wait = ::WaitForSingleObject(handle, 0);
    ::CloseHandle(handle);
    return wait == WAIT_TIMEOUT;
#else
    return false;
#endif
}

} // namespace nexa::process
