// The sign-in block of Settings → Providers, driven by a scripted ISignIn (no program, no network): that it
// shows what the presenter says, keeps its buttons off while busy or while a reply runs, asks before signing out,
// offers the code field only for a code flow, never takes a refused call for the end of a sign-in, and lets the
// program go on shutdown.

#include "app/ProviderSignInBlock.h"
#include "core/config/Config.h"

#include "QtStringMakers.h"

#include <QApplication>
#include <QClipboard>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QSignalSpy>
#include <QTest>

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace Qt::StringLiterals;

namespace {

// An ISignIn that does nothing by itself: the test says what the program answers.
class ScriptedSignIn : public ISignIn {
public:
    bool busy() const override { return busy_; }
    bool pendingNeedsCode() const override { return needsCode_; }

    void checkStatus() override { ++checks; }
    void listMethods() override { ++lists; }
    void signIn(const QString &methodId) override {
        signIns.append(methodId);
        busy_ = true;
    }
    void submitCode(const QString &code) override { codes.append(code); }
    void signOut() override {
        ++signOuts;
        busy_ = true;
    }
    void cancel() override {
        ++cancels;
        if (!busy_) return;
        busy_ = false;
        emit failed(Error{ErrorCode::Cancelled, u"Cancelled"_s});
    }

    // Answers.
    void answerStatus(SignInStatus::State state, const QString &detail = {}) {
        busy_ = false;
        emit statusChecked(SignInStatus{state, detail});
    }
    void answerMethods(const QList<SignInMethod> &methods) { emit methodsListed(methods); }
    void answerSignedIn(const QString &detail) {
        busy_ = false;
        emit signedIn(SignInStatus{SignInStatus::State::SignedIn, detail});
    }
    void answerSignedOut() {
        busy_ = false;
        emit signedOut();
    }
    void answerFailure(ErrorCode code) {
        busy_ = false;
        emit failed(Error{code, u"https://secret.example/?code=xyz"_s});
    }

    int checks{0};
    int lists{0};
    int signOuts{0};
    int cancels{0};
    QStringList signIns;
    QStringList codes;
    bool busy_{false};
    bool needsCode_{false};
};

struct Rig {
    explicit Rig(const QString &kind, std::function<bool()> replyRunning = {}) {
        auto program = std::make_unique<ScriptedSignIn>();
        fake = program.get();
        block = new ProviderSignInBlock(kind, std::move(program), std::move(replyRunning));
        block->setOpensBrowser(false);
        block->show();
    }
    ~Rig() { delete block; }

    template <class T> T *child(const char *name) const { return block->findChild<T *>(QString::fromLatin1(name)); }
    QString status() const { return child<QLabel>("signInStatus")->text(); }

    ScriptedSignIn *fake{nullptr};      // owned by the block
    ProviderSignInBlock *block{nullptr};
};

} // namespace

TEST_CASE("The sign-in block shows a signed-in status and lights Sign out only") {
    Rig rig{QString(kKindCodex)};
    rig.block->requestCheck();
    CHECK(rig.fake->checks == 1);
    CHECK(rig.status() == u"Checking…"_s);
    CHECK_FALSE(rig.child<QPushButton>("signInButton")->isEnabled());   // busy: every button is off

    rig.fake->answerStatus(SignInStatus::State::SignedIn, u"ChatGPT · Free"_s);
    CHECK(rig.status() == u"Signed in · ChatGPT · Free"_s);
    CHECK_FALSE(rig.child<QPushButton>("signInButton")->isEnabled());
    CHECK(rig.child<QPushButton>("signOutButton")->isEnabled());
    CHECK_FALSE(rig.child<QPushButton>("signInCancel")->isVisible());
}

TEST_CASE("A fresh status check is not repeated by a tab switch, and never runs twice at once") {
    Rig rig{QString(kKindAgy)};
    rig.block->requestCheck();
    rig.block->requestCheck();                       // already checking
    CHECK(rig.fake->checks == 1);
    rig.fake->answerStatus(SignInStatus::State::SignedOut);
    rig.block->requestCheck();                       // just looked
    CHECK(rig.fake->checks == 1);
    rig.block->requestCheck(true);                   // asked for
    CHECK(rig.fake->checks == 2);
}

