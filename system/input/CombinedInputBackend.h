#pragma once

#include "system/input/IInputBackend.h"

#include <memory>

namespace nexa {

// Composite: one backend types, clicks and scrolls, another puts the pointer on a given pixel.
//
// Under GNOME's Wayland the root helper reaches every window but cannot place the pointer — the
// compositor takes the clicks from its virtual absolute device and never moves the pointer — while
// the RemoteDesktop portal places it exactly. Together they cover everything, and the portal's
// one-time "allow remote interaction" prompt only appears once a position is actually asked for.
class CombinedInputBackend : public IInputBackend {
    Q_OBJECT
public:
    CombinedInputBackend(std::unique_ptr<IInputBackend> primary, std::unique_ptr<IInputBackend> positioner,
                         QObject *parent = nullptr);

    QString name() const override;
    QString reach() const override;

    void typeText(const QString &text, Done done) override;
    void pressKeys(const QList<int> &keyCodes, Done done) override;
    void mouseMove(int dx, int dy, Done done) override;
    void mouseMoveAbs(int x, int y, int screenWidth, int screenHeight, Done done) override;
    void mouseClick(int button, int count, Done done) override;
    void mouseScroll(int dx, int dy, Done done) override;

private:
    std::unique_ptr<IInputBackend> primary_;
    std::unique_ptr<IInputBackend> positioner_;
};

} // namespace nexa
