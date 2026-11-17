#include "system/browser/ChromeProfiles.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStandardPaths>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

struct Known {
    QString id;
    QString displayName;
    QString configFolder;
    QStringList programs;
    bool supportsExtension;
};

const QList<Known> &knownBrowsers() {
#ifdef Q_OS_WIN
    static const QList<Known> known{
        {u"chrome"_s, u"Google Chrome"_s, u"Google/Chrome/User Data"_s, {u"chrome.exe"_s, u"chrome"_s}, true},
        {u"msedge"_s, u"Microsoft Edge"_s, u"Microsoft/Edge/User Data"_s, {u"msedge.exe"_s, u"msedge"_s}, true},
        {u"brave"_s, u"Brave"_s, u"BraveSoftware/Brave-Browser/User Data"_s, {u"brave.exe"_s, u"brave"_s}, false},
        {u"chromium"_s, u"Chromium"_s, u"Chromium/User Data"_s, {u"chromium.exe"_s, u"chromium"_s}, false},
    };
#else
    static const QList<Known> known{
        {u"chrome"_s, u"Google Chrome"_s, u"google-chrome"_s, {u"google-chrome-stable"_s, u"google-chrome"_s}, true},
        {u"msedge"_s, u"Microsoft Edge"_s, u"microsoft-edge"_s, {u"microsoft-edge-stable"_s, u"microsoft-edge"_s}, true},
        {u"brave"_s, u"Brave"_s, u"BraveSoftware/Brave-Browser"_s, {u"brave-browser"_s, u"brave"_s}, false},
        {u"chromium"_s, u"Chromium"_s, u"chromium"_s, {u"chromium"_s, u"chromium-browser"_s}, false},
    };
#endif
    return known;
}

#ifdef Q_OS_WIN
QString findWindowsBrowserExecutable(const QString &program) {
    if (const QString onPath = QStandardPaths::findExecutable(program); !onPath.isEmpty()) return onPath;
    QStringList subdirs;
    const QString lower = program.toLower();
    if (lower.startsWith(u"chrome"_s) && !lower.startsWith(u"chromium"_s)) {
        subdirs = {u"Google/Chrome/Application"_s};
    } else if (lower.startsWith(u"msedge"_s)) {
        subdirs = {u"Microsoft/Edge/Application"_s};
    } else if (lower.startsWith(u"brave"_s)) {
        subdirs = {u"BraveSoftware/Brave-Browser/Application"_s};
    } else if (lower.startsWith(u"chromium"_s)) {
        subdirs = {u"Chromium/Application"_s};
    } else if (lower.startsWith(u"firefox"_s)) {
        subdirs = {u"Mozilla Firefox"_s};
    }
    QStringList dirs;
    for (const QString &root : {qEnvironmentVariable("ProgramFiles"), qEnvironmentVariable("ProgramFiles(x86)"),
                                qEnvironmentVariable("LOCALAPPDATA")}) {
        if (root.isEmpty()) continue;
        for (const QString &sub : subdirs) dirs.append(root + u'/' + sub);
    }
    QString found = QStandardPaths::findExecutable(program, dirs);
    if (found.isEmpty() && lower.startsWith(u"chromium"_s)) {
        found = QStandardPaths::findExecutable(u"chrome.exe"_s, dirs);
    }
    return found;
}
#endif

int profileNumber(const QString &directory) {
    return directory == u"Default"_s ? -1 : directory.mid(8).toInt();     // "Profile 12" → 12
}

bool preferencesKnow(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return false;
    const QJsonObject settings = QJsonDocument::fromJson(file.readAll()).object().value(u"extensions"_s).toObject()
                                     .value(u"settings"_s).toObject();
    return settings.contains(QString(ChromeProfiles::kExtensionId));
}

} // namespace

const BrowserProfile *BrowserInstall::profile(const QString &directory) const {
    for (const BrowserProfile &candidate : profiles) {
        if (candidate.directory == directory) return &candidate;
    }
    return nullptr;
}

