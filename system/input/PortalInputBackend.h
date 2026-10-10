#pragma once

#include "system/input/IInputBackend.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace nexa {

class RemoteDesktopSession;

// Adapter: keyboard and pointer through the desktop's RemoteDesktop portal. The compositor injects
// the events itself, so this reaches every window with no root helper at all — and it is the only
// backend that can put the pointer on an exact pixel under GNOME's Wayland. The price is a one-time
// "allow remote interaction" prompt, and the screen-sharing indicator while a session is open.
class PortalInputBackend : public IInputBackend {
    Q_OBJECT
public:
    explicit PortalInputBackend(RemoteDesktopSession *session, QObject *parent = nullptr);

    QString name() const override;
    QString reach() const override;

    void typeText(const QString &text, Done done) override;
    void pressKeys(const QList<int> &keyCodes, Done done) override;
    void mouseMove(int dx, int dy, Done done) override;
    void mouseMoveAbs(int x, int y, int screenWidth, int screenHeight, Done done) override;
    void mouseClick(int button, int count, Done done) override;
    void mouseScroll(int dx, int dy, Done done) override;

    // Pure, so the easy-to-flip details are tested. The tools number buttons 1 left, 2 right,
    // 3 middle; the portal wants evdev codes.
    [[nodiscard]] static int evdevButton(int button);
    // The tools scroll up for positive dy; the portal follows libinput, where positive is down.
    // Returns (vertical, horizontal) portal steps.
    [[nodiscard]] static std::pair<int, int> portalScrollSteps(int dx, int dy);

private:
    using Step = std::function<void(std::function<void(Result<void>)>)>;

    Step keyStep(int code, bool down) const;
    Step buttonStep(int code, bool down) const;
    // Runs the steps one after another, stopping at the first that fails. One D-Bus connection keeps
    // them in order; waiting for each answer is what makes "done" mean the events were taken.
    void run(std::vector<Step> steps, Done done, QString note = {});
    void runFrom(std::shared_ptr<const std::vector<Step>> steps, std::size_t index, Done done, QString note);

    RemoteDesktopSession *session_;
};

} // namespace nexa
