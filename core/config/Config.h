#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>

namespace nexa {

// Values of ProviderConfig::kind.
inline constexpr QLatin1StringView kKindOpenAiCompatible{"openai-compatible"};
inline constexpr QLatin1StringView kKindAnthropic{"anthropic"};
inline constexpr QLatin1StringView kKindGemini{"gemini"};

// One configured AI endpoint; `kind` selects the IAiProvider implementation.
struct ProviderConfig {
    QString id;                 // stable key, e.g. "omniroute"
    QString kind;
    QString name;               // shown in the UI
    QString baseUrl;
    QString apiKey;
    QString defaultModel;       // the model chosen in the toolbar
    bool requiresKey{true};
    QString sttModel;           // speech recognition (empty: no cloud recognition)
    QString ttsModel;           // speech output (empty: no cloud voice)
    QString ttsVoice;           // voice name; a language code for gtts/*
    bool textTools{false};      // the model cannot call tools: describe them in the prompt instead

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
    bool wakeWord{false};                               // listen for "Hey Nexa" (local recognition only)
    bool bargeIn{false};                                // talking interrupts the speech (best with headphones)

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

    QJsonObject toJson() const;
    static RemoteConfig fromJson(const QJsonObject &json);

    bool operator==(const RemoteConfig &) const = default;
};

// Typed view of ~/.config/nexa/config.json. Sections this build does not know stay in `extra` and
// are written back unchanged, so no build drops settings it does not understand.
struct Config {
    // 2: providers carry speech models (sttModel, ttsModel, ttsVoice).
    static constexpr int kVersion = 2;

    int version{kVersion};
    QList<ProviderConfig> providers;
    QString activeProvider;
    QString systemPrompt{defaultSystemPrompt()};
    QString policyMode{QStringLiteral("autonomous")};   // autonomous | confirm-risky | confirm-all
    VoiceConfig voice;
    AndroidConfig android;
    RemoteConfig remote;
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
