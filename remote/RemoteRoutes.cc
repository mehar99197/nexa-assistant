#include "remote/RemoteRoutes.h"

#include <QJsonObject>
#include <QJsonValue>
#include <QUrl>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

constexpr RemoteRoute::Kind kKinds[] = {RemoteRoute::Kind::Adb, RemoteRoute::Kind::Lan, RemoteRoute::Kind::Tailscale,
                                        RemoteRoute::Kind::Cloudflare, RemoteRoute::Kind::Ngrok};

bool loopback(const QString &host) {
    return host == u"localhost" || host == u"127.0.0.1" || host == u"::1";
}

bool acceptable(const RemoteRoute &route) {
    const QUrl url(route.url, QUrl::StrictMode);
    if (!url.isValid() || url.host().isEmpty()) return false;
    if (route.kind == RemoteRoute::Kind::Adb) return url.scheme() == u"http" && loopback(url.host());
    return url.scheme() == u"https";
}

} // namespace

QString routeKey(RemoteRoute::Kind kind) {
    switch (kind) {
    case RemoteRoute::Kind::Adb: return u"adb"_s;
    case RemoteRoute::Kind::Lan: return u"lan"_s;
    case RemoteRoute::Kind::Tailscale: return u"tailscale"_s;
    case RemoteRoute::Kind::Cloudflare: return u"cloudflare"_s;
    case RemoteRoute::Kind::Ngrok: return u"ngrok"_s;
    }
    return {};
}

QString routeName(RemoteRoute::Kind kind) {
    switch (kind) {
    case RemoteRoute::Kind::Adb: return u"adb"_s;
    case RemoteRoute::Kind::Lan: return u"Wi-Fi"_s;
    case RemoteRoute::Kind::Tailscale: return u"Tailscale"_s;
    case RemoteRoute::Kind::Cloudflare: return u"Cloudflare"_s;
    case RemoteRoute::Kind::Ngrok: return u"ngrok"_s;
    }
    return {};
}

QList<RemoteRoute> orderedRoutes(const QList<RemoteRoute> &routes) {
    QList<RemoteRoute> ordered;
    for (const RemoteRoute::Kind kind : kKinds) {
        for (const RemoteRoute &route : routes) {
            const bool seen = std::any_of(ordered.cbegin(), ordered.cend(),
                                          [&route](const RemoteRoute &kept) { return kept.url == route.url; });
            if (route.kind == kind && !seen) ordered.append(route);
        }
    }
    return ordered;
}

QJsonArray encodeRoutes(const QList<RemoteRoute> &routes) {
    QJsonArray array;
    for (const RemoteRoute &route : routes) {
        array.append(QJsonObject{{u"kind"_s, routeKey(route.kind)}, {u"url"_s, route.url}});
    }
    return array;
}

QList<RemoteRoute> decodeRoutes(const QJsonArray &array) {
    QList<RemoteRoute> routes;
    for (const QJsonValue &value : array) {
        const QJsonObject object = value.toObject();
        const QString key = object.value(u"kind"_s).toString();
        const auto kind = std::find_if(std::begin(kKinds), std::end(kKinds),
                                       [&key](RemoteRoute::Kind candidate) { return routeKey(candidate) == key; });
        if (kind == std::end(kKinds)) continue;
        const RemoteRoute route{*kind, object.value(u"url"_s).toString()};
        if (acceptable(route)) routes.append(route);
    }
    return routes;
}

} // namespace nexa
