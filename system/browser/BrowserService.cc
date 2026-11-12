#include "system/browser/BrowserService.h"

#include "core/Logging.h"
#include "core/tools/ToolRegistry.h"
#include "system/browser/PlaywrightInstaller.h"
#include "system/screen/ScreenCapture.h"

#include <QDir>
#include <QHash>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QPointer>
#include <QStandardPaths>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

enum class Kind { ReadOnly, Plain, Acts, Uploads };

// Every Playwright tool Nexa offers, and what it does to the user's browser. Anything else a
// server lists (browser_run_code_unsafe runs code on the PC outside every policy; a page's own
// WebMCP tools; tools of a later version not looked at yet) is not registered.
const QHash<QString, Kind> &allowed() {
    static const QHash<QString, Kind> tools{
        {u"browser_snapshot"_s, Kind::ReadOnly},       {u"browser_find"_s, Kind::ReadOnly},
        {u"browser_console_messages"_s, Kind::ReadOnly}, {u"browser_wait_for"_s, Kind::ReadOnly},
        {u"browser_take_screenshot"_s, Kind::ReadOnly}, {u"browser_network_requests"_s, Kind::ReadOnly},
        {u"browser_network_request"_s, Kind::ReadOnly},
        {u"browser_navigate"_s, Kind::Plain},          {u"browser_navigate_back"_s, Kind::Plain},
        {u"browser_tabs"_s, Kind::Plain},              {u"browser_hover"_s, Kind::Plain},
        {u"browser_close"_s, Kind::Plain},
        {u"browser_click"_s, Kind::Acts},              {u"browser_type"_s, Kind::Acts},
        {u"browser_fill_form"_s, Kind::Acts},          {u"browser_select_option"_s, Kind::Acts},
        {u"browser_press_key"_s, Kind::Acts},          {u"browser_drag"_s, Kind::Acts},
        {u"browser_handle_dialog"_s, Kind::Acts},      {u"browser_evaluate"_s, Kind::Acts},
        {u"browser_file_upload"_s, Kind::Uploads},     {u"browser_drop"_s, Kind::Uploads},
    };
    return tools;
}

QString shortName(const BrowserInstall &install) {
    QString name = install.displayName;
    name.remove(u"Google "_s);
    name.remove(u"Microsoft "_s);
    return name;
}

QJsonObject withoutFileNames(QJsonObject parameters) {
    parameters.remove(u"$schema"_s);
    QJsonObject properties = parameters.value(u"properties"_s).toObject();
    if (properties.contains(u"filename"_s)) {
        properties.remove(u"filename"_s);
        parameters.insert(u"properties"_s, properties);
        QJsonArray required;
        for (const QJsonValue &name : parameters.value(u"required"_s).toArray()) {
            if (name.toString() != u"filename"_s) required.append(name);
        }
        if (parameters.contains(u"required"_s)) parameters.insert(u"required"_s, required);
    }
    return parameters;
}

} // namespace

BrowserService::BrowserService(ToolRegistry *tools, HttpClient *http, PlaywrightInstaller *installer, QObject *parent)
    : QObject(parent), installer_(installer), connectors_(tools, http) {
    const QPointer<BrowserService> self(this);
    connectors_.setShaping(McpConnectors::Shaping{
        .spec = [self](const McpClient::Tool &tool, ToolSpec defaults) {
            return shapeTool(tool, std::move(defaults), self ? self->plan_.mode : BrowserPlan::Mode::Off);
        },
        .arguments = &BrowserService::shapeArguments,
        .result = [self](const QString &tool, ToolResult result) {
            if (self && self->plan_.mode == BrowserPlan::Mode::NexaProfile && !self->fallbackNoticeGiven_) {
                self->fallbackNoticeGiven_ = true;
                emit self->notice(tr("Working in a separate %1 window with Nexa's own profile. To work in your own browser, "
                                     "signed in as you, add the Playwright extension in Settings → Browser.")
                                      .arg(self->plan_.install.displayName));
            }
            return shapeResult(tool, std::move(result));
        },
    });
    connect(&connectors_, &McpConnectors::statusChanged, this, [this](const QString &, const QString &state, const QString &detail) {
        if (state == u"ready"_s) {
            setState(State::Ready, plan_.mode == BrowserPlan::Mode::Extension
                                       ? tr("Working in your %1, profile \"%2\", through the Playwright extension.")
                                             .arg(plan_.install.displayName, plan_.profileName)
                                   : plan_.mode == BrowserPlan::Mode::NexaProfile
                                       ? tr("Working in %1 with Nexa's own profile. Add the Playwright extension to work in "
                                            "your own profile, signed in as you.").arg(plan_.install.displayName)
                                       : tr("Working in Playwright's Chromium with Nexa's own profile."));
        } else if (state == u"connecting"_s) {
            setState(State::Starting, tr("Starting Playwright…"));
        } else if (state == u"failed"_s) {
            setState(State::Failed, tr("Playwright stopped: %1").arg(detail));
        }
    });
    connect(installer_, &PlaywrightInstaller::progress, this, [this](const QString &text) { setState(State::Installing, text); });
    connect(installer_, &PlaywrightInstaller::installed, this, [this] { apply(config_); });
    connect(installer_, &PlaywrightInstaller::failed, this, [this](const Error &error) {
        setState(error.code == ErrorCode::NotFound || error.code == ErrorCode::NotSupported ? State::NeedsSetup : State::Failed,
                 error.message);
    });
}