TEST_CASE("agy's signed-out status says the keyring may be locked") {
    Rig rig{QString(kKindAgy)};
    rig.block->requestCheck();
    rig.fake->answerStatus(SignInStatus::State::SignedOut);
    CHECK(rig.status() == u"Not signed in, or the keyring is locked"_s);
    CHECK(rig.child<QPushButton>("signInButton")->isEnabled());
    CHECK_FALSE(rig.child<QPushButton>("signOutButton")->isEnabled());
}

TEST_CASE("Nothing starts while a reply is running, and the check follows when it ends") {
    bool running = true;
    Rig rig{QString(kKindOpenCode), [&running] { return running; }};
    rig.block->requestCheck();
    CHECK(rig.fake->checks == 0);                    // opencode's event stream is not disturbed
    CHECK(rig.child<QLabel>("signInBlocked")->isVisible());
    CHECK_FALSE(rig.child<QPushButton>("signInButton")->isEnabled());
    rig.block->startSignIn();
    CHECK(rig.fake->lists == 0);
    CHECK(rig.fake->signIns.isEmpty());

    running = false;
    QTest::qWait(1900);                              // the block looks again every 1.5 s
    CHECK(rig.fake->checks == 1);
    CHECK_FALSE(rig.child<QLabel>("signInBlocked")->isVisible());
}

TEST_CASE("Sign in asks for the methods once, signs in with the only one, and shows a link it opens itself") {
    Rig rig{QString(kKindCodex)};
    rig.block->startSignIn();
    CHECK(rig.fake->lists == 1);
    CHECK(rig.fake->signIns.isEmpty());
    rig.fake->answerMethods({SignInMethod{u"chatgpt"_s, u"ChatGPT account"_s, false}});
    REQUIRE(rig.fake->signIns.size() == 1);
    CHECK(rig.fake->signIns.first() == u"chatgpt"_s);
    CHECK(rig.status() == u"Starting sign-in…"_s);
    CHECK(rig.child<QPushButton>("signInCancel")->isVisible());
    CHECK_FALSE(rig.child<QPushButton>("signInButton")->isEnabled());

    emit rig.fake->linkReady(QUrl(u"https://auth.example/login?state=abc"_s), u"Finish in the browser"_s, false);
    CHECK(rig.child<QLineEdit>("signInLink")->isVisible());
    CHECK(rig.child<QLineEdit>("signInLink")->text() == u"https://auth.example/login?state=abc"_s);
    CHECK(rig.child<QLineEdit>("signInLink")->isReadOnly());
    CHECK(rig.child<QLabel>("signInHint")->text().contains(u"Finish in the browser"_s));
    CHECK_FALSE(rig.child<QWidget>("signInCode")->isVisible());     // Codex never asks for a pasted code

    rig.child<QPushButton>("signInCopy")->click();
    CHECK(QApplication::clipboard()->text() == u"https://auth.example/login?state=abc"_s);

    QSignalSpy changed(rig.block, &ProviderSignInBlock::providersChanged);
    rig.fake->answerSignedIn(u"ChatGPT · Plus"_s);
    CHECK(rig.status() == u"Signed in · ChatGPT · Plus"_s);
    CHECK_FALSE(rig.child<QLineEdit>("signInLink")->isVisible());
    CHECK(changed.count() == 1);                      // the chat picks up the new state
}

TEST_CASE("agy that sends no link says to finish in the browser") {
    Rig rig{QString(kKindAgy)};
    rig.block->startSignIn();
    rig.fake->answerMethods({SignInMethod{u"oauth-personal"_s, u"Google account"_s, false}});
    REQUIRE(rig.fake->signIns.size() == 1);
    CHECK(rig.status() == u"Waiting for you to finish in the browser…"_s);
    CHECK(rig.child<QLabel>("signInHint")->text() == u"Finish signing in in your browser."_s);
    CHECK_FALSE(rig.child<QLineEdit>("signInLink")->isVisible());
}

