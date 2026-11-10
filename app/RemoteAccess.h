#pragma once

#include "core/Result.h"
#include "core/config/Config.h"
#include "remote/RemoteRoutes.h"
#include "system/net/TailscaleWatcher.h"
#include "system/net/TunnelProcess.h"

#include <QObject>

#include <functional>

namespace nexa {

class RemoteServer;

// Away from home (Mediator): from Settings → Remote, which ways a phone reaches this PC run — Tailscale (watched,
// private), a Cloudflare tunnel (named, on the owner's domain, or quick) and ngrok — and the routes they make, kept in
// the RemoteServer, which tells every phone. The tunnels connect to the server's tunnel door and run only while the
// web remote does; each program keeps its own credentials.
class RemoteAccess : public QObject {
    Q_OBJECT
public:
    explicit RemoteAccess(RemoteServer *server, QObject *parent = nullptr);
    ~RemoteAccess() override;

    void apply(const RemoteConfig &config);     // Settings saved, or the server started or stopped
    void stopAll(bool wait = false);

    const TailscaleWatcher *tailscale() const { return tailscale_; }
    const TunnelProcess *cloudflare() const { return cloudflare_; }
    const TunnelProcess *ngrok() const { return ngrok_; }
    // What Settings says of one way, in a sentence.
    QString describe(RemoteRoute::Kind kind) const;
    // `tailscale up`: the link the owner opens to put this PC on their Tailscale network (empty: it is already).
    void signInTailscale(std::function<void(Result<QString>)> done);

    // The tunnel programs: on the PATH, in ~/.local/bin, or where scripts/download-tunnels.sh puts them.
    static QString tunnelProgram(const QString &name);

signals:
    void changed();

private:
    void rebuildRoutes();

    RemoteServer *server_;
    RemoteConfig config_;
    TailscaleWatcher *tailscale_;
    TunnelProcess *cloudflare_;
    TunnelProcess *ngrok_;
};

} // namespace nexa
