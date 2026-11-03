#include "core/config/Config.h"

#include <QJsonArray>
#include <QRegularExpression>
#include <QStringList>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

// Earlier built-in defaults: configs still holding one of them get the current default.
const QStringList &legacyDefaultPrompts() {
    static const QStringList prompts{
        u"You are Nexa, a fast desktop assistant running on the user's Linux PC. "
        "Answer directly and concisely. Use Markdown for code, commands and lists."_s,
        u"You are Nexa, a fast assistant that operates the user's Linux desktop through tools. "
        "When the user asks for something a tool can do, do it with the tool instead of explaining how. "
        "Use open_app for graphical applications and run_command for shell commands (never run_command "
        "for GUI apps — it waits for them to exit). Call take_screenshot when you need to see the screen. "
        "Before deleting or overwriting the user's data, make sure that is what they asked for. "
        "Keep replies short; use Markdown for code and commands."_s,
        u"You are Nexa, a fast assistant that operates the user's Linux desktop through tools. "
        "When the user asks for something a tool can do, do it with the tool instead of explaining how. "
        "Use open_app for graphical applications and run_command for shell commands (never run_command "
        "for GUI apps — it waits for them to exit). Call take_screenshot when you need to see the screen. "
        "The user's Android phone, when connected, is reached through the android_* tools: take "
        "android_screenshot before tapping, and remember that android_sms_send only fills a message in — "
        "it is sent when Send is tapped. "
        "Before deleting or overwriting the user's data, make sure that is what they asked for. "
        "Keep replies short; use Markdown for code and commands."_s,
    };
    return prompts;
}

} // namespace

// Whether the model may look at the screen is not part of this: AppController adds a sentence
// for the state of the Share screen switch to every request.
QString defaultSystemPrompt() {
    return u"You are Nexa, a fast assistant that operates the user's Linux desktop through tools. "
           "When the user asks for something a tool can do, do it with the tool instead of explaining how. "
           "Use open_app for graphical applications and run_command for shell commands (never run_command "
           "for GUI apps — it waits for them to exit). Do what was asked through commands and tools; "
           "look at the screen only when a request cannot be done without seeing it. "
           "The user's Android phone, when connected, is reached through the android_* tools: take "
           "android_screenshot before tapping, and remember that android_sms_send only fills a message in — "
           "it is sent when Send is tapped. "
           "Before deleting or overwriting the user's data, make sure that is what they asked for. "
           "Keep replies short; use Markdown for code and commands."_s;
}

QJsonObject ProviderConfig::toJson() const {
    return QJsonObject{
        {u"id"_s, id},
        {u"kind"_s, kind},
        {u"name"_s, name},
        {u"baseUrl"_s, baseUrl},
        {u"apiKey"_s, apiKey},
        {u"defaultModel"_s, defaultModel},
        {u"requiresKey"_s, requiresKey},
        {u"sttModel"_s, sttModel},
        {u"ttsModel"_s, ttsModel},
        {u"ttsVoice"_s, ttsVoice},
        {u"textTools"_s, textTools},
    };
}

ProviderConfig ProviderConfig::fromJson(const QJsonObject &json) {
    ProviderConfig config;
    config.id = json.value(u"id"_s).toString();
    config.kind = json.value(u"kind"_s).toString(kKindOpenAiCompatible);
    config.name = json.value(u"name"_s).toString(config.id);
    config.baseUrl = json.value(u"baseUrl"_s).toString();
    config.apiKey = json.value(u"apiKey"_s).toString();
    config.defaultModel = json.value(u"defaultModel"_s).toString();
    config.requiresKey = json.value(u"requiresKey"_s).toBool(true);
    config.sttModel = json.value(u"sttModel"_s).toString();
    config.ttsModel = json.value(u"ttsModel"_s).toString();
    config.ttsVoice = json.value(u"ttsVoice"_s).toString();
    config.textTools = json.value(u"textTools"_s).toBool(false);
    return config;
}

