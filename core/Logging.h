#pragma once

#include <QLoggingCategory>

namespace nexa {

// Enable with QT_LOGGING_RULES="nexa.*.debug=true".
Q_DECLARE_LOGGING_CATEGORY(lcApp)
Q_DECLARE_LOGGING_CATEGORY(lcConfig)
Q_DECLARE_LOGGING_CATEGORY(lcNet)
Q_DECLARE_LOGGING_CATEGORY(lcAi)
Q_DECLARE_LOGGING_CATEGORY(lcVoice)

} // namespace nexa
