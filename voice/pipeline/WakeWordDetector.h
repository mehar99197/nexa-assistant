#pragma once

#include <QString>
#include <QStringList>

#include <optional>

namespace nexa {

// Finds the wake name in a transcript — anywhere in it: "I was just saying that, hey Nexa, pause the music."
// → "pause the music.". Recognisers spell a name inconsistently, so after a greeting ("hey", "ok", …) a word
// that sounds like the name counts ("Hey Nexus", "heynecks"); without a greeting only the name itself does,
// and only where an address starts — at the beginning or after a comma or full stop — so "the Nexus phone" or
// "I love Jarvis movies" never wakes Nexa. Pure.
class WakeWordDetector {
public:
    explicit WakeWordDetector(const QString &name = QStringLiteral("Nexa"));

    struct Match {
        QString command;        // what follows the name; empty when only the name was said
        int firstWord{0};       // the greeting's word, or the name's (words = whitespace-separated)
        bool greeted{false};
    };

    std::optional<Match> find(const QString &transcript) const;

    // The greeting and the name taken off the start ("Hey Jarvis, open YouTube" → "open YouTube"; "Hey Jarvis."
    // → ""); a transcript that does not start with them comes back as it is.
    QString commandOf(const QString &transcript) const;

    // Initial prompt for the recogniser: biases it towards the right spelling of the name.
    QString recognizerPrompt() const;
    QString name() const { return name_; }

    // How a word sounds, roughly: letters only; ck, c, q → k; x → ks; ph → f; z → s; doubled letters single;
    // vowels and y dropped after the first letter. "Nexa", "Necksa" and "Neksa" all give "nks".
    static QString soundKey(const QString &word);

private:
    QString name_;
    QString nameKey_;
    int nameWords_{1};
    QStringList spellings_;     // the name and its sound-alikes, lower case
};

} // namespace nexa
