#include "core/ai/PresetFilter.h"

#include <QStringList>
#include <QUrl>

#include <algorithm>
#include <utility>
#include <vector>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

// The two words of a query that mean more than their letters.
bool isFlagWord(const QString &word) {
    return word == u"free"_s || word == u"nokey"_s;
}

bool textHas(const SearchableEntry &entry, const QString &word) {
    return entry.id.contains(word, Qt::CaseInsensitive) || entry.name.contains(word, Qt::CaseInsensitive)
        || entry.host.contains(word, Qt::CaseInsensitive) || entry.category.contains(word, Qt::CaseInsensitive);
}

bool matchesWord(const SearchableEntry &entry, const QString &word) {
    if (textHas(entry, word)) return true;
    if (word == u"free"_s) return entry.freeTier;
    if (word == u"nokey"_s) return !entry.requiresKey;
    return false;
}

// 0: the name starts with the word, 1: it contains it, 2: the word was found somewhere else (host, id, category).
int rankOf(const SearchableEntry &entry, const QString &word) {
    if (word.isEmpty() || entry.name.startsWith(word, Qt::CaseInsensitive)) return 0;
    return entry.name.contains(word, Qt::CaseInsensitive) ? 1 : 2;
}

} // namespace

QList<int> PresetFilter::filter(const QList<SearchableEntry> &entries, const QString &query, const QString &category) {
    const QStringList words = query.simplified().toLower().split(u' ', Qt::SkipEmptyParts);
    const QString wantedCategory = category.trimmed();

    // The first word that is text rather than a flag orders the result; a query of flags only keeps the list's order.
    QString rankWord;
    for (const QString &word : words) {
        if (!isFlagWord(word)) {
            rankWord = word;
            break;
        }
    }

    std::vector<std::pair<int, int>> found;   // rank, index
    for (int i = 0; i < entries.size(); ++i) {
        const SearchableEntry &entry = entries.at(i);
        if (!wantedCategory.isEmpty() && entry.category.compare(wantedCategory, Qt::CaseInsensitive) != 0) continue;
        const bool all = std::all_of(words.cbegin(), words.cend(),
                                     [&entry](const QString &word) { return matchesWord(entry, word); });
        if (all) found.emplace_back(rankOf(entry, rankWord), i);
    }
    std::stable_sort(found.begin(), found.end(),
                     [](const auto &a, const auto &b) { return a.first < b.first; });

    QList<int> indices;
    indices.reserve(qsizetype(found.size()));
    for (const auto &entry : found) indices.append(entry.second);
    return indices;
}

QString PresetFilter::hostOf(const QString &baseUrl) {
    const QString text = baseUrl.trimmed();
    // A URL has no blanks; "not a url" has no host.
    if (text.isEmpty() || std::any_of(text.cbegin(), text.cend(), [](QChar c) { return c.isSpace(); })) return {};
    // Without a scheme ("api.groq.com/openai/v1") the text is read as a network-path reference.
    const QUrl url(text.contains(u"://"_s) ? text : u"//"_s + text, QUrl::TolerantMode);
    QString host = url.host().toLower();
    if (host.isEmpty()) return {};
    if (host.contains(u':')) host = u'[' + host + u']';    // an IPv6 address, as it is written in a URL
    if (url.port() >= 0) host += u':' + QString::number(url.port());
    return host;
}

} // namespace nexa
