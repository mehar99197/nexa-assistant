#include "app/RemoteAccess.h"

#include "core/Logging.h"
#include "core/ai/ProgramLookup.h"
#include "remote/RemoteServer.h"

#include <QDir>
#include <QHostInfo>
#include <QRegularExpression>
#include <QStandardPaths>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

QString tailscaleHostname() {
    // "nexa-laptop" — a name the owner recognises in their Tailscale list, safe for a DNS label.
    QString host = QHostInfo::localHostName().toLower();
    host.replace(QRegularExpression(u"[^a-z0-9-]+"_s), u"-"_s);
    return host.isEmpty() ? u"nexa-laptop"_s : u"nexa-"_s + host.left(40);
}

} // namespace

RemoteAccess::RemoteAccess(RemoteServer *server, QObject *parent)
    : QObject(parent),
      server_(server),
      tailscale_(new TailscaleWatcher(this)),
      cloudflare_(new TunnelProcess(u"cloudflared"_s, this)),
      ngrok_(new TunnelProcess(u"ngrok"_s, this)) {
    connect(tailscale_, &TailscaleWatcher::changed, this, &RemoteAccess::rebuildRoutes);
    connect(cloudflare_, &TunnelProcess::changed, this, &RemoteAccess::rebuildRoutes);
    connect(ngrok_, &TunnelProcess::changed, this, &RemoteAccess::rebuildRoutes);
    connect(server_, &RemoteServer::runningChanged, this, [this] { apply(config_); });
}

RemoteAccess::~RemoteAccess() {
    stopAll(true);
}

QString RemoteAccess::tunnelProgram(const QString &name) {
    if (const QString found = findProgram(name); !found.isEmpty()) return found;
    const QString own = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + u"/nexa/tunnels"_s;
    return findProgramIn(name, {own});
}

void RemoteAccess::apply(const RemoteConfig &config) {
    config_ = config;
    const bool on = config.enabled && server_->isRunning();
    tailscale_->setWatched(on && config.tailscale);
    if (!on) {
        stopAll();
        rebuildRoutes();
        return;
    }
    const QString door = u"http://127.0.0.1:%1"_s.arg(server_->tunnelPort());
    // Where cloudflared's own lookup of its servers fails (a router's DNS it cannot read), they are named directly.
    const QStringList edges{u"--edge"_s, u"region1.v2.argotunnel.com:7844"_s, u"--edge"_s, u"region2.v2.argotunnel.com:7844"_s};
    const QStringList tunnel{u"tunnel"_s, u"--no-autoupdate"_s};
    if (config.cloudflare == u"named"_s && !config.cloudflareHostname.isEmpty()) {
        const QStringList run{u"run"_s, u"--url"_s, door, config.cloudflareTunnel};
        cloudflare_->start({tunnelProgram(u"cloudflared"_s), tunnel + run, &tunnellog::cloudflaredLine,
                            u"https://"_s + config.cloudflareHostname, tunnel + edges + run});
    } else if (config.cloudflare == u"quick"_s) {
        const QStringList url{u"--url"_s, door};
        cloudflare_->start({tunnelProgram(u"cloudflared"_s), tunnel + url, &tunnellog::cloudflaredLine, {},
                            tunnel + edges + url});
    } else {
        cloudflare_->stop();
    }
    if (config.ngrok) {
        QStringList arguments{u"http"_s, u"127.0.0.1:%1"_s.arg(server_->tunnelPort()), u"--log=stdout"_s,
                              u"--log-format=json"_s};
        if (!config.ngrokDomain.isEmpty()) arguments.append(u"--url=https://"_s + config.ngrokDomain);
        ngrok_->start({tunnelProgram(u"ngrok"_s), arguments, &tunnellog::ngrokLine, {}});
    } else {
        ngrok_->stop();
    }
    rebuildRoutes();
}

void RemoteAccess::stopAll(bool wait) {
    cloudflare_->stop(wait);
    ngrok_->stop(wait);
}

void RemoteAccess::rebuildRoutes() {
    QList<RemoteRoute> routes;
    if (server_->isRunning()) {
        routes.append({RemoteRoute::Kind::Adb, server_->localUrl()});
        if (const QString lan = server_->lanUrl(); !lan.isEmpty()) routes.append({RemoteRoute::Kind::Lan, lan});
        const tunnellog::TailscaleState state = tailscale_->state();
        // Over Tailscale the phone talks to Nexa's own HTTPS, so only while that runs.
        if (config_.tailscale && state.signedIn && !server_->fingerprint().isEmpty()) {
            // The address first: it needs no Tailscale DNS on the phone; the name is the same place, for a browser.
            if (!state.ipv4.isEmpty()) {
                routes.append({RemoteRoute::Kind::Tailscale, u"https://%1:%2"_s.arg(state.ipv4).arg(server_->port())});
            }
            if (!state.dnsName.isEmpty()) {
                routes.append({RemoteRoute::Kind::Tailscale, u"https://%1:%2"_s.arg(state.dnsName).arg(server_->port())});
            }
        }
        if (const QString url = cloudflare_->url(); !url.isEmpty()) routes.append({RemoteRoute::Kind::Cloudflare, url});
        if (const QString url = ngrok_->url(); !url.isEmpty()) routes.append({RemoteRoute::Kind::Ngrok, url});
    }
    server_->setRoutes(routes);
    emit changed();
}

QString RemoteAccess::describe(RemoteRoute::Kind kind) const {
    const bool on = config_.enabled && server_->isRunning();
    const auto tunnelState = [on](const TunnelProcess *tunnel, bool wanted) {
        if (!wanted) return tr("Off.");
        if (!on) return tr("Starts with the web remote.");
        switch (tunnel->state()) {
        case TunnelProcess::State::Off: return tr("Off.");
        case TunnelProcess::State::Missing: return tunnel->problem();
        case TunnelProcess::State::Starting:
            return tunnel->problem().isEmpty() ? tr("Connecting…") : tr("Connecting again — %1").arg(tunnel->problem());
        case TunnelProcess::State::Running: return tr("Reachable at %1").arg(tunnel->url());
        case TunnelProcess::State::Failed: return tr("Not working: %1").arg(tunnel->problem());
        }
        return QString();
    };
    switch (kind) {
    case RemoteRoute::Kind::Tailscale: {
        if (!tailscale_->installed()) return tr("Tailscale is not installed on this PC (https://tailscale.com/download).");
        if (!config_.tailscale) return tr("Off.");
        const tunnellog::TailscaleState state = tailscale_->state();
        if (state.signedIn) {
            return tr("On your Tailscale network as %1 (%2). The phone needs the Tailscale app, signed in to the same "
                      "account.").arg(state.dnsName, state.ipv4);
        }
        return state.backend.isEmpty() ? tr("Not running — is tailscaled started?")
                                       : tr("Not signed in: press Sign in, and open the link.");
    }
    case RemoteRoute::Kind::Cloudflare:
        if (config_.cloudflare == u"named"_s && config_.cloudflareHostname.isEmpty()) {
            return tr("Enter the address on your domain (nexa.example.com).");
        }
        return tunnelState(cloudflare_, config_.cloudflare != u"off"_s);
    case RemoteRoute::Kind::Ngrok: return tunnelState(ngrok_, config_.ngrok);
    case RemoteRoute::Kind::Adb:
    case RemoteRoute::Kind::Lan: break;
    }
    return {};
}

void RemoteAccess::signInTailscale(std::function<void(Result<QString>)> done) {
    tailscale_->signIn(tailscaleHostname(), std::move(done));
}

} // namespace nexa
