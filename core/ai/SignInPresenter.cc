#include "core/ai/SignInPresenter.h"

#include "core/config/Config.h"

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

using Tone = SignInView::Tone;

// The reason alone, in words of Nexa's own. Error::message is never used: it may hold a link, a code or an
// account.
QString reason(ErrorCode code) {
    switch (code) {
    case ErrorCode::NotFound: return u"The program is not installed"_s;
    case ErrorCode::NotSupported: return u"The program does not support this"_s;
    case ErrorCode::Parse: return u"The program answered something Nexa does not understand"_s;
    case ErrorCode::Network:
    case ErrorCode::ConnectionRefused: return u"Could not reach the program"_s;
    case ErrorCode::Timeout: return u"The program did not answer in time"_s;
    case ErrorCode::Http: return u"The program answered with an error"_s;
    case ErrorCode::Io: return u"Could not run the program"_s;
    case ErrorCode::PermissionDenied: return u"Nexa is not allowed to run the program"_s;
    case ErrorCode::InvalidArgument: return u"The program refused the request"_s;
    case ErrorCode::Unauthorized: return u"The program is not signed in"_s;
    case ErrorCode::Cancelled: return u"Cancelled"_s;
    case ErrorCode::Unknown: break;
    }
    return u"Something went wrong"_s;
}

QString signedInText(const QString &detail) {
    return detail.isEmpty() ? u"Signed in"_s : u"Signed in · "_s + detail;
}

} // namespace

SignInPresenter::SignInPresenter(QString kind) : kind_(std::move(kind)) {
    refresh();
}

QString SignInPresenter::failureText(ErrorCode code, bool forCheck) {
    if (forCheck) {
        if (code == ErrorCode::NotFound) return reason(code);
        if (code == ErrorCode::Cancelled) return u"Check cancelled"_s;
        return u"Could not check the sign-in: "_s + reason(code);
    }
    switch (code) {
    case ErrorCode::Unauthorized: return u"Sign-in was not completed"_s;
    case ErrorCode::Cancelled: return u"Sign-in cancelled"_s;
    case ErrorCode::Timeout: return u"Sign-in timed out"_s;
    default: return reason(code);
    }
}

QString SignInPresenter::signOutWarning(const QString &kind, const QString &detail) {
    if (kind == kKindAgy) {
        return u"Signing out here also signs you out of agy in your terminal: they share the system keyring. "
               "Sign out?"_s;
    }
    if (kind == kKindCodex) {
        return u"Signing out here also signs you out of Codex in your terminal: they share Codex's sign-in "
               "folder. Sign out?"_s;
    }
    QString text = u"Signing out removes the stored sign-in of every connected provider, any API key you added "
                   "to opencode included, and signs you out of opencode in your terminal too."_s;
    if (!detail.isEmpty()) text += u" Connected: "_s + detail + u"."_s;
    return text + u" Sign out?"_s;
}

void SignInPresenter::setText(const QString &text, Tone tone) {
    view_.statusText = text;
    view_.tone = tone;
}

void SignInPresenter::clearLink() {
    view_.link.clear();
    view_.linkHint.clear();
    codeNeeded_ = false;
    codeSent_ = false;
    methodId_.clear();
}

void SignInPresenter::endOp() {
    op_ = Op::None;
    clearLink();
}

void SignInPresenter::refresh() {
    const bool idle = op_ == Op::None;
    view_.busy = !idle;
    view_.canSignIn = idle && state_ != SignInStatus::State::SignedIn;
    view_.canSignOut = idle && state_ == SignInStatus::State::SignedIn && signOutSupported_;
    view_.canCancel = op_ == Op::SigningIn;
    view_.showCode = op_ == Op::SigningIn && codeNeeded_ && !codeSent_;
}

void SignInPresenter::checkStarted() {
    op_ = Op::Checking;
    setText(u"Checking…"_s, Tone::Neutral);
    refresh();
}

void SignInPresenter::listStarted() {
    op_ = Op::Listing;
    refresh();
}

void SignInPresenter::signInStarted(const QString &methodId) {
    clearLink();
    op_ = Op::SigningIn;
    methodId_ = methodId;
    for (const SignInMethod &method : view_.methods) {
        if (method.id == methodId && method.needsCode) codeNeeded_ = true;
    }
    if (kind_ == kKindAgy) {
        // agy opens the browser itself and may never send a link.
        setText(u"Waiting for you to finish in the browser…"_s, Tone::Neutral);
        view_.linkHint = u"Finish signing in in your browser."_s;
    } else {
        setText(u"Starting sign-in…"_s, Tone::Neutral);
    }
    refresh();
}

