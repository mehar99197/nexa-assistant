#pragma once

#include <QByteArray>
#include <QString>

namespace nexa::tunnellog {

// What one line of a tunnel program's log says (pure): the public address it got, that it is connected, or an error —
// `fatal` when the tunnel cannot work until the owner does something (no certificate, no authtoken, no such tunnel).
struct LineEvent {
    QString url;
    bool connected{false};
    QString error;          // one line, no newlines
    bool fatal{false};
};

// cloudflared's own log (stderr): "INF |  https://….trycloudflare.com  |", "INF Registered tunnel connection …",
// "ERR …".
LineEvent cloudflaredLine(const QString &line);
// ngrok with --log=stdout --log-format=json: {"msg":"started tunnel","url":…}, {"lvl":"eror","err":…}.
LineEvent ngrokLine(const QString &line);

// `tailscale status --json`, as far as Nexa needs it: signed in (BackendState Running), the 100.x address and the
// MagicDNS name. Anything that is not its JSON — the daemon is not running — is "nothing known".
struct TailscaleState {
    QString backend;        // "Running", "NeedsLogin", "Stopped", …; empty: no answer
    bool signedIn{false};
    QString ipv4;           // "100.101.102.103"
    QString dnsName;        // "nexa-laptop.tail1234.ts.net"
};
TailscaleState parseTailscaleStatus(const QByteArray &json);
// The sign-in link `tailscale up` / `tailscale login` prints, or empty.
QString tailscaleLoginUrl(const QString &output);

} // namespace nexa::tunnellog
