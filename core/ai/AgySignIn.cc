#include "core/ai/AgySignIn.h"

#include "core/Logging.h"
#include "core/ai/AgySession.h"

#include <QRegularExpression>
#include "core/config/Portable.h"
#include <QTimer>

#include "core/ProcessUtils.h"

#include <algorithm>
#include <utility>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

constexpr int kCloseTimeoutMs = 3'000;                  // session/close is politeness, not part of the answer
constexpr qsizetype kLineLimit = 4 * 1024 * 1024;       // agy's replies are a few KB; no end of line in this much is not ACP

void killGroup(qint64 pid) {
    process::killTree(pid);
}

// The group alone, for a leader that has already gone: its language server may still be there, and the
// group lasts as long as any member does. Never the bare pid, which by now may be somebody else's.
void killGroupOnly(qint64 pgid) {
    process::killTree(pgid, true, false);
}

QString duration(int ms) {
    if (ms >= 120'000) return u"%1 minutes"_s.arg(ms / 60'000);
    if (ms >= 60'000) return u"a minute"_s;
    return u"%1 s"_s.arg(std::max(1, ms / 1'000));
}

const QRegularExpression &methodIdPattern() {
    static const QRegularExpression pattern(u"^oauth-[a-z0-9][a-z0-9-]*$"_s);
    return pattern;
}

} // namespace

AgySignIn::AgySignIn(QString program, QString workingDir, QObject *parent)
    : ISignIn(parent), program_(std::move(program)), workingDir_(std::move(workingDir)) {
    if (workingDir_.isEmpty()) {
        workingDir_ = portable::cacheDir() + u"/agy"_s;
    }
    timer_.setSingleShot(true);
    connect(&timer_, &QTimer::timeout, this, &AgySignIn::onTimeout);
}

AgySignIn::~AgySignIn() {
    // The running one and any still being put down: no agy may outlive its owner.
    killGroupOnly(groupId_);
    for (QProcess *process : findChildren<QProcess *>(Qt::FindDirectChildrenOnly)) {
        disconnect(process, nullptr, nullptr, nullptr);
        if (process->state() == QProcess::NotRunning) continue;
        killGroup(process->processId());
        process->kill();
        process->waitForFinished(1'000);
    }
}

void AgySignIn::setTimeouts(int requestMs, int signInMs) {
    requestTimeoutMs_ = std::max(1, requestMs);
    signInTimeoutMs_ = std::max(1, signInMs);
}

QString AgySignIn::name(Operation operation) {
    switch (operation) {
    case Operation::None: return u"nothing"_s;
    case Operation::Status: return u"status"_s;
    case Operation::Methods: return u"methods"_s;
    case Operation::SignIn: return u"sign-in"_s;
    case Operation::SignOut: return u"sign-out"_s;
    }
    return {};
}

// -- the operations ---------------------------------------------------------------------------------------

void AgySignIn::checkStatus() {
    begin(Operation::Status);
}

void AgySignIn::listMethods() {
    begin(Operation::Methods);
}

void AgySignIn::signIn(const QString &methodId) {
    if (refuseWhileBusy()) return;
    const QString id = methodId.trimmed().isEmpty() ? u"oauth-personal"_s : methodId.trimmed();
    if (!methodIdPattern().match(id).hasMatch()) {
        defer(Error{ErrorCode::InvalidArgument, u"agy signs in through the browser only: that is not one of its browser sign-ins"_s});
        return;
    }
    methodId_ = id;
    begin(Operation::SignIn);
}

void AgySignIn::submitCode(const QString &) {
    emit failed(Error{ErrorCode::NotSupported, u"agy never asks for a pasted code: finish signing in in the browser"_s});
}

void AgySignIn::signOut() {
    begin(Operation::SignOut);
}

void AgySignIn::cancel() {
    if (!busy()) return;
    fail(Error{ErrorCode::Cancelled, u"Cancelled"_s});
}

// -- the program ------------------------------------------------------------------------------------------

bool AgySignIn::refuseWhileBusy() {
    if (!busy()) return false;
    emit failed(Error{ErrorCode::InvalidArgument,
                      u"Another agy sign-in step is still running: wait for it, or cancel it"_s});
    return true;
}

bool AgySignIn::begin(Operation operation) {
    if (refuseWhileBusy()) return false;
    if (program_.isEmpty()) {
        defer(Error{ErrorCode::NotFound, u"agy is not installed (looked on PATH and in ~/.local/bin)"_s});
        return false;
    }
    operation_ = operation;
    step_ = Step::None;
    nextId_ = 1;
    pendingId_ = -1;
    linkShown_ = false;
    initialize_.reset();
    afterClose_ = nullptr;
    buffer_.clear();
    ++generation_;

    auto *process = new QProcess(this);
    process->setProcessChannelMode(QProcess::SeparateChannels);
    process->setProcessEnvironment(AgySession::prepare(workingDir_));
    process->setProgram(program_);
    process->setArguments({u"acp"_s});
    process->setWorkingDirectory(AgySession::workspace(workingDir_));
    process::configureChild(*process);
    connect(process, &QProcess::readyReadStandardOutput, this, [this, process] {
        if (process == process_) readOutput();
    });
    connect(process, &QProcess::readyReadStandardError, this, [process] {
        process->readAllStandardError();    // not kept, not logged: agy writes its account into logs
    });
    connect(process, &QProcess::finished, this, [this, process](int exitCode, QProcess::ExitStatus) {
        if (process != process_) return;
        readOutput();                       // whatever it said last
        if (process == process_) onProcessEnded(exitCode);
    });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart || process != process_) return;
        const quint64 generation = generation_;
        const QString why = process->errorString();
        QTimer::singleShot(0, this, [this, generation, why] {
            if (generation != generation_) return;
            fail(Error{ErrorCode::NotFound, u"agy is not installed or cannot be started (%1)"_s.arg(why)});
        });
    });
    process_ = process;
    qCDebug(lcAi) << "agy acp: starting for" << name(operation);
    process->start();
    groupId_ = process->processId();    // the process leads its own session: this is the group too, even after it ends

    const int id = nextId_++;
    send(id, AgyAcp::initializeRequest(id), u"initialize"_s, Step::Initialize, requestTimeoutMs_);
    return true;
}

