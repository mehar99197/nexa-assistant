#include "system/helper/HelperClient.h"

#include "core/Logging.h"
#include "proto/HelperTypes.h"

#include "HelperProxy.h"

#include <QDBusConnection>
#include <QFileInfo>
#include <QDBusError>
#include <QDBusMetaType>
#include <QDBusPendingCallWatcher>
#include <QDBusServiceWatcher>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

// Long enough for the user to read polkit's prompt and type a password.
constexpr int kPrivilegedTimeoutMs = 180'000;
constexpr int kProbeTimeoutMs = 25'000;     // enough to start the helper, not enough to hang on

const QString &installHint() {
    static const QString hint = u"The root helper is not installed. Install it with: sudo "
                                "./packaging/install-helper.sh"_s;
    return hint;
}

} // namespace

bool HelperClient::isInstalled() {
    // This file is what lets the bus start the helper on demand; without it the name cannot be
    // reached at all.
    for (const QString &directory : {u"/usr/share/dbus-1/system-services"_s,
                                     u"/usr/local/share/dbus-1/system-services"_s}) {
        if (QFileInfo::exists(directory + u"/org.nexa.Helper.service"_s)) return true;
    }
    return false;
}

QString HelperClient::installCommand() {
    return u"sudo ./packaging/install-helper.sh"_s;
}

HelperClient::HelperClient(QObject *parent) : QObject(parent) {
    qDBusRegisterMetaType<QList<uint>>();       // PressKeys' argument
    proxy_ = std::make_unique<HelperProxy>(helper::kService, helper::kPath, QDBusConnection::systemBus(), this);
    proxy_->setTimeout(kPrivilegedTimeoutMs);
    connect(proxy_.get(), &HelperProxy::OutputChunk, this, &HelperClient::onOutputChunk);
    connect(proxy_.get(), &HelperProxy::Finished, this, &HelperClient::onFinished);
    connect(proxy_.get(), &HelperProxy::InputUnlocked, this, &HelperClient::inputUnlocked);

    // The helper exits when idle and is started again on demand; if it goes while a job is running,
    // that job would never report, so it is ended here instead of hanging.
    watcher_ = new QDBusServiceWatcher(helper::kService, QDBusConnection::systemBus(),
                                       QDBusServiceWatcher::WatchForUnregistration, this);
    connect(watcher_, &QDBusServiceWatcher::serviceUnregistered, this, &HelperClient::onHelperGone);
}

HelperClient::~HelperClient() = default;

Error HelperClient::errorFrom(const QDBusError &error) const {
    switch (error.type()) {
    case QDBusError::ServiceUnknown:
        // NotSupported, not NotFound: the fast path treats NotFound as "I could not find what you
        // named" and quietly hands the request to the model, which would then try to work around
        // the missing helper with sudo. This is a dead end the user has to fix, so it is reported.
        return Error{ErrorCode::NotSupported, installHint()};
    case QDBusError::AccessDenied:
        return Error{ErrorCode::PermissionDenied,
                     u"D-Bus refused the call to the helper. Is packaging/org.nexa.Helper.conf installed? (%1)"_s
                         .arg(error.message())};
    case QDBusError::UnknownMethod:
        // An older helper, installed before this method existed.
        return Error{ErrorCode::NotSupported, u"The installed root helper is older than Nexa. Update it with: sudo "
                                              "./packaging/install-helper.sh"_s};
    case QDBusError::NoReply:
    case QDBusError::Timeout:
    case QDBusError::TimedOut:
        return Error{ErrorCode::Timeout, u"The root helper did not answer in time."_s};
    default:
        break;
    }
    if (error.name() == helper::kErrorDenied) {
        const QString reason = error.message().isEmpty() ? u"Permission was refused."_s : error.message();
        return Error{ErrorCode::PermissionDenied, reason};
    }
    if (error.name() == helper::kErrorInvalidArgument) return Error{ErrorCode::InvalidArgument, error.message()};
    if (error.name() == helper::kErrorLocked) return Error{ErrorCode::PermissionDenied, error.message()};
    if (error.name() == helper::kErrorNoInputDevice) return Error{ErrorCode::NotSupported, error.message()};
    return Error{ErrorCode::Unknown, error.message().isEmpty() ? error.name() : error.message()};
}

void HelperClient::setReady(bool ready, const QString &reason) {
    unavailable_ = ready ? QString() : reason;
    if (ready_ == ready) return;
    ready_ = ready;
    emit readyChanged(ready_);
}

// A reply arriving also tells us whether the helper is there at all, so each of the three shapes
// below records that before handing the result on.
void HelperClient::noteOutcome(bool failed, const Error &error) {
    if (!failed) {
        setReady(true, {});
        return;
    }
    if (error.code == ErrorCode::NotSupported || error.code == ErrorCode::Timeout) setReady(false, error.message);
}

void HelperClient::awaitVoid(const QDBusPendingCall &call, Done done) {
    auto *watcher = new QDBusPendingCallWatcher(call, this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, done](QDBusPendingCallWatcher *finished) {
        finished->deleteLater();
        const QDBusPendingReply<> reply = *finished;
        const Error error = reply.isError() ? errorFrom(reply.error()) : Error{};
        noteOutcome(reply.isError(), error);
        if (reply.isError()) {
            done(error);
            return;
        }
        done(Result<void>{});
    });
}

void HelperClient::awaitString(const QDBusPendingCall &call, TextDone done) {
    auto *watcher = new QDBusPendingCallWatcher(call, this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, done](QDBusPendingCallWatcher *finished) {
        finished->deleteLater();
        const QDBusPendingReply<QString> reply = *finished;
        const Error error = reply.isError() ? errorFrom(reply.error()) : Error{};
        noteOutcome(reply.isError(), error);
        done(reply.isError() ? Result<QString>{error} : Result<QString>{reply.value()});
    });
}

