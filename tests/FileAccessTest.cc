#include "system/files/FileAccess.h"

#include "QtStringMakers.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace Qt::StringLiterals;

TEST_CASE("FileAccess confines paths to its roots, also through .. and symlinks") {
    QTemporaryDir allowed;
    QTemporaryDir outside;
    REQUIRE(allowed.isValid());
    REQUIRE(outside.isValid());
    const FileAccess files({allowed.path()});
    const QString root = QDir(allowed.path()).canonicalPath();

    CHECK(files.resolve(allowed.filePath(u"notes/new.txt"_s)).value() == root + u"/notes/new.txt"_s);
    CHECK(files.resolve(allowed.path()).isOk());
    CHECK_FALSE(files.resolve(allowed.filePath(u"../escape.txt"_s)).isOk());
    CHECK_FALSE(files.resolve(outside.filePath(u"x.txt"_s)).isOk());
    CHECK(files.resolve(outside.filePath(u"x.txt"_s)).error().code == ErrorCode::PermissionDenied);

#ifndef Q_OS_WIN
    REQUIRE(QFile::link(outside.path(), allowed.filePath(u"link"_s)));
    CHECK_FALSE(files.resolve(allowed.filePath(u"link/secret.txt"_s)).isOk());
#endif
}

TEST_CASE("FileAccess expands ~ and resolves relative paths against home") {
    const FileAccess files({QDir::homePath()});
    const QString home = QDir(QDir::homePath()).canonicalPath();
    CHECK(files.resolve(u"~/nexa-test-file.txt"_s).value() == home + u"/nexa-test-file.txt"_s);
    CHECK(files.resolve(u"nexa-test-file.txt"_s).value() == home + u"/nexa-test-file.txt"_s);
}

TEST_CASE("FileAccess writes, appends and reads text") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const FileAccess files({dir.path()});
    const QString path = dir.filePath(u"deep/folder/note.txt"_s);

    REQUIRE(files.write(path, u"hello"_s, false).value() == 5);
    REQUIRE(files.write(path, u" world"_s, true).isOk());
    CHECK(files.read(path).value() == u"hello world"_s);
    CHECK(files.read(path, 5).value().startsWith(u"hello\n[truncated: showing the first 5 of 11 bytes]"_s));
    REQUIRE(files.write(path, u"replaced"_s, false).isOk());
    CHECK(files.read(path).value() == u"replaced"_s);
}

TEST_CASE("FileAccess refuses binary files and folders") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const FileAccess files({dir.path()});
    QFile binary(dir.filePath(u"blob.bin"_s));
    REQUIRE(binary.open(QIODevice::WriteOnly));
    binary.write(QByteArray("\x7f" "ELF\0\0\0", 7));
    binary.close();
    CHECK_FALSE(files.read(binary.fileName()).isOk());
    CHECK_FALSE(files.read(dir.path()).isOk());
}
