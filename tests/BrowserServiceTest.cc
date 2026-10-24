// system/browser: which browser Playwright works in, the server's command line, the tools Nexa
// takes from it and how, what passes in and out — and the installer's pure parts.

#include "core/mcp/McpConnectors.h"
#include "core/mcp/McpTransport.h"
#include "system/browser/BrowserService.h"
#include "system/browser/PlaywrightInstaller.h"

#include "QtStringMakers.h"

#include <QBuffer>
#include <QImage>
#include <QJsonArray>

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace Qt::StringLiterals;

namespace {

BrowserInstall chrome(bool extensionInProfile1) {
    return BrowserInstall{u"chrome"_s, u"Google Chrome"_s, u"/usr/bin/google-chrome-stable"_s, u"/home/u/.config/google-chrome"_s,
                          u"Profile 1"_s,
                          {{u"Default"_s, u"Work"_s, false}, {u"Profile 1"_s, u"Ahmad"_s, extensionInProfile1}}, true};
}

BrowserInstall brave() {
    return BrowserInstall{u"brave"_s, u"Brave"_s, u"/usr/bin/brave-browser"_s, u"/home/u/.config/BraveSoftware/Brave-Browser"_s,
                          {}, {}, false};
}

const BrowserPaths kPaths{u"/usr/bin/node"_s, u"/data/playwright/node_modules/@playwright/mcp/cli.js"_s, u"/data/playwright/output"_s,
                          u"/data/browser-profile"_s, u"/data/playwright/browsers"_s};

McpClient::Tool tool(const QString &name, QJsonObject schema = {}) {
    return McpClient::Tool{name, u"Does "_s + name, schema, {}};
}

ToolSpec defaults(const McpClient::Tool &t) {
    return ToolSpec{McpConnectors::toolName(u"browser"_s, t.name), t.description, t.inputSchema, true, true, u"calls"_s};
}

} // namespace

TEST_CASE("The plan: the user's own browser when the extension is there, Nexa's profile otherwise") {
    BrowserConfig config;
    CHECK(BrowserService::planFor(BrowserConfig{.enabled = false}, {chrome(true)}, false).mode == BrowserPlan::Mode::Off);
    config.mode = u"off"_s;
    CHECK(BrowserService::planFor(config, {chrome(true)}, false).mode == BrowserPlan::Mode::Off);

    config.mode = u"auto"_s;
    const BrowserPlan own = BrowserService::planFor(config, {chrome(true)}, false);
    CHECK(own.mode == BrowserPlan::Mode::Extension);
    CHECK(own.profile == u"Profile 1"_s);
    CHECK(own.profileName == u"Ahmad"_s);
    CHECK(BrowserService::planFor(config, {chrome(false)}, false).mode == BrowserPlan::Mode::NexaProfile);

    // A profile the user chose counts alone.
    config.profile = u"Default"_s;
    CHECK(BrowserService::planFor(config, {chrome(true)}, false).mode == BrowserPlan::Mode::NexaProfile);
    config.profile.clear();

    config.mode = u"nexa-profile"_s;
    CHECK(BrowserService::planFor(config, {chrome(true)}, false).mode == BrowserPlan::Mode::NexaProfile);

    config.mode = u"extension"_s;
    const BrowserPlan missing = BrowserService::planFor(config, {chrome(false)}, false);
    CHECK(missing.mode == BrowserPlan::Mode::Unavailable);
    CHECK(missing.reason.contains(u"Settings → Browser"_s));
    config.browser = u"brave"_s;
    CHECK(BrowserService::planFor(config, {brave()}, false).reason.contains(u"cannot take the Playwright extension"_s));

    config.mode = u"auto"_s;
    CHECK(BrowserService::planFor(config, {brave()}, false).mode == BrowserPlan::Mode::NexaProfile);
    CHECK(BrowserService::planFor(config, {}, true).mode == BrowserPlan::Mode::BundledChromium);
}

