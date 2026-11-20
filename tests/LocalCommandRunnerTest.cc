#include "system/commands/LocalCommandRunner.h"

#include "QtStringMakers.h"

#include <QFile>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <catch2/catch_test_macros.hpp>

#include "core/ProcessUtils.h"
#include <optional>

using namespace nexa;
using namespace Qt::StringLiterals;

namespace {

std::optional<Result<CommandOutput>> runAndWait(LocalCommandRunner &runner, const QString &command, int timeoutMs = 10'000,
                                                const std::function<void()> &afterStart = {}) {
    std::optional<Result<CommandOutput>> outcome;
    runner.run(command, {}, timeoutMs, [&outcome](Result<CommandOutput> result) { outcome = std::move(result); });
    if (afterStart) afterStart();
    if (!QTest::qWaitFor([&outcome] { return outcome.has_value(); }, 15'000)) return std::nullopt;
    return outcome;
}

#ifndef Q_OS_WIN
bool processAlive(qint64 pid) {
    return process::isAlive(pid);
}
#endif

} // namespace

TEST_CASE("OutputCollector keeps the head and tail of long output") {
    OutputCollector collector(4, 4);
    collector.append("abc");
    CHECK(collector.text() == "abc");
    collector.append("defghijkl");
    collector.append("mn");
    CHECK(collector.totalBytes() == 14);
    CHECK(collector.truncated());
    CHECK(collector.text() == "abcd\n[… 6 bytes omitted …]\nklmn");
}

TEST_CASE("LocalCommandRunner decides when a shell is needed") {
#ifdef Q_OS_WIN
    CHECK_FALSE(LocalCommandRunner::needsShell(u"whoami /all"_s));
    CHECK(LocalCommandRunner::needsShell(u"dir | findstr /i foo"_s));
    CHECK(LocalCommandRunner::needsShell(u"echo %USERPROFILE%"_s));
    CHECK(LocalCommandRunner::needsShell(u"cd C:\\Windows"_s));
    CHECK(LocalCommandRunner::commandLine(u"whoami /all"_s) == QStringList{u"whoami"_s, u"/all"_s});
    CHECK(LocalCommandRunner::commandLine(u"echo a | findstr a"_s).at(1) == u"/C"_s);
#else
    CHECK_FALSE(LocalCommandRunner::needsShell(u"ls -la /tmp"_s));
    CHECK(LocalCommandRunner::needsShell(u"ls | wc -l"_s));
    CHECK(LocalCommandRunner::needsShell(u"echo $HOME"_s));
    CHECK(LocalCommandRunner::needsShell(u"cat ~/notes.txt"_s));
    CHECK(LocalCommandRunner::needsShell(u"cd /tmp"_s));
    CHECK(LocalCommandRunner::needsShell(u"FOO=1 env"_s));
    CHECK(LocalCommandRunner::commandLine(u"ls -la"_s) == QStringList{u"ls"_s, u"-la"_s});
    CHECK(LocalCommandRunner::commandLine(u"echo a | tr a b"_s).at(1) == u"-c"_s);
#endif
}

TEST_CASE("LocalCommandRunner returns output and exit codes") {
    LocalCommandRunner runner;
#ifdef Q_OS_WIN
    const auto echoed = runAndWait(runner, u"echo hello&&echo oops>&2"_s);
    REQUIRE(echoed.has_value());
    REQUIRE(echoed->isOk());
    CHECK(echoed->value().exitCode == 0);
    const QString normalized = echoed->value().output.trimmed().replace(u"\r\n"_s, u"\n"_s);
    CHECK(normalized == u"hello\noops"_s);
#else
    const auto echoed = runAndWait(runner, u"echo hello && echo oops >&2"_s);
    REQUIRE(echoed.has_value());
    REQUIRE(echoed->isOk());
    CHECK(echoed->value().exitCode == 0);
    CHECK(echoed->value().output == u"hello\noops\n"_s);
#endif

    const auto failed = runAndWait(runner, u"exit 3"_s);
    REQUIRE(failed.has_value());
    REQUIRE(failed->isOk());
    CHECK(failed->value().exitCode == 3);
}

TEST_CASE("LocalCommandRunner reports programs that do not exist") {
    LocalCommandRunner runner;
    const auto missing = runAndWait(runner, u"definitely-not-a-program-xyz --flag"_s);
    REQUIRE(missing.has_value());
    REQUIRE_FALSE(missing->isOk());
    CHECK(missing->error().code == ErrorCode::NotFound);
}

#ifndef Q_OS_WIN
TEST_CASE("LocalCommandRunner kills the whole process group on timeout") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString pidFile = dir.filePath(u"child.pid"_s);
    LocalCommandRunner runner;
    // The background `sleep` is a grandchild: only a process-group kill reaches it.
    const auto outcome = runAndWait(runner, u"sleep 30 & echo $! > '%1'; wait"_s.arg(pidFile), 400);
    REQUIRE(outcome.has_value());
    REQUIRE(outcome->isOk());
    CHECK(outcome->value().timedOut);
    CHECK(outcome->value().elapsedMs < 5'000);

    QFile file(pidFile);
    REQUIRE(file.open(QIODevice::ReadOnly));
    const qint64 grandchild = file.readAll().trimmed().toLongLong();
    REQUIRE(grandchild > 0);
    CHECK(QTest::qWaitFor([grandchild] { return !processAlive(grandchild); }, 3'000));
}
#endif

TEST_CASE("LocalCommandRunner::cancelAll reports Cancelled") {
    LocalCommandRunner runner;
#ifdef Q_OS_WIN
    const QString longCmd = u"ping -n 30 127.0.0.1"_s;
#else
    const QString longCmd = u"sleep 30"_s;
#endif
    const auto outcome = runAndWait(runner, longCmd, 60'000, [&runner] {
        QTimer::singleShot(100, &runner, [&runner] { runner.cancelAll(); });
    });
    REQUIRE(outcome.has_value());
    REQUIRE_FALSE(outcome->isOk());
    CHECK(outcome->error().code == ErrorCode::Cancelled);
    CHECK(runner.runningCount() == 0);
}

TEST_CASE("LocalCommandRunner gives commands no stdin and no pager") {
    LocalCommandRunner runner;
#ifdef Q_OS_WIN
    const auto outcome = runAndWait(runner, u"echo pager=%PAGER%"_s);
    REQUIRE(outcome.has_value());
    REQUIRE(outcome->isOk());
    CHECK(outcome->value().output.trimmed() == u"pager=cat"_s);
#else
    const auto outcome = runAndWait(runner, u"cat; echo \"pager=$PAGER\""_s);
    REQUIRE(outcome.has_value());
    REQUIRE(outcome->isOk());
    CHECK(outcome->value().output == u"pager=cat\n"_s);
#endif
}
