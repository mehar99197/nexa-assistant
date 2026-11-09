#include "app/AddTasksDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

using namespace Qt::StringLiterals;

namespace nexa {

AddTasksDialog::AddTasksDialog(const Config &config, QWidget *parent) : QDialog(parent) {
    setWindowTitle(tr("Add tasks"));
    setObjectName(u"addTasksDialog"_s);
    resize(560, 380);
    auto *column = new QVBoxLayout(this);
    auto *intro = new QLabel(tr("One task per line. Each runs as an agent of its own, at the same time as the others "
                                "up to the slots in Settings → Mission; the rest wait in the queue."),
                             this);
    intro->setWordWrap(true);
    column->addWidget(intro);
    text_ = new QPlainTextEdit(this);
    text_->setObjectName(u"taskLines"_s);
    text_->setPlaceholderText(tr("Check my inbox and draft replies to unread mail\nWrite unit tests for the auth guard\n…"));
    column->addWidget(text_, 1);

    auto *form = new QFormLayout;
    provider_ = new QComboBox(this);
    provider_->setObjectName(u"taskProvider"_s);
    provider_->addItem(tr("Auto — the model each kind of agent is given"), QString());
    for (const ProviderConfig &provider : config.providers) provider_->addItem(provider.name, provider.id);
    model_ = new QLineEdit(this);
    model_->setObjectName(u"taskModel"_s);
    model_->setPlaceholderText(tr("the provider's own default"));
    form->addRow(tr("Provider:"), provider_);
    form->addRow(tr("Model:"), model_);
    column->addLayout(form);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    add_ = buttons->addButton(tr("Add tasks"), QDialogButtonBox::AcceptRole);
    add_->setObjectName(u"addTasks"_s);
    add_->setDefault(true);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    column->addWidget(buttons);

    connect(text_, &QPlainTextEdit::textChanged, this, &AddTasksDialog::refresh);
    connect(provider_, &QComboBox::currentIndexChanged, this, &AddTasksDialog::refresh);
    refresh();
    text_->setFocus();
}

QStringList AddTasksDialog::lines() const {
    QStringList lines;
    for (const QString &line : text_->toPlainText().split(u'\n')) {
        if (!line.trimmed().isEmpty()) lines.append(line.trimmed());
    }
    return lines;
}

std::optional<ModelChoice> AddTasksDialog::model() const {
    const QString provider = provider_->currentData().toString();
    if (provider.isEmpty()) return std::nullopt;
    return ModelChoice{provider, model_->text().trimmed()};
}

void AddTasksDialog::setText(const QString &text) {
    text_->setPlainText(text);
}

void AddTasksDialog::refresh() {
    const qsizetype count = lines().size();
    add_->setText(count == 1 ? tr("Add 1 task") : tr("Add %1 tasks").arg(count));
    add_->setEnabled(count > 0);
    model_->setEnabled(!provider_->currentData().toString().isEmpty());
}

} // namespace nexa