TEST_CASE("The server's command line: one argument per value, the token only in the environment") {
    BrowserConfig config;
    const BrowserPlan own = BrowserService::planFor(config, {chrome(true)}, false);
    const McpServerConfig server = BrowserService::serverConfigFor(own, kPaths, u"secret-token"_s);
    CHECK(server.command == u"/usr/bin/node"_s);
    CHECK(server.arguments.first() == kPaths.cli);
    CHECK(server.arguments.contains(u"--no-webmcp"_s));
    CHECK(server.arguments.contains(u"--extension"_s));
    const qsizetype at = server.arguments.indexOf(u"--profile-dir-name"_s);
    REQUIRE(at >= 0);
    CHECK(server.arguments.at(at + 1) == u"Profile 1"_s);          // with its space, as one argument
    CHECK(server.environment.contains(u"PLAYWRIGHT_MCP_EXTENSION_TOKEN=secret-token"_s));
    CHECK_FALSE(server.arguments.join(u' ').contains(u"secret-token"_s));
    CHECK_FALSE(McpConnectors::makeTransport(server, nullptr)->describe().contains(u"secret-token"_s));

    const McpServerConfig fallback = BrowserService::serverConfigFor(BrowserService::planFor(config, {chrome(false)}, false),
                                                                     kPaths, u"secret-token"_s);
    CHECK_FALSE(fallback.arguments.contains(u"--extension"_s));
    CHECK(fallback.arguments.contains(u"/data/browser-profile"_s));
    CHECK(fallback.arguments.at(fallback.arguments.indexOf(u"--browser"_s) + 1) == u"chrome"_s);
    CHECK_FALSE(fallback.environment.join(u' ').contains(u"secret-token"_s));

    const McpServerConfig braveServer = BrowserService::serverConfigFor(BrowserService::planFor(config, {brave()}, false), kPaths, {});
    CHECK(braveServer.arguments.at(braveServer.arguments.indexOf(u"--browser"_s) + 1) == u"chromium"_s);
    CHECK(braveServer.arguments.at(braveServer.arguments.indexOf(u"--executable-path"_s) + 1) == u"/usr/bin/brave-browser"_s);
}

TEST_CASE("Only the browser tools Nexa knows are taken, each with its own flags and risks") {
    using Mode = BrowserPlan::Mode;
    const auto shaped = [](const McpClient::Tool &t, Mode mode = Mode::NexaProfile) {
        return BrowserService::shapeTool(t, defaults(t), mode);
    };
    CHECK_FALSE(shaped(tool(u"browser_run_code_unsafe"_s)).has_value());
    CHECK_FALSE(shaped(tool(u"webmcp_buy_now"_s)).has_value());
    CHECK_FALSE(shaped(tool(u"browser_something_new"_s)).has_value());
    CHECK_FALSE(shaped(tool(u"browser_resize"_s)).has_value());
    CHECK(shaped(tool(u"browser_network_requests"_s), Mode::NexaProfile).has_value());
    CHECK_FALSE(shaped(tool(u"browser_network_requests"_s), Mode::Extension).has_value());     // cookies of the user's sessions

    const auto snapshot = shaped(tool(u"browser_snapshot"_s));
    REQUIRE(snapshot.has_value());
    CHECK(snapshot->name == u"browser_snapshot"_s);
    CHECK_FALSE(snapshot->mutating);
    CHECK_FALSE(snapshot->privileged);

    const auto navigate = shaped(tool(u"browser_navigate"_s));
    CHECK(navigate->mutating);
    CHECK_FALSE(navigate->privileged);

    const auto click = shaped(tool(u"browser_click"_s));
    CHECK(click->privileged);
    CHECK(click->risk.contains(u"signed in as you"_s));
    const auto upload = shaped(tool(u"browser_file_upload"_s));
    CHECK(upload->privileged);
    CHECK(upload->risk.contains(u"uploads files"_s));
    CHECK(shaped(tool(u"browser_drop"_s))->risk.contains(u"uploads files"_s));

    // File names leave the schema; so does $schema.
    const QJsonObject schema{{u"$schema"_s, u"http://json-schema.org/draft-07/schema#"_s}, {u"type"_s, u"object"_s},
                             {u"properties"_s, QJsonObject{{u"depth"_s, QJsonObject{{u"type"_s, u"number"_s}}},
                                                           {u"filename"_s, QJsonObject{{u"type"_s, u"string"_s}}}}},
                             {u"required"_s, QJsonArray{u"filename"_s}}};
    const auto withFile = shaped(tool(u"browser_take_screenshot"_s, schema));
    CHECK_FALSE(withFile->parameters.contains(u"$schema"_s));
    CHECK_FALSE(withFile->parameters.value(u"properties"_s).toObject().contains(u"filename"_s));
    CHECK(withFile->parameters.value(u"properties"_s).toObject().contains(u"depth"_s));
    CHECK(withFile->parameters.value(u"required"_s).toArray().isEmpty());
}

