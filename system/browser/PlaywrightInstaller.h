#pragma once

#include "core/Result.h"

#include <QObject>
#include <QString>
#include <QStringList>

#include <optional>

namespace nexa {

class LocalCommandRunner;

// Puts Playwright's MCP server — the browser automation that is part of Nexa — into
// ~/.local/share/nexa/playwright, at the version Nexa was built against, with the user's own Node
// and npm (no root), and Playwright's Chromium beside it when the PC has no Chrome-family browser.
// Runs in the background through a runner of its own, so a stop or a panic does not end it.
class PlaywrightInstaller : public QObject {
    Q_OBJECT
public:
    static constexpr QLatin1StringView kPackage{"@playwright/mcp"};
    static constexpr QLatin1StringView kVersion{"0.0.83"};
    static constexpr int kMinNodeMajor = 18;

    explicit PlaywrightInstaller(QString prefix = defaultPrefix(), QObject *parent = nullptr);
    ~PlaywrightInstaller() override;
    static QString defaultPrefix();

    QString prefix() const { return prefix_; }
    QString cliPath() const;            // the server's entry point
    QString browsersPath() const;       // where Playwright's own Chromium goes
    QString installedVersion() const;   // from the package's package.json; empty when absent
    bool isInstalled() const;           // there, at kVersion
    bool hasChromium() const;
    bool isRunning() const { return running_; }

    // Node by absolute path (PATH, then the usual places), or empty.
    static QString nodeProgram();
    // Pure: "v24.19.0" → 24.
    static std::optional<int> nodeMajor(const QString &versionOutput);
    // Pure: npm's arguments for the install.
    static QStringList npmArguments(const QString &prefix);

    void install(bool withChromium);
    void cancel();

signals:
    void progress(const QString &text);
    void installed();
    void failed(const nexa::Error &error);

private:
    void installPackage(const QString &node, bool withChromium);
    void installChromium(const QString &node);
    void fail(const Error &error);

    QString prefix_;
    LocalCommandRunner *runner_;
    bool running_{false};
};

} // namespace nexa