void AgySignIn::defer(const Error &error) {
    QTimer::singleShot(0, this, [this, error] { emit failed(error); });
}

void AgySignIn::send(int id, const QByteArray &line, const QString &method, Step step, int timeoutMs) {
    step_ = step;
    pendingId_ = id;
    pendingMethod_ = method;
    qCDebug(lcAi) << "agy acp: sending" << method;
    // A program that could not start is reported by the error handler; there is nothing to write to.
    if (process_ && process_->state() != QProcess::NotRunning) process_->write(line);
    timer_.start(timeoutMs);
}

void AgySignIn::readOutput() {
    if (!process_) return;
    buffer_ += process_->readAllStandardOutput();
    const quint64 generation = generation_;
    qsizetype newline = 0;
    while (generation == generation_ && (newline = buffer_.indexOf('\n')) >= 0) {
        const QByteArray line = buffer_.left(newline);
        buffer_.remove(0, newline + 1);
        if (line.trimmed().isEmpty()) continue;
        const AcpMessage message = AgyAcp::decode(line);
        if (!message.valid) {
            qCDebug(lcAi) << "agy acp: a line that is not a message";     // the line itself may hold anything
            fail(Error{ErrorCode::Parse, u"agy sent something Nexa cannot read: is this a newer agy?"_s});
            return;
        }
        handle(message);
    }
    if (generation == generation_ && buffer_.size() > kLineLimit) {
        fail(Error{ErrorCode::Parse, u"agy sent something Nexa cannot read: is this a newer agy?"_s});
    }
}

