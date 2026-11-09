// system/info/ProcessStats: one line of /proc/<pid>/stat, and Nexa's own share of the machine.

#include "system/info/ProcessStats.h"

#include "QtStringMakers.h"

#include <QCoreApplication>
#include <QThread>

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace Qt::StringLiterals;

TEST_CASE("A stat line is read past a name with spaces and brackets in it") {
    const QByteArray line = "4242 (Web (Content) 1) S 4100 4242 4242 0 -1 4194560 1234 0 0 0 150 50 0 0 20 0 30 0 "
                            "987654 123456789 2048 18446744073709551615 1 1 0 0 0 0 0 4096 0 0 0 0 17 3 0 0 0 0 0\n";
    const std::optional<ProcStat> stat = ProcessStats::parseStat(line);
    REQUIRE(stat.has_value());
    CHECK(stat->pid == 4242);
    CHECK(stat->ppid == 4100);
    CHECK(stat->cpuTicks == 200);       // utime 150 + stime 50
    CHECK(stat->rssPages == 2048);
    CHECK_FALSE(ProcessStats::parseStat("garbage").has_value());
    CHECK_FALSE(ProcessStats::parseStat("12 (x) S").has_value());
}

TEST_CASE("Nexa's own sample has its memory, and its CPU once there is a second sample") {
    ProcessStats stats;
    const ProcessStats::Sample first = stats.sample();
    CHECK(first.residentBytes > 0);
    CHECK(first.processes >= 1);
    CHECK(first.cpuPercent < 0);        // nothing to compare with yet
    volatile double spin = 0;
    for (int i = 0; i < 20'000'000; ++i) spin = spin + i * 0.5;
    QThread::msleep(50);
    const ProcessStats::Sample second = stats.sample();
    CHECK(second.cpuPercent >= 0);
    CHECK(second.cpuPercent <= 100);
}
