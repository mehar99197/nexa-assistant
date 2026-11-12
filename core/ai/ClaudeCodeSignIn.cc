#include "core/ai/ClaudeCodeSignIn.h"

#include "core/Logging.h"
#include "core/ai/ClaudeCodeEvents.h"
#include "core/ai/ClaudeCodeSession.h"
#include "core/ai/ProgramLookup.h"

#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>

#include <signal.h>
#include <sys/prctl.h>

#include <algorithm>
#include <utility>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

constexpr qsizetype kLoginOutputKept = 64 * 1024;   // a login prints a few lines; more than this is not the login

// The same flags ClaudeCodeSession runs the program with: its own session, so -pid reaches it and anything it started.
constexpr auto kProcessFlags = QProcess::UnixProcessFlag::CreateNewSession
    | QProcess::UnixProcessFlag::ResetSignalHandlers | QProcess::UnixProcessFlag::CloseFileDescriptors;

void killGroup(qint64 pid) {
    if (pid <= 0) return;
    if (::kill(pid_t(-pid), SIGKILL) != 0) ::kill(pid_t(pid), SIGKILL);
}

// The group alone, for a leader that has already gone: the browser it started may still be there as a member. Never
// the bare pid, which by now may be somebody else's.
void killGroupOnly(qint64 pgid) {
    if (pgid > 0) ::kill(pid_t(-pgid), SIGKILL);
}

