#include "ChatPage.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QScroller>
#include <QToolButton>
#include <QVBoxLayout>

using namespace Qt::StringLiterals;

namespace nexa {

ChatPage::ChatPage(QWidget *parent) : QWidget(parent) {
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);

    auto *top = new QHBoxLayout;
    status_ = new QLabel(tr("offline"), this);
    status_->setWordWrap(true);
    top->addWidget(status_, 1);
    auto *more = new QToolButton(this);
    more->setText(u"⋮"_s);
    more->setPopupMode(QToolButton::InstantPopup);
    auto *menu = new QMenu(more);
    menu->addAction(tr("Forget this PC"), this, &ChatPage::forgetRequested);
    more->setMenu(menu);
    top->addWidget(more);
    layout->addLayout(top);

    messages_ = new QListWidget(this);
    messages_->setWordWrap(true);
    messages_->setTextElideMode(Qt::ElideNone);
    messages_->setSelectionMode(QAbstractItemView::NoSelection);
    messages_->setFocusPolicy(Qt::NoFocus);
    messages_->setSpacing(4);
    messages_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    QScroller::grabGesture(messages_->viewport(), QScroller::TouchGesture);
    layout->addWidget(messages_, 1);

    talk_ = new QPushButton(tr("Hold to talk"), this);
    talk_->setMinimumHeight(56);
    talk_->hide();      // until Nexa says it can hear
    layout->addWidget(talk_);

    auto *bottom = new QHBoxLayout;
    input_ = new QLineEdit(this);
    input_->setPlaceholderText(tr("Ask Nexa…"));
    input_->setClearButtonEnabled(true);
    bottom->addWidget(input_, 1);
    send_ = new QPushButton(u"➤"_s, this);
    send_->setMinimumWidth(56);
    bottom->addWidget(send_);
    layout->addLayout(bottom);

    connect(send_, &QPushButton::clicked, this, &ChatPage::submit);
    connect(input_, &QLineEdit::returnPressed, this, &ChatPage::submit);
    connect(talk_, &QPushButton::pressed, this, &ChatPage::talkPressed);
    connect(talk_, &QPushButton::released, this, &ChatPage::talkReleased);
}

void ChatPage::setStatus(const QString &text, Status status) {
    status_->setText(text);
    QString colour;
    switch (status) {
    case Status::Ok: colour = u"#2e7d32"_s; break;
    case Status::Warn: colour = u"#ef6c00"_s; break;
    case Status::Recording: colour = u"#c62828"_s; break;
    case Status::Plain: colour = u"palette(text)"_s; break;
    }
    status_->setStyleSheet(u"color: %1;"_s.arg(colour));
}

void ChatPage::addMessage(Kind kind, const QString &text) {
    auto *item = new QListWidgetItem(text);
    switch (kind) {
    case Kind::User:
        item->setTextAlignment(Qt::AlignRight);
        item->setBackground(QColor(u"#dcedc8"_s));
        item->setForeground(QColor(u"#1b1b1b"_s));
        break;
    case Kind::FromPc:
        item->setTextAlignment(Qt::AlignRight);
        item->setBackground(QColor(u"#e0e0e0"_s));
        item->setForeground(QColor(u"#1b1b1b"_s));
        break;
    case Kind::Nexa:
        item->setBackground(QColor(u"#ffffff"_s));
        item->setForeground(QColor(u"#1b1b1b"_s));
        break;
    case Kind::Step:
    case Kind::StepFailed:
    case Kind::Heard: {
        QFont font = item->font();
        font.setItalic(true);
        font.setPointSizeF(font.pointSizeF() * 0.9);
        item->setFont(font);
        item->setForeground(QColor(kind == Kind::StepFailed ? u"#c62828"_s : u"#616161"_s));
        break;
    }
    case Kind::Error:
        item->setForeground(QColor(u"#c62828"_s));
        break;
    }
    messages_->addItem(item);
    messages_->scrollToBottom();
    if (kind == Kind::Nexa) reply_ = item;
}

void ChatPage::appendReply(const QString &text) {
    if (!reply_) {
        addMessage(Kind::Nexa, text);
        return;
    }
    reply_->setText(reply_->text() + text);
    messages_->scrollToBottom();
}

void ChatPage::endReply() {
    reply_ = nullptr;
}

void ChatPage::setBusy(bool busy) {
    busy_ = busy;
    send_->setText(busy ? u"■"_s : u"➤"_s);
}

void ChatPage::setVoiceAvailable(bool available) {
    talk_->setVisible(available);
}

void ChatPage::setRecording(bool recording) {
    talk_->setText(recording ? tr("Listening… let go to send") : tr("Hold to talk"));
    talk_->setStyleSheet(recording ? u"background: #c62828; color: white;"_s : QString());
}

void ChatPage::clear() {
    messages_->clear();
    reply_ = nullptr;
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

} // namespace nexa
