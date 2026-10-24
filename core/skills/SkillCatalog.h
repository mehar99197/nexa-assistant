#pragma once

#include "core/Result.h"
#include "core/skills/SkillParser.h"

#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

namespace nexa {

struct Skill {
    SkillDocument document;
    QString directory;          // the folder holding its SKILL.md (":/nexa/skills/<name>" for Nexa's own)
    bool builtIn{false};
    bool enabled{true};

    QString name() const { return document.name; }
};

// Every skill Nexa knows: its own, shipped as a Qt resource, and the user's, one folder each under
// ~/.local/share/nexa/skills (a user's skill with the same name replaces Nexa's). The model sees
// each enabled skill's name and description in the system prompt and reads the rest through
// use_skill; Settings → Skills and save_skill write the user's folders. Skills only instruct — a
// script one mentions still runs through run_command and its policy. (Repository)
class SkillCatalog : public QObject {
    Q_OBJECT
public:
    static constexpr int kMaxImportFiles = 100;
    static constexpr qint64 kMaxImportBytes = 5 << 20;
    static constexpr qint64 kMaxSkillFile = 256 << 10;

    struct Problem {
        QString path;
        QString reason;
    };

    explicit SkillCatalog(QString builtInDir = QStringLiteral(":/nexa/skills"), QString userDir = defaultUserDir(),
                          QObject *parent = nullptr);
    static QString defaultUserDir();
    QString userDir() const { return userDir_; }

    void setDisabled(const QStringList &names);
    // Reads every SKILL.md again: cheap, and it finds a skill written a moment ago.
    void refresh();
    QList<Skill> skills() const { return skills_; }                // by name
    QList<Problem> problems() const { return problems_; }          // files that are not skills, and why
    const Skill *find(const QString &name) const;
    // The other files in a user's skill folder (absolute paths, for read_file), never a link out of it.
    QStringList extraFiles(const Skill &skill) const;
    // The enabled skills' names and descriptions for the system prompt, within `budget` characters;
    // empty when there are none.
    QString promptSection(int budget = 4000) const;
    QString text(const Skill &skill) const;                         // its SKILL.md as written

    // The user's folders. write() saves a SKILL.md (its name must stay `expectedName` when given);
    // importFolder() copies a skill folder in (regular files only, within the limits above);
    // copyToUser() makes Nexa's own skill the user's to change; remove() deletes a user's skill.
    Result<Skill> write(const QString &text, const QString &expectedName = {});
    Result<Skill> importFolder(const QString &source, bool replace = false);
    Result<Skill> copyToUser(const QString &name);
    Result<void> remove(const QString &name);

signals:
    void changed();

private:
    void scan(const QString &root, bool builtIn, QList<Skill> &into);
    QString userFolder(const QString &name) const;

    QString builtInDir_;
    QString userDir_;
    QStringList disabled_;
    QList<Skill> skills_;
    QList<Problem> problems_;
};

} // namespace nexa
