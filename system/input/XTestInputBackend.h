#pragma once

#include "system/input/IInputBackend.h"

#include <memory>

namespace nexa {

// Keyboard and pointer through the X11 XTEST extension: no root needed, but the events are
// injected into the X server, so under a Wayland compositor they only reach XWayland windows —
// never a native Wayland one. That makes this the fallback, used when the root helper is not
// installed, and the whole story on an X11 session.
//
// isUsable() says whether an X display with XTEST is actually there, so the selector can explain
// itself rather than failing at the first keystroke.
class XTestInputBackend : public IInputBackend {
    Q_OBJECT
public:
    explicit XTestInputBackend(QObject *parent = nullptr);
    ~XTestInputBackend() override;

    static bool isUsable(QString *reason = nullptr);
    // True on a Wayland session, where this backend only reaches X11 windows.
    static bool isWaylandSession();

    QString name() const override;
    QString reach() const override;

    void typeText(const QString &text, Done done) override;
    void pressKeys(const QList<int> &keyCodes, Done done) override;
    void mouseMove(int dx, int dy, Done done) override;
    void mouseMoveAbs(int x, int y, int screenWidth, int screenHeight, Done done) override;
    void mouseClick(int button, int count, Done done) override;
    void mouseScroll(int dx, int dy, Done done) override;

private:
    struct Display;
    Result<Display *> display();

    std::unique_ptr<Display> display_;
};

} // namespace nexa
