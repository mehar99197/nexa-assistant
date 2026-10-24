#pragma once

#include "core/config/Config.h"

#include <QWidget>

class QCheckBox;
class QLabel;
class QListWidget;
class QPlainTextEdit;
class QPushButton;

namespace nexa {

class SkillCatalog;

// Settings → Skills: every skill, Nexa's own and the user's, each with a switch; the SKILL.md of
// the one selected, to edit and save (Nexa's own are read-only — copied to the user's skills to
// change); new, imported (a Claude skill folder, say) and deleted ones. Files change at once; the
// switches take effect on OK.
class SkillsSettingsPage : public QWidget {
    Q_OBJECT
public:
    SkillsSettingsPage(const SkillsConfig &config, SkillCatalog *catalog, QWidget *parent = nullptr);

    void store(SkillsConfig &config) const;

private:
    void populate(const QString &select = {});
    void showSkill();
    QString selectedName() const;
    void save();
    void create();
    void import();
    void copyToMine();
    void remove();
    void say(const QString &text, bool error = false);

    SkillCatalog *catalog_;
    QStringList disabled_;
    QCheckBox *enabled_;
    QListWidget *list_;
    QLabel *description_;
    QPlainTextEdit *editor_;
    QPushButton *saveButton_;
    QPushButton *copyButton_;
    QPushButton *deleteButton_;
    QLabel *message_;
    QLabel *problems_;
};

} // namespace nexa
