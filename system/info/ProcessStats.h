#pragma once

#include <QByteArray>
#include <QElapsedTimer>

#include <optional>

namespace nexa {

// What /proc/<pid>/stat says of one process.
struct ProcStat {
    int pid{0};
    int ppid{0};
    qint64 cpuTicks{0};     // user + system time, in clock ticks
    qint64 rssPages{0};     // resident memory, in pages
};

// How much of the machine Nexa uses — itself and every program it started (opencode, agy, Codex, Piper, Playwright…),
// found by the parent links in /proc — for Mission Control's CPU and MEMORY tiles.
class ProcessStats {
public:
    struct Sample {
        double cpuPercent{-1};      // of the whole machine since the previous sample; −1 on the first
        qint64 residentBytes{0};
        int processes{0};
    };

    static std::optional<ProcStat> parseStat(const QByteArray &line);
    Sample sample();

private:
    QElapsedTimer wall_;
    qint64 lastTicks_{-1};
};

} // namespace nexa
