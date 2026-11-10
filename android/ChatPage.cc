#include "ChatPage.h"

#include "MessageDelegate.h"
#include "ui/NexaCoreWidget.h"
#include "ui/NexaIcons.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QScroller>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

constexpr int kPresenceLarge = 200;
constexpr int kPresenceSmall = 56;

void repolish(QWidget *widget) {
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
    widget->update();
}

} // namespace

ChatPage::ChatPage(QWidget *parent) : QWidget(parent), tokens_(NexaTheme::tokens(ThemeScheme::Dark)) {
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // The header: the brand, and a pill saying whether the PC is there.
    auto *header = new QHBoxLayout;
    header->setContentsMargins(16, 10, 10, 6);
    header->setSpacing(8);
    brandCore_ = new NexaCoreWidget(this);
    brandCore_->setShowNodes(false);
    brandCore_->setDiameter(28);
    brandCore_->setAnimated(false);
    header->addWidget(brandCore_);
    auto *brand = new QLabel(tr("Nexa"), this);
    brand->setObjectName(u"brandTitle"_s);
    header->addWidget(brand);
    header->addStretch();
    status_ = new QLabel(this);
    status_->setObjectName(u"statusPill"_s);
    status_->setTextFormat(Qt::RichText);
    header->addWidget(status_);
    more_ = new QToolButton(this);
    more_->setObjectName(u"moreButton"_s);
    more_->setPopupMode(QToolButton::InstantPopup);
    more_->setToolTip(tr("More"));
    auto *menu = new QMenu(more_);
    menu->addAction(tr("Forget this PC"), this, &ChatPage::forgetRequested);
    more_->setMenu(menu);
    header->addWidget(more_);
    layout->addLayout(header);

    // The presence: the core and the state's name, never colour alone.
    presence_ = new QWidget(this);
    presence_->setObjectName(u"presence"_s);
    presenceLayout_ = new QBoxLayout(QBoxLayout::TopToBottom, presence_);
    presenceLayout_->setContentsMargins(16, 8, 16, 12);
    presenceLayout_->setSpacing(10);
    core_ = new NexaCoreWidget(presence_);
    core_->setObjectName(u"core"_s);
    core_->setShowNodes(true);
    core_->setDiameter(kPresenceLarge);
    presenceText_ = new QWidget(presence_);
    auto *textLayout = new QVBoxLayout(presenceText_);
    textLayout->setContentsMargins(0, 0, 0, 0);
    textLayout->setSpacing(2);
    title_ = new QLabel(presenceText_);
    title_->setObjectName(u"stateTitle"_s);
    hint_ = new QLabel(presenceText_);
    hint_->setObjectName(u"stateHint"_s);
    hint_->setWordWrap(true);
    textLayout->addWidget(title_);
    textLayout->addWidget(hint_);
    presenceLayout_->addWidget(core_, 0, Qt::AlignHCenter);
    presenceLayout_->addWidget(presenceText_);
    layout->addWidget(presence_);

    // Hardcore mode on the PC: its keyboard and mouse are locked, and this phone is a way to end it.
    lockBar_ = new QFrame(this);
    lockBar_->setObjectName(u"lockBar"_s);
    auto *lockRow = new QHBoxLayout(lockBar_);
    lockRow->setContentsMargins(12, 10, 12, 10);
    lockRow->setSpacing(10);
    lockText_ = new QLabel(lockBar_);
    lockText_->setObjectName(u"lockText"_s);
    lockText_->setWordWrap(true);
    lockText_->setTextFormat(Qt::PlainText);
    lockRow->addWidget(lockText_, 1);
    unlock_ = new QPushButton(tr("Unlock PC"), lockBar_);
    unlock_->setObjectName(u"unlockButton"_s);
    lockRow->addWidget(unlock_);
    lockBar_->hide();
    auto *lockMargin = new QHBoxLayout;
    lockMargin->setContentsMargins(16, 8, 16, 0);
    lockMargin->addWidget(lockBar_);
    layout->addLayout(lockMargin);
    connect(unlock_, &QPushButton::clicked, this, [this] {
        unlock_->setEnabled(false);
        emit unlockRequested();
    });

    approvals_ = new QWidget(this);
    approvals_->setObjectName(u"approvals"_s);
    approvalsLayout_ = new QVBoxLayout(approvals_);
    approvalsLayout_->setContentsMargins(16, 8, 16, 0);
    approvalsLayout_->setSpacing(8);
    approvals_->hide();
    layout->addWidget(approvals_);

    messages_ = new QListWidget(this);
    messages_->setObjectName(u"messages"_s);
    delegate_ = new MessageDelegate(messages_);
    messages_->setItemDelegate(delegate_);
    messages_->setResizeMode(QListView::Adjust);     // bubbles re-wrap when the width changes
    messages_->setSelectionMode(QAbstractItemView::NoSelection);
    messages_->setFocusPolicy(Qt::NoFocus);
    messages_->setSpacing(2);
    messages_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    messages_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    QScroller::grabGesture(messages_->viewport(), QScroller::TouchGesture);
    layout->addWidget(messages_, 1);

    auto *bottom = new QHBoxLayout;
    bottom->setContentsMargins(16, 8, 16, 8);
    bottom->setSpacing(10);
    input_ = new QLineEdit(this);
    input_->setObjectName(u"phoneInput"_s);
    input_->setPlaceholderText(tr("Type a request"));
    input_->setClearButtonEnabled(true);
    bottom->addWidget(input_, 1);
    send_ = new QToolButton(this);
    send_->setObjectName(u"send"_s);
    send_->setToolTip(tr("Send"));
    send_->setAccessibleName(tr("Send"));
    send_->setIconSize(QSize(24, 24));
    bottom->addWidget(send_);
    layout->addLayout(bottom);

    // Talk: the core again. Tap to listen until the pause; hold for push-to-talk.
    talkRow_ = new QWidget(this);
    auto *talkLayout = new QVBoxLayout(talkRow_);
    talkLayout->setContentsMargins(16, 4, 16, 14);
    talkLayout->setSpacing(6);
    talk_ = new NexaCoreButton(talkRow_);
    talk_->setObjectName(u"talk"_s);
    talk_->setDiameter(76);
    talk_->setAccessibleName(tr("Talk"));
    talkHint_ = new QLabel(tr("Tap to talk. Hold for push-to-talk."), talkRow_);
    talkHint_->setObjectName(u"talkHint"_s);
    talkLayout->addWidget(talk_, 0, Qt::AlignHCenter);
    talkLayout->addWidget(talkHint_, 0, Qt::AlignHCenter);
    talkRow_->hide();      // until Nexa says it can hear
    layout->addWidget(talkRow_);

    connect(send_, &QToolButton::clicked, this, &ChatPage::submit);
    connect(input_, &QLineEdit::returnPressed, this, &ChatPage::submit);
    connect(talk_, &NexaCoreButton::pressed, this, &ChatPage::talkPressed);
    connect(talk_, &NexaCoreButton::released, this, &ChatPage::talkReleased);

    refreshIcons();
    layoutPresence();
    setCore(CoreMode::Offline, NoNode, tr("Not connected to the PC."));
}

