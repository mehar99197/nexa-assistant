#include "system/browser/PlaywrightInstaller.h"

#include "core/Logging.h"
#include "system/commands/LocalCommandRunner.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStandardPaths>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

constexpr int kNodeTimeoutMs = 15'000;
constexpr int kInstallTimeoutMs = 10 * 60'000;

QString shellQuoted(const QString &text) {
    QString quoted = text;
    quoted.replace(u'\'', u"'\\''"_s);
    return u"'"_s + quoted + u"'"_s;
}

QString commandLine(const QStringList &argv) {
    QStringList quoted;
    for (const QString &argument : argv) quoted.append(shellQuoted(argument));
    return quoted.join(u' ');
}

QString tail(const QString &output) {
    const QString trimmed = output.trimmed();
    return trimmed.size() > 600 ? u"…"_s + trimmed.right(600) : trimmed;
}

} // namespace

PlaywrightInstaller::PlaywrightInstaller(QString prefix, QObject *parent)
    : QObject(parent), prefix_(std::move(prefix)), runner_(new LocalCommandRunner(this)) {}

PlaywrightInstaller::~PlaywrightInstaller() = default;

QString PlaywrightInstaller::defaultPrefix() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + u"/nexa/playwright"_s;
}

QString PlaywrightInstaller::cliPath() const {
    return QDir(prefix_).filePath(u"node_modules/"_s + QString(kPackage) + u"/cli.js"_s);
}

QString PlaywrightInstaller::browsersPath() const {
    return QDir(prefix_).filePath(u"browsers"_s);
}

QString PlaywrightInstaller::installedVersion() const {
    QFile file(QDir(prefix_).filePath(u"node_modules/"_s + QString(kPackage) + u"/package.json"_s));
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(file.readAll()).object().value(u"version"_s).toString();
}

bool PlaywrightInstaller::isInstalled() const {
    return QFileInfo::exists(cliPath()) && installedVersion() == QString(kVersion);
}

bool PlaywrightInstaller::hasChromium() const {
    return !QDir(browsersPath()).entryList({u"chromium-*"_s}, QDir::Dirs).isEmpty();
}

QString PlaywrightInstaller::nodeProgram() {
    if (const QString found = QStandardPaths::findExecutable(u"node"_s); !found.isEmpty()) return found;
    return QStandardPaths::findExecutable(u"node"_s, {u"/usr/local/bin"_s, u"/usr/bin"_s,
                                                      QDir::homePath() + u"/.local/bin"_s});
}

std::optional<int> PlaywrightInstaller::nodeMajor(const QString &versionOutput) {
    static const QRegularExpression version(u"^\\s*v?(\\d{1,3})\\.\\d+"_s);
    const QRegularExpressionMatch m = version.match(versionOutput);
    if (!m.hasMatch()) return std::nullopt;
    return m.captured(1).toInt();
}

QStringList PlaywrightInstaller::npmArguments(const QString &prefix) {
    return {u"install"_s, u"--prefix"_s, prefix, u"--no-audit"_s, u"--no-fund"_s, u"--ignore-scripts"_s,
            u"--loglevel=error"_s, QString(kPackage) + u'@' + QString(kVersion)};
}

void PlaywrightInstaller::fail(const Error &error) {
    running_ = false;
    qCWarning(lcBrowser).noquote() << "Playwright install:" << error.message;
    emit failed(error);
}

void PlaywrightInstaller::install(bool withChromium) {
    if (running_) return;
    const QString node = nodeProgram();
    if (node.isEmpty()) {
        emit failed(Error{ErrorCode::NotFound, tr("Node.js is not installed. Install it with: sudo apt install nodejs npm")});
        return;
    }
    running_ = true;
    emit progress(tr("Checking Node.js…"));
    runner_->run(commandLine({node, u"--version"_s}), QDir::homePath(), kNodeTimeoutMs,
                 [this, node, withChromium](Result<CommandOutput> result) {
        const std::optional<int> major = result ? nodeMajor(result.value().output) : std::nullopt;
        if (!major) {
            fail(Error{ErrorCode::NotFound, tr("Cannot run %1.").arg(node)});
            return;
        }
        if (*major < kMinNodeMajor) {
            fail(Error{ErrorCode::NotSupported, tr("Node.js %1 is too old; Playwright needs %2 or newer.").arg(*major).arg(kMinNodeMajor)});
            return;
        }
        installPackage(node, withChromium);
    });
}

void PlaywrightInstaller::installPackage(const QString &node, bool withChromium) {
    // npm sits beside node; failing that, on the PATH.
    QString npm = QFileInfo(node).dir().filePath(u"npm"_s);
    if (!QFileInfo(npm).isExecutable()) npm = QStandardPaths::findExecutable(u"npm"_s);
    if (npm.isEmpty()) {
        fail(Error{ErrorCode::NotFound, tr("npm is not installed. Install it with: sudo apt install npm")});
        return;
    }
    if (!QDir().mkpath(prefix_)) {
        fail(Error{ErrorCode::Io, tr("Cannot create %1").arg(prefix_)});
        return;
    }
    emit progress(tr("Installing Playwright %1…").arg(QString(kVersion)));
    qCInfo(lcBrowser).noquote() << "installing" << kPackage << kVersion << "into" << prefix_;
    QStringList argv{npm};
    argv += npmArguments(prefix_);
    runner_->run(commandLine(argv), prefix_, kInstallTimeoutMs, [this, node, withChromium](Result<CommandOutput> result) {
        if (!result) {
            fail(result.error());
            return;
        }
        if (result.value().exitCode != 0 || !isInstalled()) {
            fail(Error{ErrorCode::Unknown, tr("npm could not install Playwright: %1").arg(tail(result.value().output))});
            return;
        }
        if (withChromium && !hasChromium()) {
            installChromium(node);
            return;
        }
        running_ = false;
        qCInfo(lcBrowser) << "Playwright installed";
        emit installed();
    });
}

void PlaywrightInstaller::installChromium(const QString &node) {
    emit progress(tr("Downloading Playwright's Chromium (no Chrome on this PC)…"));
    const QString command = u"PLAYWRIGHT_BROWSERS_PATH="_s + shellQuoted(browsersPath()) + u' '
                            + commandLine({node, cliPath(), u"install-browser"_s, u"chromium"_s});
    runner_->run(command, prefix_, kInstallTimeoutMs, [this](Result<CommandOutput> result) {
        if (!result || result.value().exitCode != 0 || !hasChromium()) {
            fail(Error{ErrorCode::Unknown, tr("Playwright's Chromium could not be installed: %1")
                                               .arg(result ? tail(result.value().output) : result.error().message)});
            return;
        }
        running_ = false;
        emit installed();
    });
}

void PlaywrightInstaller::cancel() {
    runner_->cancelAll();
}

} // namespace nexa
