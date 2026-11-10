#pragma once

#include "core/Result.h"
#include "system/net/TunnelLog.h"

#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QTimer>

#include <functional>

namespace nexa {

// Whether this PC is on the owner's Tailscale network, and at which address (Facade over the `tailscale` program,
// read only: `tailscale status --json`, every 30 s while watched). signIn() runs `tailscale up`, whose link the owner
// opens to sign in; the key stays with tailscaled. Running `tailscale up` as the user needs the one-time
// `sudo tailscale set --operator=$USER`.
class TailscaleWatcher : public QObject {
    Q_OBJECT
public:
    static constexpr int kRefreshMs = 30'000;

    explicit TailscaleWatcher(QObject *parent = nullptr);
    ~TailscaleWatcher() override;

    bool installed() const { return !program_.isEmpty(); }
    tunnellog::TailscaleState state() const { return state_; }
    void setWatched(bool watched);
    void refresh();
    // The sign-in link to open, or an empty one when it is signed in already.
    void signIn(const QString &hostname, std::function<void(Result<QString>)> done);

signals:
    void changed();

private:
    QString program_;
    tunnellog::TailscaleState state_;
    QTimer timer_;
    QPointer<QProcess> status_;     // a refresh under way
    QPointer<QProcess> login_;      // `tailscale up`, until it ends
};

} // namespace nexa