void ChatPage::setScheme(ThemeScheme scheme) {
    tokens_ = NexaTheme::tokens(scheme);
    delegate_->setTokens(tokens_);
    refreshIcons();
    setCore(mode_, NoNode, hint_->text());
}

void ChatPage::refreshIcons() {
    send_->setIcon(NexaIcons::icon(NexaIcons::Kind::Send, tokens_.onAccent, 24));
    more_->setIcon(NexaIcons::icon(NexaIcons::Kind::More, tokens_.muted, 20));
}

void ChatPage::setStatus(const QString &text, Status status) {
    QColor dot;
    switch (status) {
    case Status::Ok: dot = tokens_.wake; break;
    case Status::Warn: dot = tokens_.act; break;
    case Status::Recording: dot = tokens_.listen; break;
    case Status::Plain: dot = tokens_.muted; break;
    }
    status_->setText(u"<span style=\"color:%1\">●</span>&nbsp; %2"_s.arg(dot.name(), text.toHtmlEscaped()));
}

void ChatPage::setCore(CoreMode mode, CoreNodes nodes, const QString &hint) {
    mode_ = mode;
    core_->setMode(mode);
    core_->setLitNodes(nodes);
    talk_->setMode(mode);
    brandCore_->setMode(mode);
    title_->setText(mode == CoreMode::Idle ? tr("Ready") : mode == CoreMode::WakeWord ? tr("Wake word on") : coreModeName(mode));
    title_->setStyleSheet(u"color: %1;"_s.arg(core_->colorFor(mode).name()));
    hint_->setText(hint);
}

void ChatPage::setLevel(double level) {
    core_->setLevel(level);
    talk_->setLevel(level);
}

void ChatPage::layoutPresence() {
    const bool compact = messages_->count() > 0;
    if (compact == compact_ && title_->alignment() != Qt::AlignLeft) return;
    compact_ = compact;
    presenceLayout_->setDirection(compact ? QBoxLayout::LeftToRight : QBoxLayout::TopToBottom);
    presenceLayout_->setContentsMargins(16, compact ? 4 : 8, 16, compact ? 8 : 12);
    presenceLayout_->setSpacing(compact ? 14 : 10);
    core_->setDiameter(compact ? kPresenceSmall : kPresenceLarge);
    presenceLayout_->setAlignment(core_, compact ? Qt::AlignVCenter : Qt::AlignHCenter);
    title_->setAlignment(compact ? Qt::AlignLeft : Qt::AlignHCenter);
    hint_->setAlignment(compact ? Qt::AlignLeft : Qt::AlignHCenter);
    QFont titleFont = NexaTheme::scaled(font(), compact ? 1.25 : 1.7);
    titleFont.setBold(true);
    title_->setFont(titleFont);
    presence_->setProperty("compact", compact);
    repolish(presence_);
}

