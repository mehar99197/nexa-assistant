#include "app/SkillsSettingsPage.h"

#include "core/skills/SkillCatalog.h"

#include <QCheckBox>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

constexpr int kNameRole = Qt::UserRole;

} // namespace

SkillsSettingsPage::SkillsSettingsPage(const SkillsConfig &config, SkillCatalog *catalog, QWidget *parent)
    : QWidget(parent), catalog_(catalog), disabled_(config.disabled) {
    enabled_ = new QCheckBox(tr("Use skills — instructions for particular jobs the model reads when one fits"), this);
    enabled_->setChecked(config.enabled);

    list_ = new QListWidget(this);
    list_->setMinimumWidth(220);
    connect(list_, &QListWidget::currentRowChanged, this, &SkillsSettingsPage::showSkill);
    connect(list_, &QListWidget::itemChanged, this, [this](QListWidgetItem *item) {
        const QString name = item->data(kNameRole).toString();
        disabled_.removeAll(name);
        if (item->checkState() != Qt::Checked) disabled_.append(name);
    });

    description_ = new QLabel(this);
    description_->setWordWrap(true);
    editor_ = new QPlainTextEdit(this);
    editor_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    editor_->setLineWrapMode(QPlainTextEdit::WidgetWidth);

    saveButton_ = new QPushButton(tr("&Save"), this);
    copyButton_ = new QPushButton(tr("&Copy to my skills"), this);
    deleteButton_ = new QPushButton(tr("&Delete"), this);
    auto *newButton = new QPushButton(tr("&New…"), this);
    auto *importButton = new QPushButton(tr("&Import…"), this);
    importButton->setToolTip(tr("Copy a skill folder in — a Claude skill, for instance (a folder with a SKILL.md)."));
    auto *folderButton = new QPushButton(tr("Open &folder"), this);
    connect(saveButton_, &QPushButton::clicked, this, &SkillsSettingsPage::save);
    connect(copyButton_, &QPushButton::clicked, this, &SkillsSettingsPage::copyToMine);
    connect(deleteButton_, &QPushButton::clicked, this, &SkillsSettingsPage::remove);
    connect(newButton, &QPushButton::clicked, this, &SkillsSettingsPage::create);
    connect(importButton, &QPushButton::clicked, this, &SkillsSettingsPage::import);
    connect(folderButton, &QPushButton::clicked, this, [this] {
        if (!catalog_) return;
        QDir().mkpath(catalog_->userDir());
        QDesktopServices::openUrl(QUrl::fromLocalFile(catalog_->userDir()));
    });
    for (QPushButton *button : {newButton, importButton, folderButton}) button->setEnabled(catalog_ != nullptr);

    message_ = new QLabel(this);
    message_->setWordWrap(true);
    message_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    problems_ = new QLabel(this);
    problems_->setWordWrap(true);
    problems_->setTextInteractionFlags(Qt::TextSelectableByMouse);

    auto *leftButtons = new QHBoxLayout;
    leftButtons->addWidget(newButton);
    leftButtons->addWidget(importButton);
    auto *left = new QVBoxLayout;
    left->addWidget(list_, 1);
    left->addLayout(leftButtons);
    left->addWidget(folderButton);

    auto *rightButtons = new QHBoxLayout;
    rightButtons->addWidget(saveButton_);
    rightButtons->addWidget(copyButton_);
    rightButtons->addStretch(1);
    rightButtons->addWidget(deleteButton_);
    auto *right = new QVBoxLayout;
    right->addWidget(description_);
    right->addWidget(editor_, 1);
    right->addLayout(rightButtons);

    auto *columns = new QHBoxLayout;
    columns->addLayout(left);
    columns->addLayout(right, 1);

    auto *help = new QLabel(
        tr("The model sees each skill's name and description with every request and reads the rest when a request "
           "fits it. Your skills live in %1, one folder each with a SKILL.md (the same format as Claude's skills); "
           "one with the name of Nexa's own replaces it. Ask Nexa to \"make a skill for …\" and it writes one "
           "(in confirm-risky mode it asks first). Skills only instruct: anything they run still goes through the "
           "policy.").arg(catalog_ ? catalog_->userDir() : SkillCatalog::defaultUserDir()),
        this);
    help->setWordWrap(true);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(enabled_);
    layout->addLayout(columns, 1);
    layout->addWidget(message_);
    layout->addWidget(problems_);
    layout->addSpacing(6);
    layout->addWidget(help);

    if (catalog_) catalog_->refresh();
    populate();
}

void SkillsSettingsPage::store(SkillsConfig &config) const {
    config.enabled = enabled_->isChecked();
    QStringList disabled = disabled_;
    disabled.removeDuplicates();
    config.disabled = disabled;
}

