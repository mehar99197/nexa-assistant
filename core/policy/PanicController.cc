#include "core/policy/PanicController.h"

#include "core/Logging.h"

namespace nexa {

void PanicController::panic() {
    qCInfo(lcApp) << "Panic: stopping everything";
    for (const auto &handler : handlers_) handler();
    emit panicked();
}

} // namespace nexa
