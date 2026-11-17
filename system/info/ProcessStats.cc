#include "system/info/ProcessStats.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QThread>
#include <QHash>
#include <QList>
#include <QThread>

#ifdef Q_OS_UNIX
#include <unistd.h>
#elif defined(Q_OS_WIN)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#endif

#include <algorithm>

namespace nexa {

std::optional<ProcStat> ProcessStats::parseStat(const QByteArray &line) {
    // "pid (comm) state ppid …": the name may hold spaces and brackets, so the fields count from its last ')'.
    const qsizetype open = line.indexOf('(');
    const qsizetype close = line.lastIndexOf(')');
    if (open <= 0 || close < open) return std::nullopt;
    bool ok = false;
    ProcStat stat;
    stat.pid = line.left(open).trimmed().toInt(&ok);
    if (!ok) return std::nullopt;
    const QList<QByteArray> fields = line.mid(close + 1).simplified().split(' ');
    // After the name: [0] state, [1] ppid, … [11] utime, [12] stime, … [21] rss.
    if (fields.size() < 22) return std::nullopt;
    bool ppidOk = false;
    bool userOk = false;
    bool systemOk = false;
    bool rssOk = false;
    stat.ppid = fields.at(1).toInt(&ppidOk);
    stat.cpuTicks = fields.at(11).toLongLong(&userOk) + fields.at(12).toLongLong(&systemOk);
    stat.rssPages = fields.at(21).toLongLong(&rssOk);
    if (!ppidOk || !userOk || !systemOk || !rssOk) return std::nullopt;
    return stat;
}

ProcessStats::Sample ProcessStats::sample() {
#ifdef Q_OS_WIN
    Sample result;
    const DWORD selfPid = ::GetCurrentProcessId();
    std::unordered_map<DWORD, std::vector<DWORD>> children;
    HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (::Process32FirstW(snap, &entry)) {
            do {
                children[entry.th32ParentProcessID].push_back(entry.th32ProcessID);
            } while (::Process32NextW(snap, &entry));
        }
        ::CloseHandle(snap);
    }

    std::queue<DWORD> pending;
    std::unordered_set<DWORD> seen;
    pending.push(selfPid);
    seen.insert(selfPid);
    qint64 totalTicks = 0;

    while (!pending.empty()) {
        const DWORD pid = pending.front();
        pending.pop();
        ++result.processes;

        HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pid);
        if (!h) h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (h) {
            PROCESS_MEMORY_COUNTERS pmc{};
            if (::GetProcessMemoryInfo(h, &pmc, sizeof(pmc))) {
                result.residentBytes += static_cast<qint64>(pmc.WorkingSetSize);
            }
            FILETIME c{}, e{}, k{}, u{};
            if (::GetProcessTimes(h, &c, &e, &k, &u)) {
                ULARGE_INTEGER ki{}, ui{};
                ki.LowPart = k.dwLowDateTime;
                ki.HighPart = k.dwHighDateTime;
                ui.LowPart = u.dwLowDateTime;
                ui.HighPart = u.dwHighDateTime;
                totalTicks += static_cast<qint64>(ki.QuadPart + ui.QuadPart);
            }
            ::CloseHandle(h);
        }

        if (const auto it = children.find(pid); it != children.end()) {
            for (const DWORD child : it->second) {
                if (seen.insert(child).second) pending.push(child);
            }
        }
    }

    if (!wall_.isValid()) {
        wall_.start();
        lastTicks_ = totalTicks;
    } else {
        const qint64 elapsedMs = wall_.restart();
        if (lastTicks_ >= 0 && elapsedMs > 0) {
            const qint64 delta = qMax<qint64>(0, totalTicks - lastTicks_);
            const double capacity = double(elapsedMs) * 10'000.0 * double(qMax(1, QThread::idealThreadCount()));
            result.cpuPercent = qBound(0.0, (double(delta) / capacity) * 100.0, 100.0);
        }
        lastTicks_ = totalTicks;
    }
    return result;
#elif defined(Q_OS_UNIX)
    // Every process, then the ones descended from Nexa.
    QHash<int, ProcStat> all;
    QHash<int, QList<int>> children;
    const QStringList pids = QDir(QStringLiteral("/proc")).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &name : pids) {
        bool numeric = false;
        name.toInt(&numeric);
        if (!numeric) continue;
        QFile file(QStringLiteral("/proc/%1/stat").arg(name));
        if (!file.open(QIODevice::ReadOnly)) continue;      // it ended meanwhile
        if (const std::optional<ProcStat> stat = parseStat(file.readAll())) {
            all.insert(stat->pid, *stat);
            children[stat->ppid].append(stat->pid);
        }
    }
    Sample sample;
    qint64 ticks = 0;
    qint64 pages = 0;
    QList<int> todo{int(::getpid())};
    while (!todo.isEmpty()) {
        const int pid = todo.takeLast();
        const auto found = all.constFind(pid);
        if (found == all.cend()) continue;
        ++sample.processes;
        ticks += found->cpuTicks;
        pages += found->rssPages;
        todo += children.value(pid);
    }
    static const long pageSize = ::sysconf(_SC_PAGESIZE);
    static const long ticksPerSecond = ::sysconf(_SC_CLK_TCK);
    sample.residentBytes = pages * pageSize;
    if (lastTicks_ >= 0 && wall_.isValid() && wall_.elapsed() > 0) {
        const double seconds = double(wall_.elapsed()) / 1000.0;
        const double used = double(std::max<qint64>(0, ticks - lastTicks_)) / double(ticksPerSecond);
        const int cores = std::max(1, QThread::idealThreadCount());
        sample.cpuPercent = std::clamp(100.0 * used / (seconds * cores), 0.0, 100.0);
    }
    lastTicks_ = ticks;
    wall_.start();
    return sample;
#else
    return {};
#endif
}

} // namespace nexa
