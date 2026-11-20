#pragma once

#include <QList>
#include <QString>

namespace nexa {

// What the gallery searches in: one provider, reduced to the text and flags a query can hit.
struct SearchableEntry {
    QString id;
    QString name;
    QString category;
    QString host;               // PresetFilter::hostOf(baseUrl)
    bool freeTier{false};
    bool requiresKey{true};
};

// Specification: the pure matching and ranking logic of the provider gallery (the widget only draws what this
// returns), kept out of app/ so that it is tested without a screen.
class PresetFilter {
public:
    // Indices into `entries` of the ones to show, best first.
    //  - `category` empty: every category; otherwise the entry's category must be this one (case-insensitive).
    //  - `query`: whitespace-separated words, every one of which must occur (case-insensitively) in the id, name,
    //    host or category. The word "free" also matches an entry with a free tier, "nokey" one that needs no key.
    //  - Ranking: a name that starts with the first word comes first, then a name that contains it, then the rest;
    //    entries of equal rank keep their order in `entries`. An empty query keeps the order as it is.
    static QList<int> filter(const QList<SearchableEntry> &entries, const QString &query, const QString &category);

    // The host of a base URL as the gallery shows it, lower-case, with ":port" when the URL names one
    // ("api.groq.com", "localhost:11434"). Empty when there is none.
    static QString hostOf(const QString &baseUrl);
};

} // namespace nexa