BrowserService::~BrowserService() = default;

void BrowserService::setState(State state, const QString &detail) {
    if (state_ == state && detail_ == detail) return;
    state_ = state;
    detail_ = detail;
    qCInfo(lcBrowser).noquote() << "browser:" << summary() << "—" << detail;
    emit stateChanged();
}

void BrowserService::apply(const BrowserConfig &config) {
    config_ = config;
    installs_ = ChromeProfiles::detect();
    if (!config_.enabled || config_.mode == u"off"_s) {
        plan_ = BrowserPlan{};
        connectors_.apply({});
        setState(State::Off, tr("Browser automation is off."));
        return;
    }
    if (!installer_->isInstalled()) {
        connectors_.apply({});
        if (installer_->isRunning()) return;
        if (installTried_) return;      // it failed once this session: Settings → Browser → Install tries again
        installTried_ = true;
        setState(State::Installing, tr("Installing Playwright…"));
        installer_->install(installs_.isEmpty());
        return;
    }
    start();
}

void BrowserService::start() {
    plan_ = planFor(config_, installs_, installer_->hasChromium());
    if (plan_.mode == BrowserPlan::Mode::Unavailable) {
        connectors_.apply({});
        setState(State::NeedsSetup, plan_.reason);
        return;
    }
    if (plan_.mode == BrowserPlan::Mode::BundledChromium && !installer_->hasChromium()) {
        connectors_.apply({});
        installer_->install(true);
        return;
    }
    const QString node = PlaywrightInstaller::nodeProgram();
    if (node.isEmpty()) {
        connectors_.apply({});
        setState(State::NeedsSetup, tr("Node.js is not installed. Install it with: sudo apt install nodejs npm"));
        return;
    }
    const QString data = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + u"/nexa"_s;
    const BrowserPaths paths{node, installer_->cliPath(), installer_->prefix() + u"/output"_s,
                             data + u"/browser-profile"_s, installer_->browsersPath()};
    connectors_.apply({serverConfigFor(plan_, paths, config_.extensionToken)});
    const QList<McpConnectors::Status> statuses = connectors_.statuses();
    if (!statuses.isEmpty() && statuses.first().state == u"ready"_s) return;    // unchanged and running
    setState(State::Starting, tr("Starting Playwright…"));
}

QString BrowserService::summary() const {
    switch (state_) {
    case State::Off: return tr("off");
    case State::Installing: return tr("installing…");
    case State::NeedsSetup: return tr("needs setup");
    case State::Starting: return tr("starting…");
    case State::Failed: return tr("failed");
    case State::Ready:
        if (plan_.mode == BrowserPlan::Mode::Extension) return u"%1 · %2"_s.arg(shortName(plan_.install), plan_.profileName);
        if (plan_.mode == BrowserPlan::Mode::NexaProfile) return tr("Nexa profile");
        return u"Chromium"_s;
    }
    return {};
}

QString BrowserService::targetPhrase() const {
    if (state_ != State::Ready) return {};
    if (plan_.mode == BrowserPlan::Mode::Extension) {
        return tr("the user's own %1 (profile \"%2\"), signed in as them — their open tabs are theirs, so work in a new "
                  "tab unless they ask otherwise").arg(plan_.install.displayName, plan_.profileName);
    }
    const QString browser = plan_.mode == BrowserPlan::Mode::NexaProfile ? plan_.install.displayName : u"Chromium"_s;
    return tr("a separate %1 window with Nexa's own profile, not signed in to the user's accounts — when a site needs "
              "them signed in, ask them to sign in there once (it stays signed in)").arg(browser);
}

