#include "core/ai/SignInPresenter.h"

#include "core/config/Config.h"

#include "QtStringMakers.h"

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace Qt::StringLiterals;

namespace {

using Tone = SignInView::Tone;

SignInStatus signedInAs(const QString &detail = {}) {
    return SignInStatus{SignInStatus::State::SignedIn, detail};
}
SignInStatus signedOutStatus() { return SignInStatus{SignInStatus::State::SignedOut, {}}; }

SignInPresenter signedInPresenter(const QString &kind, const QString &detail = u"Plan"_s) {
    SignInPresenter presenter(kind);
    presenter.checkStarted();
    presenter.statusChecked(signedInAs(detail));
    return presenter;
}

Error error(ErrorCode code, const QString &message = u"boom"_s) { return Error{code, message}; }

} // namespace

TEST_CASE("A fresh sign-in row has nothing to say and no button lit") {
    const SignInPresenter presenter{QString(kKindCodex)};
    CHECK(presenter.view().statusText.isEmpty());
    CHECK(presenter.view().tone == Tone::Neutral);
    CHECK_FALSE(presenter.busy());
    CHECK(presenter.view().canSignIn);   // unknown state: signing in is the way forward
    CHECK_FALSE(presenter.view().canSignOut);
    CHECK_FALSE(presenter.view().canCancel);
    CHECK_FALSE(presenter.view().showCode);
}

TEST_CASE("A status check shows Checking and holds every button") {
    SignInPresenter presenter{QString(kKindCodex)};
    presenter.checkStarted();
    CHECK(presenter.view().statusText == u"Checking…"_s);
    CHECK(presenter.view().tone == Tone::Neutral);
    CHECK(presenter.busy());
    CHECK(presenter.view().busy);
    CHECK_FALSE(presenter.view().canSignIn);
    CHECK_FALSE(presenter.view().canSignOut);
    CHECK_FALSE(presenter.view().canCancel);
}

TEST_CASE("Signed in shows the plan, omits the separator without one, and lights Sign out only") {
    SignInPresenter presenter{QString(kKindCodex)};
    presenter.checkStarted();
    presenter.statusChecked(signedInAs(u"ChatGPT · Free"_s));
    CHECK(presenter.view().statusText == u"Signed in · ChatGPT · Free"_s);
    CHECK(presenter.view().tone == Tone::Good);
    CHECK_FALSE(presenter.busy());
    CHECK_FALSE(presenter.view().canSignIn);
    CHECK(presenter.view().canSignOut);

    SignInPresenter bare{QString(kKindAgy)};
    bare.checkStarted();
    bare.statusChecked(signedInAs());
    CHECK(bare.view().statusText == u"Signed in"_s);
}

TEST_CASE("Signed out says so, and agy adds that the keyring may be locked") {
    SignInPresenter codex{QString(kKindCodex)};
    codex.checkStarted();
    codex.statusChecked(signedOutStatus());
    CHECK(codex.view().statusText == u"Not signed in"_s);
    CHECK(codex.view().tone == Tone::Warn);
    CHECK(codex.view().canSignIn);
    CHECK_FALSE(codex.view().canSignOut);

    SignInPresenter agy{QString(kKindAgy)};
    agy.checkStarted();
    agy.statusChecked(signedOutStatus());
    CHECK(agy.view().statusText == u"Not signed in, or the keyring is locked"_s);
    CHECK(agy.view().tone == Tone::Warn);
    CHECK(agy.view().canSignIn);
}

TEST_CASE("An unknown status is not read as signed out") {
    SignInPresenter presenter{QString(kKindOpenCode)};
    presenter.checkStarted();
    presenter.statusChecked(SignInStatus{});
    CHECK(presenter.view().statusText != u"Not signed in"_s);
    CHECK(presenter.view().tone == Tone::Warn);
    CHECK(presenter.view().canSignIn);
    CHECK_FALSE(presenter.view().canSignOut);
}

TEST_CASE("A failed status check names the reason and frees the buttons") {
    SignInPresenter presenter{QString(kKindCodex)};
    presenter.checkStarted();
    presenter.checkFailed(error(ErrorCode::NotFound));
    CHECK(presenter.view().statusText == u"The program is not installed"_s);
    CHECK(presenter.view().tone == Tone::Bad);
    CHECK_FALSE(presenter.busy());
    CHECK(presenter.view().canSignIn);

    SignInPresenter parse{QString(kKindAgy)};
    parse.checkStarted();
    parse.checkFailed(error(ErrorCode::Parse));
    CHECK(parse.view().statusText.contains(u"The program answered something Nexa does not understand"_s));
    CHECK(parse.view().tone == Tone::Bad);
    // Not "signed out": a reply that was not understood says nothing about the sign-in.
    CHECK_FALSE(parse.view().statusText.contains(u"keyring"_s));
}

