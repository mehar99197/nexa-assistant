#include "core/config/Portable.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>

#include <mutex>

using namespace Qt::StringLiterals;

namespace nexa::portable {

namespace {

std::mutex &stateMutex() {
    static std::mutex mutex;
    return mutex;
}

QString &overrideDir() {
    static QString dir;
    return dir;
}

bool &checked() {
    static bool flag = false;
    return flag;
}

bool &portableState() {
    static bool flag = false;
    return flag;
}

QString effectiveAppDir() {
    if (!overrideDir().isEmpty()) return overrideDir();
    return QCoreApplication::applicationDirPath();
}

} // namespace

bool detectPortable(const QString &appDir) {
    if (appDir.isEmpty()) return false;
    const QFileInfo marker(QDir(appDir).absoluteFilePath(u"portable.txt"_s));
    return marker.exists() && QFileInfo(appDir).isWritable();
}

bool isPortable() {
    std::lock_guard<std::mutex> lock(stateMutex());
    if (!checked()) {
        checked() = true;
        portableState() = detectPortable(effectiveAppDir());
    }
    return portableState();
}

void setRootForTesting(const QString &appDir) {
    std::lock_guard<std::mutex> lock(stateMutex());
    overrideDir() = appDir;
    checked() = false;
    portableState() = false;
}

void clearRootForTesting() {
    setRootForTesting(QString());
}

QString rootDir() {
    if (!isPortable()) return QString();
    std::lock_guard<std::mutex> lock(stateMutex());
    const QString dir = QDir(effectiveAppDir()).absoluteFilePath(u"NexaData"_s);
    QDir().mkpath(dir);
    return dir;
}

QString configDir() {
    if (const QString root = rootDir(); !root.isEmpty()) {
        const QString dir = root + u"/config"_s;
        QDir().mkpath(dir);
        return dir;
    }
    QString base = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    if (base.isEmpty()) base = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    return base + u"/nexa"_s;
}

QString dataDir() {
    if (const QString root = rootDir(); !root.isEmpty()) {
        const QString dir = root + u"/data"_s;
        QDir().mkpath(dir);
        return dir;
    }
    QString base = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    if (base.isEmpty()) base = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    return base + u"/nexa"_s;
}

QString cacheDir() {
    if (const QString root = rootDir(); !root.isEmpty()) {
        const QString dir = root + u"/cache"_s;
        QDir().mkpath(dir);
        return dir;
    }
    QString base = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation);
    if (base.isEmpty()) base = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    return base + u"/nexa"_s;
}

void initialise() {
    const QString root = rootDir();
    if (root.isEmpty()) return;
    const QString settingsDir = root + u"/settings"_s;
    QDir().mkpath(settingsDir);
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir);
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settingsDir);
}

} // namespace nexa::portable