void SkillsSettingsPage::populate(const QString &select) {
    const QString keep = select.isEmpty() ? selectedName() : select;
    {
        const QSignalBlocker blocker(list_);
        list_->clear();
        if (catalog_) {
            for (const Skill &skill : catalog_->skills()) {
                auto *item = new QListWidgetItem(u"%1  ·  %2"_s.arg(skill.name(), skill.builtIn ? tr("Nexa's") : tr("yours")), list_);
                item->setData(kNameRole, skill.name());
                item->setToolTip(skill.document.description);
                item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
                item->setCheckState(disabled_.contains(skill.name()) ? Qt::Unchecked : Qt::Checked);
                if (skill.name() == keep) list_->setCurrentItem(item);
            }
        }
    }
    if (!list_->currentItem() && list_->count() > 0) list_->setCurrentRow(0);
    showSkill();

    QStringList skipped;
    if (catalog_) {
        for (const SkillCatalog::Problem &problem : catalog_->problems()) skipped.append(u"%1 — %2"_s.arg(problem.path, problem.reason));
    }
    problems_->setText(skipped.isEmpty() ? QString() : tr("Not read: %1").arg(skipped.join(u"; "_s)));
    problems_->setVisible(!skipped.isEmpty());
}

QString SkillsSettingsPage::selectedName() const {
    const QListWidgetItem *item = list_->currentItem();
    return item ? item->data(kNameRole).toString() : QString();
}

void SkillsSettingsPage::showSkill() {
    const Skill *skill = catalog_ ? catalog_->find(selectedName()) : nullptr;
    editor_->setEnabled(skill != nullptr);
    editor_->setPlainText(skill ? catalog_->text(*skill) : QString());
    editor_->setReadOnly(!skill || skill->builtIn);
    description_->setText(!skill          ? tr("No skill selected.")
                          : skill->builtIn ? tr("Nexa's own skill, read-only — copy it to your skills to change it.")
                                           : tr("Your skill, in %1.").arg(skill->directory));
    saveButton_->setEnabled(skill && !skill->builtIn);
    copyButton_->setVisible(skill && skill->builtIn);
    deleteButton_->setEnabled(skill && !skill->builtIn);
}

void SkillsSettingsPage::say(const QString &text, bool error) {
    message_->setText(error ? u"⚠ "_s + text : text);
}

void SkillsSettingsPage::save() {
    const QString name = selectedName();
    const Result<Skill> saved = catalog_->write(editor_->toPlainText(), name);
    if (!saved) {
        say(saved.error().message, true);
        return;
    }
    say(tr("Saved %1.").arg(name));
    populate(name);
}

void SkillsSettingsPage::create() {
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("New skill"),
                                               tr("Its name — lower-case words joined by hyphens, e.g. morning-routine:"),
                                               QLineEdit::Normal, QString(), &ok).trimmed();
    if (!ok || name.isEmpty()) return;
    if (!SkillParser::isValidName(name)) {
        say(tr("\"%1\" is not a skill name: lower-case letters, digits and hyphens.").arg(name), true);
        return;
    }
    if (const Skill *existing = catalog_->find(name); existing && !existing->builtIn) {
        say(tr("You already have a skill named %1.").arg(name), true);
        return;
    }
    const QString text = SkillParser::compose(SkillDocument{
        name, tr("What this skill does, and when to use it."),
        tr("# Steps\n\n1. …\n2. …\n\nName the tools to use (browser_navigate, run_command, open_app, …).")});
    const Result<Skill> written = catalog_->write(text);
    if (!written) {
        say(written.error().message, true);
        return;
    }
    say(tr("Made %1: write its description and steps, then Save.").arg(name));
    populate(name);
    editor_->setFocus();
}

void SkillsSettingsPage::import() {
    const QString start = QDir::homePath() + u"/.claude/skills"_s;
    const QString folder = QFileDialog::getExistingDirectory(this, tr("Import a skill folder"),
                                                             QDir(start).exists() ? start : QDir::homePath());
    if (folder.isEmpty()) return;
    Result<Skill> imported = catalog_->importFolder(folder);
    if (!imported && imported.error().message.contains(u"already have"_s)) {
        if (QMessageBox::question(this, tr("Replace skill"), imported.error().message + u"\n\n"_s + tr("Replace it?"))
            != QMessageBox::Yes) {
            return;
        }
        imported = catalog_->importFolder(folder, true);
    }
    if (!imported) {
        say(imported.error().message, true);
        return;
    }
    say(tr("Imported %1.").arg(imported.value().name()));
    populate(imported.value().name());
}

void SkillsSettingsPage::copyToMine() {
    const QString name = selectedName();
    const Result<Skill> copied = catalog_->copyToUser(name);
    if (!copied) {
        say(copied.error().message, true);
        return;
    }
    say(tr("%1 is now yours to change; Nexa's own comes back if you delete it.").arg(name));
    populate(name);
}

void SkillsSettingsPage::remove() {
    const QString name = selectedName();
    if (QMessageBox::question(this, tr("Delete skill"), tr("Delete the skill %1 and its folder?").arg(name)) != QMessageBox::Yes) {
        return;
    }
    const Result<void> removed = catalog_->remove(name);
    if (!removed) {
        say(removed.error().message, true);
        return;
    }
    say(tr("Deleted %1.").arg(name));
    populate();
}

} // namespace nexa
