#pragma once

#include "core/Result.h"

#ifdef Q_OS_UNIX
#include <QDBusConnection>
#endif
#include <QObject>
#include <QString>

#include <functional>
#include <optional>

namespace nexa {

// Facade over media control (MPRIS over D-Bus on Linux, Win32 media keys via SendInput on Windows).
class MprisController : public QObject {
    Q_OBJECT
public:
    enum class Action { Play, Pause, Toggle, Next, Previous, Stop };
    using Done = std::function<void(Result<QString>)>;

#ifdef Q_OS_UNIX
    explicit MprisController(QDBusConnection bus = QDBusConnection::sessionBus(), QObject *parent = nullptr);
#else
    explicit MprisController(QObject *parent = nullptr);
#endif

    // Reports e.g. "Paused firefox" or "Sent media command".
    void control(Action action, Done done);

    static std::optional<Action> actionFromString(const QString &name);
    static QString methodName(Action action);

private:
#ifdef Q_OS_UNIX
    void choosePlayer(const QStringList &players, Action action, Done done);
    void send(const QString &player, Action action, Done done);

    QDBusConnection bus_;
#endif
};

} // namespace nexa