QString duration(int ms) {
    if (ms >= 120'000) return u"%1 minutes"_s.arg(ms / 60'000);
    if (ms >= 60'000) return u"a minute"_s;
    return u"%1 s"_s.arg(std::max(1, ms / 1'000));
}

const QString kMethodId = u"claudeai"_s;
const QString kInstructions =
    u"Claude Code opened your browser to sign in with your Claude subscription. If the page shows a code, paste it below."_s;

} // namespace

ClaudeCodeSignIn::ClaudeCodeSignIn(QString program, QString workingDir, QObject *parent)
    : ISignIn(parent), program_(std::move(program)), workingDir_(std::move(workingDir)) {
    if (workingDir_.isEmpty()) {
        workingDir_ = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + u"/nexa/claude"_s;
    }
    timer_.setSingleShot(true);
    connect(&timer_, &QTimer::timeout, this, &ClaudeCodeSignIn::onTimeout);
}

ClaudeCodeSignIn::~ClaudeCodeSignIn() {
    // The running one and any still being put down: no Claude Code of a sign-in may outlive its owner.
    killGroupOnly(groupId_);
    for (QProcess *process : findChildren<QProcess *>(Qt::FindDirectChildrenOnly)) {
        disconnect(process, nullptr, nullptr, nullptr);
        if (process->state() == QProcess::NotRunning) continue;
        killGroup(process->processId());
        process->kill();
        process->waitForFinished(1'000);
    }
}

void ClaudeCodeSignIn::setTimeouts(int stepMs, int signInMs) {
    stepTimeoutMs_ = std::max(1, stepMs);
    signInTimeoutMs_ = std::max(1, signInMs);
}

QString ClaudeCodeSignIn::name(Op op) {
    switch (op) {
    case Op::None: return u"nothing"_s;
    case Op::Status: return u"status"_s;
    case Op::SignIn: return u"sign-in"_s;
    case Op::SignOut: return u"sign-out"_s;
    }
    return {};
}

// -- the operations ---------------------------------------------------------------------------------------

void ClaudeCodeSignIn::checkStatus() {
    if (!begin(Op::Status)) return;
    run({u"auth"_s, u"status"_s, u"--json"_s}, Step::Status, stepTimeoutMs_);
}

void ClaudeCodeSignIn::listMethods() {
    QTimer::singleShot(0, this, [this] {
        emit methodsListed({SignInMethod{kMethodId, u"Claude subscription"_s, true}});
    });
}

void ClaudeCodeSignIn::signIn(const QString &methodId) {
    if (!methodId.trimmed().isEmpty() && methodId.trimmed() != kMethodId) {
        defer(Error{ErrorCode::InvalidArgument,
                    u"Claude Code signs in with a Claude subscription: that is not one of its ways"_s});
        return;
    }
    if (!begin(Op::SignIn)) return;
    run({u"auth"_s, u"login"_s, u"--claudeai"_s}, Step::Login, signInTimeoutMs_);
}

void ClaudeCodeSignIn::submitCode(const QString &code) {
    if (!pendingNeedsCode() || !process_ || process_->state() == QProcess::NotRunning) {
        defer(Error{ErrorCode::InvalidArgument, u"No Claude Code sign-in is waiting for a code"_s});
        return;
    }
    const QString trimmed = code.trimmed();
    if (trimmed.isEmpty()) {
        defer(Error{ErrorCode::InvalidArgument, u"The code is empty"_s});
        return;
    }
    codeSent_ = true;
    process_->write(trimmed.toUtf8() + '\n');
    qCDebug(lcAi) << "claude auth: code sent";       // never the code
}

void ClaudeCodeSignIn::signOut() {
    if (!begin(Op::SignOut)) return;
    run({u"auth"_s, u"logout"_s}, Step::Logout, stepTimeoutMs_);
}

void ClaudeCodeSignIn::cancel() {
    if (!busy()) return;
    fail(Error{ErrorCode::Cancelled, u"Sign-in was cancelled"_s});
}

// -- the program ------------------------------------------------------------------------------------------

bool ClaudeCodeSignIn::begin(Op op) {
    if (busy()) {
        // At once and while still busy, so the caller can tell a refusal from the end of what runs.
        emit failed(Error{ErrorCode::InvalidArgument,
                          u"Another Claude Code sign-in step is still running: wait for it, or cancel it"_s});
        return false;
    }
    if (program_.isEmpty()) {
        defer(Error{ErrorCode::NotFound, u"Claude Code is not installed (%1)"_s.arg(programSearchHint())});
        return false;
    }
    op_ = op;
    step_ = Step::None;
    loginOutput_.clear();
    linkShown_ = false;
    codeSent_ = false;
    ++generation_;
    return true;
}

void ClaudeCodeSignIn::defer(const Error &error) {
    QTimer::singleShot(0, this, [this, error] { emit failed(error); });
}

void ClaudeCodeSignIn::run(const QStringList &arguments, Step step, int timeoutMs) {
    step_ = step;
    auto *process = new QProcess(this);
    // The login's link and prompt may come on either channel: read them together. A status answer is stdout alone.
    process->setProcessChannelMode(step == Step::Login ? QProcess::MergedChannels : QProcess::SeparateChannels);
    process->setProcessEnvironment(ClaudeCodeSession::prepare(program_));
    process->setProgram(program_);
    process->setArguments(arguments);
    process->setWorkingDirectory(ClaudeCodeSession::workspace(workingDir_));
    process->setUnixProcessParameters(kProcessFlags);
    // Its own session takes it out of reach of the terminal's hangup: tie it to Nexa instead.
    process->setChildProcessModifier([] { ::prctl(PR_SET_PDEATHSIG, SIGKILL); });
    if (step == Step::Login) {
        connect(process, &QProcess::readyReadStandardOutput, this, [this, process] {
            if (process == process_) readLogin();
        });
    } else {
        connect(process, &QProcess::readyReadStandardError, this, [process] {
            process->readAllStandardError();    // not kept, not logged
        });
    }
    connect(process, &QProcess::finished, this, [this, process](int exitCode, QProcess::ExitStatus) {
        if (process != process_) return;
        if (step_ == Step::Login) readLogin();      // whatever it said last
        if (process == process_) onProcessEnded(exitCode);
    });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart || process != process_) return;
        const quint64 generation = generation_;
        const QString why = process->errorString();
        QTimer::singleShot(0, this, [this, generation, why] {
            if (generation != generation_) return;
            fail(Error{ErrorCode::NotFound, u"Claude Code is not installed or cannot be started (%1)"_s.arg(why)});
        });
    });
    process_ = process;
    qCDebug(lcAi) << "claude auth: running" << arguments.join(u' ') << "for" << name(op_);
    process->start();
    groupId_ = process->processId();    // the process leads its own session: this is the group too, even after it ends
    if (step != Step::Login) process->closeWriteChannel();      // nothing to type: a command that waited would see the end
    timer_.start(timeoutMs);
}

void ClaudeCodeSignIn::readLogin() {
    if (!process_) return;
    loginOutput_ += QString::fromUtf8(process_->readAllStandardOutput());
    if (loginOutput_.size() > kLoginOutputKept) loginOutput_ = loginOutput_.right(kLoginOutputKept);
    if (linkShown_) return;
    // The link is the first web address it printed ("If the browser didn't open, visit: https://…").
    static const QRegularExpression address(u"https?://[^\\s\"'<>]+"_s);
    if (const QRegularExpressionMatch match = address.match(loginOutput_); match.hasMatch()) {
        QString raw = match.captured(0);
        while (raw.endsWith(u'.') || raw.endsWith(u',') || raw.endsWith(u')') || raw.endsWith(u'…')) raw.chop(1);
        const QUrl url(raw);
        if (url.isValid() && (url.scheme() == u"https" || url.scheme() == u"http")) {
            linkShown_ = true;
            qCInfo(lcAi) << "claude auth: the browser is open, waiting";       // the link is never logged
            emit linkReady(url, kInstructions, true);
            return;
        }
    }
    // The prompt without a link: the browser is open all the same, and a code may be asked.
    if (loginOutput_.contains(u"paste code"_s, Qt::CaseInsensitive)) {
        linkShown_ = true;
        emit linkReady(QUrl(), kInstructions, true);
    }
}

