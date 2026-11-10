#pragma once

#include <QJsonArray>
#include <QList>
#include <QString>

namespace nexa {

// One way a phone can reach this PC (value): through adb (plain HTTP on the phone's own loopback), on the same Wi-Fi
// or over Tailscale (HTTPS with Nexa's own certificate, pinned by its fingerprint), or through a public tunnel —
// Cloudflare or ngrok — whose HTTPS is the tunnel's, with a certificate the phone's system trusts.
struct RemoteRoute {
    enum class Kind { Adb, Lan, Tailscale, Cloudflare, Ngrok };
    Kind kind{Kind::Lan};
    QString url;            // "https://192.168.0.109:7777", "https://nexa.nexacodesolutions.online"

    bool ownCertificate() const { return kind == Kind::Lan || kind == Kind::Tailscale; }
    bool isTunnel() const { return kind == Kind::Cloudflare || kind == Kind::Ngrok; }
    bool operator==(const RemoteRoute &) const = default;
};

QString routeKey(RemoteRoute::Kind kind);       // "adb", "lan", "tailscale", "cloudflare", "ngrok"
QString routeName(RemoteRoute::Kind kind);      // "adb", "Wi-Fi", "Tailscale", "Cloudflare", "ngrok"

// The order a phone tries them in: the nearest first (adb, the Wi-Fi, Tailscale, then the tunnels); one of each URL.
QList<RemoteRoute> orderedRoutes(const QList<RemoteRoute> &routes);

// The wire form ([{kind, url}, …]). Reading it back keeps only what is exactly right: a known kind, an http(s) address
// with a host, HTTPS for everything but adb, and adb only on the phone's own loopback.
QJsonArray encodeRoutes(const QList<RemoteRoute> &routes);
QList<RemoteRoute> decodeRoutes(const QJsonArray &array);

} // namespace nexa