ChromeProfiles::LocalState ChromeProfiles::parseLocalState(const QByteArray &json) {
    static const QRegularExpression chromeName(u"^(?:Default|Profile [0-9]{1,4})$"_s);
    LocalState state;
    const QJsonObject profile = QJsonDocument::fromJson(json).object().value(u"profile"_s).toObject();
    const QJsonObject cache = profile.value(u"info_cache"_s).toObject();
    for (auto it = cache.constBegin(); it != cache.constEnd(); ++it) {
        if (!chromeName.match(it.key()).hasMatch()) continue;
        const QString name = it.value().toObject().value(u"name"_s).toString();
        state.profiles.append(BrowserProfile{it.key(), name.isEmpty() ? it.key() : name, false});
    }
    std::sort(state.profiles.begin(), state.profiles.end(), [](const BrowserProfile &a, const BrowserProfile &b) {
        return profileNumber(a.directory) < profileNumber(b.directory);
    });
    const QString lastUsed = profile.value(u"last_used"_s).toString();
    if (chromeName.match(lastUsed).hasMatch()) state.lastUsed = lastUsed;
    return state;
}

bool ChromeProfiles::extensionInstalled(const QString &profileDir) {
    const QDir dir(profileDir);
    if (QFileInfo(dir.filePath(u"Extensions/"_s + QString(kExtensionId))).isDir()) return true;
    return preferencesKnow(dir.filePath(u"Preferences"_s)) || preferencesKnow(dir.filePath(u"Secure Preferences"_s));
}

QString ChromeProfiles::extensionProfile(const BrowserInstall &install, const QString &wanted) {
    const auto has = [&install](const QString &directory) {
        const BrowserProfile *profile = install.profile(directory);
        return profile && profile->extension;
    };
    if (!wanted.isEmpty() && has(wanted)) return wanted;
    QStringList order;
    if (!install.lastUsed.isEmpty()) order.append(install.lastUsed);
    for (const BrowserProfile &profile : install.profiles) order.append(profile.directory);     // Default, Profile N
    for (const QString &directory : order) {
        if (has(directory)) return directory;
    }
    return {};
}

QString ChromeProfiles::webStoreUrl() {
    return u"https://chromewebstore.google.com/detail/"_s + QString(kExtensionId);
}

QList<BrowserInstall> ChromeProfiles::detect(const QString &configHome, const FindExecutable &findExecutable) {
    QList<BrowserInstall> installs;
    for (const Known &known : knownBrowsers()) {
        QString executable;
        for (const QString &program : known.programs) {
            executable = findExecutable(program);
            if (!executable.isEmpty()) break;
        }
        if (executable.isEmpty()) continue;
        BrowserInstall install{known.id, known.displayName, executable, QDir(configHome).filePath(known.configFolder),
                               {}, {}, known.supportsExtension};
        QFile localState(QDir(install.userDataDir).filePath(u"Local State"_s));
        if (localState.open(QIODevice::ReadOnly)) {
            const LocalState state = parseLocalState(localState.readAll());
            install.lastUsed = state.lastUsed;
            install.profiles = state.profiles;
            for (BrowserProfile &profile : install.profiles) {
                profile.extension = extensionInstalled(QDir(install.userDataDir).filePath(profile.directory));
            }
        }
        installs.append(install);
    }
    return installs;
}

QList<BrowserInstall> ChromeProfiles::detect() {
#ifdef Q_OS_WIN
    QString localAppData = qEnvironmentVariable("LOCALAPPDATA");
    if (localAppData.isEmpty()) localAppData = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    return detect(localAppData, [](const QString &program) { return findWindowsBrowserExecutable(program); });
#else
    return detect(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation),
                  [](const QString &program) { return QStandardPaths::findExecutable(program); });
#endif
}

bool ChromeProfiles::firefoxInstalled(const FindExecutable &findExecutable) {
    return !findExecutable(u"firefox"_s).isEmpty() || !findExecutable(u"firefox-esr"_s).isEmpty();
}

bool ChromeProfiles::firefoxInstalled() {
#ifdef Q_OS_WIN
    return firefoxInstalled([](const QString &program) { return findWindowsBrowserExecutable(program); });
#else
    return firefoxInstalled([](const QString &program) { return QStandardPaths::findExecutable(program); });
#endif
}

} // namespace nexa
