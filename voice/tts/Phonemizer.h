#pragma once

#include "core/Result.h"

#include <QByteArray>
#include <QString>
#include <QStringList>

namespace nexa {

// English text → the phoneme tokens ZipVoice reads (Adapter over Piper's phonemizer): runs `piper_phonemize`,
// installed with Piper (scripts/download-piper.sh) beside espeak-ng's data, as a short process and takes its JSON —
// one token per phoneme: IPA letters with their stress and length marks, spaces and punctuation, the alphabet
// ZipVoice's tokens.txt lists — so Nexa's own name and any word new to the model's lexicon are spoken, not skipped.
// Synchronous: it is called on the synthesizer's worker thread, never the GUI's.
class Phonemizer {
public:
    explicit Phonemizer(QString piperDir);

    static QString binary(const QString &piperDir);      // <piperDir>/piper_phonemize
    bool available() const;
    QString problem() const;                             // why not, with the install step

    // The tokens of `text` (one line; a newline is a space). Up to a few seconds for a long paragraph.
    Result<QStringList> tokens(const QString &text) const;
    // One JSON line of piper_phonemize's output → its "phonemes" (pure).
    static Result<QStringList> parse(const QByteArray &jsonLine);

private:
    QString piperDir_;
};

} // namespace nexa
