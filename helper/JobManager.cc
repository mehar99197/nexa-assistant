#include "helper/JobManager.h"

#include "core/Logging.h"
#include "proto/HelperTypes.h"

#include <QFileInfo>
#include <QTimer>

#include <csignal>
#include <tuple>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

constexpr int kDefaultTimeoutMs = 60'000;
constexpr int kMaxTimeoutMs = 600'000;
constexpr int kGraceBeforeKillMs = 1'500;
// A command that floods its output would otherwise fill the bus; past this the rest is dropped.
constexpr qint64 kMaxForwardedBytes = 1 << 20;
constexpr qsizetype kMaxChunkBytes = 16 * 1024;

// Its own session, no inherited descriptors, and default signal handlers — so the job is not
// affected by the helper's own signal setup and can be ended as a group.
constexpr QProcess::UnixProcessFlags kProcessFlags = QProcess::UnixProcessFlag::CreateNewSession
    | QProcess::UnixProcessFlag::ResetSignalHandlers | QProcess::UnixProcessFlag::CloseFileDescriptors;

} // namespace

JobManager::JobManager(QObject *parent) : QObject(parent) {}

JobManager::~JobManager() {
    // Nothing the helper started outlives it.
    for (auto &[id, job] : jobs_) {
        if (job.process->state() == QProcess::NotRunning) continue;
        signalGroup(job, SIGKILL);
        job.process->waitForFinished(500);
    }
}

Result<quint64> JobManager::start(const QStringList &argv, const QString &cwd, const QStringList &env,
                                  int timeoutMs) {
    if (argv.isEmpty() || argv.first().trimmed().isEmpty()) {
        return Error{ErrorCode::InvalidArgument, u"No command was given."_s};
    }
    for (const QString &word : argv) {
        if (word.contains(QChar(0))) return Error{ErrorCode::InvalidArgument, u"A command word contains a null byte."_s};
    }
    if (!cwd.isEmpty() && !QFileInfo(cwd).isDir()) {
        return Error{ErrorCode::InvalidArgument, u"The working directory %1 does not exist."_s.arg(cwd)};
    }

    const quint64 id = ++nextId_;
    Job job;
    job.process = std::make_unique<QProcess>();
    QProcess *process = job.process.get();
    process->setProgram(argv.first());
    process->setArguments(argv.mid(1));
    if (!cwd.isEmpty()) process->setWorkingDirectory(cwd);
    process->setUnixProcessParameters(kProcessFlags);
    process->setProcessChannelMode(QProcess::SeparateChannels);

    if (!env.isEmpty()) {
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        for (const QString &entry : env) {
            const qsizetype equals = entry.indexOf(u'=');
            if (equals > 0) environment.insert(entry.left(equals), entry.sliced(equals + 1));
        }
        process->setProcessEnvironment(environment);
    }

    connect(process, &QProcess::readyReadStandardOutput, this,
            [this, id] { readFrom(id, QProcess::StandardOutput, helper::kStdout); });
    connect(process, &QProcess::readyReadStandardError, this,
            [this, id] { readFrom(id, QProcess::StandardError, helper::kStderr); });
    connect(process, &QProcess::finished, this, [this, id](int exitCode, QProcess::ExitStatus status) {
        // Whatever is still buffered belongs to this job.
        readFrom(id, QProcess::StandardOutput, helper::kStdout);
        readFrom(id, QProcess::StandardError, helper::kStderr);
        const auto found = jobs_.find(id);
        const QString decided = found != jobs_.end() ? found->second.endedAs : QString();
        if (!decided.isEmpty()) {
            endJob(id, exitCode, decided);
        } else if (status == QProcess::CrashExit) {
            endJob(id, exitCode, helper::kStatusCrashed);
        } else {
            endJob(id, exitCode, helper::kStatusExited);
        }
    });
    connect(process, &QProcess::errorOccurred, this, [this, id](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart) return;
        endJob(id, -1, helper::kStatusCrashed);
    });

    process->start();
    if (!process->waitForStarted(5'000)) {
        const QString message = process->errorString();
        return Error{ErrorCode::NotFound, u"Cannot run %1: %2"_s.arg(argv.first(), message)};
    }
    job.pid = process->processId();

    const int limit = timeoutMs <= 0 ? kDefaultTimeoutMs : qMin(timeoutMs, kMaxTimeoutMs);
    job.timeout = new QTimer(process);
    job.timeout->setSingleShot(true);
    job.timeout->setInterval(limit);
    connect(job.timeout, &QTimer::timeout, this, [this, id] {
        const auto found = jobs_.find(id);
        if (found == jobs_.end()) return;
        found->second.endedAs = helper::kStatusTimeout;
        signalGroup(found->second, SIGTERM);
        QTimer::singleShot(kGraceBeforeKillMs, this, [this, id] {
            const auto still = jobs_.find(id);
            if (still != jobs_.end()) signalGroup(still->second, SIGKILL);
        });
    });
    job.timeout->start();

    qCInfo(lcApp).noquote() << u"job %1 as root: %2 (pid %3)"_s.arg(id).arg(argv.join(u' ')).arg(job.pid);
    jobs_.emplace(id, std::move(job));
    return id;
}

