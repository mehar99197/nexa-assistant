#pragma once

#include "core/Result.h"

#include <QString>
#include <QStringList>

namespace nexa {

class WordPieces;

enum class WakeSensitivity { Low, Normal, High };

// How easily each detector takes what it hears for the name (Settings → Voice → Sensitivity).
struct SpotterTuning {
    float boost{2.0f};              // the keyword spotter's keywords_score
    float threshold{0.10f};         // the keyword spotter's keywords_threshold
    float sileroThreshold{0.5f};    // Silero VAD: how sure it must be that someone speaks
    double startMarginDb{13.0};     // the energy VAD's margin over the room, without Silero
};

// What the detectors look for, built from the wake name the owner chose: the ways the name is spelled
// ("Nexa", "Necksa", "Neksa"), the keyword spotter's lines ("▁HE Y ▁NE X A @HEY_NEXA") and the tuning for a
// sensitivity. Pure.
class WakePhrases {
public:
    // Settings' rule for a name: 2–24 characters, at least two letters, Latin letters (the spotter's
    // pieces are A–Z), letters, spaces, hyphens and apostrophes only, at most three words.
    static Result<void> check(const QString &name);

    // Lower case, the name first, then up to three sound-alikes.
    static QStringList spellings(const QString &name);

    // One line per spelling and form (hey, ok, okay, hi, the bare name), "\n"-separated.
    static Result<QString> keywordLines(const QString &name, const WordPieces &pieces);

    // A label of a greeted form ("HEY_NEXA"), as opposed to the bare name ("NEXA").
    static bool isGreeted(const QString &label);

    static WakeSensitivity sensitivityFrom(const QString &value);    // "low" | "normal" | "high"
    static SpotterTuning tuning(WakeSensitivity sensitivity);
};

} // namespace nexa