void SignInPresenter::codeSubmitted() {
    if (op_ != Op::SigningIn) return;
    codeSent_ = true;
    setText(u"Checking the code…"_s, Tone::Neutral);
    refresh();
}

void SignInPresenter::signOutStarted() {
    op_ = Op::SigningOut;
    setText(u"Signing out…"_s, Tone::Neutral);
    refresh();
}

void SignInPresenter::statusChecked(const SignInStatus &status) {
    if (op_ == Op::SigningIn || op_ == Op::SigningOut) return;     // not the answer to what is running
    endOp();
    state_ = status.state;
    detail_ = status.state == SignInStatus::State::SignedIn ? status.detail : QString();
    switch (status.state) {
    case SignInStatus::State::SignedIn: setText(signedInText(detail_), Tone::Good); break;
    case SignInStatus::State::SignedOut:
        // A keyring that is locked or out of reach looks the same to agy as no sign-in at all.
        setText(kind_ == kKindAgy ? u"Not signed in, or the keyring is locked"_s : u"Not signed in"_s, Tone::Warn);
        break;
    case SignInStatus::State::Unknown: setText(u"Could not check the sign-in"_s, Tone::Warn); break;
    }
    refresh();
}

void SignInPresenter::checkFailed(const Error &error) {
    endOp();
    // The last known state stays: a check that failed says nothing about the sign-in itself.
    setText(failureText(error.code, true), error.code == ErrorCode::Cancelled ? Tone::Warn : Tone::Bad);
    refresh();
}

void SignInPresenter::methodsListed(const QList<SignInMethod> &methods) {
    view_.methods = methods;
    if (op_ == Op::Listing) op_ = Op::None;
    refresh();
}

void SignInPresenter::noMethodsOffered() {
    if (op_ != Op::None) return;
    setText(u"This program has no browser sign-in to offer"_s, Tone::Warn);
    refresh();
}

void SignInPresenter::linkReady(const QUrl &url, const QString &instructions, bool browserOpened, bool needsCode) {
    if (op_ != Op::SigningIn) return;
    if (needsCode) codeNeeded_ = true;
    setText(u"Waiting for you to finish in the browser…"_s, Tone::Neutral);
    QString hint;
    if (url.isEmpty()) {
        view_.link.clear();
        hint = u"Finish signing in in your browser."_s;
    } else {
        view_.link = url.toString(QUrl::FullyEncoded);
        hint = browserOpened ? u"Your browser was opened to finish signing in. If it did not open, use this link:"_s
                             : u"Open this link in your browser to sign in:"_s;
    }
    const QString words = instructions.trimmed();
    if (!words.isEmpty()) hint += u"\n"_s + words;
    view_.linkHint = hint;
    refresh();
}

void SignInPresenter::signedIn(const SignInStatus &status) {
    endOp();
    state_ = SignInStatus::State::SignedIn;
    detail_ = status.detail;
    setText(signedInText(detail_), Tone::Good);
    refresh();
}

void SignInPresenter::signedOut() {
    endOp();
    state_ = SignInStatus::State::SignedOut;
    detail_.clear();
    setText(u"Not signed in"_s, Tone::Warn);
    refresh();
}

void SignInPresenter::failed(const Error &error, bool stillBusy) {
    // A call refused because the program is busy: the operation it interrupted goes on.
    if (stillBusy) return;
    const Op op = op_;
    if (op == Op::Checking) {
        checkFailed(error);
        return;
    }
    endOp();
    const bool cancelled = error.code == ErrorCode::Cancelled;
    if (op == Op::SigningOut) {
        if (error.code == ErrorCode::NotSupported) signOutSupported_ = false;
        setText(cancelled ? failureText(error.code) : u"Could not sign out: "_s + reason(error.code),
                cancelled ? Tone::Warn : Tone::Bad);
    } else {
        setText(failureText(error.code), cancelled ? Tone::Warn : Tone::Bad);
    }
    refresh();
}

void SignInPresenter::cancelled() {
    endOp();
    setText(failureText(ErrorCode::Cancelled), Tone::Warn);
    refresh();
}

} // namespace nexa
