// voice/EngineLocator: the user's engines first, the package's second, and models always the user's.

#include "voice/EngineLocator.h"

#include "QtStringMakers.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace Qt::StringLiterals;

namespace {

void placeSherpa(const QString &root) {
    QDir().mkpath(root + u"/sherpa/lib"_s);
#ifdef Q_OS_WIN
    QFile lib(root + u"/sherpa/lib/sherpa-onnx-c-api.dll"_s);
#else
    QFile lib(root + u"/sherpa/lib/libsherpa-onnx-c-api.so"_s);
#endif
    REQUIRE(lib.open(QIODevice::WriteOnly));
    lib.write("not really a library");
}

void placePiper(const QString &root) {
    QDir().mkpath(root + u"/piper"_s);
#ifdef Q_OS_WIN
    QFile piper(root + u"/piper/piper.exe"_s);
    REQUIRE(piper.open(QIODevice::WriteOnly));
    piper.write("MZ");
#else
    QFile piper(root + u"/piper/piper"_s);
    REQUIRE(piper.open(QIODevice::WriteOnly));
    piper.write("#!/bin/sh\n");
#endif
    piper.close();
    piper.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
}

} // namespace

TEST_CASE("The user's engine wins over the package's, the package stands in, and nothing means the user's folder") {
    QTemporaryDir user, package;
    const EngineLocator nothing(user.path(), package.path());
    CHECK(nothing.sherpaLibraryDir() == user.path() + u"/sherpa"_s);
    CHECK(nothing.piperDir() == user.path() + u"/piper"_s);
    CHECK_FALSE(nothing.sherpaFromPackage());
    CHECK_FALSE(nothing.piperFromPackage());

    placeSherpa(package.path());
    placePiper(package.path());
    const EngineLocator packaged(user.path(), package.path());
    CHECK(packaged.sherpaLibraryDir() == package.path() + u"/sherpa"_s);
    CHECK(packaged.piperDir() == package.path() + u"/piper"_s);
    CHECK(packaged.sherpaFromPackage());
    CHECK(packaged.piperFromPackage());

    placeSherpa(user.path());
    placePiper(user.path());
    const EngineLocator own(user.path(), package.path());
    CHECK(own.sherpaLibraryDir() == user.path() + u"/sherpa"_s);
    CHECK(own.piperDir() == user.path() + u"/piper"_s);
    CHECK_FALSE(own.sherpaFromPackage());
    CHECK_FALSE(own.piperFromPackage());
}

TEST_CASE("Models and voices are the user's wherever the engine is") {
    QTemporaryDir user, package;
    placeSherpa(package.path());
    placePiper(package.path());
    const EngineLocator locator(user.path(), package.path());
    CHECK(locator.sherpaModelsDir() == user.path() + u"/sherpa"_s);
    CHECK(locator.modelsDir() == user.path() + u"/models"_s);
    CHECK(locator.piperVoicesDir() == user.path() + u"/piper/voices"_s);
    CHECK(locator.piperVoiceSearchDirs() == QStringList{user.path() + u"/piper/voices"_s, package.path() + u"/piper/voices"_s});
    placePiper(user.path());
    const EngineLocator own(user.path(), package.path());
    CHECK(own.piperVoiceSearchDirs() == QStringList{user.path() + u"/piper/voices"_s});     // the same folder once
}

TEST_CASE("No package folder compiled in means no second place") {
    QTemporaryDir user;
    const EngineLocator locator(user.path(), QString());
    CHECK(locator.sherpaLibraryDir() == user.path() + u"/sherpa"_s);
    CHECK(locator.piperDir() == user.path() + u"/piper"_s);
    CHECK_FALSE(locator.sherpaFromPackage());
    CHECK(locator.packageDir().isEmpty());
    CHECK(EngineLocator::defaultUserDataDir().endsWith(u"/nexa"_s));
}
