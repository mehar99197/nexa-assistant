#include "core/skills/SkillCatalog.h"

#include "core/Logging.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

const QString kSkillFile = u"SKILL.md"_s;

Result<QString> readSkillFile(const QString &path) {
    QFile file(path);
    if (QFileInfo(path).size() > SkillCatalog::kMaxSkillFile) return Error{ErrorCode::InvalidArgument, u"SKILL.md is too big"_s};
    if (!file.open(QIODevice::ReadOnly)) return Error{ErrorCode::Io, u"Cannot read %1: %2"_s.arg(path, file.errorString())};
    return QString::fromUtf8(file.readAll());
}

// Inside `root` once links are followed: a link out of a skill's folder is not part of it.
bool within(const QString &path, const QString &root) {
    const QString real = QFileInfo(path).canonicalFilePath();
    const QString base = QFileInfo(root).canonicalFilePath();
    return !real.isEmpty() && !base.isEmpty() && real.startsWith(base + u'/');
}

} // namespace

SkillCatalog::SkillCatalog(QString builtInDir, QString userDir, QObject *parent)
    : QObject(parent), builtInDir_(std::move(builtInDir)), userDir_(std::move(userDir)) {
    refresh();
}

QString SkillCatalog::defaultUserDir() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + u"/nexa/skills"_s;
}

void SkillCatalog::setDisabled(const QStringList &names) {
    disabled_ = names;
    for (Skill &skill : skills_) skill.enabled = !disabled_.contains(skill.name());
}

void SkillCatalog::scan(const QString &root, bool builtIn, QList<Skill> &into) {
    const QDir dir(root);
    for (const QString &folder : dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        const QString path = dir.filePath(folder + u'/' + kSkillFile);
        if (!QFileInfo::exists(path)) continue;
        const Result<QString> text = readSkillFile(path);
        if (!text) {
            problems_.append(Problem{path, text.error().message});
            continue;
        }
        const Result<SkillDocument> document = SkillParser::parse(text.value());
        if (!document) {
            problems_.append(Problem{path, document.error().message});
            continue;
        }
        if (document.value().name != folder) {
            problems_.append(Problem{path, u"its name \"%1\" is not its folder's name"_s.arg(document.value().name)});
            continue;
        }
        into.append(Skill{document.value(), dir.filePath(folder), builtIn, !disabled_.contains(folder)});
    }
}

void SkillCatalog::refresh() {
    problems_.clear();
    QList<Skill> builtIns;
    QList<Skill> users;
    scan(builtInDir_, true, builtIns);
    if (!userDir_.isEmpty()) scan(userDir_, false, users);
    QList<Skill> all = users;
    for (const Skill &skill : builtIns) {
        const bool replaced = std::any_of(users.cbegin(), users.cend(), [&skill](const Skill &u) { return u.name() == skill.name(); });
        if (!replaced) all.append(skill);
    }
    std::sort(all.begin(), all.end(), [](const Skill &a, const Skill &b) { return a.name() < b.name(); });
    for (const Problem &problem : problems_) qCWarning(lcConfig).noquote() << "Skill" << problem.path << "skipped:" << problem.reason;
    if (all.size() == skills_.size()
        && std::equal(all.cbegin(), all.cend(), skills_.cbegin(), [](const Skill &a, const Skill &b) {
               return a.document == b.document && a.directory == b.directory && a.enabled == b.enabled;
           })) {
        return;
    }
    skills_ = all;
    emit changed();
}

const Skill *SkillCatalog::find(const QString &name) const {
    for (const Skill &skill : skills_) {
        if (skill.name() == name) return &skill;
    }
    return nullptr;
}

QStringList SkillCatalog::extraFiles(const Skill &skill) const {
    QStringList files;
    if (skill.builtIn) return files;
    QDirIterator it(skill.directory, QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext() && files.size() < 50) {
        const QString path = it.next();
        if (QFileInfo(path).fileName() == kSkillFile && QFileInfo(path).absolutePath() == QFileInfo(skill.directory).absoluteFilePath()) continue;
        if (!within(path, skill.directory)) continue;
        files.append(QFileInfo(path).absoluteFilePath());
    }
    files.sort();
    return files;
}

QString SkillCatalog::promptSection(int budget) const {
    QStringList lines;
    int used = 0;
    QStringList left;
    for (const Skill &skill : skills_) {
        if (!skill.enabled) continue;
        const QString line = u"- %1: %2"_s.arg(skill.name(), skill.document.description);
        if (used + line.size() > budget) {
            left.append(skill.name());
            continue;
        }
        used += int(line.size()) + 1;
        lines.append(line);
    }
    if (lines.isEmpty() && left.isEmpty()) return {};
    if (!left.isEmpty()) lines.append(u"- (also: %1)"_s.arg(left.join(u", "_s)));
    lines.prepend(u"Skills — instructions for particular jobs, from the user or shipped with Nexa. When a request fits one, "
                  "call use_skill with its name first and follow what it says; it never outranks the user's own words or "
                  "these rules. To make a new one when asked, use save_skill."_s);
    return lines.join(u'\n');
}