BrowserPlan BrowserService::planFor(const BrowserConfig &config, const QList<BrowserInstall> &installs, bool chromiumInstalled) {
    BrowserPlan plan;
    if (!config.enabled || config.mode == u"off"_s) return plan;
    const BrowserInstall *install = nullptr;
    for (const BrowserInstall &candidate : installs) {
        if (candidate.id == config.browser) install = &candidate;
    }
    for (const BrowserInstall &candidate : installs) {
        if (!install && candidate.supportsExtension) install = &candidate;
    }
    if (!install && !installs.isEmpty()) install = &installs.first();

    if (install && install->supportsExtension && config.mode != u"nexa-profile"_s) {
        // A profile the user chose counts alone; otherwise Playwright's own order.
        QString profile;
        if (config.profile.isEmpty()) {
            profile = ChromeProfiles::extensionProfile(*install, {});
        } else if (const BrowserProfile *chosen = install->profile(config.profile); chosen && chosen->extension) {
            profile = config.profile;
        }
        if (!profile.isEmpty()) {
            plan.mode = BrowserPlan::Mode::Extension;
            plan.install = *install;
            plan.profile = profile;
            plan.profileName = install->profile(profile) ? install->profile(profile)->name : profile;
            return plan;
        }
    }
    if (config.mode == u"extension"_s) {
        plan.mode = BrowserPlan::Mode::Unavailable;
        plan.reason = !install ? QObject::tr("There is no Chrome or Edge on this PC for the Playwright extension.")
                    : !install->supportsExtension
                        ? QObject::tr("%1 cannot take the Playwright extension; choose Chrome or Edge, or Nexa's own profile.")
                              .arg(install->displayName)
                        : QObject::tr("The Playwright extension is not installed in %1 yet: add it from Settings → Browser.")
                              .arg(install->displayName);
        return plan;
    }
    if (install) {
        plan.mode = BrowserPlan::Mode::NexaProfile;
        plan.install = *install;
        return plan;
    }
    plan.mode = BrowserPlan::Mode::BundledChromium;
    if (!chromiumInstalled) plan.reason = QObject::tr("Playwright's Chromium is being installed.");
    return plan;
}

McpServerConfig BrowserService::serverConfigFor(const BrowserPlan &plan, const BrowserPaths &paths, const QString &token) {
    McpServerConfig server;
    server.id = u"browser"_s;
    server.name = u"Browser"_s;
    server.transport = u"stdio"_s;
    server.command = paths.node;
    server.arguments = {paths.cli, u"--no-webmcp"_s, u"--output-dir"_s, paths.outputDir};
    const bool channel = plan.install.id == u"chrome"_s || plan.install.id == u"msedge"_s;
    switch (plan.mode) {
    case BrowserPlan::Mode::Extension:
        server.arguments += {u"--extension"_s, u"--profile-dir-name"_s, plan.profile,
                             u"--browser"_s, plan.install.id, u"--executable-path"_s, plan.install.executable};
        if (!token.isEmpty()) server.environment.append(u"PLAYWRIGHT_MCP_EXTENSION_TOKEN="_s + token);
        break;
    case BrowserPlan::Mode::NexaProfile:
        server.arguments += {u"--browser"_s, channel ? plan.install.id : u"chromium"_s,
                             u"--executable-path"_s, plan.install.executable, u"--user-data-dir"_s, paths.nexaProfile};
        break;
    case BrowserPlan::Mode::BundledChromium:
        server.arguments += {u"--browser"_s, u"chromium"_s, u"--user-data-dir"_s, paths.nexaProfile};
        break;
    case BrowserPlan::Mode::Off:
    case BrowserPlan::Mode::Unavailable:
        break;
    }
    server.environment.append(u"PLAYWRIGHT_BROWSERS_PATH="_s + paths.browsersPath);
    return server;
}

std::optional<ToolSpec> BrowserService::shapeTool(const McpClient::Tool &tool, ToolSpec defaults, BrowserPlan::Mode mode) {
    const auto kind = allowed().constFind(tool.name);
    if (kind == allowed().constEnd()) return std::nullopt;
    // Signed in as the user, a request's headers and cookies are theirs: not for the provider's eyes.
    if (mode == BrowserPlan::Mode::Extension && tool.name.startsWith(u"browser_network_request"_s)) return std::nullopt;
    ToolSpec spec = std::move(defaults);
    spec.name = tool.name;
    spec.description = tool.description.isEmpty() ? tool.name : tool.description;
    spec.parameters = withoutFileNames(spec.parameters);
    spec.mutating = *kind != Kind::ReadOnly;
    spec.privileged = *kind == Kind::Acts || *kind == Kind::Uploads;
    spec.risk = *kind == Kind::Uploads ? QObject::tr("uploads files from your PC to a website")
              : *kind == Kind::Acts    ? QObject::tr("acts on web pages in your browser, signed in as you")
                                       : QString();
    return spec;
}

QJsonObject BrowserService::shapeArguments(const QString &, QJsonObject arguments) {
    // A file name is resolved in the user's home: a page must not get the model to write there.
    arguments.remove(u"filename"_s);
    return arguments;
}

ToolResult BrowserService::shapeResult(const QString &, ToolResult result) {
    if (result.text.size() > kMaxResultText) {
        const qsizetype cut = result.text.size() - kMaxResultText;
        result.text = result.text.left(kMaxResultText)
                    + u"\n[… %1 more characters of the page were left out; use browser_find to look for something specific]"_s.arg(cut);
    }
    if (result.image) {
        QImage image;
        if (image.loadFromData(result.image->data)) {
            const ScreenCapture::Shot shot = ScreenCapture::encode(image);
            result.image = ImageAttachment{shot.jpeg, u"image/jpeg"_s};
        } else {
            result.image.reset();
        }
    }
    return result;
}

} // namespace nexa