void ClaudeCodeSignIn::onProcessEnded(int exitCode) {
    timer_.stop();
    QProcess *process = process_.data();
    switch (step_) {
    case Step::Status: {
        const QByteArray output = process ? process->readAllStandardOutput() : QByteArray();
        stopProcess();
        onStatusAnswered(exitCode, output);
        break;
    }
    case Step::Login: {
        loginOutput_.clear();       // nothing of it is kept
        stopProcess();
        if (exitCode != 0) {
            fail(Error{ErrorCode::Unauthorized,
                       codeSent_ ? u"Claude Code did not accept the code, or the sign-in did not finish (exit %1)"_s.arg(exitCode)
                                 : u"Claude Code's sign-in did not finish (exit %1)"_s.arg(exitCode)});
            return;
        }
        // Trust what Claude Code says now, not what a clean exit seems to mean.
        run({u"auth"_s, u"status"_s, u"--json"_s}, Step::Status, stepTimeoutMs_);
        break;
    }
    case Step::Logout:
        stopProcess();
        if (exitCode != 0) {
            fail(Error{ErrorCode::Unknown, u"Claude Code could not sign out (exit %1)"_s.arg(exitCode)});
            return;
        }
        run({u"auth"_s, u"status"_s, u"--json"_s}, Step::Status, stepTimeoutMs_);
        break;
    case Step::None:
        break;
    }
}

void ClaudeCodeSignIn::onStatusAnswered(int exitCode, const QByteArray &output) {
    const Result<SignInStatus> status = ClaudeCodeEvents::parseStatus(output);
    if (!status) {
        if (output.trimmed().isEmpty() && exitCode != 0) {
            fail(Error{ErrorCode::ConnectionRefused, u"Claude Code stopped (exit %1) before it answered"_s.arg(exitCode)});
        } else {
            fail(status.error());
        }
        return;
    }
    const SignInStatus answer = status.value();
    switch (op_) {
    case Op::Status:
        finish([this, answer] { emit statusChecked(answer); });
        break;
    case Op::SignIn:
        if (answer.state == SignInStatus::State::SignedIn) {
            finish([this, answer] { emit signedIn(answer); });
        } else {
            fail(Error{ErrorCode::Unauthorized, u"Claude Code finished, but it still reports that nobody is signed in"_s});
        }
        break;
    case Op::SignOut:
        if (answer.state == SignInStatus::State::SignedOut) {
            finish([this] { emit signedOut(); });
        } else {
            fail(Error{ErrorCode::Unknown, u"Claude Code says it logged out, but it is still signed in"_s});
        }
        break;
    case Op::None:
        break;
    }
}

void ClaudeCodeSignIn::onTimeout() {
    if (step_ == Step::Login) {
        fail(Error{ErrorCode::Timeout, u"Signing in did not finish within %1: nobody was signed in"_s.arg(
                                           duration(signInTimeoutMs_))});
        return;
    }
    fail(Error{ErrorCode::Timeout, u"Claude Code did not answer within %1"_s.arg(duration(stepTimeoutMs_))});
}

// -- the end of an operation ------------------------------------------------------------------------------

void ClaudeCodeSignIn::finish(const std::function<void()> &emitResult) {
    timer_.stop();
    stopProcess();
    op_ = Op::None;
    step_ = Step::None;
    loginOutput_.clear();
    linkShown_ = false;
    codeSent_ = false;
    ++generation_;
    // Last, and from a clean state: a slot may start the next operation from inside this signal.
    if (emitResult) emitResult();
}

void ClaudeCodeSignIn::fail(const Error &error) {
    qCDebug(lcAi) << "claude auth:" << name(op_) << "failed, error code" << int(error.code);
    finish([this, error] { emit failed(error); });
}

void ClaudeCodeSignIn::stopProcess() {
    QProcess *process = process_.data();
    process_.clear();
    const qint64 group = std::exchange(groupId_, 0);
    if (!process) return;
    disconnect(process, nullptr, this, nullptr);
    killGroupOnly(group);
    if (process->state() == QProcess::NotRunning) {
        process->deleteLater();
        return;
    }
    killGroup(process->processId());
    if (process->processId() <= 0) process->kill();
    connect(process, &QProcess::finished, process, &QObject::deleteLater);
}

} // namespace nexa