QJsonObject VoiceConfig::toJson() const {
    return QJsonObject{
        {u"sttBackend"_s, sttBackend},
        {u"whisperModel"_s, whisperModel},
        {u"language"_s, language},
        {u"ttsBackend"_s, ttsBackend},
        {u"piperVoice"_s, piperVoice},
        {u"speechdVoice"_s, speechdVoice},
        {u"ttsRate"_s, ttsRate},
        {u"speakReplies"_s, speakReplies},
        {u"wakeWord"_s, wakeWord},
        {u"bargeIn"_s, bargeIn},
    };
}

VoiceConfig VoiceConfig::fromJson(const QJsonObject &json) {
    const VoiceConfig defaults;
    VoiceConfig config;
    config.sttBackend = json.value(u"sttBackend"_s).toString(defaults.sttBackend);
    config.whisperModel = json.value(u"whisperModel"_s).toString(defaults.whisperModel);
    config.language = json.value(u"language"_s).toString(defaults.language);
    config.ttsBackend = json.value(u"ttsBackend"_s).toString(defaults.ttsBackend);
    config.piperVoice = json.value(u"piperVoice"_s).toString();
    config.speechdVoice = json.value(u"speechdVoice"_s).toString();
    config.ttsRate = std::clamp(json.value(u"ttsRate"_s).toDouble(0.0), -1.0, 1.0);
    config.speakReplies = json.value(u"speakReplies"_s).toString(defaults.speakReplies);
    config.wakeWord = json.value(u"wakeWord"_s).toBool(false);
    config.bargeIn = json.value(u"bargeIn"_s).toBool(false);
    return config;
}

QJsonObject AndroidConfig::toJson() const {
    return QJsonObject{{u"serial"_s, serial}, {u"autoReconnect"_s, autoReconnect}};
}

AndroidConfig AndroidConfig::fromJson(const QJsonObject &json) {
    AndroidConfig config;
    config.serial = json.value(u"serial"_s).toString().trimmed();
    config.autoReconnect = json.value(u"autoReconnect"_s).toBool(true);
    return config;
}

QJsonObject RemoteConfig::toJson() const {
    return QJsonObject{{u"enabled"_s, enabled}, {u"port"_s, port}};
}

RemoteConfig RemoteConfig::fromJson(const QJsonObject &json) {
    RemoteConfig config;
    config.enabled = json.value(u"enabled"_s).toBool(false);
    // Two ports are used (port and port + 1), both above the privileged range.
    config.port = std::clamp(json.value(u"port"_s).toInt(7777), 1024, 65534);
    return config;
}

QJsonObject AgentConfig::toJson() const {
    return QJsonObject{{u"maxToolRounds"_s, maxToolRounds}};
}

AgentConfig AgentConfig::fromJson(const QJsonObject &json) {
    AgentConfig config;
    config.maxToolRounds = std::clamp(json.value(u"maxToolRounds"_s).toInt(config.maxToolRounds), 1, 200);
    return config;
}

QJsonObject McpServerConfig::toJson() const {
    return QJsonObject{{u"id"_s, id},
                       {u"name"_s, name},
                       {u"enabled"_s, enabled},
                       {u"transport"_s, transport},
                       {u"command"_s, command},
                       {u"arguments"_s, QJsonArray::fromStringList(arguments)},
                       {u"environment"_s, QJsonArray::fromStringList(environment)},
                       {u"url"_s, url},
                       {u"headers"_s, QJsonArray::fromStringList(headers)}};
}

McpServerConfig McpServerConfig::fromJson(const QJsonObject &json) {
    const auto strings = [&json](const QString &key) {
        QStringList values;
        for (const QJsonValue &value : json.value(key).toArray()) {
            if (value.isString()) values.append(value.toString());
        }
        return values;
    };
    McpServerConfig server;
    server.id = json.value(u"id"_s).toString();
    server.name = json.value(u"name"_s).toString(server.id);
    server.enabled = json.value(u"enabled"_s).toBool(true);
    server.transport = json.value(u"transport"_s).toString() == u"http"_s ? u"http"_s : u"stdio"_s;
    server.command = json.value(u"command"_s).toString();
    server.arguments = strings(u"arguments"_s);
    server.environment = strings(u"environment"_s);
    server.url = json.value(u"url"_s).toString();
    server.headers = strings(u"headers"_s);
    return server;
}

