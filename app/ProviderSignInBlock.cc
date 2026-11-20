#include "app/ProviderSignInBlock.h"

#include "core/config/Config.h"

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

constexpr int kPollMs = 1500;           // how often a block that waits for a reply to end looks again
constexpr qint64 kFreshMs = 20'000;     // a status check this recent is not repeated by a tab switch

bool isWebUrl(const QUrl &url) {
    return url.isValid() && (url.scheme() == u"https" || url.scheme() == u"http");
}

double luminance(const QColor &c) {
    const auto lin = [](double v) { return v <= 0.03928 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
    return 0.2126 * lin(c.redF()) + 0.7152 * lin(c.greenF()) + 0.0722 * lin(c.blueF());
}

double contrast(const QColor &a, const QColor &b) {
    const double la = luminance(a);
    const double lb = luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

// The theme's tone colours are tuned for the dark ground; on a light one the green and the amber come out
// pale. Moves the colour away from the ground, a step at a time, until the text reads (WCAG 4.5:1).
QColor readableOn(QColor fg, const QColor &ground) {
    const bool darkGround = luminance(ground) < 0.5;
    for (int i = 0; i < 12 && contrast(fg, ground) < 4.5; ++i) fg = darkGround ? fg.lighter(112) : fg.darker(112);
    return fg;
}

} // namespace

ProviderSignInBlock::ProviderSignInBlock(const QString &kind, std::unique_ptr<ISignIn> signIn,
                                         std::function<bool()> replyRunning, QWidget *parent)
    : QWidget(parent),
      kind_(kind),
      signIn_(signIn.release()),
      presenter_(kind),
      replyRunning_(std::move(replyRunning)) {
    setObjectName(u"providerSignIn"_s);
    signIn_->setParent(this);

    status_ = new QLabel(this);
    status_->setObjectName(u"signInStatus"_s);
    status_->setWordWrap(true);
    status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    blockedNote_ = new QLabel(tr("A reply is running — signing in or out waits until it has finished."), this);
    blockedNote_->setObjectName(u"signInBlocked"_s);
    blockedNote_->setWordWrap(true);
    blockedNote_->hide();

    signInButton_ = new QPushButton(tr("Sign in…"), this);
    signInButton_->setObjectName(u"signInButton"_s);
    signOutButton_ = new QPushButton(tr("Sign out"), this);
    signOutButton_->setObjectName(u"signOutButton"_s);
    cancelButton_ = new QPushButton(tr("Cancel"), this);
    cancelButton_->setObjectName(u"signInCancel"_s);
    connect(signInButton_, &QPushButton::clicked, this, &ProviderSignInBlock::startSignIn);
    connect(signOutButton_, &QPushButton::clicked, this, &ProviderSignInBlock::startSignOut);
    connect(cancelButton_, &QPushButton::clicked, this, &ProviderSignInBlock::cancelSignIn);

    hint_ = new QLabel(this);
    hint_->setObjectName(u"signInHint"_s);
    hint_->setWordWrap(true);
    hint_->setTextInteractionFlags(Qt::TextSelectableByMouse);

    // The link is a read-only line, not a label: a login address has no spaces to wrap at, and a line scrolls
    // and selects all of it.
    link_ = new QLineEdit(this);
    link_->setObjectName(u"signInLink"_s);
    link_->setReadOnly(true);
    link_->setAccessibleName(tr("Sign-in link"));
    copyButton_ = new QPushButton(tr("Copy link"), this);
    copyButton_->setObjectName(u"signInCopy"_s);
    connect(copyButton_, &QPushButton::clicked, this, [this] {
        QApplication::clipboard()->setText(link_->text());
    });
    linkRow_ = new QWidget(this);
    auto *linkLayout = new QHBoxLayout(linkRow_);
    linkLayout->setContentsMargins(0, 0, 0, 0);
    linkLayout->addWidget(link_, 1);
    linkLayout->addWidget(copyButton_);

    code_ = new QLineEdit(this);
    code_->setObjectName(u"signInCode"_s);
    code_->setPlaceholderText(tr("Paste the code the browser shows"));
    sendButton_ = new QPushButton(tr("Send code"), this);
    sendButton_->setObjectName(u"signInSendCode"_s);
    connect(sendButton_, &QPushButton::clicked, this, &ProviderSignInBlock::sendCode);
    connect(code_, &QLineEdit::returnPressed, this, &ProviderSignInBlock::sendCode);
    codeRow_ = new QWidget(this);
    auto *codeLayout = new QHBoxLayout(codeRow_);
    codeLayout->setContentsMargins(0, 0, 0, 0);
    codeLayout->addWidget(code_, 1);
    codeLayout->addWidget(sendButton_);

    auto *buttons = new QHBoxLayout;
    buttons->addWidget(signInButton_);
    buttons->addWidget(signOutButton_);
    buttons->addWidget(cancelButton_);
    buttons->addStretch(1);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(status_);
    layout->addWidget(blockedNote_);
    layout->addLayout(buttons);
    layout->addWidget(hint_);
    layout->addWidget(linkRow_);
    layout->addWidget(codeRow_);

    // What the program says goes through the presenter, then to the screen.
    connect(signIn_, &ISignIn::statusChecked, this, [this](const SignInStatus &status) {
        lastCheck_.start();
        presenter_.statusChecked(status);
        render();
    });
    connect(signIn_, &ISignIn::methodsListed, this, [this](const QList<SignInMethod> &methods) {
        methodsKnown_ = true;
        presenter_.methodsListed(methods);
        render();
        if (listing_) {
            listing_ = false;
            chooseMethod();
        }
    });
    connect(signIn_, &ISignIn::linkReady, this, [this](const QUrl &url, const QString &instructions, bool opened) {
        presenter_.linkReady(url, instructions, opened, signIn_->pendingNeedsCode());
        // The presenter keeps a link only for a sign-in that is running: a late one is not opened.
        if (!opened && opensBrowser_ && !url.isEmpty() && !presenter_.view().link.isEmpty() && isWebUrl(url)) {
            if (opener_) {
                opener_(url);
            } else {
                QDesktopServices::openUrl(url);
            }
        }
        render();
    });
    connect(signIn_, &ISignIn::signedIn, this, [this](const SignInStatus &status) {
        lastCheck_.start();
        presenter_.signedIn(status);
        render();
        emit providersChanged();
    });
    connect(signIn_, &ISignIn::signedOut, this, [this] {
        lastCheck_.start();
        presenter_.signedOut();
        render();
        emit providersChanged();
    });
    connect(signIn_, &ISignIn::failed, this, [this](const Error &error) {
        // A refused call (the program is busy) is told apart from the end of an operation by busy() right here.
        const bool stillBusy = signIn_->busy();
        if (!stillBusy) {
            listing_ = false;
            lastCheck_.invalidate();    // a failed step says nothing: the next ask checks again
        }
        presenter_.failed(error, stillBusy);
        render();
    });

    poll_.setInterval(kPollMs);
    connect(&poll_, &QTimer::timeout, this, &ProviderSignInBlock::pollBlocked);
    render();
}

ProviderSignInBlock::~ProviderSignInBlock() = default;

QColor ProviderSignInBlock::toneColor(SignInView::Tone tone) const {
    QColor color = muted_;
    switch (tone) {
    case SignInView::Tone::Good: color = good_; break;
    case SignInView::Tone::Warn: color = warn_; break;
    case SignInView::Tone::Bad: color = bad_; break;
    case SignInView::Tone::Neutral: break;
    }
    return readableOn(color, palette().color(QPalette::Window));
}

bool ProviderSignInBlock::blockedNow() const {
    return replyRunning_ && replyRunning_();
}

void ProviderSignInBlock::setBlocked(bool blocked) {
    blocked_ = blocked;
    // Looking again while it matters: a reply ends without anyone telling this block.
    if (blocked || checkPending_) {
        if (!poll_.isActive()) poll_.start();
    } else {
        poll_.stop();
    }
    render();
}

void ProviderSignInBlock::pollBlocked() {
    if (shut_) return;
    if (blockedNow()) {
        if (!blocked_) setBlocked(true);
        return;
    }
    if (blocked_) setBlocked(false);
    if (checkPending_) requestCheck(true);
    if (!blocked_ && !checkPending_) poll_.stop();
}

void ProviderSignInBlock::requestCheck(bool force) {
    if (shut_ || presenter_.busy()) return;
    if (blockedNow()) {
        checkPending_ = true;
        setBlocked(true);
        return;
    }
    if (blocked_) setBlocked(false);
    if (!force && lastCheck_.isValid() && lastCheck_.elapsed() < kFreshMs) return;
    checkPending_ = false;
    presenter_.checkStarted();
    render();
    signIn_->checkStatus();
}

void ProviderSignInBlock::startSignIn() {
    if (shut_ || presenter_.busy() || blockedNow()) return;
    if (!methodsKnown_) {
        listing_ = true;
        presenter_.listStarted();
        render();
        signIn_->listMethods();
        return;
    }
    chooseMethod();
}

void ProviderSignInBlock::chooseMethod() {
    const QList<SignInMethod> methods = presenter_.view().methods;
    if (methods.isEmpty() && kind_ == kKindOpenCode) {
        // opencode's sign-ins are the ones it lists; a blank one is a choice it can only refuse. (agy and Codex
        // have a way in of their own that a blank id means.)
        presenter_.noMethodsOffered();
        render();
        return;
    }
    if (methods.size() <= 1) {
        beginSignIn(methods.isEmpty() ? QString() : methods.first().id);
        return;
    }
    // Several ways in (opencode's providers): ask which, under the button.
    if (menu_) menu_->deleteLater();
    menu_ = new QMenu(this);
    for (const SignInMethod &method : methods) {
        menu_->addAction(method.label, this, [this, id = method.id] { beginSignIn(id); });
    }
    connect(menu_, &QMenu::aboutToHide, menu_, &QObject::deleteLater);
    menu_->popup(signInButton_->mapToGlobal(QPoint(0, signInButton_->height())));
}

void ProviderSignInBlock::beginSignIn(const QString &methodId) {
    if (shut_ || presenter_.busy() || blockedNow()) return;
    presenter_.signInStarted(methodId);
    render();
    signIn_->signIn(methodId);
}

void ProviderSignInBlock::startSignOut() {
    if (shut_ || presenter_.busy() || blockedNow() || !presenter_.view().canSignOut) return;
    const QString warning = presenter_.signOutWarning();
    const bool yes = confirmer_ ? confirmer_(warning)
                                : QMessageBox::question(this, tr("Sign out"), warning,
                                                        QMessageBox::Yes | QMessageBox::No, QMessageBox::No) ==
                                      QMessageBox::Yes;
    // The question is modal: what it was about may have changed while it was up.
    if (!yes || shut_ || presenter_.busy() || blockedNow()) return;
    presenter_.signOutStarted();
    render();
    signIn_->signOut();
}

void ProviderSignInBlock::cancelSignIn() {
    if (shut_ || !presenter_.view().canCancel) return;
    signIn_->cancel();      // ends the program and answers with failed(Cancelled)
    // Should nothing have been said, the row must not stay busy.
    if (presenter_.busy() && !signIn_->busy()) {
        presenter_.cancelled();
        render();
    }
}

void ProviderSignInBlock::sendCode() {
    const QString text = code_->text().trimmed();
    if (shut_ || text.isEmpty() || !presenter_.view().showCode) return;
    presenter_.codeSubmitted();
    code_->clear();
    render();
    signIn_->submitCode(text);
}

void ProviderSignInBlock::shutdown() {
    if (shut_) return;
    shut_ = true;
    poll_.stop();
    disconnect(signIn_, nullptr, this, nullptr);
    if (signIn_->busy()) signIn_->cancel();
    if (menu_) menu_->close();
    hide();
    deleteLater();
}

void ProviderSignInBlock::render() {
    if (!status_) return;
    const SignInView &view = presenter_.view();
    status_->setText(view.statusText.isEmpty() ? tr("Sign-in not checked yet") : view.statusText);
    status_->setStyleSheet(u"color: %1; font-weight: 600;"_s.arg(
        toneColor(view.statusText.isEmpty() ? SignInView::Tone::Neutral : view.tone).name()));
    blockedNote_->setVisible(blocked_);
    blockedNote_->setStyleSheet(u"color: %1;"_s.arg(muted_.name()));

    signInButton_->setEnabled(view.canSignIn && !blocked_);
    signOutButton_->setEnabled(view.canSignOut && !blocked_);
    cancelButton_->setVisible(view.canCancel);

    const bool haveLink = !view.link.isEmpty();
    hint_->setVisible(!view.linkHint.isEmpty());
    hint_->setText(view.linkHint);
    hint_->setStyleSheet(u"color: %1;"_s.arg(muted_.name()));
    linkRow_->setVisible(haveLink);
    if (link_->text() != view.link) {
        link_->setText(view.link);
        link_->setCursorPosition(0);
    }
    codeRow_->setVisible(view.showCode);
    sendButton_->setEnabled(view.showCode);
}

} // namespace nexa
