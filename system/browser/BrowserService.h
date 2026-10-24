#pragma once

#include "core/config/Config.h"
#include "core/mcp/McpConnectors.h"
#include "system/browser/ChromeProfiles.h"

#include <QObject>
#include <QString>

#include <optional>

namespace nexa {

class HttpClient;
class PlaywrightInstaller;
class ToolRegistry;

// Which browser Playwright works in, and how.
struct BrowserPlan {
    enum class Mode {
        Off,                // turned off in Settings
        Unavailable,        // asked for something this PC cannot do (reason says what)
        Extension,          // the user's own browser, through the Playwright extension
        NexaProfile,        // the user's browser program with Nexa's own profile
        BundledChromium,    // no Chrome-family browser: Playwright's Chromium
    };
    Mode mode{Mode::Off};
    BrowserInstall install;     // the browser used (empty for BundledChromium)
    QString profile;            // Extension: the profile's folder ("Profile 1")
    QString profileName;        // …and its name ("Ahmad")
    QString reason;             // Unavailable: why, in words
};

// Paths the server's command line is built from.
struct BrowserPaths {
    QString node;               // absolute
    QString cli;                // @playwright/mcp's cli.js
    QString outputDir;          // where its files go (screenshots it is asked to save, …)
    QString nexaProfile;        // the user data folder of Nexa's own profile
    QString browsersPath;       // Playwright's own Chromium, when there is no other
};

// The browser that is part of Nexa: Playwright's MCP server, installed by PlaywrightInstaller,
// started with the command line the plan calls for, and its tools registered under their own names
// (browser_navigate, browser_snapshot, …) through a McpConnectors of its own — an allow-list, with
// flags and risks set here, the arguments stripped of file names and the results kept small enough
// for the conversation. The window shows its state; the prompt names where it works. (Facade)
class BrowserService : public QObject {
    Q_OBJECT
public:
    enum class State { Off, Installing, NeedsSetup, Starting, Ready, Failed };
    static constexpr qsizetype kMaxResultText = 16'000;

    BrowserService(ToolRegistry *tools, HttpClient *http, PlaywrightInstaller *installer, QObject *parent = nullptr);
    ~BrowserService() override;

    // Settings say what to use: installs Playwright once if it is missing, then (re)starts the
    // server when the plan changed.
    void apply(const BrowserConfig &config);

    State state() const { return state_; }
    const BrowserPlan &plan() const { return plan_; }
    QList<BrowserInstall> installs() const { return installs_; }
    PlaywrightInstaller *installer() const { return installer_; }
    bool toolsReady() const { return state_ == State::Ready; }
    // The status bar's words: "Chrome · Ahmad", "Nexa's Chrome profile", "installing…", "off".
    QString summary() const;
    // A sentence for Settings: what it is doing, or what went wrong and what to do.
    QString detail() const { return detail_; }
    // Where the browser tools work, for the prompt: "the user's own Google Chrome (profile "Ahmad"),
    // signed in as them". Empty while they are not there.
    QString targetPhrase() const;

    // Pure, public for the tests.
    static BrowserPlan planFor(const BrowserConfig &config, const QList<BrowserInstall> &installs, bool chromiumInstalled);
    static McpServerConfig serverConfigFor(const BrowserPlan &plan, const BrowserPaths &paths, const QString &token);
    static std::optional<ToolSpec> shapeTool(const McpClient::Tool &tool, ToolSpec defaults, BrowserPlan::Mode mode);
    static QJsonObject shapeArguments(const QString &tool, QJsonObject arguments);
    static ToolResult shapeResult(const QString &tool, ToolResult result);

signals:
    void stateChanged();
    // Something the user should hear once, in the chat: "working in Nexa's own Chrome window — …".
    void notice(const QString &text);

private:
    void setState(State state, const QString &detail);
    void start();

    PlaywrightInstaller *installer_;
    McpConnectors connectors_;
    BrowserConfig config_;
    QList<BrowserInstall> installs_;
    BrowserPlan plan_;
    State state_{State::Off};
    QString detail_;
    bool installTried_{false};
    bool fallbackNoticeGiven_{false};
};

} // namespace nexa
