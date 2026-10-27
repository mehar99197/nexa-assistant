#pragma once

#include "core/Result.h"

#include <QByteArray>
#include <QString>
#include <QStringList>

namespace nexa {

// One line of agy's `--output-format stream-json`, as far as Nexa cares.
struct AgyEvent {
    enum class Kind { Init, Text, ToolStep, Result, Ignored };
    Kind kind{Kind::Ignored};
    QString text;               // Text: a piece of the reply (step_type agent_response)
    QString toolName;           // ToolStep: one of agy's own tools
    bool ok{true};              // Result: status SUCCESS; ToolStep: not refused
    QString response;           // Result: the whole reply
    QString error;              // Result
    QStringList deniedActions;  // Result: agy's own actions it refused, by display name
};

// agy's print mode, both ways (pure): events read from its standard output, turns written to its
// standard input (`--input-format stream-json`), `agy models` output, and what its failures mean.
class AgyEvents {
public:
    static AgyEvent decode(const QByteArray &line);
    // {"event":"user","message":{"content":[{"type":"text","text":…}]}} and a newline.
    static QByteArray userTurn(const QString &text);
    // The ids of `agy models`' "id<TAB>name" lines.
    static QStringList parseModels(const QString &output);
    // agy ended before it answered: not signed in, or what it said last.
    static Error exitError(int exitCode, const QString &stderrTail);
    // A turn that ended with status ERROR; a quota or rate limit reads as HTTP 429.
    static Error resultError(const QString &error);
};

} // namespace nexa