QJsonObject McpConfig::toJson() const {
    QJsonArray list;
    for (const McpServerConfig &server : servers) list.append(server.toJson());
    return QJsonObject{{u"servers"_s, list}};
}

McpConfig McpConfig::fromJson(const QJsonObject &json) {
    McpConfig config;
    for (const QJsonValue &value : json.value(u"servers"_s).toArray()) {
        if (!value.isObject()) continue;
        const McpServerConfig server = McpServerConfig::fromJson(value.toObject());
        const bool duplicate = std::any_of(config.servers.cbegin(), config.servers.cend(),
                                           [&server](const McpServerConfig &s) { return s.id == server.id; });
        if (!server.id.isEmpty() && !duplicate) config.servers.append(server);
    }
    return config;
}

QJsonObject UiConfig::toJson() const {
    return QJsonObject{{u"theme"_s, theme}, {u"design"_s, design}};
}

UiConfig UiConfig::fromJson(const QJsonObject &json) {
    UiConfig config;
    // system, dark, light — or a palette's id ("ocean-dark"), and a design's id ("console"); the
    // catalogues decide what they mean.
    static const QRegularExpression id(u"^[a-z0-9-]{1,40}$"_s);
    const QString theme = json.value(u"theme"_s).toString(config.theme);
    if (id.match(theme).hasMatch()) config.theme = theme;
    const QString design = json.value(u"design"_s).toString(config.design);
    if (id.match(design).hasMatch()) config.design = design;
    return config;
}

QJsonObject BrowserConfig::toJson() const {
    return QJsonObject{{u"enabled"_s, enabled},
                       {u"mode"_s, mode},
                       {u"browser"_s, browser},
                       {u"profile"_s, profile},
                       {u"extensionToken"_s, extensionToken}};
}

BrowserConfig BrowserConfig::fromJson(const QJsonObject &json) {
    BrowserConfig config;
    config.enabled = json.value(u"enabled"_s).toBool(config.enabled);
    const QString mode = json.value(u"mode"_s).toString();
    if (QStringList{u"auto"_s, u"extension"_s, u"nexa-profile"_s, u"off"_s}.contains(mode)) config.mode = mode;
    static const QRegularExpression browserId(u"^[a-z0-9-]{1,20}$"_s);
    const QString browser = json.value(u"browser"_s).toString();
    if (browserId.match(browser).hasMatch()) config.browser = browser;
    // A path component under the browser's folder: only the names Chrome itself gives profiles.
    static const QRegularExpression profileDir(u"^(Default|Profile [0-9]{1,4})$"_s);
    const QString profile = json.value(u"profile"_s).toString();
    if (profileDir.match(profile).hasMatch()) config.profile = profile;
    config.extensionToken = json.value(u"extensionToken"_s).toString().trimmed();
    return config;
}

QJsonObject MemoryConfig::toJson() const {
    return QJsonObject{{u"enabled"_s, enabled}, {u"learn"_s, learn}};
}

MemoryConfig MemoryConfig::fromJson(const QJsonObject &json) {
    MemoryConfig config;
    config.enabled = json.value(u"enabled"_s).toBool(config.enabled);
    config.learn = json.value(u"learn"_s).toBool(config.learn);
    return config;
}

QJsonObject SkillsConfig::toJson() const {
    return QJsonObject{{u"enabled"_s, enabled}, {u"disabled"_s, QJsonArray::fromStringList(disabled)}};
}

