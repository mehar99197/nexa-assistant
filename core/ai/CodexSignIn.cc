#include "core/ai/CodexSignIn.h"

#include "core/Logging.h"
#include "core/ai/CodexEvents.h"

#include <QJsonObject>
#include <QPointer>
#include "core/config/Portable.h"

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

constexpr int kCancelGraceMs = 1'000;

} // namespace

CodexSignIn::CodexSignIn(QString program, QString workingDir, QObject *parent)
    : ISignIn(parent), program_(std::move(program)), workingDir_(std::move(workingDir)) {
    if (workingDir_.isEmpty()) {
        workingDir_ = portable::cacheDir() + u"/codex"_s;
    }
    timer_.setSingleShot(true);
    connect(&timer_, &QTimer::timeout, this, [this] {
        if (op_ == Op::None) return;
        const bool login = waitingForLogin_;
        if (login) abandon();       // tell Codex to stop waiting for a sign-in nobody is finishing
        fail(Error{ErrorCode::Timeout, login ? u"Signing in to Codex took longer than %1 minutes — try again"_s.arg(
                                                   std::max(1, loginTimeoutMs_ / 60'000))
                                             : u"Codex did not answer within %1 s"_s.arg(std::max(1, stepTimeoutMs_ / 1000))});
    });
}

CodexSignIn::~CodexSignIn() = default;

void CodexSignIn::failSoon(const Error &error) {
    QTimer::singleShot(0, this, [this, error] { emit failed(error); });
}

bool CodexSignIn::begin(Op op) {
    if (busy_) {
        failSoon(Error{ErrorCode::Unknown, u"Another Codex sign-in step is still running"_s});
        return false;
    }
    if (program_.isEmpty()) {
        failSoon(Error{ErrorCode::NotFound, u"Codex is not installed (looked on PATH, in ~/.local/bin and in ~/.npm-global/bin)"_s});
        return false;
    }
    busy_ = true;
    op_ = op;
    ++serial_;
    loginId_.clear();
    waitingForLogin_ = false;
    connection_ = std::make_unique<CodexConnection>(program_, workingDir_);
    connect(connection_.get(), &CodexConnection::notification, this, &CodexSignIn::onNotification);
    connect(connection_.get(), &CodexConnection::closed, this, &CodexSignIn::onClosed);
    timer_.start(stepTimeoutMs_);
    connection_->start();
    // A program that cannot even start may fail the operation from inside start(): `closed` has come, the
    // failure is out, and there is no server left for the caller to talk to.
    return busy_ && connection_;
}

void CodexSignIn::end() {
    busy_ = false;
    op_ = Op::None;
    waitingForLogin_ = false;
    timer_.stop();
    ++serial_;
    if (!connection_) return;
    connection_->disconnect(this);
    CodexConnection *done = connection_.release();
    done->kill();
    done->deleteLater();
}

void CodexSignIn::fail(const Error &error) {
    if (op_ == Op::None) return;
    end();
    emit failed(error);
}

void CodexSignIn::abandon() {
    if (!connection_) return;
    // Codex hears the cancel before the process goes: a short grace, or its reply, whichever is first.
    CodexConnection *lingering = connection_.release();
    lingering->disconnect(this);
    lingering->setParent(this);
    if (!loginId_.isEmpty()) {
        lingering->call(u"account/login/cancel"_s, CodexProtocol::loginCancelParams(loginId_),
                        [lingering](Result<QJsonObject>) { lingering->deleteLater(); });
    }
    QTimer::singleShot(kCancelGraceMs, lingering, [lingering] { lingering->deleteLater(); });
}

void CodexSignIn::onClosed(const Error &error) {
    if (op_ == Op::None) return;
    fail(error);
}

void CodexSignIn::checkStatus() {
    if (!begin(Op::Status)) return;
    readAccountThen([this](const SignInStatus &status) {
        if (status.state != SignInStatus::State::SignedIn) {
            end();
            emit statusChecked(status);
            return;
        }
        // Signed in is not working: Codex reads its own config.toml for every turn (the account calls do not), and
        // a file it cannot read fails every turn. Reading the config starts none of the servers it names.
        const quint64 serial = serial_;
        connection_->call(u"config/read"_s, QJsonObject{{u"includeLayers"_s, false}},
                          [self = QPointer<CodexSignIn>(this), serial, status](Result<QJsonObject> result) {
                              if (!self || self->serial_ != serial) return;
                              SignInStatus checked = status;
                              if (!result && result.error().code == ErrorCode::InvalidArgument) {
                                  checked.trouble = result.error().message;
                              }
                              self->end();
                              emit self->statusChecked(checked);
                          });
    });
}

void CodexSignIn::readAccountThen(const std::function<void(const SignInStatus &)> &next) {
    const quint64 serial = serial_;
    connection_->call(u"account/read"_s, {}, [self = QPointer<CodexSignIn>(this), serial, next](Result<QJsonObject> result) {
        if (!self || self->serial_ != serial) return;
        if (!result) return self->fail(result.error());
        const Result<CodexAccount> account = CodexProtocol::parseAccount(result.value());
        if (!account) return self->fail(account.error());
        next(CodexProtocol::statusOf(account.value()));
    });
}

void CodexSignIn::listMethods() {
    QTimer::singleShot(0, this, [this] {
        emit methodsListed({SignInMethod{u"chatgpt"_s, u"ChatGPT account"_s, false}});
    });
}

void CodexSignIn::signIn(const QString &methodId) {
    if (!methodId.isEmpty() && methodId != u"chatgpt") {
        failSoon(Error{ErrorCode::InvalidArgument, u"Codex has one way to sign in: with a ChatGPT account"_s});
        return;
    }
    if (!begin(Op::SignIn)) return;
    const quint64 serial = serial_;
    connection_->call(u"account/login/start"_s, CodexProtocol::loginStartParams(),
                      [self = QPointer<CodexSignIn>(this), serial](Result<QJsonObject> result) {
                          if (!self || self->serial_ != serial) return;
                          if (!result) return self->fail(result.error());
                          const Result<CodexLogin> login = CodexProtocol::parseLoginStart(result.value());
                          if (!login) return self->fail(login.error());
                          self->loginId_ = login.value().loginId;
                          self->waitingForLogin_ = true;
                          self->timer_.start(self->loginTimeoutMs_);
                          qCInfo(lcAi) << "codex sign-in: waiting for the browser";      // the link is never logged
                          emit self->linkReady(login.value().authUrl, u"Finish signing in in your browser."_s, false);
                      });
}

void CodexSignIn::onNotification(const CodexMessage &message) {
    if (op_ != Op::SignIn || !waitingForLogin_) return;
    const CodexEvent event = CodexEvents::decode(message);
    if (event.kind != CodexEvent::Kind::LoginCompleted) return;
    if (!event.loginId.isEmpty() && event.loginId != loginId_) return;      // another login's end
    waitingForLogin_ = false;
    if (!event.ok) {
        const QString why = CodexEvents::tidy(event.errorMessage);
        fail(Error{ErrorCode::Unauthorized,
                   why.isEmpty() ? u"Codex sign-in did not finish"_s : u"Codex sign-in did not finish: %1"_s.arg(why)});
        return;
    }
    timer_.start(stepTimeoutMs_);
    readAccountThen([this](const SignInStatus &status) {
        if (status.state != SignInStatus::State::SignedIn) {
            fail(Error{ErrorCode::Unauthorized, u"Codex said the sign-in worked, but no account is signed in"_s});
            return;
        }
        end();
        emit signedIn(status);
    });
}

void CodexSignIn::submitCode(const QString &code) {
    Q_UNUSED(code);
    failSoon(Error{ErrorCode::NotSupported, u"Codex's sign-in has no code to paste: finish it in the browser"_s});
}

void CodexSignIn::signOut() {
    if (!begin(Op::SignOut)) return;
    const quint64 serial = serial_;
    connection_->call(u"account/logout"_s, {}, [self = QPointer<CodexSignIn>(this), serial](Result<QJsonObject> result) {
        if (!self || self->serial_ != serial) return;
        if (!result) return self->fail(result.error());
        self->end();
        emit self->signedOut();
    });
}

void CodexSignIn::cancel() {
    if (op_ == Op::None) return;
    if (waitingForLogin_) abandon();
    fail(Error{ErrorCode::Cancelled, u"Sign-in was cancelled"_s});
}

} // namespace nexa
