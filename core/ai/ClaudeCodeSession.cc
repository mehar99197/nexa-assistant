#include "core/ai/ClaudeCodeSession.h"

#include "core/Logging.h"
#include "core/ai/ProgramLookup.h"

#include <QDir>
#include <QFile>

#include "core/ProcessUtils.h"

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

constexpr int kIdleStopMs = 10 * 60 * 1000;
constexpr qsizetype kStderrKept = 4'096;

void killGroup(qint64 pid) {
    process::killTree(pid);
}

void makePrivateDir(const QString &path) {
    QDir().mkpath(path);
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
}

} // namespace

QStringList ClaudeCodeSession::arguments(const QString &model, const QString &systemPrompt) {
    QStringList arguments{u"-p"_s,
                          u"--input-format"_s, u"stream-json"_s,
                          u"--output-format"_s, u"stream-json"_s,
                          u"--verbose"_s,                       // stream-json needs it in print mode
                          u"--include-partial-messages"_s,      // the text deltas
                          u"--tools"_s, QString(),              // none of Claude Code's own tools
                          u"--strict-mcp-config"_s,             // none of the owner's MCP servers
                          u"--setting-sources"_s, QString(),    // none of the owner's settings, hooks or CLAUDE.md
                          u"--disable-slash-commands"_s,
                          u"--no-session-persistence"_s,
                          u"--permission-mode"_s, u"dontAsk"_s,
                          u"--permission-prompts"_s, u"none"_s};
    if (!model.isEmpty()) arguments += {u"--model"_s, model};
    if (!systemPrompt.isEmpty()) arguments += {u"--system-prompt"_s, systemPrompt};
    return arguments;
}

QProcessEnvironment ClaudeCodeSession::prepare(const QString &program) {
    QProcessEnvironment environment = environmentFor(program);
    environment.remove(u"CLAUDECODE"_s);
    environment.remove(u"CLAUDE_CODE_ENTRYPOINT"_s);
    return environment;
}

QString ClaudeCodeSession::workspace(const QString &dir) {
    const QString work = dir + u"/work"_s;
    makePrivateDir(dir);
    makePrivateDir(work);
    return work;
}

ClaudeCodeSession::ClaudeCodeSession(QString program, QString model, QString systemPrompt, QString workingDir,
                                     QObject *parent)
    : QObject(parent), program_(std::move(program)), model_(std::move(model)), systemPrompt_(std::move(systemPrompt)),
      workingDir_(std::move(workingDir)) {
    idle_.setSingleShot(true);
    idle_.setInterval(kIdleStopMs);
    connect(&idle_, &QTimer::timeout, this, &ClaudeCodeSession::stop);
    process_.setProcessChannelMode(QProcess::SeparateChannels);
    connect(&process_, &QProcess::readyReadStandardOutput, this, &ClaudeCodeSession::readEvents);
    connect(&process_, &QProcess::readyReadStandardError, this, [this] {
        stderrTail_ += QString::fromUtf8(process_.readAllStandardError());
        if (stderrTail_.size() > kStderrKept) stderrTail_ = stderrTail_.right(kStderrKept);
    });
    connect(&process_, &QProcess::finished, this, [this](int exitCode, QProcess::ExitStatus) {
        readEvents();       // whatever it said last: a refused turn's result comes right before exit 1
        stderrTail_ += QString::fromUtf8(process_.readAllStandardError());
        ended(ClaudeCodeEvents::exitError(exitCode, stderrTail_));
    });
    connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart) return;
        ended(Error{ErrorCode::NotFound, u"Cannot start Claude Code (%1): %2"_s.arg(program_, process_.errorString())});
    });
}

ClaudeCodeSession::~ClaudeCodeSession() {
    disconnect(&process_, nullptr, this, nullptr);
    if (process_.state() == QProcess::NotRunning) return;
    killGroup(process_.processId());
    process_.waitForFinished(1'000);
}

void ClaudeCodeSession::start() {
    if (started_) return;
    started_ = true;
    process_.setProcessEnvironment(prepare(program_));
    process_.setProgram(program_);
    process_.setArguments(arguments(model_, systemPrompt_));
    process_.setWorkingDirectory(workspace(workingDir_));
    process::configureChild(process_);
    qCInfo(lcAi) << "starting claude" << (model_.isEmpty() ? u"(its own model)"_s : model_) << "with a system prompt of"
                 << systemPrompt_.size() << "characters";
    process_.start();
}

void ClaudeCodeSession::send(const QString &text, const QList<ImageAttachment> &images) {
    busy_ = true;           // before start(): a program that cannot start fails this turn
    ++turns_;
    idle_.stop();
    apiError_.clear();
    start();
    if (broken_) return;
    process_.write(ClaudeCodeEvents::userTurn(text, images));
}

void ClaudeCodeSession::stop() {
    idle_.stop();
    broken_ = true;
    if (process_.state() == QProcess::NotRunning) return;
    killGroup(process_.processId());
}

void ClaudeCodeSession::readEvents() {
    buffer_ += process_.readAllStandardOutput();
    qsizetype newline = 0;
    while ((newline = buffer_.indexOf('\n')) >= 0) {
        const QByteArray line = buffer_.left(newline);
        buffer_.remove(0, newline + 1);
        ClaudeCodeEvent event = ClaudeCodeEvents::decode(line);
        if (busy_) emit activity();
        switch (event.kind) {
        case ClaudeCodeEvent::Kind::Text:
            if (busy_) emit text(event.text);
            break;
        case ClaudeCodeEvent::Kind::ApiError:
            qCInfo(lcAi) << "claude: the API refused the turn:" << event.apiError;
            apiError_ = event.apiError;
            break;
        case ClaudeCodeEvent::Kind::Result:
            if (event.apiError.isEmpty()) event.apiError = apiError_;
            busy_ = false;
            idle_.start();
            emit turnFinished(event);
            break;
        case ClaudeCodeEvent::Kind::Init:
            qCDebug(lcAi) << "claude session on model" << event.model;
            break;
        case ClaudeCodeEvent::Kind::Ignored:
            break;
        }
    }
}

void ClaudeCodeSession::ended(const Error &error) {
    const bool wasBusy = std::exchange(busy_, false);
    broken_ = true;
    idle_.stop();
    if (wasBusy) emit turnFailed(error);
}

} // namespace nexa
