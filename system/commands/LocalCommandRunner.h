#pragma once

#include "core/Result.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>

class QProcess;

namespace nexa {

// Keeps the head and the tail of output that may be huge, so a runaway command cannot fill memory
// and the model still sees how it started and how it ended (pure).
class OutputCollector {
public:
    explicit OutputCollector(qsizetype headLimit = 8 * 1024, qsizetype tailLimit = 8 * 1024);

    void append(const QByteArray &data);
    QByteArray text() const;                    // head + "[… N bytes omitted …]" + tail
    qsizetype totalBytes() const { return total_; }
    bool truncated() const { return total_ > head_.size() + tail_.size(); }

private:
    qsizetype headLimit_;
    qsizetype tailLimit_;
    QByteArray head_;
    QByteArray tail_;
    qsizetype total_{0};
};

struct CommandOutput {
    int exitCode{0};
    bool crashed{false};                        // ended by a signal
    bool timedOut{false};
    QString output;                             // stdout + stderr interleaved, head and tail kept
    qint64 elapsedMs{0};
};

// Runs shell commands without blocking the GUI. Every child starts a new session (its own process
// group), so a timeout, cancel or panic kills everything it spawned, not just the shell.
class LocalCommandRunner : public QObject {
    Q_OBJECT
public:
    using Done = std::function<void(Result<CommandOutput>)>;

    explicit LocalCommandRunner(QObject *parent = nullptr);
    ~LocalCommandRunner() override;

    // `done` runs exactly once; cancelAll() reports ErrorCode::Cancelled.
    void run(const QString &command, const QString &workingDirectory, int timeoutMs, Done done);
    // GUI programs and servers: started detached and not waited for; returns the pid.
    Result<qint64> startDetached(const QString &command, const QString &workingDirectory);
    void cancelAll();
    int runningCount() const { return int(jobs_.size()); }

    // Program and arguments: a direct exec, or `bash -c` when the line uses shell syntax.
    static QStringList commandLine(const QString &command);
    static bool needsShell(const QString &command);
    // Inherited environment with pagers and colours disabled and Nexa's own debug variables removed.
    static QProcessEnvironment environment();

private:
    struct Job;
    void finish(QProcess *process, Result<CommandOutput> result);

    QHash<QProcess *, std::shared_ptr<Job>> jobs_;
};

} // namespace nexa
