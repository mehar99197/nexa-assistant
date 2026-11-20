#include "core/config/Portable.h"

#include "QtStringMakers.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace Qt::StringLiterals;

TEST_CASE("Portable: detects portable.txt only in writable directory", "[portable]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    REQUIRE_FALSE(portable::detectPortable(dir.path()));

    QFile marker(dir.filePath(u"portable.txt"_s));
    REQUIRE(marker.open(QIODevice::WriteOnly));
    marker.close();
    REQUIRE(portable::detectPortable(dir.path()));

    REQUIRE_FALSE(portable::detectPortable(QString()));
    REQUIRE_FALSE(portable::detectPortable(dir.filePath(u"nonexistent"_s)));
}

TEST_CASE("Portable: redirects config, data, cache, and QSettings into NexaData", "[portable]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    QFile marker(dir.filePath(u"portable.txt"_s));
    REQUIRE(marker.open(QIODevice::WriteOnly));
    marker.close();

    portable::setRootForTesting(dir.path());
    REQUIRE(portable::isPortable());
    const QString root = dir.filePath(u"NexaData"_s);
    REQUIRE(portable::rootDir() == root);
    REQUIRE(portable::configDir() == root + u"/config"_s);
    REQUIRE(portable::dataDir() == root + u"/data"_s);
    REQUIRE(portable::cacheDir() == root + u"/cache"_s);
    REQUIRE(QFileInfo::exists(portable::configDir()));
    REQUIRE(QFileInfo::exists(portable::dataDir()));
    REQUIRE(QFileInfo::exists(portable::cacheDir()));

    portable::initialise();
    REQUIRE(QFileInfo::exists(root + u"/settings"_s));
    REQUIRE(QSettings::defaultFormat() == QSettings::IniFormat);

    portable::clearRootForTesting();
    REQUIRE_FALSE(portable::isPortable());
    REQUIRE(portable::configDir()
            == QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + u"/nexa"_s);
}
