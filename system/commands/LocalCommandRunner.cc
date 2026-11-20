#include "system/commands/LocalCommandRunner.h"

#include "core/Logging.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>

#include "core/ProcessUtils.h"

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

constexpr int kKillGraceMs = 1'500;

void terminateTree(qint64 pid, bool force) {
    process::killTree(pid, force);
}

QString shellProgram() {
#ifdef Q_OS_WIN
    static const QString shell = qEnvironmentVariable("COMSPEC", u"cmd.exe"_s);
#else
    static const QString shell = QFileInfo::exists(u"/bin/bash"_s) ? u"/bin/bash"_s : u"/bin/sh"_s;
#endif
    return shell;
}

} // namespace

// ---- OutputCollector ----

OutputCollector::OutputCollector(qsizetype headLimit, qsizetype tailLimit)
    : headLimit_(headLimit), tailLimit_(tailLimit) {}

void OutputCollector::append(const QByteArray &data) {
    total_ += data.size();
    const qsizetype toHead = qMin(headLimit_ - head_.size(), data.size());
    if (toHead > 0) head_.append(data.first(toHead));
    if (toHead < data.size()) {
        tail_.append(data.sliced(qMax<qsizetype>(toHead, 0)));
        if (tail_.size() > tailLimit_) tail_ = tail_.right(tailLimit_);
    }
}

QByteArray OutputCollector::text() const {
    if (!truncated()) return head_ + tail_;
    const qsizetype omitted = total_ - head_.size() - tail_.size();
    return head_ + "\n[… " + QByteArray::number(omitted) + " bytes omitted …]\n" + tail_;
}

// ---- LocalCommandRunner ----

struct LocalCommandRunner::Job {
    Done done;
    OutputCollector output;
    QElapsedTimer timer;
    bool timedOut{false};
    bool cancelled{false};
};

LocalCommandRunner::LocalCommandRunner(QObject *parent) : QObject(parent) {}

