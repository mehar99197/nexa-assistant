#pragma once

#include <QString>

#include <optional>

namespace nexa {

// Finds the wake phrase at the start of a transcript: "Hey Nexa, open Firefox." → "open Firefox.".
// Recognisers spell the made-up name inconsistently, so close variants count after a greeting
// ("Hey Nexus", "OK Neksa"); without a greeting only the name itself does, so ordinary words
// ("Next song", "The Nexus phone…") never wake Nexa. Pure.
class WakeWordDetector {
public:
    struct Match {
        QString command;    // what follows the wake phrase; empty when only the phrase was said
    };

    static std::optional<Match> match(const QString &transcript);

    // Initial prompt for the recogniser: biases it towards the right spelling of the name.
    static QString recognizerPrompt() { return QStringLiteral("Hey Nexa."); }
};

} // namespace nexa
