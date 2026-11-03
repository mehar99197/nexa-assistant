#include "app/MemorySettingsPage.h"

#include <QCheckBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

constexpr int kIdRole = Qt::UserRole;

QListWidgetItem *itemFor(const QString &text, const QString &id, const QString &tooltip, QListWidget *list) {
    auto *item = new QListWidgetItem(text, list);
    item->setData(kIdRole, id);
    item->setToolTip(tooltip);
    item->setFlags(item->flags() | Qt::ItemIsEditable);
    return item;
}

} // namespace

MemorySettingsPage::MemorySettingsPage(const MemoryConfig &config, MemoryStore *memory, QWidget *parent)
    : QWidget(parent), memory_(memory) {
    enabled_ = new QCheckBox(tr("Remember things about me between conversations"), this);
    enabled_->setChecked(config.enabled);
    learn_ = new QCheckBox(tr("Let Nexa save what it learns on its own (otherwise only what I ask it to remember)"), this);
    learn_->setChecked(config.learn);
    learn_->setEnabled(config.enabled);
    connect(enabled_, &QCheckBox::toggled, learn_, &QCheckBox::setEnabled);

    list_ = new QListWidget(this);
    list_->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    list_->setWordWrap(true);
    if (memory_) {
        shown_ = memory_->entries();
        for (const MemoryEntry &entry : shown_) {
            itemFor(entry.text, entry.id,
                    tr("%1 · %2").arg(entry.origin == u"learned"_s ? tr("learned by Nexa") : tr("you asked"),
                                      entry.updated.toString(u"d MMM yyyy, HH:mm"_s)),
                    list_);
        }
    }

    auto *add = new QPushButton(tr("&Add"), this);
    auto *remove = new QPushButton(tr("&Delete"), this);
    auto *clear = new QPushButton(tr("Clear &all"), this);
    connect(add, &QPushButton::clicked, this, [this] {
        QListWidgetItem *item = itemFor(QString(), QString(), tr("new"), list_);
        list_->setCurrentItem(item);
        list_->editItem(item);
        showCount();
    });
    connect(remove, &QPushButton::clicked, this, [this] {
        for (QListWidgetItem *item : list_->selectedItems()) {
            const QString id = item->data(kIdRole).toString();
            if (!id.isEmpty()) removed_.append(id);
            delete item;
        }
        showCount();
    });
    connect(clear, &QPushButton::clicked, this, [this] {
        if (list_->count() == 0) return;
        if (QMessageBox::question(this, tr("Clear memory"), tr("Forget all %n thing(s) Nexa remembers about you?", nullptr,
                                                                list_->count()))
            != QMessageBox::Yes) {
            return;
        }
        while (list_->count() > 0) {
            QListWidgetItem *item = list_->takeItem(0);
            if (const QString id = item->data(kIdRole).toString(); !id.isEmpty()) removed_.append(id);
            delete item;
        }
        showCount();
    });
    for (QPushButton *button : {add, remove, clear}) button->setEnabled(memory_ != nullptr);

    count_ = new QLabel(this);
    count_->setWordWrap(true);
    showCount();

    auto *buttons = new QHBoxLayout;
    buttons->addWidget(add);
    buttons->addWidget(remove);
    buttons->addStretch(1);
    buttons->addWidget(clear);

    auto *help = new QLabel(
        tr("Say \"remember that …\" or \"forget that …\", or ask \"what do you know about me?\". Nexa never keeps "
           "passwords, PINs, keys or card numbers. Everything here goes to the AI provider with each request, as "
           "background about you. Double-click a line to correct it; changes take effect when you press OK."),
        this);
    help->setWordWrap(true);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(enabled_);
    layout->addWidget(learn_);
    layout->addWidget(list_, 1);
    layout->addLayout(buttons);
    layout->addWidget(count_);
    layout->addSpacing(6);
    layout->addWidget(help);
}

void MemorySettingsPage::showCount() {
    count_->setText(memory_ ? tr("%n thing(s) remembered · kept in %1", nullptr, list_->count()).arg(MemoryStore::defaultPath())
                            : tr("Memory is not available in this copy."));
}

void MemorySettingsPage::store(MemoryConfig &config) const {
    config.enabled = enabled_->isChecked();
    config.learn = learn_->isChecked();
}

MemoryStore::Edits MemorySettingsPage::edits() const {
    MemoryStore::Edits edits;
    edits.removed = removed_;
    for (int row = 0; row < list_->count(); ++row) {
        const QListWidgetItem *item = list_->item(row);
        const QString id = item->data(kIdRole).toString();
        const QString text = item->text().simplified();
        if (id.isEmpty()) {
            if (!text.isEmpty()) edits.added.append(text);
            continue;
        }
        for (const MemoryEntry &entry : shown_) {
            if (entry.id != id) continue;
            if (text.isEmpty()) {
                edits.removed.append(id);       // emptied: as good as deleted
            } else if (text != entry.text) {
                MemoryEntry changed = entry;
                changed.text = text;
                edits.changed.append(changed);
            }
        }
    }
    return edits;
}

Result<void> MemorySettingsPage::applyEdits() const {
    if (!memory_) return {};
    return memory_->applyEdits(edits());
}

} // namespace nexa