LocalCommandRunner::~LocalCommandRunner() {
    for (auto it = jobs_.begin(); it != jobs_.end(); ++it) {
        QProcess *process = it.key();
        process->disconnect(this);
        terminateTree(process->processId(), true);
        process->waitForFinished(1'000);
    }
}

QStringList LocalCommandRunner::commandLine(const QString &command) {
    const QString trimmed = command.trimmed();
    if (trimmed.isEmpty()) return {};
#ifdef Q_OS_WIN
    if (needsShell(trimmed)) return {shellProgram(), u"/C"_s, trimmed};
#else
    if (needsShell(trimmed)) return {shellProgram(), u"-c"_s, trimmed};
#endif
    return QProcess::splitCommand(trimmed);
}

bool LocalCommandRunner::needsShell(const QString &command) {
#ifdef Q_OS_WIN
    static const QRegularExpression syntax(u"[|&<>()^%\\r\\n]"_s);
    static const QSet<QString> builtins{
        u"assoc"_s, u"break"_s, u"call"_s, u"cd"_s, u"chdir"_s, u"cls"_s, u"color"_s, u"copy"_s, u"date"_s,
        u"del"_s,   u"dir"_s,   u"echo"_s, u"endlocal"_s, u"erase"_s, u"exit"_s, u"for"_s, u"ftype"_s, u"goto"_s,
        u"if"_s,    u"md"_s,    u"mkdir"_s, u"mklink"_s, u"move"_s, u"path"_s, u"pause"_s, u"popd"_s, u"prompt"_s,
        u"pushd"_s, u"rd"_s,    u"rem"_s,  u"ren"_s, u"rename"_s, u"rmdir"_s, u"set"_s, u"setlocal"_s, u"shift"_s,
        u"start"_s, u"time"_s,  u"title"_s, u"type"_s, u"ver"_s, u"verify"_s, u"vol"_s,
    };
    if (syntax.match(command).hasMatch()) return true;
    const QString first = command.section(u' ', 0, 0, QString::SectionSkipEmpty).toLower();
    return builtins.contains(first);
#else
    static const QRegularExpression syntax(u"[|&;<>()$`\\\\*?\\[\\]{}~'\"#!]|^\\s*\\w+="_s);
    static const QSet<QString> builtins{
        u"cd"_s, u"export"_s, u"source"_s, u"."_s, u"alias"_s, u"unset"_s, u"set"_s, u"eval"_s, u"exec"_s,
        u"ulimit"_s, u"umask"_s, u"type"_s, u"command"_s, u"builtin"_s, u"declare"_s, u"read"_s, u"shopt"_s,
        u"history"_s, u"jobs"_s, u"wait"_s, u"trap"_s, u"let"_s, u"for"_s, u"while"_s, u"if"_s, u"case"_s,
        u"exit"_s, u"return"_s, u"pushd"_s, u"popd"_s, u"dirs"_s, u"hash"_s, u"times"_s, u"getopts"_s,
        u"readonly"_s, u"shift"_s, u"local"_s, u"function"_s, u"until"_s, u"select"_s,
    };
    if (syntax.match(command).hasMatch()) return true;
    return builtins.contains(command.section(u' ', 0, 0, QString::SectionSkipEmpty));
#endif
}

QProcessEnvironment LocalCommandRunner::environment() {
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.remove(u"QT_LOGGING_RULES"_s);
    const QString platform = env.value(u"QT_QPA_PLATFORM"_s);
    if (platform == u"offscreen" || platform == u"minimal") env.remove(u"QT_QPA_PLATFORM"_s);
    env.insert(u"PAGER"_s, u"cat"_s);
    env.insert(u"GIT_PAGER"_s, u"cat"_s);
    env.insert(u"MANPAGER"_s, u"cat"_s);
    env.insert(u"SYSTEMD_PAGER"_s, QString());
    env.insert(u"NO_COLOR"_s, u"1"_s);
    env.insert(u"DEBIAN_FRONTEND"_s, u"noninteractive"_s);
    return env;
}

void LocalCommandRunner::run(const QString &command, const QString &workingDirectory, int timeoutMs, Done done) {
    const QStringList argv = commandLine(command);
    if (argv.isEmpty()) {
        done(Error{ErrorCode::InvalidArgument, u"The command is empty"_s});
        return;
    }
    runProgram(argv.first(), argv.mid(1), workingDirectory, timeoutMs, environment(), std::move(done));
}

void LocalCommandRunner::runProgram(const QString &program, const QStringList &arguments,
                                    const QString &workingDirectory, int timeoutMs,
                                    const QProcessEnvironment &env, Done done) {
    if (program.isEmpty()) {
        done(Error{ErrorCode::InvalidArgument, u"The command is empty"_s});
        return;
    }

    auto *process = new QProcess(this);
    auto job = std::make_shared<Job>();
    job->done = std::move(done);
    job->timer.start();
    jobs_.insert(process, job);

    process->setProcessChannelMode(QProcess::MergedChannels);
    process->setProcessEnvironment(env);
    process->setWorkingDirectory(workingDirectory.isEmpty() ? QDir::homePath() : workingDirectory);
    process->setStandardInputFile(QProcess::nullDevice());  // nothing may wait for keyboard input
    process::configureChild(*process);

    connect(process, &QProcess::readyReadStandardOutput, this, [process, job] {
        job->output.append(process->readAllStandardOutput());
    });
    connect(process, &QProcess::finished, this, [this, process, job](int exitCode, QProcess::ExitStatus status) {
        job->output.append(process->readAllStandardOutput());
        if (job->cancelled) {
            finish(process, Error{ErrorCode::Cancelled, u"Cancelled"_s});
            return;
        }
        finish(process, CommandOutput{exitCode, status == QProcess::CrashExit, job->timedOut,
                                      QString::fromLocal8Bit(job->output.text()), job->timer.elapsed()});
    });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart) return;  // the other errors are followed by finished()
        finish(process, Error{ErrorCode::NotFound,
                              u"Cannot start \"%1\": %2"_s.arg(process->program(), process->errorString())});
    });

    auto *timeout = new QTimer(process);
    timeout->setSingleShot(true);
    connect(timeout, &QTimer::timeout, process, [process, job] {
        job->timedOut = true;
        terminateTree(process->processId(), false);
        QTimer::singleShot(kKillGraceMs, process, [process] { terminateTree(process->processId(), true); });
    });
    timeout->start(timeoutMs);

    process->start(program, arguments);
}

Result<qint64> LocalCommandRunner::startDetached(const QString &command, const QString &workingDirectory) {
    const QStringList argv = commandLine(command);
    if (argv.isEmpty()) return Error{ErrorCode::InvalidArgument, u"The command is empty"_s};
    QProcess process;
    process.setProgram(argv.first());
    process.setArguments(argv.mid(1));
    process.setWorkingDirectory(workingDirectory.isEmpty() ? QDir::homePath() : workingDirectory);
    process.setProcessEnvironment(environment());
    process.setStandardInputFile(QProcess::nullDevice());
    process.setStandardOutputFile(QProcess::nullDevice());
    process.setStandardErrorFile(QProcess::nullDevice());
#ifndef Q_OS_WIN
    process::configureChild(process);
#endif
    qint64 pid = 0;
    if (!process.startDetached(&pid)) {
        return Error{ErrorCode::NotFound, u"Cannot start \"%1\": %2"_s.arg(argv.first(), process.errorString())};
    }
    return pid;
}

void LocalCommandRunner::cancelAll() {
    for (auto it = jobs_.begin(); it != jobs_.end(); ++it) {
        QProcess *process = it.key();
        it.value()->cancelled = true;
        terminateTree(process->processId(), false);
        QTimer::singleShot(kKillGraceMs, process, [process] { terminateTree(process->processId(), true); });
    }
}

void LocalCommandRunner::finish(QProcess *process, Result<CommandOutput> result) {
    const std::shared_ptr<Job> job = jobs_.take(process);
    if (!job) return;  // already reported
    process->deleteLater();
    qCDebug(lcApp) << "command finished" << process->program() << "ok:" << result.isOk();
    job->done(std::move(result));
}

} // namespace nexa
