#include "system/input/CombinedInputBackend.h"

namespace nexa {

CombinedInputBackend::CombinedInputBackend(std::unique_ptr<IInputBackend> primary,
                                           std::unique_ptr<IInputBackend> positioner, QObject *parent)
    : IInputBackend(parent), primary_(std::move(primary)), positioner_(std::move(positioner)) {}

QString CombinedInputBackend::name() const {
    return primary_->name();
}

QString CombinedInputBackend::reach() const {
    return primary_->reach();
}

void CombinedInputBackend::typeText(const QString &text, Done done) {
    primary_->typeText(text, std::move(done));
}

void CombinedInputBackend::pressKeys(const QList<int> &keyCodes, Done done) {
    primary_->pressKeys(keyCodes, std::move(done));
}

void CombinedInputBackend::mouseMove(int dx, int dy, Done done) {
    primary_->mouseMove(dx, dy, std::move(done));
}

void CombinedInputBackend::mouseMoveAbs(int x, int y, int screenWidth, int screenHeight, Done done) {
    positioner_->mouseMoveAbs(x, y, screenWidth, screenHeight, std::move(done));
}

// The click itself comes from the primary backend: the desktop has one pointer, so it lands
// wherever the positioner just put it.
void CombinedInputBackend::mouseClick(int button, int count, Done done) {
    primary_->mouseClick(button, count, std::move(done));
}

void CombinedInputBackend::mouseScroll(int dx, int dy, Done done) {
    primary_->mouseScroll(dx, dy, std::move(done));
}

} // namespace nexa
