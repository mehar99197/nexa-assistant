#pragma once

#include "system/input/IInputBackend.h"

namespace nexa {

class HelperClient;

// Keyboard and pointer through nexa-helper's virtual devices (uinput). The only backend that
// reaches native Wayland windows, because the events enter at the kernel and the compositor sees
// them as coming from a real device.
class HelperInputBackend : public IInputBackend {
    Q_OBJECT
public:
    explicit HelperInputBackend(HelperClient *helper, QObject *parent = nullptr);

    QString name() const override;
    QString reach() const override;

    void typeText(const QString &text, Done done) override;
    void pressKeys(const QList<int> &keyCodes, Done done) override;
    void mouseMove(int dx, int dy, Done done) override;
    void mouseMoveAbs(int x, int y, int screenWidth, int screenHeight, Done done) override;
    void mouseClick(int button, int count, Done done) override;
    void mouseScroll(int dx, int dy, Done done) override;

private:
    // Presses and releases `count` times, one D-Bus call at a time; polkit only asks on the first.
    void clickStep(int button, int remaining, bool down, Done done);

    HelperClient *helper_;
};

} // namespace nexa
