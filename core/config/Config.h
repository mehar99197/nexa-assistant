#pragma once

#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

namespace nexa {

// Values of ProviderConfig::kind.
inline constexpr QLatin1StringView kKindOpenAiCompatible{"openai-compatible"};
inline constexpr QLatin1StringView kKindAnthropic{"anthropic"};
inline constexpr QLatin1StringView kKindGemini{"gemini"};
// Programs Nexa runs rather than APIs it posts to: they answer with their own sign-in and quota.
inline constexpr QLatin1StringView kKindAgy{"agy"};
inline constexpr QLatin1StringView kKindOpenCode{"opencode"};
inline constexpr QLatin1StringView kKindCodex{"codex"};

// One configured AI endpoint; `kind` selects the IAiProvider implementation.
struct ProviderConfig {
    QString id;                 // stable key, e.g. "opencode"
    QString kind;
    QString name;               // shown in the UI
    QString baseUrl;            // agy: the program (empty: found); opencode: a server to use (empty: Nexa's own)
    QString apiKey;
    QString defaultModel;       // the model chosen in the toolbar
    bool requiresKey{true};
    QString sttModel;           // speech recognition (empty: no cloud recognition)
    QString ttsModel;           // speech output (empty: no cloud voice)
    QString ttsVoice;           // voice name; a language code for gtts/*
    bool textTools{false};      // the model cannot call tools: describe them in the prompt instead
    bool fallback{true};        // may answer when the providers before it in the list cannot
    QString modelsUrl;          // where the model list is, when not {baseUrl}/models

    QJsonObject toJson() const;
    static ProviderConfig fromJson(const QJsonObject &json);

    bool operator==(const ProviderConfig &) const = default;
};

QString defaultSystemPrompt();

// Settings → Voice. The cloud models come from the active provider (ProviderConfig::sttModel,
// ttsModel); "auto" uses them when set and falls back to the local engines.
struct VoiceConfig {
    QString sttBackend{QStringLiteral("auto")};         // auto (cloud, then local) | cloud | whisper
    QString whisperModel{QStringLiteral("base.en")};    // ~/.local/share/nexa/models/ggml-<name>.bin
    QString language{QStringLiteral("en")};             // spoken language (ISO 639-1), "auto" to detect
    QString ttsBackend{QStringLiteral("auto")};         // auto (cloud, then Piper) | cloud | piper | speechd | off
    QString piperVoice;                                 // e.g. en_US-lessac-medium (empty: the first installed)
    QString speechdVoice;                               // speech-dispatcher voice (empty: its default)
    double ttsRate{0.0};                                // -1 (slow) … 1 (fast)
    QString speakReplies{QStringLiteral("voice")};      // voice (replies to spoken requests) | always | never
    bool wakeWord{false};                               // listen for the wake name (local recognition only)
    // What interrupts Nexa while it speaks: name (saying its name — the owner's voice, not the speakers'), speech (any
    // voice: with headphones only, or Nexa hears itself) or off. Until v5 a switch: on and off both became "name".
    QString bargeIn{QStringLiteral("name")};
    QString wakeName{QStringLiteral("Nexa")};           // what the owner calls Nexa ("Hey Jarvis")
    QString wakeSensitivity{QStringLiteral("normal")};  // low | normal | high: how easily the name wakes Nexa
    QString endPause{QStringLiteral("normal")};         // short (0.9 s) | normal (1.4 s) | long (2 s) of quiet ends a request
    bool followUps{true};                               // listen a few seconds after a spoken reply, no name needed
    bool noiseReduction{true};                          // clean the sound for the speech detector and the spotter
    QString sttProvider;                                // the provider that recognises requests; empty: automatic
    QString ttsProvider;                                // the provider whose voice speaks; empty: automatic
    QString listenTo{QStringLiteral("owner")};          // owner (once the voice is enrolled) | anyone
    QString ownerStrictness{QStringLiteral("normal")};  // low | normal | high: how sure "it is the owner" must be
    bool conversation{false};                           // keep listening to the owner after each reply, no name needed
    QString announceTasks{QStringLiteral("voice")};     // off | voice (tasks asked by voice) | all: said aloud when they end

