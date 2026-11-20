#pragma once

#include "core/Result.h"

#include <QDateTime>
#include <QJsonObject>
#include <QString>

namespace nexa {

// Append-only JSON-lines record of every action (~/.local/share/nexa/audit.jsonl, 0600).
// Long strings are truncated; tool arguments never contain API keys.
class AuditLog {
public:
    struct Entry {
        QDateTime time;
        QString source;             // gui | voice | phone | agent
        QString tool;
        QJsonObject arguments;
        bool ok{false};
        QString result;             // tool text or error message
        qint64 elapsedMs{0};
        bool destructive{false};
        bool confirmed{false};      // the user approved it in a confirmation prompt
        quint64 task{0};            // the task on the mission board it was done for (0: none)
    };

    explicit AuditLog(QString path = defaultPath());

    static QString defaultPath();
    const QString &path() const { return path_; }

    Result<void> append(const Entry &entry) const;
    static QJsonObject toJson(const Entry &entry);

private:
    QString path_;
};

} // namespace nexa
