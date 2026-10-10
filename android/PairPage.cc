#include "PairPage.h"

#include "remote/RemoteClient.h"

#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QVBoxLayout>

using namespace Qt::StringLiterals;

namespace nexa {

PairPage::PairPage(QWidget *parent) : QWidget(parent) {
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 32, 24, 24);
    layout->setSpacing(12);

    auto *title = new QLabel(tr("Pair with Nexa"), this);
    QFont titleFont = title->font();
    titleFont.setPointSizeF(titleFont.pointSizeF() * 1.6);
    titleFont.setBold(true);
    title->setFont(titleFont);
    layout->addWidget(title);

    auto *hint = new QLabel(tr("On the PC, open Settings → Remote and press \"Pair a phone\". "
                               "Type the address under the QR code and the six-digit code — "
                               "or paste the whole link."), this);
    hint->setWordWrap(true);
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
    error_->setStyleSheet(u"color: #c62828;"_s);
    layout->addWidget(error_);

    pair_ = new QPushButton(tr("Pair"), this);
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