    QJsonObject toJson() const;
    static VoiceConfig fromJson(const QJsonObject &json);

    bool operator==(const VoiceConfig &) const = default;
};

// Settings → Android. The phone is remembered by its hardware serial, which is the same over USB
// and over Wi-Fi; the wireless debugging address changes every time it is switched on.
struct AndroidConfig {
    QString serial;                 // empty: whichever single phone is connected
    bool autoReconnect{true};       // reconnect to it whenever it offers wireless debugging

    QJsonObject toJson() const;
    static AndroidConfig fromJson(const QJsonObject &json);

    bool operator==(const AndroidConfig &) const = default;
};

// Settings → Remote: the web remote a phone's browser talks to. Off until the user switches it on.
struct RemoteConfig {
    bool enabled{false};
    int port{7777};                 // HTTPS on the network; plain HTTP on loopback at port + 1
    bool announceMessages{true};    // the PC says when a request comes from the phone ("Message received…")
    // Away from home (Settings → Remote): the ways a phone reaches this PC from anywhere.
    bool tailscale{true};                           // offer the Tailscale address when this PC is on the tailnet
    QString cloudflare{QStringLiteral("off")};      // off | quick (a trycloudflare.com address) | named (your domain)
    QString cloudflareTunnel{QStringLiteral("nexa")};   // the named tunnel (cloudflared tunnel create <name>)
    QString cloudflareHostname;                     // "nexa.nexacodesolutions.online"
    bool ngrok{false};
    QString ngrokDomain;                            // the account's fixed domain, or empty for ngrok's own choice

    QJsonObject toJson() const;
    static RemoteConfig fromJson(const QJsonObject &json);

    bool operator==(const RemoteConfig &) const = default;
};

// The agent loop. Driving a phone by screenshot and tap takes a dozen rounds for one request, and a
// longer task many dozens, so the cap is generous; it only stops a model that is going in circles.
struct AgentConfig {
    static constexpr int kMaxToolRounds = 1000;
    int maxToolRounds{200};

    QJsonObject toJson() const;
    static AgentConfig fromJson(const QJsonObject &json);

    bool operator==(const AgentConfig &) const = default;
};

// Settings → Connectors: an MCP server (Model Context Protocol) whose tools the model may call —
// a program Nexa runs and talks to on its standard streams, or a URL it posts to.
struct McpServerConfig {
    QString id;                                     // stable key, e.g. "filesystem"
    QString name;                                   // shown in the UI and in the tools' descriptions
    bool enabled{true};
    QString transport{QStringLiteral("stdio")};     // stdio | http
    QString command;                                // stdio: the program
    QStringList arguments;
    QStringList environment;                        // KEY=VALUE, on top of Nexa's own
    QString url;                                    // http: where every message is posted
    QStringList headers;                            // "Name: value" — an Authorization, typically

    QJsonObject toJson() const;
    static McpServerConfig fromJson(const QJsonObject &json);

    bool operator==(const McpServerConfig &) const = default;
};

struct McpConfig {
    QList<McpServerConfig> servers;

    QJsonObject toJson() const;
    static McpConfig fromJson(const QJsonObject &json);

    bool operator==(const McpConfig &) const = default;
};

// Which provider and model a task, or a role's agent, runs on. Auto (no provider): the first choice and its chain.
struct ModelChoice {
    QString provider;       // a provider's id
    QString model;          // empty: that provider's own default model
    bool isAuto() const { return provider.isEmpty(); }

    QJsonObject toJson() const;
    static ModelChoice fromJson(const QJsonObject &json);

