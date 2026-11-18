// core/skills/SkillCatalog and the skill tools: Nexa's own skills from the resource, the user's
// folders, which wins, what the prompt shows, and what Settings and save_skill may write.

#include "core/config/Config.h"
#include "core/skills/SkillCatalog.h"
#include "core/skills/SkillTools.h"
#include "core/tools/ToolRegistry.h"

#include "QtStringMakers.h"

#include <QDir>
#include <QFile>
#include <QJsonObject>
#include <QTemporaryDir>

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace Qt::StringLiterals;

namespace {

void writeFile(const QString &path, const QByteArray &content) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly));
    file.write(content);
}

QByteArray skillText(const QString &name, const QString &description, const QString &body = u"Do the thing."_s) {
    return (u"---\nname: "_s + name + u"\ndescription: "_s + description + u"\n---\n"_s + body).toUtf8();
}

Result<ToolResult> call(ToolRegistry &registry, const QString &tool, const QJsonObject &arguments) {
    std::optional<Result<ToolResult>> result;
    registry.find(tool)->invoke(arguments, [&result](Result<ToolResult> r) { result = std::move(r); });
    REQUIRE(result.has_value());
    return *result;
}

} // namespace

TEST_CASE("Nexa's own skills come from the resource, and a user's skill of the same name replaces one") {
    QTemporaryDir user;
    SkillCatalog catalog(u":/nexa/skills"_s, user.path());
    const Skill *browsing = catalog.find(u"web-browsing"_s);
    REQUIRE(browsing != nullptr);
    CHECK(browsing->builtIn);
    CHECK(browsing->document.body.contains(u"browser_snapshot"_s));
    CHECK(catalog.extraFiles(*browsing).isEmpty());

    writeFile(user.filePath(u"web-browsing/SKILL.md"_s), skillText(u"web-browsing"_s, u"Mine instead."_s));
    writeFile(user.filePath(u"groceries/SKILL.md"_s), skillText(u"groceries"_s, u"Orders the weekly shopping."_s));
    writeFile(user.filePath(u"groceries/list.txt"_s), "milk\n");
    writeFile(user.filePath(u"wrong-folder/SKILL.md"_s), skillText(u"other-name"_s, u"x"_s));
    writeFile(user.filePath(u"broken/SKILL.md"_s), "no front matter");
    catalog.refresh();

    REQUIRE(catalog.find(u"web-browsing"_s) != nullptr);
    CHECK_FALSE(catalog.find(u"web-browsing"_s)->builtIn);
    CHECK(catalog.find(u"web-browsing"_s)->document.description == u"Mine instead."_s);
    CHECK(catalog.skills().size() == 2);
    CHECK(catalog.problems().size() == 2);
    const Skill *groceries = catalog.find(u"groceries"_s);
    REQUIRE(groceries != nullptr);
    CHECK(catalog.extraFiles(*groceries) == QStringList{QFileInfo(user.filePath(u"groceries/list.txt"_s)).absoluteFilePath()});
}

TEST_CASE("The prompt lists the skills that are on, within its budget") {
    QTemporaryDir user;
    QTemporaryDir none;
    writeFile(user.filePath(u"alpha/SKILL.md"_s), skillText(u"alpha"_s, u"First."_s));
    writeFile(user.filePath(u"beta/SKILL.md"_s), skillText(u"beta"_s, u"Second."_s));
    writeFile(user.filePath(u"gamma/SKILL.md"_s), skillText(u"gamma"_s, QString(300, u'g')));
    SkillCatalog catalog(none.path(), user.path());
    catalog.setDisabled({u"beta"_s});

    const QString prompt = catalog.promptSection();
    CHECK(prompt.contains(u"use_skill"_s));
    CHECK(prompt.contains(u"- alpha: First."_s));
    CHECK_FALSE(prompt.contains(u"beta"_s));
    CHECK(catalog.find(u"beta"_s) != nullptr);          // still listed, for Settings
    CHECK_FALSE(catalog.find(u"beta"_s)->enabled);

    const QString tight = catalog.promptSection(30);
    CHECK(tight.contains(u"- alpha: First."_s));
    CHECK(tight.contains(u"(also: gamma)"_s));

    catalog.setDisabled({u"alpha"_s, u"beta"_s, u"gamma"_s});
    CHECK(catalog.promptSection().isEmpty());
}