TEST_CASE("A check that failed leaves a known sign-in as it was") {
    SignInPresenter presenter = signedInPresenter(QString(kKindCodex), u"Plus"_s);
    presenter.checkStarted();
    presenter.checkFailed(error(ErrorCode::Timeout));
    CHECK(presenter.view().tone == Tone::Bad);
    // The last known state still decides the buttons: Nexa has not heard that it signed out.
    CHECK(presenter.view().canSignOut);
    CHECK_FALSE(presenter.view().canSignIn);
}

TEST_CASE("Listing the methods keeps them for the Sign in menu") {
    SignInPresenter presenter{QString(kKindOpenCode)};
    presenter.listStarted();
    CHECK(presenter.busy());
    CHECK_FALSE(presenter.view().canSignIn);
    presenter.methodsListed({SignInMethod{u"github-copilot#0"_s, u"GitHub Copilot"_s, false},
                             SignInMethod{u"openai#0"_s, u"ChatGPT Pro/Plus"_s, false}});
    CHECK_FALSE(presenter.busy());
    REQUIRE(presenter.view().methods.size() == 2);
    CHECK(presenter.view().methods.at(1).label == u"ChatGPT Pro/Plus"_s);
    CHECK(presenter.view().canSignIn);
}

TEST_CASE("Signing in: the link is shown as text whether or not the browser was opened") {
    SignInPresenter presenter{QString(kKindCodex)};
    presenter.signInStarted();
    CHECK(presenter.busy());
    CHECK(presenter.view().canCancel);
    CHECK_FALSE(presenter.view().canSignIn);
    CHECK_FALSE(presenter.view().canSignOut);
    CHECK(presenter.view().link.isEmpty());
    CHECK(presenter.view().statusText == u"Starting sign-in…"_s);

    presenter.linkReady(QUrl(u"https://auth.example/login?state=abc"_s), u"Sign in with ChatGPT"_s, false);
    CHECK(presenter.view().statusText == u"Waiting for you to finish in the browser…"_s);
    CHECK(presenter.view().link == u"https://auth.example/login?state=abc"_s);
    CHECK(presenter.view().linkHint.contains(u"Sign in with ChatGPT"_s));
    CHECK(presenter.view().canCancel);
    CHECK_FALSE(presenter.view().showCode);
    // The link is for the screen only: never in the status line.
    CHECK_FALSE(presenter.view().statusText.contains(u"auth.example"_s));

    SignInPresenter opened{QString(kKindCodex)};
    opened.signInStarted();
    opened.linkReady(QUrl(u"https://auth.example/x"_s), {}, true);
    CHECK(opened.view().link == u"https://auth.example/x"_s);   // still selectable and copyable
    CHECK(opened.view().linkHint != presenter.view().linkHint); // but it says the browser was opened
}

TEST_CASE("agy opens the browser itself and may never send a link") {
    SignInPresenter presenter{QString(kKindAgy)};
    presenter.signInStarted();
    CHECK(presenter.view().statusText == u"Waiting for you to finish in the browser…"_s);
    CHECK(presenter.view().link.isEmpty());
    CHECK(presenter.view().linkHint.contains(u"browser"_s));
    CHECK(presenter.view().canCancel);
    CHECK(presenter.busy());

    presenter.linkReady(QUrl(), {}, true);
    CHECK(presenter.view().link.isEmpty());
    CHECK(presenter.view().statusText == u"Waiting for you to finish in the browser…"_s);
}

TEST_CASE("A code flow shows the code field, from the link signal or from the method") {
    SignInPresenter presenter{QString(kKindOpenCode)};
    presenter.signInStarted(u"openai#0"_s);
    CHECK_FALSE(presenter.view().showCode);
    presenter.linkReady(QUrl(u"https://auth.example/c"_s), u"Paste the code"_s, false, true);
    CHECK(presenter.view().showCode);
    CHECK(presenter.view().canCancel);

    presenter.codeSubmitted();
    CHECK_FALSE(presenter.view().showCode);
    CHECK(presenter.busy());
    CHECK(presenter.view().canCancel);

    SignInPresenter byMethod{QString(kKindOpenCode)};
    byMethod.methodsListed({SignInMethod{u"x#0"_s, u"X"_s, true}});
    byMethod.signInStarted(u"x#0"_s);
    CHECK(byMethod.view().showCode);
}

