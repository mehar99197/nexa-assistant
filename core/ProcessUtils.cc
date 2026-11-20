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
#include <memory>
#include <mutex>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#endif

namespace nexa::process {

#ifdef Q_OS_WIN
namespace {

struct WinJobEntry {
    qint64 pid = 0;
    HANDLE handle = nullptr;
};

std::mutex &jobMutex() {
    static std::mutex mutex;
    return mutex;
}

std::unordered_map<qint64, std::shared_ptr<WinJobEntry>> &jobRegistry() {
    static std::unordered_map<qint64, std::shared_ptr<WinJobEntry>> registry;
    return registry;
}

void releaseWinJob(const std::shared_ptr<WinJobEntry> &entry, bool terminateFirst) {
    if (!entry) return;
    HANDLE handle = nullptr;
    {
        std::lock_guard lock(jobMutex());
        if (entry->pid > 0) {
            auto &reg = jobRegistry();
            if (const auto it = reg.find(entry->pid); it != reg.end() && it->second == entry) {
                reg.erase(it);
            }
            entry->pid = 0;
        }
        handle = entry->handle;
        entry->handle = nullptr;
    }
    if (handle) {
        if (terminateFirst) ::TerminateJobObject(handle, 1);
        ::CloseHandle(handle);
    }
}

class WinJobGuard final : public QObject {
public:
    explicit WinJobGuard(QProcess &process) : QObject(&process), process_(process) {
        setObjectName(QStringLiteral("_nexaWinJobGuard"));
        connect(&process_, &QProcess::started, this, &WinJobGuard::attachJob);
    }
    ~WinJobGuard() override { releaseWinJob(entry_, false); }

private:
    void attachJob() {
        releaseWinJob(entry_, false);
        const qint64 pid = process_.processId();
        if (pid <= 0) return;
        HANDLE job = ::CreateJobObjectW(nullptr, nullptr);
        if (!job) return;
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        ::SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
        HANDLE proc = ::OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
        if (!proc) {
            ::CloseHandle(job);
            return;
        }
        const BOOL assigned = ::AssignProcessToJobObject(job, proc);
        ::CloseHandle(proc);
        if (!assigned) {
            ::CloseHandle(job);
            return;
        }
        auto entry = std::make_shared<WinJobEntry>();
        entry->pid = pid;
        entry->handle = job;
        {
            std::lock_guard lock(jobMutex());
            jobRegistry()[pid] = entry;
        }
        entry_ = std::move(entry);
    }

    QProcess &process_;
    std::shared_ptr<WinJobEntry> entry_;
};

} // namespace
#endif

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
    if (!process.findChild<QObject *>(QStringLiteral("_nexaWinJobGuard"), Qt::FindDirectChildrenOnly)) {
        new WinJobGuard(process);
    }
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
    std::shared_ptr<WinJobEntry> jobEntry;
    {
        std::lock_guard lock(jobMutex());
        if (const auto it = jobRegistry().find(pid); it != jobRegistry().end()) {
            jobEntry = it->second;
        }
    }
    if (jobEntry) {
        releaseWinJob(jobEntry, true);
        return;
    }
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