Result<void> JobManager::cancel(quint64 jobId) {
    const auto found = jobs_.find(jobId);
    if (found == jobs_.end()) return Error{ErrorCode::NotFound, u"There is no job %1."_s.arg(jobId)};
    found->second.endedAs = helper::kStatusCancelled;
    signalGroup(found->second, SIGTERM);
    QTimer::singleShot(kGraceBeforeKillMs, this, [this, jobId] {
        const auto still = jobs_.find(jobId);
        if (still != jobs_.end()) signalGroup(still->second, SIGKILL);
    });
    return {};
}

void JobManager::cancelAll() {
    QList<quint64> ids;
    for (const auto &[id, job] : jobs_) ids.append(id);
    // Each id came from the map a moment ago, so the only possible failure is a job that just
    // ended on its own; nothing to report either way.
    for (const quint64 id : ids) std::ignore = cancel(id);
}

void JobManager::readFrom(quint64 jobId, QProcess::ProcessChannel channel, uchar stream) {
    const auto found = jobs_.find(jobId);
    if (found == jobs_.end()) return;
    Job &job = found->second;
    job.process->setReadChannel(channel);
    QByteArray data = job.process->readAll();
    if (data.isEmpty()) return;

    if (job.forwarded >= kMaxForwardedBytes) return;
    if (job.forwarded + data.size() > kMaxForwardedBytes) {
        data.truncate(kMaxForwardedBytes - job.forwarded);
        job.truncated = true;
    }
    job.forwarded += data.size();
    for (qsizetype offset = 0; offset < data.size(); offset += kMaxChunkBytes) {
        emit outputChunk(jobId, stream, data.sliced(offset, qMin(kMaxChunkBytes, data.size() - offset)));
    }
    if (job.truncated) {
        emit outputChunk(jobId, helper::kStderr, "\n[the helper stopped forwarding output here]\n");
    }
}

void JobManager::endJob(quint64 jobId, int exitCode, const QString &status) {
    const auto found = jobs_.find(jobId);
    if (found == jobs_.end()) return;       // already reported
    jobs_.erase(found);
    qCInfo(lcApp).noquote() << u"job %1 %2 (exit code %3)"_s.arg(jobId).arg(status).arg(exitCode);
    emit finished(jobId, exitCode, status);
}

void JobManager::signalGroup(const Job &job, int signalNumber) {
    if (job.pid <= 0) return;
    // Negative pid: the whole process group, so children go too.
    if (::kill(pid_t(-job.pid), signalNumber) != 0 && job.process->state() != QProcess::NotRunning) {
        ::kill(pid_t(job.pid), signalNumber);
    }
}

} // namespace nexa