void ChatPage::addMessage(Kind kind, const QString &text) {
    auto *item = new QListWidgetItem(text);
    item->setData(MessageDelegate::KindRole, static_cast<int>(kind));
    messages_->addItem(item);
    messages_->scrollToBottom();
    if (kind == Kind::Nexa) reply_ = item;
    layoutPresence();
}

void ChatPage::appendReply(const QString &text) {
    if (!reply_) {
        addMessage(Kind::Nexa, text);
        return;
    }
    reply_->setText(reply_->text() + text);
    messages_->doItemsLayout();     // the line grew: its bubble-less row needs its new height
    messages_->scrollToBottom();
}

void ChatPage::endReply() {
    reply_ = nullptr;
}

void ChatPage::setBusy(bool busy) {
    busy_ = busy;
    send_->setIcon(NexaIcons::icon(busy ? NexaIcons::Kind::Stop : NexaIcons::Kind::Send,
                                   busy ? tokens_.ground : tokens_.onAccent, 24));
    send_->setToolTip(busy ? tr("Stop") : tr("Send"));
    send_->setProperty("busy", busy);
    repolish(send_);
}

void ChatPage::setVoiceAvailable(bool available) {
    talkRow_->setVisible(available);
}

void ChatPage::setRecording(bool recording) {
    talkHint_->setText(recording ? tr("Listening… let go, or tap, to send") : tr("Tap to talk. Hold for push-to-talk."));
}

void ChatPage::clear() {
    messages_->clear();
    reply_ = nullptr;
    layoutPresence();
}

void ChatPage::submit() {
    if (busy_) {
        emit cancelRequested();
        return;
    }
    const QString text = input_->text().trimmed();
    if (text.isEmpty()) return;
    input_->clear();
    emit askRequested(text);
}

void ChatPage::setLocked(bool locked, const QString &text) {
    lockText_->setText(text.isEmpty() ? tr("Your PC's keyboard and mouse are locked.") : text);
    unlock_->setEnabled(true);
    lockBar_->setVisible(locked);
}

void ChatPage::addApproval(quint64 id, const QString &agent, const QString &tool, const QString &question) {
    if (id == 0) return;
    for (const QFrame *card : approvals_->findChildren<QFrame *>(u"approvalCard"_s, Qt::FindDirectChildrenOnly)) {
        if (card->property("approvalId").toULongLong() == id) return;       // shown already
    }
    auto *card = new QFrame(approvals_);
    card->setObjectName(u"approvalCard"_s);
    card->setProperty("approvalId", QVariant::fromValue(id));
    auto *column = new QVBoxLayout(card);
    column->setContentsMargins(12, 10, 12, 10);
    column->setSpacing(6);
    auto *title = new QLabel(tr("%1 needs you · %2").arg(agent.isEmpty() ? tr("A task") : agent, tool), card);
    title->setObjectName(u"approvalTitle"_s);
    title->setTextFormat(Qt::PlainText);
    auto *text = new QLabel(question, card);
    text->setWordWrap(true);
    text->setTextFormat(Qt::PlainText);
    column->addWidget(title);
    column->addWidget(text);
    auto *row = new QHBoxLayout;
    row->setSpacing(8);
    auto *approve = new QPushButton(tr("Approve"), card);
    approve->setObjectName(u"approve"_s);
    auto *deny = new QPushButton(tr("Deny"), card);
    deny->setObjectName(u"deny"_s);
    row->addWidget(approve, 1);
    row->addWidget(deny, 1);
    column->addLayout(row);
    for (auto [button, allow] : {std::pair{approve, true}, std::pair{deny, false}}) {
        connect(button, &QPushButton::clicked, this, [this, approve, deny, id, allow = allow] {
            approve->setEnabled(false);
            deny->setEnabled(false);
            emit approvalAnswered(id, allow);
        });
    }
    approvalsLayout_->addWidget(card);
    approvals_->show();
}

void ChatPage::settleApproval(quint64 id) {
    bool waiting = false;
    for (QFrame *card : approvals_->findChildren<QFrame *>(u"approvalCard"_s, Qt::FindDirectChildrenOnly)) {
        if (card->property("approvalId").toULongLong() != id) {
            waiting = true;
            continue;
        }
        // Later: this may be the card whose button was just pressed.
        card->setObjectName(QString());
        card->hide();
        card->deleteLater();
    }
    approvals_->setVisible(waiting);
}

void ChatPage::clearApprovals() {
    for (QFrame *card : approvals_->findChildren<QFrame *>(u"approvalCard"_s, Qt::FindDirectChildrenOnly)) delete card;
    approvals_->hide();
}

} // namespace nexa
