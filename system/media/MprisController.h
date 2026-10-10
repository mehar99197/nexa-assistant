#pragma once

#include "core/Result.h"

#include <QDBusConnection>
#include <QObject>
#include <QString>

#include <functional>
#include <optional>

namespace nexa {

// Facade over MPRIS (org.mpris.MediaPlayer2.*) on the session bus: picks the player that is
// playing (else a paused one, else the first) and sends it the action. Fully asynchronous.
class MprisController : public QObject {
    Q_OBJECT
public:
    enum class Action { Play, Pause, Toggle, Next, Previous, Stop };
    using Done = std::function<void(Result<QString>)>;

    explicit MprisController(QDBusConnection bus = QDBusConnection::sessionBus(), QObject *parent = nullptr);

    // Reports e.g. "Paused firefox".
    void control(Action action, Done done);

    static std::optional<Action> actionFromString(const QString &name);
    static QString methodName(Action action);

private:
    void choosePlayer(const QStringList &players, Action action, Done done);
    void send(const QString &player, Action action, Done done);

    QDBusConnection bus_;
};

} // namespace nexa