TEST_CASE("Signed in after a sign-in clears the link and the code field") {
    SignInPresenter presenter{QString(kKindOpenCode)};
    presenter.signInStarted(u"openai#0"_s);
    presenter.linkReady(QUrl(u"https://auth.example/c"_s), u"Paste"_s, false, true);
    presenter.signedIn(signedInAs(u"ChatGPT"_s));
    CHECK(presenter.view().statusText == u"Signed in · ChatGPT"_s);
    CHECK(presenter.view().tone == Tone::Good);
    CHECK(presenter.view().link.isEmpty());
    CHECK(presenter.view().linkHint.isEmpty());
    CHECK_FALSE(presenter.view().showCode);
    CHECK_FALSE(presenter.view().canCancel);
    CHECK_FALSE(presenter.busy());
    CHECK(presenter.view().canSignOut);
}

TEST_CASE("A failed sign-in says why, drops the link and allows another try") {
    struct Row { ErrorCode code; QString text; Tone tone; };
    const QList<Row> rows = {
        {ErrorCode::Unauthorized, u"Sign-in was not completed"_s, Tone::Bad},
        {ErrorCode::Cancelled, u"Sign-in cancelled"_s, Tone::Warn},
        {ErrorCode::Timeout, u"Sign-in timed out"_s, Tone::Bad},
        {ErrorCode::NotFound, u"The program is not installed"_s, Tone::Bad},
        {ErrorCode::NotSupported, u"The program does not support this"_s, Tone::Bad},
        {ErrorCode::Parse, u"The program answered something Nexa does not understand"_s, Tone::Bad},
    };
    for (const Row &row : rows) {
        CAPTURE(int(row.code));
        SignInPresenter presenter{QString(kKindCodex)};
        presenter.signInStarted();
        presenter.linkReady(QUrl(u"https://auth.example/l"_s), u"hi"_s, false);
        presenter.failed(error(row.code), false);
        CHECK(presenter.view().statusText == row.text);
        CHECK(presenter.view().tone == row.tone);
        CHECK(presenter.view().link.isEmpty());
        CHECK_FALSE(presenter.view().canCancel);
        CHECK_FALSE(presenter.busy());
        CHECK(presenter.view().canSignIn);
    }
}

TEST_CASE("Every error code has a sentence, none of them carrying the program's own words") {
    const QList<ErrorCode> codes = {ErrorCode::Unknown, ErrorCode::InvalidArgument, ErrorCode::NotFound,
                                    ErrorCode::NotSupported, ErrorCode::Io, ErrorCode::Parse, ErrorCode::Network,
                                    ErrorCode::ConnectionRefused, ErrorCode::Timeout, ErrorCode::Http,
                                    ErrorCode::Unauthorized, ErrorCode::PermissionDenied, ErrorCode::Cancelled};
    for (const ErrorCode code : codes) {
        CAPTURE(int(code));
        CHECK_FALSE(SignInPresenter::failureText(code).isEmpty());
        CHECK_FALSE(SignInPresenter::failureText(code, true).isEmpty());
    }
    const QString secret = u"https://evil.example/cb?code=SECRETCODE token=sk-abc user@mail.example"_s;
    for (const ErrorCode code : codes) {
        SignInPresenter p{QString(kKindAgy)};
        p.signInStarted();
        p.failed(Error{code, secret}, false);
        CHECK_FALSE(p.view().statusText.contains(u"SECRETCODE"_s));
        CHECK_FALSE(p.view().statusText.contains(u"evil.example"_s));
        CHECK_FALSE(p.view().statusText.contains(u"sk-abc"_s));
        CHECK_FALSE(p.view().statusText.contains(u"@"_s));
        p.signInStarted();
        p.failed(Error{code, secret}, false);
        SignInPresenter c{QString(kKindAgy)};
        c.checkStarted();
        c.checkFailed(Error{code, secret});
        CHECK_FALSE(c.view().statusText.contains(u"SECRETCODE"_s));
        CHECK_FALSE(c.view().statusText.contains(u"@"_s));
    }
}

TEST_CASE("Network failures are described as not reaching the program") {
    CHECK(SignInPresenter::failureText(ErrorCode::Network).contains(u"reach"_s));
    CHECK(SignInPresenter::failureText(ErrorCode::ConnectionRefused).contains(u"reach"_s));
}