void AgySignIn::handle(const AcpMessage &message) {
    if (message.isRequest()) {
        // Nexa serves nothing (it announced no capabilities): say so, or agy waits for an answer.
        qCDebug(lcAi) << "agy acp: request from agy refused:" << message.method;
        if (process_ && process_->state() != QProcess::NotRunning) {
            process_->write(AgyAcp::errorReply(message.rawId, AgyAcp::kMethodNotFoundCode, u"Method not found"_s));
        }
        return;
    }
    if (message.isNotification()) {
        qCDebug(lcAi) << "agy acp: notification" << message.method;
        if (step_ == Step::Authenticate && !linkShown_) {
            if (const QUrl link = AgyAcp::authLink(message); !link.isEmpty()) {
                // agy gave the link instead of opening a browser (or besides it): show it, never log it.
                linkShown_ = true;
                emit linkReady(link, u"Finish signing in in your browser: Nexa carries on when agy is done."_s,
                               false);
            }
        }
        return;
    }
    if (!message.isResponse() || message.id != pendingId_) {
        qCDebug(lcAi) << "agy acp: a reply nobody asked for";
        return;
    }
    if (message.isError) {
        qCDebug(lcAi) << "agy acp: reply to" << pendingMethod_ << "was an error, code" << message.code;
    } else {
        qCDebug(lcAi) << "agy acp: reply to" << pendingMethod_ << "ok";
    }
    timer_.stop();
    pendingId_ = -1;
    switch (step_) {
    case Step::Initialize: onInitialized(message); break;
    case Step::NewSession: onSession(message); break;
    case Step::Authenticate: onAuthenticated(message); break;
    case Step::Logout: onLoggedOut(message); break;
    case Step::CloseSession: {
        // Closed, or agy said it could not: the answer is known either way.
        const std::function<void()> result = std::move(afterClose_);
        finish(result);
        break;
    }
    case Step::None: break;
    }
}

void AgySignIn::openSession() {
    const int id = nextId_++;
    send(id, AgyAcp::newSessionRequest(id, AgySession::workspace(workingDir_)), u"session/new"_s, Step::NewSession,
         requestTimeoutMs_);
}

void AgySignIn::closeSessionThen(const QString &sessionId, std::function<void()> result) {
    afterClose_ = std::move(result);
    const int id = nextId_++;
    send(id, AgyAcp::closeSessionRequest(id, sessionId), u"session/close"_s, Step::CloseSession,
         std::min(requestTimeoutMs_, kCloseTimeoutMs));
}

void AgySignIn::onInitialized(const AcpMessage &message) {
    if (message.isError) {
        fail(Error{ErrorCode::Unknown, u"agy would not start talking to Nexa (error code %1)"_s.arg(message.code)});
        return;
    }
    initialize_ = AgyAcp::parseInitialize(message.result);
    if (!initialize_) {
        fail(Error{ErrorCode::Parse, u"agy's greeting is not what Nexa expects: is this a newer agy?"_s});
        return;
    }
    switch (operation_) {
    case Operation::Methods: {
        const QList<SignInMethod> methods = AgyAcp::oauthMethods(*initialize_);
        finish([this, methods] { emit methodsListed(methods); });
        break;
    }
    case Operation::Status:
        openSession();
        break;
    case Operation::SignIn: {
        // agy opens the browser and answers when the browser has come back: a long wait, cancel() ends it.
        const int id = nextId_++;
        send(id, AgyAcp::authenticateRequest(id, methodId_), u"authenticate"_s, Step::Authenticate,
             signInTimeoutMs_);
        break;
    }
    case Operation::SignOut: {
        if (!initialize_->canLogout) {
            fail(Error{ErrorCode::NotSupported,
                       u"This agy cannot be signed out from another program: sign out in agy itself"_s});
            break;
        }
        const int id = nextId_++;
        send(id, AgyAcp::logoutRequest(id), u"logout"_s, Step::Logout, requestTimeoutMs_);
        break;
    }
    case Operation::None:
        break;
    }
}

void AgySignIn::onSession(const AcpMessage &message) {
    if (message.isError) {
        if (AgyAcp::isAuthRequired(message)) {
            switch (operation_) {
            case Operation::Status:
                finish([this] { emit statusChecked(SignInStatus{SignInStatus::State::SignedOut, {}}); });
                return;
            case Operation::SignOut:
                finish([this] { emit signedOut(); });
                return;
            case Operation::SignIn:
                // agy answered the sign-in but then says nobody is signed in: it must not be called success.
                fail(Error{ErrorCode::Unauthorized,
                           u"agy finished, but it still reports that nobody is signed in. A locked keyring does "
                           "that: unlock it and try again"_s});
                return;
            default:
                break;
            }
        }
        fail(Error{ErrorCode::Unknown, u"agy could not tell whether it is signed in (error code %1)"_s.arg(message.code)});
        return;
    }
    const std::optional<AcpSession> session = AgyAcp::parseSession(message.result);
    if (!session) {
        fail(Error{ErrorCode::Parse, u"agy's answer about its session is not what Nexa expects: is this a newer agy?"_s});
        return;
    }
    const SignInStatus status{SignInStatus::State::SignedIn, session->plan};
    switch (operation_) {
    case Operation::Status:
        closeSessionThen(session->sessionId, [this, status] { emit statusChecked(status); });
        break;
    case Operation::SignIn:
        closeSessionThen(session->sessionId, [this, status] { emit signedIn(status); });
        break;
    case Operation::SignOut:
        closeSessionThen(session->sessionId, [this] {
            emit failed(Error{ErrorCode::Unknown, u"agy says it logged out, but it is still signed in"_s});
        });
        break;
    default:
        fail(Error{ErrorCode::Unknown, u"agy answered something Nexa did not ask"_s});
        break;
    }
}

