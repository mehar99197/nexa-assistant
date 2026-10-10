#include "voice/pipeline/WakeWordDetector.h"

#include <QRegularExpression>

using namespace Qt::StringLiterals;

namespace nexa {

std::optional<WakeWordDetector::Match> WakeWordDetector::match(const QString &transcript) {
    static const QRegularExpression greeted(
        u"^\\W*(?:hey|hi|hello|ok|okay|yo)[\\s,]+"
        "(?:nexa|nexxa|nexah|neksa|nexer|nexar|nexus|nexia|nixa|nexo|next a)(?:'s)?\\b"
        "[\\s,.!?:;-]*(.*)$"_s,
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression bare(u"^\\W*(?:nexa|nexxa|nexah|neksa)\\b[\\s,.!?:;-]*(.*)$"_s,
                                         QRegularExpression::CaseInsensitiveOption
                                             | QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression onlyPunctuation(u"^[\\s\\p{P}]*$"_s);

    for (const QRegularExpression *pattern : {&greeted, &bare}) {
        const QRegularExpressionMatch found = pattern->match(transcript.trimmed());
        if (!found.hasMatch()) continue;
        QString command = found.captured(1).trimmed();
        if (onlyPunctuation.match(command).hasMatch()) command.clear();
        return Match{command};
    }
    return std::nullopt;
}

} // namespace nexa
