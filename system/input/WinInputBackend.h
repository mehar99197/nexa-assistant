#pragma once

#include "system/input/IInputBackend.h"

namespace nexa {

// Windows keyboard & pointer simulation via Win32 SendInput.
// Events injected by SendInput carry LLKHF_INJECTED / LLMHF_INJECTED, so WinInputLock passes
// them through even while physical keyboard/mouse input is locked in Hardcore Mode.
class WinInputBackend : public IInputBackend {
    Q_OBJECT
public:
    explicit WinInputBackend(QObject *parent = nullptr);

    QString name() const override;
    QString reach() const override;

    void typeText(const QString &text, Done done) override;
    void pressKeys(const QList<int> &keyCodes, Done done) override;
    void mouseMove(int dx, int dy, Done done) override;
    void mouseMoveAbs(int x, int y, int screenWidth, int screenHeight, Done done) override;
    void mouseClick(int button, int count, Done done) override;
    void mouseScroll(int dx, int dy, Done done) override;
};

} // namespace nexa