void HelperClient::awaitFlag(const QDBusPendingCall &call, FlagDone done) {
    auto *watcher = new QDBusPendingCallWatcher(call, this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, done](QDBusPendingCallWatcher *finished) {
        finished->deleteLater();
        const QDBusPendingReply<bool> reply = *finished;
        const Error error = reply.isError() ? errorFrom(reply.error()) : Error{};
        noteOutcome(reply.isError(), error);
        done(reply.isError() ? Result<bool>{error} : Result<bool>{reply.value()});
    });
}

void HelperClient::isAuthorized(FlagDone done) {
    awaitFlag(proxy_->IsAuthorized(), std::move(done));
}

void HelperClient::forgetAuthorization(Done done) {
    awaitVoid(proxy_->ForgetAuthorization(), std::move(done));
}

void HelperClient::probe(TextDone done) {
    proxy_->setTimeout(kProbeTimeoutMs);
    awaitString(proxy_->Version(), [this, done](Result<QString> version) {
        proxy_->setTimeout(kPrivilegedTimeoutMs);
        if (version) {
            qCInfo(lcApp) << "root helper" << version.value() << "ready";
        } else {
            qCInfo(lcApp) << "root helper unavailable:" << version.error().message;
        }
        done(std::move(version));
    });
}

void HelperClient::runCommand(const QStringList &argv, const QString &cwd, const QStringList &env, int timeoutMs,
                              JobOutput output, JobFinished finished, std::function<void(Result<quint64>)> started) {
    auto *watcher = new QDBusPendingCallWatcher(proxy_->RunCommand(argv, cwd, env, timeoutMs), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, output, finished, started](QDBusPendingCallWatcher *done) {
        done->deleteLater();
        const QDBusPendingReply<qulonglong> reply = *done;
        const Error error = reply.isError() ? errorFrom(reply.error()) : Error{};
        noteOutcome(reply.isError(), error);
        if (reply.isError()) {
            started(error);
            return;
        }
        // The helper sends this reply before any OutputChunk of the job, and D-Bus keeps the order
        // on one connection, so the id is always known by the time output arrives.
        jobs_.emplace(reply.value(), Job{output, finished});
        started(quint64(reply.value()));
    });
}

void HelperClient::cancelJob(quint64 jobId) {
    if (!jobs_.contains(jobId)) return;
    proxy_->CancelJob(jobId);
}

void HelperClient::cancelAllJobs() {
    for (const auto &[id, job] : jobs_) proxy_->CancelJob(id);
}

void HelperClient::onOutputChunk(qulonglong jobId, uchar stream, const QByteArray &data) {
    const auto found = jobs_.find(jobId);
    if (found == jobs_.end()) return;
    if (found->second.output) found->second.output(quint8(stream), data);
}

void HelperClient::onFinished(qulonglong jobId, int exitCode, const QString &status) {
    const auto found = jobs_.find(jobId);
    if (found == jobs_.end()) return;
    const Job job = found->second;
    jobs_.erase(found);
    if (job.finished) job.finished(exitCode, status);
}

void HelperClient::onHelperGone() {
    emit helperGone();
    if (jobs_.empty()) return;
    qCWarning(lcApp) << "the root helper stopped while" << jobs_.size() << "job(s) were running";
    const auto running = std::exchange(jobs_, {});
    for (const auto &[id, job] : running) {
        if (job.finished) job.finished(-1, helper::kStatusCrashed);
    }
}

void HelperClient::typeText(const QString &text, TextDone done) {
    awaitString(proxy_->TypeText(text), std::move(done));   // the value is what could not be typed
}

void HelperClient::pressKeys(const QList<int> &codes, Done done) {
    QList<uint> keycodes;
    keycodes.reserve(codes.size());
    for (const int code : codes) keycodes.append(uint(code));
    awaitVoid(proxy_->PressKeys(keycodes), std::move(done));
}

void HelperClient::mouseMove(int dx, int dy, Done done) {
    awaitVoid(proxy_->MouseMove(dx, dy), std::move(done));
}

void HelperClient::mouseMoveAbs(int x, int y, int screenWidth, int screenHeight, Done done) {
    awaitVoid(proxy_->MouseMoveAbs(x, y, screenWidth, screenHeight), std::move(done));
}

void HelperClient::mouseButton(int button, bool down, Done done) {
    awaitVoid(proxy_->MouseButton(uint(button), down), std::move(done));
}

void HelperClient::mouseScroll(int dx, int dy, Done done) {
    awaitVoid(proxy_->MouseScroll(dx, dy), std::move(done));
}

void HelperClient::lockInput(std::function<void(Result<QStringList>)> done) {
    auto *watcher = new QDBusPendingCallWatcher(proxy_->LockInput(), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, done](QDBusPendingCallWatcher *finished) {
        finished->deleteLater();
        const QDBusPendingReply<QStringList> reply = *finished;
        const Error error = reply.isError() ? errorFrom(reply.error()) : Error{};
        noteOutcome(reply.isError(), error);
        done(reply.isError() ? Result<QStringList>{error} : Result<QStringList>{reply.value()});
    });
}

void HelperClient::unlockInput(Done done) {
    awaitVoid(proxy_->UnlockInput(), std::move(done));
}

void HelperClient::unlockInputAndWait() {
    proxy_->setTimeout(2'000);
    QDBusPendingReply<> reply = proxy_->UnlockInput();
    reply.waitForFinished();
    proxy_->setTimeout(kPrivilegedTimeoutMs);
    if (reply.isError()) qCWarning(lcApp).noquote() << "hardcore: the last unlock was not answered:" << reply.error().message();
}

} // namespace nexa
