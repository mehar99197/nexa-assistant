#pragma once

#include "core/ai/SignIn.h"
#include "core/ai/SignInPresenter.h"

#include <QColor>
#include <QElapsedTimer>
#include <QPointer>
#include <QTimer>
#include <QWidget>

#include <functional>
#include <memory>

class QLabel;
class QLineEdit;
class QMenu;
class QPushButton;

namespace nexa {

// Mediator between one program's ISignIn, the SignInPresenter that decides what Settings shows of it, and the
// widgets of the sign-in block in Settings → Providers: the status line, Sign in… / Sign out / Cancel, the
// login link with Copy link, and the code field of a "paste the code back" flow. Owns the ISignIn; Settings makes
// one per program row shown and calls shutdown() when another row is shown or the dialog closes.
//
// Rules it keeps (each from what the sign-in classes promise):
//  - a call made while the program is busy is refused with failed() while the operation goes on, so every
//    button is off while busy and the presenter is told whether the ISignIn is still busy when a failure comes;
//  - the code field shows only when the presenter says so, from ISignIn::pendingNeedsCode() as the link arrives;
//  - a link the program did not open itself is opened here (http/https only) and is always shown as selectable
//    text; agy opens the browser itself and may send no link at all;
//  - nothing starts, a status check included, while `replyRunning()` says a chat turn is in flight (opencode's
//    shared event stream and agy's app-data folder must not be disturbed); the block waits, polling gently.
// Nothing is logged: a link, a code and a token never leave the widgets.
//
// Colours: the sheet sets the four tone colours (`nexa--ProviderSignInBlock` in theme.qss); the defaults are the
// dark scheme's tokens.
class ProviderSignInBlock : public QWidget {
    Q_OBJECT
    Q_PROPERTY(QColor goodColor READ goodColor WRITE setGoodColor)
    Q_PROPERTY(QColor warnColor READ warnColor WRITE setWarnColor)
    Q_PROPERTY(QColor badColor READ badColor WRITE setBadColor)
    Q_PROPERTY(QColor mutedColor READ mutedColor WRITE setMutedColor)
public:
    // `kind` is kKindAgy, kKindOpenCode or kKindCodex. `replyRunning` may be empty (never blocked).
    ProviderSignInBlock(const QString &kind, std::unique_ptr<ISignIn> signIn, std::function<bool()> replyRunning,
                        QWidget *parent = nullptr);
    ~ProviderSignInBlock() override;

    QColor goodColor() const { return good_; }
    QColor warnColor() const { return warn_; }
    QColor badColor() const { return bad_; }
    QColor mutedColor() const { return muted_; }
    void setGoodColor(const QColor &color) { good_ = color; render(); }
    void setWarnColor(const QColor &color) { warn_ = color; render(); }
    void setBadColor(const QColor &color) { bad_ = color; render(); }
    void setMutedColor(const QColor &color) { muted_ = color; render(); }

    // What is shown, for the tests and the pictures.
    const SignInView &view() const { return presenter_.view(); }
    bool busy() const { return presenter_.busy(); }

    // Pictures and tests: do not open the login link in the real browser; say yes or no to "Sign out?" for it.
    void setOpensBrowser(bool opens) { opensBrowser_ = opens; }
    // What opens a login link the program did not open itself (QDesktopServices::openUrl unless a test says otherwise).
    void setLinkOpener(std::function<void(const QUrl &)> opener) { opener_ = std::move(opener); }
    void setSignOutConfirmer(std::function<bool(const QString &warning)> confirmer) { confirmer_ = std::move(confirmer); }

    // A status check, unless one is running, a sign-in is under way or a reply is in flight (then it waits for the
    // reply to end). Without `force`, one made within the last 20 s is not repeated.
    void requestCheck(bool force = false);
    // What the Sign in… button does: ask for the methods once, then sign in (a menu when there are several).
    void startSignIn();
    void startSignOut();        // asks first
    void cancelSignIn();
    void sendCode();            // the code field's text
    // Stops everything for good: cancels a running operation, lets go of the program, deletes itself later.
    void shutdown();

signals:
    // A sign-in or sign-out succeeded: the providers should be built again and their models listed.
    void providersChanged();

private:
    bool blockedNow() const;
    void setBlocked(bool blocked);
    void pollBlocked();
    void chooseMethod();
    void beginSignIn(const QString &methodId);
    void render();
    QColor toneColor(SignInView::Tone tone) const;

    QString kind_;
    ISignIn *signIn_{nullptr};
    SignInPresenter presenter_;
    std::function<bool()> replyRunning_;
    std::function<bool(const QString &)> confirmer_;
    std::function<void(const QUrl &)> opener_;

    QLabel *status_{nullptr};
    QLabel *blockedNote_{nullptr};
    QPushButton *signInButton_{nullptr};
    QPushButton *signOutButton_{nullptr};
    QPushButton *cancelButton_{nullptr};
    QLabel *hint_{nullptr};
    QWidget *linkRow_{nullptr};
    QLineEdit *link_{nullptr};
    QPushButton *copyButton_{nullptr};
    QWidget *codeRow_{nullptr};
    QLineEdit *code_{nullptr};
    QPushButton *sendButton_{nullptr};
    QTimer poll_;
    QElapsedTimer lastCheck_;
    QPointer<QMenu> menu_;

    QColor good_{QStringLiteral("#6FD38A")};
    QColor warn_{QStringLiteral("#F0B24B")};
    QColor bad_{QStringLiteral("#FF7A66")};
    QColor muted_{QStringLiteral("#98A4B3")};
    bool blocked_{false};
    bool checkPending_{false};
    bool listing_{false};           // Sign in… was pressed and the methods are being asked for
    bool methodsKnown_{false};
    bool opensBrowser_{true};
    bool shut_{false};
};

} // namespace nexa