TEST_CASE("A call the program refused while busy does not end the sign-in that is running") {
    Rig rig{QString(kKindOpenCode)};
    rig.block->startSignIn();
    rig.fake->answerMethods({SignInMethod{u"a#0"_s, u"A"_s, false}});
    REQUIRE(rig.fake->signIns.size() == 1);
    // The program answers a second call with failed() while the first goes on: busy() is still true.
    emit rig.fake->failed(Error{ErrorCode::InvalidArgument, u"still busy"_s});
    CHECK(rig.status() == u"Starting sign-in…"_s);
    CHECK(rig.child<QPushButton>("signInCancel")->isVisible());
    // The real end of it: busy no more, and a sentence of Nexa's own, never the program's message.
    rig.fake->answerFailure(ErrorCode::Timeout);
    CHECK(rig.status() == u"Sign-in timed out"_s);
    CHECK_FALSE(rig.status().contains(u"secret"_s));
    CHECK(rig.child<QPushButton>("signInButton")->isEnabled());
}

TEST_CASE("The code field shows only for a code flow, and Send code passes the pasted text on") {
    Rig rig{QString(kKindOpenCode)};
    rig.block->startSignIn();
    rig.fake->answerMethods({SignInMethod{u"x#0"_s, u"X"_s, true}});
    REQUIRE(rig.fake->signIns.size() == 1);
    rig.fake->needsCode_ = true;
    emit rig.fake->linkReady(QUrl(u"https://auth.example/c"_s), {}, false);
    CHECK(rig.child<QLineEdit>("signInCode")->isVisible());
    CHECK(rig.child<QPushButton>("signInSendCode")->isVisible());

    rig.child<QPushButton>("signInSendCode")->click();      // nothing typed: nothing sent
    CHECK(rig.fake->codes.isEmpty());
    rig.child<QLineEdit>("signInCode")->setText(u"  AB-12  "_s);
    rig.child<QPushButton>("signInSendCode")->click();
    REQUIRE(rig.fake->codes.size() == 1);
    CHECK(rig.fake->codes.first() == u"AB-12"_s);
    CHECK(rig.child<QLineEdit>("signInCode")->text().isEmpty());
    CHECK_FALSE(rig.child<QLineEdit>("signInCode")->isVisible());
    CHECK(rig.status() == u"Checking the code…"_s);
}

TEST_CASE("Several methods are offered in a menu, and none is started before one is picked") {
    Rig rig{QString(kKindOpenCode)};
    rig.block->startSignIn();
    rig.fake->answerMethods({SignInMethod{u"openai#0"_s, u"ChatGPT"_s, false}, SignInMethod{u"github#0"_s, u"Copilot"_s, false}});
    CHECK(rig.fake->signIns.isEmpty());
    QMenu *menu = rig.block->findChild<QMenu *>();
    REQUIRE(menu != nullptr);
    REQUIRE(menu->actions().size() == 2);
    CHECK(menu->actions().at(1)->text() == u"Copilot"_s);
    menu->actions().at(1)->trigger();
    REQUIRE(rig.fake->signIns.size() == 1);
    CHECK(rig.fake->signIns.first() == u"github#0"_s);
}

TEST_CASE("Sign out asks first, and says what else it signs out") {
    QString asked;
    bool answer = false;
    Rig rig{QString(kKindAgy)};
    rig.block->setSignOutConfirmer([&](const QString &warning) {
        asked = warning;
        return answer;
    });
    rig.block->requestCheck();
    rig.fake->answerStatus(SignInStatus::State::SignedIn, u"Google AI Pro"_s);

    rig.child<QPushButton>("signOutButton")->click();
    CHECK(asked.contains(u"terminal"_s));
    CHECK(rig.fake->signOuts == 0);                  // answered no

    answer = true;
    QSignalSpy changed(rig.block, &ProviderSignInBlock::providersChanged);
    rig.child<QPushButton>("signOutButton")->click();
    CHECK(rig.fake->signOuts == 1);
    CHECK(rig.status() == u"Signing out…"_s);
    rig.fake->answerSignedOut();
    CHECK(rig.status() == u"Not signed in"_s);
    CHECK(changed.count() == 1);
}

TEST_CASE("Cancel ends a sign-in under way and says so") {
    Rig rig{QString(kKindCodex)};
    rig.block->startSignIn();
    rig.fake->answerMethods({SignInMethod{u"chatgpt"_s, u"ChatGPT"_s, false}});
    rig.child<QPushButton>("signInCancel")->click();
    CHECK(rig.fake->cancels == 1);
    CHECK(rig.status() == u"Sign-in cancelled"_s);
    CHECK(rig.child<QPushButton>("signInButton")->isEnabled());
}

