#pragma once

#include "core/Result.h"

#ifdef Q_OS_UNIX
#include <QDBusConnection>
#endif
#include <QList>
#include <QObject>
#include <QPoint>
#include <QPointF>
#include <QRect>
#include <QSize>
#include <QString>
#include <QTimer>
#include <QVariantMap>

#include <functional>
#include <optional>
#include <vector>

namespace nexa {

// Facade over the desktop's RemoteDesktop portal, with a ScreenCast stream attached — the stream is
// what gives an absolute pointer position a coordinate space. This is how a Wayland desktop lets an
// application put the pointer on an exact pixel: the compositor moves its own pointer, so there is
// no pointer acceleration to fight and no root privilege involved. (A virtual absolute device from
// the root helper was tried first: GNOME took its clicks and never moved the pointer.)
//
// The desktop asks the user once — "allow remote interaction" — and hands back a restore token,
// kept in ~/.local/share/nexa, so later sessions start without asking. While a session is open
// GNOME shows that the screen is shared, so the session closes itself after a short idle spell
// rather than leaving the indicator up. Nothing reads the stream: it exists only to be pointed at.
class RemoteDesktopSession : public QObject {
    Q_OBJECT
public:
    using Done = std::function<void(Result<void>)>;

    // One monitor as the portal describes it: a PipeWire node, and the area it covers in the
    // desktop's logical coordinates.
    struct Stream {
        uint nodeId{0};
        QRect logical;
    };
    // A point inside one stream, in the units the desktop reads it in.
    struct StreamPoint {
        uint nodeId{0};
        QPointF position;
    };
    // How the desktop reads a position inside a stream. The portal documents the stream's logical
    // coordinates, and that is what other desktops get. GNOME's mutter divides whatever it is sent
    // by the monitor's scale: it reads the stream's own *pixels*, though it reports the stream's
    // size in logical units. Measured on GNOME 50 at 125 %: logical coordinates landed at 80 % of
    // the intended spot.
    enum class StreamUnits { Logical, Pixels };

    RemoteDesktopSession(QString appId, QString tokenPath, QObject *parent = nullptr);
    ~RemoteDesktopSession() override;

    static QString defaultTokenPath();
    // Whether the portal is there at all. Cheap and silent: the user is asked nothing until a
    // session is actually needed.
    static bool isAvailable();

    // Pure. Maps a point measured in the physical pixels of a desktop that size — the space of a
    // portal screenshot — onto the stream showing it. Empty when there is nothing to map onto.
    // Assumes one scale for the whole desktop, which holds for a single monitor and for monitors
    // sharing a scale.
    [[nodiscard]] static std::optional<StreamPoint> locate(QPoint physical, QSize physicalDesktop,
                                                           const QList<Stream> &streams, StreamUnits units);
    // Pure. Which units the desktop named by $XDG_CURRENT_DESKTOP reads positions in.
    [[nodiscard]] static StreamUnits unitsFor(const QString &currentDesktop);

    void pointerTo(QPoint physical, QSize physicalDesktop, Done done);
    void pointerBy(double dx, double dy, Done done);
    // evdev button codes (BTN_LEFT …).
    void pointerButton(int button, bool down, Done done);
    // libinput's direction: positive steps scroll down and right.
    void pointerScroll(int verticalSteps, int horizontalSteps, Done done);
    // evdev key codes (KEY_A …), as proto/KeyMap produces them.
    void keyboardKey(int key, bool down, Done done);

    bool isActive() const { return state_ == State::Ready; }
    void close();

private slots:
    void onResponse(uint response, const QVariantMap &results);
    void onClosed(const QVariantMap &details);

private:
    enum class State { Idle, Creating, SelectingDevices, SelectingSources, Starting, Ready };

    // Runs `action` once the session is up, starting it first if need be.
    void whenReady(std::function<void()> action, Done done);
    void registerApp();
    void createSession();
    void selectDevices();
    void selectSources();
    void start();
    // Calls a portal method that answers through a Request object, and waits for its Response.
    void request(const QString &interface, const QString &method, QList<QVariant> arguments, QVariantMap options,
                 State next);
    // Calls a Notify* method, which answers directly.
    void notify(const QString &method, QList<QVariant> arguments, Done done);
    void fail(const Error &error);
    void subscribe(const QString &path);
    void unsubscribe();
    void loadToken();
    void saveToken(const QString &token);
    void touch();       // an action happened: the idle close starts again

    QString appId_;
    QString tokenPath_;
    QString restoreToken_;
    StreamUnits units_;
#ifdef Q_OS_UNIX
    QDBusConnection bus_;
#endif
    bool registered_{false};

    State state_{State::Idle};
    QString sessionPath_;
    QString requestPath_;
    QList<Stream> streams_;
    std::vector<std::pair<std::function<void()>, Done>> waiting_;
    QTimer startTimeout_;
    QTimer idle_;
};

} // namespace nexa
