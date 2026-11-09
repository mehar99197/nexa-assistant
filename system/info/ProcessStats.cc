#include "system/info/ProcessStats.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QList>
#include <QThread>

#include <unistd.h>

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
}

} // namespace nexa