TEST_CASE("Shutting down cancels what runs and lets the program go") {
    auto *rig = new Rig{QString(kKindCodex)};
    ScriptedSignIn *fake = rig->fake;
    rig->block->startSignIn();
    fake->answerMethods({SignInMethod{u"chatgpt"_s, u"ChatGPT"_s, false}});
    QSignalSpy changed(rig->block, &ProviderSignInBlock::providersChanged);
    rig->block->shutdown();
    CHECK(fake->cancels == 1);
    CHECK_FALSE(rig->block->isVisible());
    // Nothing the program says afterwards reaches a block that is going away.
    fake->answerSignedIn(u"Plus"_s);
    CHECK(changed.count() == 0);
    rig->block->requestCheck(true);
    CHECK(fake->checks == 0);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    rig->block = nullptr;                            // deleted by shutdown(): the rig must not delete it again
    delete rig;
}

TEST_CASE("opencode with no sign-in on offer says so and starts nothing") {
    Rig rig{QString(kKindOpenCode)};
    rig.block->startSignIn();
    rig.fake->answerMethods({});
    CHECK(rig.fake->signIns.isEmpty());              // not a blank method id that the program can only refuse
    CHECK(rig.status().contains(u"no browser sign-in"_s));
    CHECK(rig.child<QPushButton>("signInButton")->isEnabled());
    CHECK_FALSE(rig.child<QPushButton>("signInCancel")->isVisible());
}

TEST_CASE("agy that lists no method is still signed in with its default") {
    Rig rig{QString(kKindAgy)};
    rig.block->startSignIn();
    rig.fake->answerMethods({});
    REQUIRE(rig.fake->signIns.size() == 1);
    CHECK(rig.fake->signIns.first().isEmpty());
}

TEST_CASE("A link the program did not open is opened here, once, and only a web address") {
    Rig rig{QString(kKindCodex)};
    QList<QUrl> opened;
    rig.block->setLinkOpener([&opened](const QUrl &url) { opened.append(url); });
    rig.block->setOpensBrowser(true);
    rig.block->startSignIn();
    rig.fake->answerMethods({SignInMethod{u"chatgpt"_s, u"ChatGPT"_s, false}});

    emit rig.fake->linkReady(QUrl(u"https://auth.example/login?state=abc"_s), {}, false);
    REQUIRE(opened.size() == 1);
    CHECK(opened.first() == QUrl(u"https://auth.example/login?state=abc"_s));
    CHECK(rig.child<QLineEdit>("signInLink")->text() == u"https://auth.example/login?state=abc"_s);   // shown all the same

    // A program that opened the browser itself is not helped a second time.
    emit rig.fake->linkReady(QUrl(u"https://auth.example/other"_s), {}, true);
    CHECK(opened.size() == 1);

    // Nothing but http(s) is handed to the desktop: a file or a script address is shown as text and left alone.
    emit rig.fake->linkReady(QUrl(u"file:///etc/passwd"_s), {}, false);
    emit rig.fake->linkReady(QUrl(u"javascript:alert(1)"_s), {}, false);
    CHECK(opened.size() == 1);

    // A link that arrives after the sign-in is over belongs to nothing: not opened, not shown.
    rig.fake->answerSignedIn(u"Plus"_s);
    emit rig.fake->linkReady(QUrl(u"https://auth.example/late"_s), {}, false);
    CHECK(opened.size() == 1);
    CHECK_FALSE(rig.child<QLineEdit>("signInLink")->isVisible());
}

TEST_CASE("With the browser kept shut (the pictures, the tests) no link is opened") {
    Rig rig{QString(kKindCodex)};                    // the rig keeps it shut
    QList<QUrl> opened;
    rig.block->setLinkOpener([&opened](const QUrl &url) { opened.append(url); });
    rig.block->startSignIn();
    rig.fake->answerMethods({SignInMethod{u"chatgpt"_s, u"ChatGPT"_s, false}});
    emit rig.fake->linkReady(QUrl(u"https://auth.example/x"_s), {}, false);
    CHECK(opened.isEmpty());
}