TEST_CASE("cancelled() ends a sign-in as a warning, not an error") {
    SignInPresenter presenter{QString(kKindAgy)};
    presenter.signInStarted();
    presenter.cancelled();
    CHECK(presenter.view().statusText == u"Sign-in cancelled"_s);
    CHECK(presenter.view().tone == Tone::Warn);
    CHECK_FALSE(presenter.busy());
    CHECK(presenter.view().canSignIn);
    CHECK_FALSE(presenter.view().canCancel);
}

TEST_CASE("A call refused because the program is busy does not end the running sign-in") {
    SignInPresenter presenter{QString(kKindCodex)};
    presenter.signInStarted();
    presenter.linkReady(QUrl(u"https://auth.example/l"_s), u"hi"_s, false);
    const SignInView before = presenter.view();

    presenter.failed(error(ErrorCode::Unknown, u"Another Codex sign-in step is still running"_s), true);
    CHECK(presenter.busy());
    CHECK(presenter.view().statusText == before.statusText);
    CHECK(presenter.view().link == before.link);
    CHECK(presenter.view().canCancel);
    CHECK_FALSE(presenter.view().canSignIn);

    // A real failure that comes later still ends it.
    presenter.failed(error(ErrorCode::Timeout), false);
    CHECK_FALSE(presenter.busy());
    CHECK(presenter.view().statusText == u"Sign-in timed out"_s);
}

TEST_CASE("A refused call while a status check runs leaves Checking up") {
    SignInPresenter presenter{QString(kKindOpenCode)};
    presenter.checkStarted();
    presenter.failed(error(ErrorCode::InvalidArgument), true);
    CHECK(presenter.busy());
    CHECK(presenter.view().statusText == u"Checking…"_s);
}

TEST_CASE("opencode reports Unknown and then fails when its server will not start") {
    SignInPresenter presenter{QString(kKindOpenCode)};
    presenter.checkStarted();
    presenter.statusChecked(SignInStatus{});
    presenter.failed(error(ErrorCode::NotFound), false);
    CHECK(presenter.view().statusText == u"The program is not installed"_s);
    CHECK(presenter.view().tone == Tone::Bad);
    CHECK_FALSE(presenter.busy());
}

TEST_CASE("A failure while checking reads as a failed check") {
    SignInPresenter presenter{QString(kKindCodex)};
    presenter.checkStarted();
    presenter.failed(error(ErrorCode::Timeout), false);
    CHECK(presenter.view().statusText.contains(u"check"_s));
    CHECK_FALSE(presenter.busy());
}

TEST_CASE("Signing out: Sign out is lit only when signed in, and the result follows") {
    SignInPresenter presenter = signedInPresenter(QString(kKindCodex), u"Plus"_s);
    REQUIRE(presenter.view().canSignOut);
    presenter.signOutStarted();
    CHECK(presenter.busy());
    CHECK(presenter.view().statusText == u"Signing out…"_s);
    CHECK_FALSE(presenter.view().canSignOut);
    CHECK_FALSE(presenter.view().canSignIn);
    CHECK_FALSE(presenter.view().canCancel);

    presenter.signedOut();
    CHECK(presenter.view().statusText == u"Not signed in"_s);
    CHECK(presenter.view().tone == Tone::Warn);
    CHECK_FALSE(presenter.busy());
    CHECK(presenter.view().canSignIn);
    CHECK_FALSE(presenter.view().canSignOut);
}

TEST_CASE("A sign-out that failed keeps the sign-in and says why") {
    SignInPresenter presenter = signedInPresenter(QString(kKindOpenCode), u"xAI"_s);
    presenter.signOutStarted();
    presenter.failed(error(ErrorCode::Network), false);
    CHECK(presenter.view().tone == Tone::Bad);
    CHECK(presenter.view().statusText.contains(u"sign out"_s));
    CHECK_FALSE(presenter.busy());
    CHECK(presenter.view().canSignOut);
    CHECK_FALSE(presenter.view().canSignIn);
}

TEST_CASE("opencode that cannot sign out stops offering it") {
    SignInPresenter presenter = signedInPresenter(QString(kKindOpenCode), u"xAI"_s);
    presenter.signOutStarted();
    presenter.failed(error(ErrorCode::NotSupported), false);
    CHECK_FALSE(presenter.view().canSignOut);
    CHECK_FALSE(presenter.view().canSignIn);   // still signed in
    CHECK_FALSE(presenter.busy());
    // A later check does not bring the button back.
    presenter.checkStarted();
    presenter.statusChecked(signedInAs(u"xAI"_s));
    CHECK_FALSE(presenter.view().canSignOut);
}

