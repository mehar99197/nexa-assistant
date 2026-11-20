// core/ai/ProgramLookup: where Nexa looks for the programs it drives, against a home folder made up for the test.

#include "core/ai/ProgramLookup.h"

#include "QtStringMakers.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace Qt::StringLiterals;

namespace {

QString programPath(const QString &base) {
#ifdef Q_OS_WIN
    return base + u".cmd"_s;
#else
    return base;
#endif
}

void makeProgram(const QString &path) {
    const QString actual = programPath(path);
    QDir().mkpath(QFileInfo(actual).absolutePath());
    QFile file(actual);
    REQUIRE(file.open(QIODevice::WriteOnly));
#ifdef Q_OS_WIN
    file.write("@echo off\r\n");
#else
    file.write("#!/bin/sh\n");
#endif
    file.close();
    file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
}

void write(const QString &path, const QByteArray &content) {
    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly));
    file.write(content);
}

} // namespace

TEST_CASE("The folders searched are the per-user installers' own, nvm's newest node first") {
    QTemporaryDir home;
    REQUIRE(home.isValid());
    const QString h = home.path();
    QDir().mkpath(h + u"/.nvm/versions/node/v9.11.2/bin"_s);
    QDir().mkpath(h + u"/.nvm/versions/node/v22.11.0/bin"_s);
    QDir().mkpath(h + u"/.nvm/versions/node/v20.1.0/bin"_s);
    write(h + u"/.npmrc"_s, "registry=https://registry.npmjs.org/\nprefix=${HOME}/.npm-packages\n");

    const QStringList dirs = programSearchDirs(h);
    CHECK(dirs.contains(h + u"/.local/bin"_s));
    CHECK(dirs.contains(h + u"/.npm-global/bin"_s));
#ifdef Q_OS_WIN
    CHECK(dirs.contains(h + u"/.npm-packages"_s));
#else
    CHECK(dirs.indexOf(h + u"/.local/bin"_s) == 0);
    CHECK(dirs.contains(h + u"/.npm-packages/bin"_s));       // npm's own prefix, from ~/.npmrc
#endif
    const qsizetype newest = dirs.indexOf(h + u"/.nvm/versions/node/v22.11.0/bin"_s);
    const qsizetype middle = dirs.indexOf(h + u"/.nvm/versions/node/v20.1.0/bin"_s);
    const qsizetype oldest = dirs.indexOf(h + u"/.nvm/versions/node/v9.11.2/bin"_s);
    REQUIRE(newest >= 0);
    CHECK(newest < middle);
    CHECK(middle < oldest);                                  // by version, not by name
    CHECK(dirs.contains(h + u"/.volta/bin"_s));
    CHECK(dirs.contains(h + u"/.bun/bin"_s));
}

TEST_CASE("A program is found in the first folder that has it, and a configured path is used as given") {
    QTemporaryDir home;
    REQUIRE(home.isValid());
    const QString h = home.path();
    makeProgram(h + u"/.nvm/versions/node/v22.11.0/bin/codex"_s);
    makeProgram(h + u"/.npm-global/bin/codex"_s);

    const QStringList dirs = programSearchDirs(h);
    CHECK(findProgramIn(u"codex"_s, dirs) == programPath(h + u"/.npm-global/bin/codex"_s));
    CHECK(findProgramIn(u"no-such-program"_s, dirs).isEmpty());

    CHECK(findProgram(u"codex"_s, programPath(h + u"/.nvm/versions/node/v22.11.0/bin/codex"_s))
          == programPath(h + u"/.nvm/versions/node/v22.11.0/bin/codex"_s));
    CHECK(findProgram(u"codex"_s, h + u"/missing/codex"_s).isEmpty());    // a wrong path is not second-guessed
}

TEST_CASE("A program started by Nexa finds what lives beside it (node beside an nvm codex)") {
#ifdef Q_OS_WIN
    const QProcessEnvironment environment = environmentFor(u"C:/nvm/versions/node/v22/bin/codex.cmd"_s);
    CHECK(environment.value(u"PATH"_s).startsWith(u"C:/nvm/versions/node/v22/bin;"_s));
    CHECK(environmentFor(u"C:/Windows/System32/cmd.exe"_s).value(u"PATH"_s).startsWith(u"C:/Windows/System32"_s));
#else
    const QProcessEnvironment environment = environmentFor(u"/home/u/.nvm/versions/node/v22/bin/codex"_s);
    CHECK(environment.value(u"PATH"_s).startsWith(u"/home/u/.nvm/versions/node/v22/bin:"_s));
    CHECK(environmentFor(u"/usr/bin/true"_s).value(u"PATH"_s).startsWith(u"/usr/bin"_s));
#endif
}
