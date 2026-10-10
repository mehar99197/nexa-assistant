#pragma once

#include "core/Result.h"

#include <QObject>
#include <QProcess>
#include <QStringList>

#include <map>
#include <memory>

class QTimer;

namespace nexa {

// Runs the commands the helper has been authorised to run, as root.
//
// Each job gets its own session (and so its own process group), so ending it ends everything it
// started rather than leaving orphans behind; output is streamed back in chunks instead of being
// buffered to the end, so Nexa can show a long build as it happens. No shell is involved: argv is
// executed directly, which keeps the caller's words from being re-interpreted.
class JobManager : public QObject {
    Q_OBJECT
public:
    explicit JobManager(QObject *parent = nullptr);
    ~JobManager() override;

    Result<quint64> start(const QStringList &argv, const QString &cwd, const QStringList &env, int timeoutMs);
    Result<void> cancel(quint64 jobId);
    void cancelAll();
    bool isIdle() const { return jobs_.empty(); }

signals:
    void outputChunk(quint64 jobId, uchar stream, const QByteArray &data);
    void finished(quint64 jobId, int exitCode, const QString &status);

private:
    struct Job {
        std::unique_ptr<QProcess> process;
        QTimer *timeout{nullptr};       // owned by the process
        qint64 pid{0};
        qint64 forwarded{0};            // bytes already sent on, to cap a runaway command
        bool truncated{false};
        QString endedAs;                // set when a timeout or a cancel decided the outcome
    };

    void readFrom(quint64 jobId, QProcess::ProcessChannel channel, uchar stream);
    void endJob(quint64 jobId, int exitCode, const QString &status);
    void signalGroup(const Job &job, int signalNumber);

    std::map<quint64, Job> jobs_;
    quint64 nextId_{0};
};

} // namespace nexa
