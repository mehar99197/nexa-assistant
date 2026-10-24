#pragma once

#include "core/Result.h"

#include <QDateTime>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

namespace nexa {

// One thing Nexa knows about the user: "my office is in Multan".
struct MemoryEntry {
    QString id;                 // "m3fa9c1": what forget and Settings name it by
    QString text;
    QString origin;             // asked (the user said "remember …") | learned (the model noticed it)
    QDateTime created;
    QDateTime updated;          // a fact said again is refreshed, not repeated

    bool operator==(const MemoryEntry &) const = default;
};

// What Nexa remembers about the user between conversations, kept in
// ~/.local/share/nexa/memory.json (0600). The model saves and forgets through the memory tools, the
// fast path through "remember that …", the user edits in Settings → Memory; every fact comes back in
// the system prompt (promptSection), marked as data about the user rather than instructions.
// Passwords, keys and card numbers are refused, whoever asks. (Repository)
class MemoryStore : public QObject {
    Q_OBJECT
public:
    static constexpr int kMaxEntries = 300;
    static constexpr int kMaxText = 500;

    explicit MemoryStore(QString path = defaultPath(), QObject *parent = nullptr);
    static QString defaultPath();

    // Reads the file. A missing file is an empty memory; a malformed one is moved aside (its name
    // in the error) and memory starts empty, so one bad write never stops Nexa remembering.
    Result<void> load();
    const QList<MemoryEntry> &entries() const { return entries_; }

    // Whether the model may save what it learns on its own (Settings → Memory → Learn).
    void setLearning(bool learn) { learn_ = learn; }
    bool learning() const { return learn_; }

    // Saves a fact, or refreshes the one already saved in the same words. Refused: an empty or too
    // long text, a secret, a "learned" fact while learning is off, a full memory.
    Result<MemoryEntry> remember(const QString &text, const QString &origin);
    // Removes the entry with that id, or the one entry the words point to. Words that fit several
    // remove nothing: the error lists them with their ids. Words that fit none are an error too.
    Result<QList<MemoryEntry>> forget(const QString &idOrWords);
    // Best matches first; an empty query lists the newest.
    QList<MemoryEntry> search(const QString &query, int limit = 10) const;

    // What Settings → Memory changed while it was open, applied to the memory as it is now, so a
    // fact the model saved in the meantime survives the OK.
    struct Edits {
        QStringList removed;            // ids
        QList<MemoryEntry> changed;     // id + new text
        QStringList added;              // texts, saved as asked
    };
    Result<void> applyEdits(const Edits &edits);

    // For the system prompt: what Nexa knows (newest first, as much as fits `budget` characters,
    // then how many more there are) and how to use the memory tools — saving on its own only while
    // learning is on.
    QString promptSection(int budget = 6000) const;

    // Pure helpers, public for the tests.
    static QString normalized(const QString &text);         // lower case, letters and digits, single spaces
    static QString looksSecret(const QString &text);        // what it looks like ("a password"), or empty
    static double score(const QString &query, const QString &text);   // 0..1: how much of the query it holds

signals:
    void changed();

private:
    Result<void> save();
    Result<QString> checked(const QString &text) const;     // trimmed and allowed, or why not
    QString newId() const;
    qsizetype indexOf(const QString &id) const;

    QString path_;
    QList<MemoryEntry> entries_;
    bool learn_{true};
};

} // namespace nexa
