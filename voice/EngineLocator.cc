#include "voice/EngineLocator.h"

#include <QFileInfo>
#include <QStandardPaths>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

constexpr QLatin1StringView kSherpaLibrary{"/sherpa/lib/libsherpa-onnx-c-api.so"};
constexpr QLatin1StringView kPiperBinary{"/piper/piper"};

bool hasSherpa(const QString &root) {
    return !root.isEmpty() && QFileInfo::exists(root + kSherpaLibrary);
}

bool hasPiper(const QString &root) {
    return !root.isEmpty() && QFileInfo(root + kPiperBinary).isExecutable();
}

} // namespace

EngineLocator::EngineLocator(QString userDataDir, QString packageDir)
    : user_(std::move(userDataDir)), package_(std::move(packageDir)) {}

QString EngineLocator::defaultUserDataDir() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + u"/nexa"_s;
}

QString EngineLocator::packageEngineDir() {
#ifdef NEXA_ENGINE_DIR
    return QString::fromUtf8(NEXA_ENGINE_DIR);
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