TEST_CASE("Writing, copying, importing and removing the user's skills") {
    QTemporaryDir user;
    QTemporaryDir source;
    SkillCatalog catalog(u":/nexa/skills"_s, user.filePath(u"skills"_s));

    // Nexa's own skill becomes the user's to change; it cannot be removed while it is Nexa's.
    CHECK_FALSE(catalog.remove(u"web-browsing"_s).isOk());
    REQUIRE(catalog.copyToUser(u"web-browsing"_s).isOk());
    CHECK_FALSE(catalog.find(u"web-browsing"_s)->builtIn);
    REQUIRE(catalog.remove(u"web-browsing"_s).isOk());
    CHECK(catalog.find(u"web-browsing"_s)->builtIn);    // Nexa's own is back

    // Settings' editor may not rename a skill by editing its file.
    CHECK_FALSE(catalog.write(QString::fromUtf8(skillText(u"renamed"_s, u"x"_s)), u"original"_s).isOk());
    CHECK_FALSE(catalog.write(u"not a skill"_s).isOk());

    // Import: regular files only; a link out of the folder is left behind.
    writeFile(source.filePath(u"pdf/SKILL.md"_s), skillText(u"pdf"_s, u"PDF jobs."_s));
    writeFile(source.filePath(u"pdf/scripts/fill.py"_s), "print('x')\n");
    writeFile(source.filePath(u"secret.txt"_s), "do not copy");
#ifdef Q_OS_WIN
    REQUIRE(QFile::link(source.filePath(u"secret.txt"_s), source.filePath(u"pdf/linked.lnk"_s)));
    const auto imported = catalog.importFolder(source.filePath(u"pdf"_s));
    REQUIRE(imported.isOk());
    CHECK(QFile::exists(user.filePath(u"skills/pdf/scripts/fill.py"_s)));
    CHECK_FALSE(QFile::exists(user.filePath(u"skills/pdf/linked.lnk"_s)));
#else
    QFile::link(source.filePath(u"secret.txt"_s), source.filePath(u"pdf/linked.txt"_s));
    const auto imported = catalog.importFolder(source.filePath(u"pdf"_s));
    REQUIRE(imported.isOk());
    CHECK(QFile::exists(user.filePath(u"skills/pdf/scripts/fill.py"_s)));
    CHECK_FALSE(QFile::exists(user.filePath(u"skills/pdf/linked.txt"_s)));
#endif
    CHECK_FALSE(catalog.importFolder(source.filePath(u"pdf"_s)).isOk());          // already there
    CHECK(catalog.importFolder(source.filePath(u"pdf"_s), true).isOk());          // unless replacing
    CHECK_FALSE(catalog.importFolder(source.path()).isOk());                       // no SKILL.md

    // Too big: refused whole.
    writeFile(source.filePath(u"huge/SKILL.md"_s), skillText(u"huge"_s, u"Big."_s));
    for (int i = 0; i <= SkillCatalog::kMaxImportFiles; ++i) writeFile(source.filePath(u"huge/f%1"_s.arg(i)), "x");
    CHECK_FALSE(catalog.importFolder(source.filePath(u"huge"_s)).isOk());
    CHECK_FALSE(QFile::exists(user.filePath(u"skills/huge"_s)));
}

TEST_CASE("use_skill reads a skill's instructions, save_skill writes one, both go with the setting") {
    QTemporaryDir user;
    SkillCatalog catalog(u":/nexa/skills"_s, user.path());
    ToolRegistry registry;
    SkillsConfig config;
    syncSkillTools(registry, &catalog, config);
    REQUIRE(registry.contains(u"use_skill"_s));
    CHECK_FALSE(registry.find(u"use_skill"_s)->spec().mutating);
    CHECK(registry.find(u"save_skill"_s)->spec().privileged);
    CHECK_FALSE(registry.find(u"save_skill"_s)->spec().risk.isEmpty());

    const auto read = call(registry, u"use_skill"_s, QJsonObject{{u"name"_s, u"web-browsing"_s}});
    REQUIRE(read.isOk());
    CHECK(read.value().text.contains(u"browser_navigate"_s));
    CHECK_FALSE(call(registry, u"use_skill"_s, QJsonObject{{u"name"_s, u"nope"_s}}).isOk());

    const auto saved = call(registry, u"save_skill"_s, QJsonObject{{u"name"_s, u"morning-routine"_s},
                                                                  {u"description"_s, u"Opens Gmail and the calendar: every morning."_s},
                                                                  {u"instructions"_s, u"1. browser_navigate to mail.google.com"_s}});
    REQUIRE(saved.isOk());
    REQUIRE(catalog.find(u"morning-routine"_s) != nullptr);
    CHECK(catalog.find(u"morning-routine"_s)->document.description == u"Opens Gmail and the calendar: every morning."_s);
    CHECK(call(registry, u"use_skill"_s, QJsonObject{{u"name"_s, u"morning-routine"_s}}).value().text.contains(u"mail.google.com"_s));
    CHECK_FALSE(call(registry, u"save_skill"_s, QJsonObject{{u"name"_s, u"Bad Name"_s}, {u"description"_s, u"x"_s},
                                                            {u"instructions"_s, u"y"_s}}).isOk());

    config.disabled = {u"morning-routine"_s};
    syncSkillTools(registry, &catalog, config);
    CHECK(call(registry, u"use_skill"_s, QJsonObject{{u"name"_s, u"morning-routine"_s}}).error().message.contains(u"turned off"_s));

    config.enabled = false;
    syncSkillTools(registry, &catalog, config);
    CHECK_FALSE(registry.contains(u"use_skill"_s));
    CHECK_FALSE(registry.contains(u"save_skill"_s));
}
