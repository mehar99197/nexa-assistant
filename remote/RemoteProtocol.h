#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QString>
#include <QStringList>

#include <optional>

namespace nexa::remote {

// The web remote's wire format (pure). JSON text frames in both directions, every message
// {"t": type, …}; a spoken request travels as a binary frame instead — one frame is one whole
// utterance of 16 kHz mono 16-bit PCM — so there is no base64 and no message type for it.

inline constexpr int kVersion = 1;
// A request of a few thousand characters, not a file.
inline constexpr qsizetype kMaxTextFrame = 16 * 1024;
// One minute of 16 kHz mono 16-bit audio.
inline constexpr qsizetype kMaxAudioFrame = 60 * 16'000 * 2;

// What a client may send.
struct ClientMessage {
    enum class Type { Hello, Text, Cancel, Ping };
    Type type{Type::Ping};
    QString token;      // Hello: the token pairing handed out
    QString text;       // Text: the request, as typed
};

// Anything malformed, unknown or oversized is refused rather than guessed at.
std::optional<ClientMessage> decode(QByteArrayView frame);

// What the server sends. `mode` is the Nexa core's state as NexaCoreSpec names it on the wire
// ("idle", "listen", "act", …) and `nodes` the satellites an action lights ("pc", "tools", …),
// so the phone can draw the same core as the PC.
QByteArray welcome(const QString &device, bool voice, bool busy, const QString &mode = {});
QByteArray state(const QString &mode, const QStringList &nodes = {});
QByteArray accepted(const QString &text, const QString &source);    // a request, from this phone or elsewhere
QByteArray delta(const QString &text);                              // more of the reply
QByteArray step(const QString &description, bool ok);               // a tool ran
QByteArray finished(const QString &reason);                         // "stop", "cancelled", "error", …
QByteArray failed(const QString &message);
QByteArray transcript(const QString &text);                         // what the phone's recording said
QByteArray speech(const QString &url);                              // the reply read aloud, as a WAV to fetch
QByteArray busy(bool busy);
// `code` lets the page act without reading prose: "unauthorized", "busy", "invalid", "too-fast", "no-voice".
QByteArray error(const QString &code, const QString &message);
QByteArray pong();

// The same messages as Nexa's own phone app reads them (the browser page reads the JSON itself).
struct ServerMessage {
    enum class Type { Welcome, Accepted, Delta, Step, Finished, Failed, Transcript, Speech, Busy, Error, Pong, State };
    Type type{Type::Pong};
    QString text;       // Accepted, Delta, Step, Failed, Transcript, Error: the words
    QString source;     // Accepted: "pc", "phone" or "phone-voice"
    QString device;     // Welcome: this phone, as it was named when it paired
    QString reason;     // Finished
    QString url;        // Speech: the WAV, relative to the server
    QString code;       // Error
    QString mode;       // Welcome, State: the core's state ("idle", "listen", …; empty from an older PC)
    QStringList nodes;  // State: the satellites lit ("voice", "pc", "tools", "phone")
    int version{0};     // Welcome
    bool ok{true};      // Step
    bool busy{false};   // Welcome, Busy
    bool voice{false};  // Welcome: the PC can hear a recording
};

// Unknown types are refused; missing fields take their empty value.
std::optional<ServerMessage> decodeServer(QByteArrayView frame);

// What a client sends.
QByteArray hello(const QString &token);
QByteArray text(const QString &request);
QByteArray cancel();
QByteArray ping();

} // namespace nexa::remote