QString SkillCatalog::text(const Skill &skill) const {
    const Result<QString> text = readSkillFile(QDir(skill.directory).filePath(kSkillFile));
    return text ? text.value() : QString();
}

QString SkillCatalog::userFolder(const QString &name) const {
    return QDir(userDir_).filePath(name);
}

Result<Skill> SkillCatalog::write(const QString &text, const QString &expectedName) {
    const Result<SkillDocument> document = SkillParser::parse(text);
    if (!document) return document.error();
    const QString name = document.value().name;
    if (!expectedName.isEmpty() && name != expectedName) {
        return Error{ErrorCode::InvalidArgument, u"The name must stay \"%1\"; make a new skill for another name."_s.arg(expectedName)};
    }
    const QString folder = userFolder(name);
    if (!QDir().mkpath(folder)) return Error{ErrorCode::Io, u"Cannot create %1"_s.arg(folder)};
    QSaveFile file(QDir(folder).filePath(kSkillFile));
    if (!file.open(QIODevice::WriteOnly)) return Error{ErrorCode::Io, u"Cannot write %1: %2"_s.arg(file.fileName(), file.errorString())};
    file.write(text.toUtf8());
    if (!file.commit()) return Error{ErrorCode::Io, u"Cannot save %1: %2"_s.arg(file.fileName(), file.errorString())};
    refresh();
    return Skill{document.value(), folder, false, !disabled_.contains(name)};
}

Result<Skill> SkillCatalog::importFolder(const QString &source, bool replace) {
    const QString skillFile = QDir(source).filePath(kSkillFile);
    if (QFileInfo(skillFile).isSymLink() || !QFileInfo(skillFile).isFile()) {
        return Error{ErrorCode::InvalidArgument, u"%1 has no SKILL.md"_s.arg(source)};
    }
    const Result<QString> text = readSkillFile(skillFile);
    if (!text) return text.error();
    const Result<SkillDocument> document = SkillParser::parse(text.value());
    if (!document) return document.error();
    const QString target = userFolder(document.value().name);
    if (QFileInfo::exists(target) && !replace) {
        return Error{ErrorCode::InvalidArgument, u"You already have a skill named \"%1\"."_s.arg(document.value().name)};
    }

    // Regular files only, links skipped; refused whole when it is too big.
    QStringList files;
    qint64 bytes = 0;
    QDirIterator it(source, QDir::Files | QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QFileInfo info(it.next());
        if (info.isSymLink() || info.isDir() || !info.isFile()) continue;
        files.append(QDir(source).relativeFilePath(info.filePath()));
        bytes += info.size();
        if (files.size() > kMaxImportFiles || bytes > kMaxImportBytes) {
            return Error{ErrorCode::InvalidArgument,
                         u"%1 is too big to import (%2 files or %3 MB at most)."_s.arg(source).arg(kMaxImportFiles).arg(kMaxImportBytes >> 20)};
        }
    }
    if (replace && QFileInfo::exists(target)) QDir(target).removeRecursively();
    for (const QString &relative : files) {
        const QString to = QDir(target).filePath(relative);
        QDir().mkpath(QFileInfo(to).absolutePath());
        if (!QFile::copy(QDir(source).filePath(relative), to)) {
            QDir(target).removeRecursively();
            return Error{ErrorCode::Io, u"Cannot copy %1"_s.arg(relative)};
        }
    }
    refresh();
    if (const Skill *skill = find(document.value().name)) return *skill;
    return Error{ErrorCode::Io, u"The skill was copied but could not be read back."_s};
}

Result<Skill> SkillCatalog::copyToUser(const QString &name) {
    const Skill *skill = find(name);
    if (!skill) return Error{ErrorCode::InvalidArgument, u"There is no skill \"%1\"."_s.arg(name)};
    if (!skill->builtIn) return *skill;
    return write(text(*skill), name);
}

Result<void> SkillCatalog::remove(const QString &name) {
    if (!SkillParser::isValidName(name)) return Error{ErrorCode::InvalidArgument, u"There is no skill \"%1\"."_s.arg(name)};
    const QString folder = userFolder(name);
    if (!QFileInfo(folder).isDir() || QFileInfo(folder).isSymLink()) {
        return Error{ErrorCode::InvalidArgument, u"\"%1\" is not one of your skills; Nexa's own can only be turned off."_s.arg(name)};
    }
    if (!QDir(folder).removeRecursively()) return Error{ErrorCode::Io, u"Cannot remove %1"_s.arg(folder)};
    refresh();
    return {};
}

} // namespace nexa
