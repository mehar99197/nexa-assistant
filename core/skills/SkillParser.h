#pragma once

#include "core/Result.h"

#include <QString>

namespace nexa {

// What a SKILL.md holds: the front matter's name and description, and the instructions after it.
struct SkillDocument {
    QString name;           // [a-z0-9-], at most 64: also its folder's name
    QString description;    // when to use it — what the model reads in every request
    QString body;           // the instructions, read when the skill is used

    bool operator==(const SkillDocument &) const = default;
};

// Reads and writes SKILL.md, the format Claude's skills use: a "---" front matter of `key: value`
// lines, then Markdown. Keys other than name and description are ignored (real skills carry
// argument-hint, model, …); values may be quoted or "|" / ">" blocks; CRLF and a BOM are fine.
// Pure. (Parser)
class SkillParser {
public:
    static constexpr int kMaxDescription = 1024;

    static Result<SkillDocument> parse(const QString &text);
    static bool isValidName(const QString &name);
    // A SKILL.md for the document, the description quoted so any text survives the round trip.
    static QString compose(const SkillDocument &document);
};

} // namespace nexa
