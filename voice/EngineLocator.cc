#include "voice/EngineLocator.h"

#include "core/config/Portable.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

#ifdef Q_OS_WIN
constexpr QLatin1StringView kPiperBinary{"/piper/piper.exe"};

bool hasSherpa(const QString &root) {
    if (root.isEmpty()) return false;
    return QFileInfo::exists(root + u"/sherpa/lib/sherpa-onnx-c-api.dll"_s)
        || QFileInfo::exists(root + u"/sherpa/bin/sherpa-onnx-c-api.dll"_s)
        || QFileInfo::exists(root + u"/sherpa/sherpa-onnx-c-api.dll"_s);
}
#else
constexpr QLatin1StringView kSherpaLibrary{"/sherpa/lib/libsherpa-onnx-c-api.so"};
constexpr QLatin1StringView kPiperBinary{"/piper/piper"};

bool hasSherpa(const QString &root) {
    return !root.isEmpty() && QFileInfo::exists(root + kSherpaLibrary);
}
#endif

bool hasPiper(const QString &root) {
    return !root.isEmpty() && QFileInfo(root + kPiperBinary).isExecutable();
}

} // namespace

EngineLocator::EngineLocator(QString userDataDir, QString packageDir)
    : user_(std::move(userDataDir)), package_(std::move(packageDir)) {}

QString EngineLocator::defaultUserDataDir() {
    return portable::dataDir();
}

QString EngineLocator::packageEngineDir() {
#ifdef NEXA_ENGINE_DIR
    const QString configured = QString::fromUtf8(NEXA_ENGINE_DIR);
    if (!configured.isEmpty() && QDir::isRelativePath(configured) && QCoreApplication::instance()) {
        return QDir::cleanPath(QCoreApplication::applicationDirPath() + u'/' + configured);
    }
    return configured;
#else
    return {};
#endif
}

QString EngineLocator::sherpaLibraryDir() const {
    if (hasSherpa(user_)) return user_ + u"/sherpa"_s;
    if (hasSherpa(package_)) return package_ + u"/sherpa"_s;
    return user_ + u"/sherpa"_s;
}

QString EngineLocator::sherpaModelsDir() const {
    return user_ + u"/sherpa"_s;
}

bool EngineLocator::sherpaFromPackage() const {
    return !hasSherpa(user_) && hasSherpa(package_);
}

QString EngineLocator::piperDir() const {
    if (hasPiper(user_)) return user_ + u"/piper"_s;
    if (hasPiper(package_)) return package_ + u"/piper"_s;
    return user_ + u"/piper"_s;
}

QString EngineLocator::piperVoicesDir() const {
    return user_ + u"/piper/voices"_s;
}

bool EngineLocator::piperFromPackage() const {
    return !hasPiper(user_) && hasPiper(package_);
}

QStringList EngineLocator::piperVoiceSearchDirs() const {
    QStringList dirs{piperVoicesDir()};
    if (const QString beside = piperDir() + u"/voices"_s; beside != dirs.first()) dirs.append(beside);
    return dirs;
}

QString EngineLocator::modelsDir() const {
    return user_ + u"/models"_s;
}

} // namespace nexa
