#include "system/commands/RootCommandRunner.h"

#include "core/Logging.h"
#include "proto/HelperTypes.h"
#include "system/helper/HelperClient.h"

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

// The same quiet, non-interactive environment the unprivileged runner uses, so output is text
// rather than a pager full of escape codes.
QStringList quietEnvironment() {
    return {u"PAGER=cat"_s,  u"GIT_PAGER=cat"_s, u"MANPAGER=cat"_s,         u"SYSTEMD_PAGER="_s,
            u"NO_COLOR=1"_s, u"TERM=dumb"_s,     u"DEBIAN_FRONTEND=noninteractive"_s};
}

} // namespace

RootCommandRunner::RootCommandRunner(HelperClient *helper, QObject *parent) : QObject(parent), helper_(helper) {}

void RootCommandRunner::run(const QString &command, const QString &workingDirectory, int timeoutMs, Done done) {
    const QStringList argv = LocalCommandRunner::commandLine(command);
    if (argv.isEmpty()) {
        done(Error{ErrorCode::InvalidArgument, u"There is no command to run."_s});
        return;
    }

    auto job = std::make_shared<Job>();
    job->done = std::move(done);
    job->clock.start();

    helper_->runCommand(
        argv, workingDirectory, quietEnvironment(), timeoutMs,
        [job](quint8, const QByteArray &data) { job->output.append(data); },
        [this, job](int exitCode, const QString &status) {
            jobs_.erase(job->id);
            CommandOutput result;
            result.exitCode = exitCode;
            result.crashed = status == helper::kStatusCrashed;
            result.timedOut = status == helper::kStatusTimeout;
            result.output = QString::fromUtf8(job->output.text());
            result.elapsedMs = job->clock.elapsed();
            if (status == helper::kStatusCancelled) {
                job->done(Error{ErrorCode::Cancelled, u"The root command was stopped."_s});
                return;
            }
            job->done(result);
        },
        [this, job](Result<quint64> started) {
            if (!started) {
                job->done(started.error());
                return;
            }
            job->id = started.value();
            jobs_.emplace(job->id, job);
        });
}

void RootCommandRunner::cancelAll() {
    jobs_.clear();
    helper_->cancelAllJobs();
}

} // namespace nexa