TEST_CASE("What goes to the server loses its file names; what comes back is kept small") {
    const QJsonObject sent = BrowserService::shapeArguments(u"browser_snapshot"_s,
                                                            QJsonObject{{u"filename"_s, u".bashrc"_s}, {u"depth"_s, 3}});
    CHECK_FALSE(sent.contains(u"filename"_s));
    CHECK(sent.value(u"depth"_s).toInt() == 3);

    const ToolResult longPage = BrowserService::shapeResult(u"browser_snapshot"_s, ToolResult{QString(40'000, u'x'), std::nullopt});
    CHECK(longPage.text.size() < 16'200);
    CHECK(longPage.text.contains(u"browser_find"_s));

    QImage big(3000, 2000, QImage::Format_RGB32);
    big.fill(Qt::white);
    QByteArray png;
    QBuffer buffer(&png);
    REQUIRE(buffer.open(QIODevice::WriteOnly));
    REQUIRE(big.save(&buffer, "PNG"));
    const ToolResult picture = BrowserService::shapeResult(u"browser_take_screenshot"_s,
                                                           ToolResult{u"shot"_s, ImageAttachment{png, u"image/png"_s}});
    REQUIRE(picture.image.has_value());
    CHECK(picture.image->mimeType == u"image/jpeg"_s);
    const QImage small = QImage::fromData(picture.image->data);
    CHECK(qMax(small.width(), small.height()) <= 1280);
    CHECK_FALSE(BrowserService::shapeResult(u"x"_s, ToolResult{u"t"_s, ImageAttachment{"not an image", u"image/png"_s}}).image.has_value());
}

TEST_CASE("The installer reads Node's version and asks npm for the pinned package, scripts off") {
    CHECK(PlaywrightInstaller::nodeMajor(u"v24.19.0\n"_s) == 24);
    CHECK(PlaywrightInstaller::nodeMajor(u"v18.0.1"_s) == 18);
    CHECK_FALSE(PlaywrightInstaller::nodeMajor(u"command not found"_s).has_value());
    const QStringList npm = PlaywrightInstaller::npmArguments(u"/data/nexa/playwright"_s);
    CHECK(npm.first() == u"install"_s);
    CHECK(npm.contains(u"--ignore-scripts"_s));
    CHECK(npm.at(npm.indexOf(u"--prefix"_s) + 1) == u"/data/nexa/playwright"_s);
    CHECK(npm.last() == u"@playwright/mcp@0.0.83"_s);

    PlaywrightInstaller installer(u"/nonexistent/prefix"_s);
    CHECK_FALSE(installer.isInstalled());
    CHECK(installer.cliPath() == u"/nonexistent/prefix/node_modules/@playwright/mcp/cli.js"_s);
}