SkillsConfig SkillsConfig::fromJson(const QJsonObject &json) {
    SkillsConfig config;
    config.enabled = json.value(u"enabled"_s).toBool(config.enabled);
    static const QRegularExpression name(u"^[a-z0-9-]{1,64}$"_s);
    for (const QJsonValue &value : json.value(u"disabled"_s).toArray()) {
        const QString skill = value.toString();
        if (name.match(skill).hasMatch() && !config.disabled.contains(skill)) config.disabled.append(skill);
    }
    return config;
}

QJsonObject Config::toJson() const {
    QJsonObject json = extra;
    QJsonArray list;
    for (const ProviderConfig &provider : providers) list.append(provider.toJson());
    json.insert(u"version"_s, version);
    json.insert(u"providers"_s, list);
    json.insert(u"activeProvider"_s, activeProvider);
    json.insert(u"systemPrompt"_s, systemPrompt);
    json.insert(u"policy"_s, QJsonObject{{u"mode"_s, policyMode}});
    json.insert(u"voice"_s, voice.toJson());
    json.insert(u"android"_s, android.toJson());
    json.insert(u"remote"_s, remote.toJson());
    json.insert(u"agent"_s, agent.toJson());
    json.insert(u"ui"_s, ui.toJson());
    json.insert(u"mcp"_s, mcp.toJson());
    json.insert(u"browser"_s, browser.toJson());
    json.insert(u"memory"_s, memory.toJson());
    json.insert(u"skills"_s, skills.toJson());
    return json;
}

Config Config::fromJson(const QJsonObject &json) {
    Config config;
    config.version = json.value(u"version"_s).toInt(kVersion);
    const QJsonArray list = json.value(u"providers"_s).toArray();
    for (const QJsonValue &value : list) {
        if (!value.isObject()) continue;
        ProviderConfig provider = ProviderConfig::fromJson(value.toObject());
        if (!provider.id.isEmpty() && !config.provider(provider.id)) config.providers.append(provider);
    }
    config.activeProvider = json.value(u"activeProvider"_s).toString();
    config.systemPrompt = json.value(u"systemPrompt"_s).toString(defaultSystemPrompt());
    if (legacyDefaultPrompts().contains(config.systemPrompt)) config.systemPrompt = defaultSystemPrompt();
    config.policyMode = json.value(u"policy"_s).toObject().value(u"mode"_s).toString(u"autonomous"_s);
    config.voice = VoiceConfig::fromJson(json.value(u"voice"_s).toObject());
    config.android = AndroidConfig::fromJson(json.value(u"android"_s).toObject());
    config.remote = RemoteConfig::fromJson(json.value(u"remote"_s).toObject());
    config.agent = AgentConfig::fromJson(json.value(u"agent"_s).toObject());
    config.ui = UiConfig::fromJson(json.value(u"ui"_s).toObject());
    config.mcp = McpConfig::fromJson(json.value(u"mcp"_s).toObject());
    config.browser = BrowserConfig::fromJson(json.value(u"browser"_s).toObject());
    config.memory = MemoryConfig::fromJson(json.value(u"memory"_s).toObject());
    config.skills = SkillsConfig::fromJson(json.value(u"skills"_s).toObject());

    config.extra = json;
    for (const QString &key :
         {u"version"_s, u"providers"_s, u"activeProvider"_s, u"systemPrompt"_s, u"policy"_s, u"voice"_s, u"android"_s,
          u"remote"_s, u"agent"_s, u"ui"_s, u"mcp"_s, u"browser"_s, u"memory"_s, u"skills"_s}) {
        config.extra.remove(key);
    }
    return config;
}

const ProviderConfig *Config::provider(const QString &id) const {
    for (const ProviderConfig &candidate : providers) {
        if (candidate.id == id) return &candidate;
    }
    return nullptr;
}

ProviderConfig *Config::provider(const QString &id) {
    for (ProviderConfig &candidate : providers) {
        if (candidate.id == id) return &candidate;
    }
    return nullptr;
}

const ProviderConfig *Config::active() const {
    if (const ProviderConfig *chosen = provider(activeProvider)) return chosen;
    return providers.isEmpty() ? nullptr : &providers.first();
}

} // namespace nexa