TEST_CASE("Sign out is not offered when the sign-in is not known") {
    SignInPresenter presenter{QString(kKindAgy)};
    CHECK_FALSE(presenter.view().canSignOut);
    presenter.checkStarted();
    presenter.checkFailed(error(ErrorCode::Timeout));
    CHECK_FALSE(presenter.view().canSignOut);
}

TEST_CASE("The sign-out confirmation says what else is signed out") {
    const QString agy = SignInPresenter::signOutWarning(u"agy"_s, {});
    CHECK(agy.contains(u"terminal"_s));
    CHECK(agy.contains(u"keyring"_s));

    const QString codex = SignInPresenter::signOutWarning(u"codex"_s, u"ChatGPT · Plus"_s);
    CHECK(codex.contains(u"terminal"_s));
    CHECK(codex.contains(u"Codex"_s));

    const QString opencode = SignInPresenter::signOutWarning(u"opencode"_s, u"ChatGPT, xAI"_s);
    CHECK(opencode.contains(u"ChatGPT, xAI"_s));
    CHECK(opencode.contains(u"API key"_s));
    CHECK(opencode.contains(u"terminal"_s));
    CHECK(opencode.contains(u"every"_s));

    // Without a detail the list is left out, not shown empty.
    const QString bare = SignInPresenter::signOutWarning(u"opencode"_s, {});
    CHECK_FALSE(bare.contains(u"()"_s));
    CHECK_FALSE(bare.contains(u": ."_s));

    SignInPresenter presenter = signedInPresenter(QString(kKindOpenCode), u"xAI"_s);
    CHECK(presenter.signOutWarning().contains(u"xAI"_s));
}

TEST_CASE("Tone is never the only carrier: every state writes words") {
    SignInPresenter presenter{QString(kKindAgy)};
    presenter.checkStarted();
    CHECK_FALSE(presenter.view().statusText.isEmpty());
    presenter.statusChecked(signedOutStatus());
    CHECK_FALSE(presenter.view().statusText.isEmpty());
    presenter.signInStarted();
    CHECK_FALSE(presenter.view().statusText.isEmpty());
    presenter.failed(error(ErrorCode::Cancelled), false);
    CHECK_FALSE(presenter.view().statusText.isEmpty());
    presenter.checkStarted();
    presenter.statusChecked(signedInAs());
    CHECK_FALSE(presenter.view().statusText.isEmpty());
}

TEST_CASE("A new sign-in after a failure starts clean") {
    SignInPresenter presenter{QString(kKindOpenCode)};
    presenter.signInStarted(u"a#0"_s);
    presenter.linkReady(QUrl(u"https://auth.example/c"_s), u"x"_s, false, true);
    presenter.failed(error(ErrorCode::Unauthorized), false);
    presenter.signInStarted(u"b#0"_s);
    CHECK(presenter.view().link.isEmpty());
    CHECK_FALSE(presenter.view().showCode);
    CHECK(presenter.busy());
}

TEST_CASE("A program that offers no browser sign-in says so instead of refusing a blank one") {
    SignInPresenter presenter{QString(kKindOpenCode)};
    presenter.checkStarted();
    presenter.statusChecked(signedOutStatus());
    presenter.listStarted();
    presenter.methodsListed({});
    presenter.noMethodsOffered();
    CHECK_FALSE(presenter.busy());
    CHECK(presenter.view().statusText.contains(u"no browser sign-in"_s));
    CHECK(presenter.view().tone == Tone::Warn);
    CHECK(presenter.view().canSignIn);      // the list may change: another try asks again
    CHECK_FALSE(presenter.view().canSignOut);
}

TEST_CASE("Signed in with trouble says both, in the warning colour, and keeps Sign out") {
    SignInPresenter presenter{QString(kKindCodex)};
    presenter.checkStarted();
    SignInStatus status = signedInAs(u"ChatGPT · Free"_s);
    status.trouble = u"Codex's own settings file is invalid (~/.codex/config.toml, line 149)"_s;
    presenter.statusChecked(status);
    CHECK(presenter.view().statusText.startsWith(u"Signed in · ChatGPT · Free"_s));
    CHECK(presenter.view().statusText.contains(u"line 149"_s));
    CHECK(presenter.view().tone == Tone::Warn);
    CHECK(presenter.view().canSignOut);
    CHECK_FALSE(presenter.view().canSignIn);
}
