#include "PairPage.h"

#include "remote/RemoteClient.h"
#include "ui/NexaCoreWidget.h"
#include "ui/NexaTheme.h"

#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QScrollArea>
#include <QScroller>
#include <QVBoxLayout>

using namespace Qt::StringLiterals;

namespace nexa {

PairPage::PairPage(QWidget *parent) : QWidget(parent) {
    // Everything sits in a scroll area: in landscape, or with the keyboard up, a phone's screen is
    // shorter than this form, and a word-wrapped label must never be drawn under the field below it.
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    QScroller::grabGesture(scroll->viewport(), QScroller::TouchGesture);
    outer->addWidget(scroll);
    auto *form = new QWidget(scroll);
    scroll->setWidget(form);

    auto *layout = new QVBoxLayout(form);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(12);

    // The core, at rest, says which app this is before a word is read.
    auto *core = new NexaCoreWidget(this);
    core->setShowNodes(true);
    core->setDiameter(140);
    layout->addWidget(core, 0, Qt::AlignHCenter);

    auto *title = new QLabel(tr("Pair with Nexa"), this);
    QFont titleFont = NexaTheme::scaled(title->font(), 1.6);
    titleFont.setBold(true);
    title->setFont(titleFont);
    title->setAlignment(Qt::AlignHCenter);
    layout->addWidget(title);

    auto *hint = new QLabel(tr("On the PC, open Settings → Remote and press \"Pair a phone\". "
                               "Type the address under the QR code and the six-digit code — "
                               "or paste the whole link."), this);
    hint->setWordWrap(true);
    hint->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
    layout->addWidget(hint);

    address_ = new QLineEdit(this);
    address_->setPlaceholderText(tr("192.168.0.10:7777 or the link"));
    address_->setInputMethodHints(Qt::ImhUrlCharactersOnly | Qt::ImhNoAutoUppercase | Qt::ImhNoPredictiveText);
    address_->setClearButtonEnabled(true);
    layout->addWidget(address_);

    code_ = new QLineEdit(this);
    code_->setPlaceholderText(tr("Six-digit code"));
    code_->setInputMethodHints(Qt::ImhDigitsOnly);
    code_->setMaxLength(6);
    code_->setValidator(new QRegularExpressionValidator(QRegularExpression(u"\\d{0,6}"_s), code_));
    layout->addWidget(code_);

    name_ = new QLineEdit(this);
    name_->setPlaceholderText(tr("This phone's name on the PC"));
    layout->addWidget(name_);

    error_ = new QLabel(this);
    error_->setWordWrap(true);
    error_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);    // wrapped: as tall as it needs
    error_->setStyleSheet(u"color: %1;"_s.arg(NexaTheme::tokens(ThemeScheme::Dark).error.name()));
    layout->addWidget(error_);

    pair_ = new QPushButton(tr("Pair"), this);
    pair_->setObjectName(u"primary"_s);
    pair_->setDefault(true);
    layout->addWidget(pair_);
    layout->addStretch();

    // A pasted link brings its code along.
    connect(address_, &QLineEdit::textChanged, this, [this](const QString &text) {
        const std::optional<RemoteEndpoint> endpoint = RemoteEndpoint::parse(text);
        if (endpoint && !endpoint->code.isEmpty()) code_->setText(endpoint->code);
    });
    connect(pair_, &QPushButton::clicked, this, &PairPage::submit);
    connect(code_, &QLineEdit::returnPressed, this, &PairPage::submit);
    connect(address_, &QLineEdit::returnPressed, code_, qOverload<>(&QWidget::setFocus));
}

void PairPage::setAddress(const QString &address) {
    address_->setText(address);
}

void PairPage::setDeviceName(const QString &name) {
    name_->setText(name);
}

QString PairPage::deviceName() const {
    return name_->text().trimmed();
}

void PairPage::setBusy(bool busy) {
    pair_->setEnabled(!busy);
    pair_->setText(busy ? tr("Pairing…") : tr("Pair"));
}

void PairPage::setError(const QString &text) {
    error_->setText(text);
}

void PairPage::submit() {
    setError({});
    const QString address = address_->text().trimmed();
    const QString code = code_->text().trimmed();
    if (address.isEmpty()) {
        setError(tr("Where is Nexa? Type the address shown under the QR code."));
        address_->setFocus();
        return;
    }
    if (code.size() != 6) {
        setError(tr("The code has six digits."));
        code_->setFocus();
        return;
    }
    emit pairRequested(address, code, deviceName());
}

} // namespace nexa