void AgySignIn::onAuthenticated(const AcpMessage &message) {
    if (message.isError) {
        const QString said = AgyAcp::safeText(message.message);
        if (message.code == AgyAcp::kInvalidParamsCode) {
            fail(Error{ErrorCode::InvalidArgument, u"agy does not offer that way of signing in"_s});
        } else if (message.code == AgyAcp::kMethodNotFoundCode) {
            fail(Error{ErrorCode::NotSupported, u"This agy cannot sign in from another program: run agy once in a terminal"_s});
        } else if (said.isEmpty()) {
            fail(Error{ErrorCode::Unauthorized, u"agy could not sign in (error code %1)"_s.arg(message.code)});
        } else {
            fail(Error{ErrorCode::Unauthorized, u"agy could not sign in: %1"_s.arg(said)});
        }
        return;
    }
    openSession();      // trust what agy says now, not what an unmeasured reply seems to mean
}

void AgySignIn::onLoggedOut(const AcpMessage &message) {
    if (message.isError) {
        const QString said = AgyAcp::safeText(message.message);
        if (message.code == AgyAcp::kMethodNotFoundCode) {
            fail(Error{ErrorCode::NotSupported, u"This agy cannot be signed out from another program: sign out in agy itself"_s});
        } else if (said.isEmpty()) {
            fail(Error{ErrorCode::Unknown, u"agy could not sign out (error code %1)"_s.arg(message.code)});
        } else {
            fail(Error{ErrorCode::Unknown, u"agy could not sign out: %1"_s.arg(said)});
        }
        return;
    }
    openSession();      // signed out is what agy now says, not what an unmeasured reply seems to mean
}

void AgySignIn::onProcessEnded(int exitCode) {
    if (step_ == Step::CloseSession) {
        const std::function<void()> result = std::move(afterClose_);
        finish(result);
        return;
    }
    fail(Error{ErrorCode::ConnectionRefused, u"agy stopped (exit %1) before it answered"_s.arg(exitCode)});
}

void AgySignIn::onTimeout() {
    if (step_ == Step::CloseSession) {
        const std::function<void()> result = std::move(afterClose_);
        finish(result);
        return;
    }
    if (step_ == Step::Authenticate) {
        fail(Error{ErrorCode::Timeout, u"Signing in did not finish within %1: nobody was signed in"_s.arg(
                                           duration(signInTimeoutMs_))});
        return;
    }
    fail(Error{ErrorCode::Timeout, u"agy did not answer within %1"_s.arg(duration(requestTimeoutMs_))});
}

// -- the end of an operation ------------------------------------------------------------------------------

void AgySignIn::finish(const std::function<void()> &emitResult) {
    timer_.stop();
    stopProcess();
    operation_ = Operation::None;
    step_ = Step::None;
    pendingId_ = -1;
    initialize_.reset();
    afterClose_ = nullptr;
    buffer_.clear();
    ++generation_;
    // Last, and from a clean state: a slot may start the next operation from inside this signal.
    if (emitResult) emitResult();
}

void AgySignIn::fail(const Error &error) {
    qCDebug(lcAi) << "agy acp:" << name(operation_) << "failed, error code" << int(error.code);
    finish([this, error] { emit failed(error); });
}

void AgySignIn::stopProcess() {
    QProcess *process = process_.data();
    process_.clear();
    const qint64 group = std::exchange(groupId_, 0);
    if (!process) return;
    disconnect(process, nullptr, this, nullptr);
    // Whatever the leader's state, the group goes: agy that ended by itself may have left its language server.
    killGroupOnly(group);
    if (process->state() == QProcess::NotRunning) {
        process->deleteLater();
        return;
    }
    // The leader still runs: the group kill above reached it, the bare pid only if the group call failed.
    killGroup(process->processId());
    if (process->processId() <= 0) process->kill();
    connect(process, &QProcess::finished, process, &QObject::deleteLater);
}

} // namespace nexa