    bool operator==(const ModelChoice &) const = default;
};

// Settings → Mission: how many tasks run at once (the rest wait in the queue), and the model of each kind of agent
// (keys: AgentRole's keys — web, code, screen, phone, general; a role left out is Auto).
struct MissionConfig {
    static constexpr int kMaxSlots = 8;
    int maxConcurrent{3};
    QMap<QString, ModelChoice> roleModels;

    QJsonObject toJson() const;
    static MissionConfig fromJson(const QJsonObject &json);

    bool operator==(const MissionConfig &) const = default;
};

// Settings → General → Appearance, and the toolbar's palette button. Every theme is dark; "system",
// "dark" and "light" (from when there was a light look) mean Nexa's own.
struct UiConfig {
    QString theme{QStringLiteral("system")};    // a theme pack's id, or system | dark | light = Nexa's own
    QString design{QStringLiteral("mission")};  // one of ui/NexaDesigns: the shape of everything else

    QJsonObject toJson() const;
    static UiConfig fromJson(const QJsonObject &json);

    bool operator==(const UiConfig &) const = default;
};

// Settings → Browser: Playwright, which Nexa installs and runs, works in the user's own Chrome (its
// extension, in the profile chosen) or, until that is set up, in a Chrome with Nexa's own profile.
struct BrowserConfig {
    bool enabled{true};
    QString mode{QStringLiteral("auto")};       // auto | extension | nexa-profile | off
    QString browser{QStringLiteral("chrome")};  // chrome | msedge | brave | chromium
    QString profile;                            // "Default", "Profile 1", …; empty: the one used last
    QString extensionToken;                     // the extension's token: connects without asking. Never logged or shown

    QJsonObject toJson() const;
    static BrowserConfig fromJson(const QJsonObject &json);

    bool operator==(const BrowserConfig &) const = default;
};

// Settings → Memory: facts about the user kept between conversations (MemoryStore holds them).
struct MemoryConfig {
    bool enabled{true};
    bool learn{true};       // the model may save what it learns, not only what it is asked to

    QJsonObject toJson() const;
    static MemoryConfig fromJson(const QJsonObject &json);

    bool operator==(const MemoryConfig &) const = default;
};

// Settings → Skills: instructions for particular jobs, loaded by the model when one fits.
struct SkillsConfig {
    bool enabled{true};
    QStringList disabled;   // skill names left out

    QJsonObject toJson() const;
    static SkillsConfig fromJson(const QJsonObject &json);

    bool operator==(const SkillsConfig &) const = default;
};

// Typed view of ~/.config/nexa/config.json. Sections this build does not know stay in `extra` and
// are written back unchanged, so no build drops settings it does not understand.
struct Config {
    // 2: providers carry speech models (sttModel, ttsModel, ttsVoice).
    // 3: providers carry fallback and modelsUrl; an empty list gets the installed agy and opencode once.
    // 4: the agent's old default of 20 tool rounds becomes the new default.
    // 5: Mission Control — the mission section; the old default design (studio) becomes the new one (mission).
    static constexpr int kVersion = 5;

    int version{kVersion};
    QList<ProviderConfig> providers;
    QString activeProvider;
    QString systemPrompt{defaultSystemPrompt()};
    QString policyMode{QStringLiteral("autonomous")};   // autonomous | confirm-risky | confirm-all
    VoiceConfig voice;
    AndroidConfig android;
    RemoteConfig remote;
    AgentConfig agent;
    UiConfig ui;
    McpConfig mcp;
    BrowserConfig browser;
    MemoryConfig memory;
    SkillsConfig skills;
    MissionConfig mission;
    QJsonObject extra;

    QJsonObject toJson() const;
    static Config fromJson(const QJsonObject &json);

    const ProviderConfig *provider(const QString &id) const;
    ProviderConfig *provider(const QString &id);
    // The active provider, falling back to the first one; nullptr when none is configured.
    const ProviderConfig *active() const;

    bool operator==(const Config &) const = default;
};

} // namespace nexa
