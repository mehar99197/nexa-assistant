#include "system/apps/DesktopEntryIndex.h"

#include "core/Logging.h"

#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>

#include <algorithm>
#include <vector>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

constexpr qint64 kMaxDesktopFileSize = 256 * 1024;

QStringList words(const QString &text) {
    static const QRegularExpression separators(u"[^\\p{L}\\p{N}]+"_s);
    return text.split(separators, Qt::SkipEmptyParts);
}

bool containsAllWords(const QStringList &haystack, const QStringList &needles) {
    if (needles.isEmpty()) return false;
    for (const QString &needle : needles) {
        if (!haystack.contains(needle)) return false;
    }
    return true;
}

int levenshtein(const QString &a, const QString &b) {
    std::vector<int> previous(size_t(b.size()) + 1);
    std::vector<int> current(size_t(b.size()) + 1);
    for (size_t j = 0; j < previous.size(); ++j) previous[j] = int(j);
    for (qsizetype i = 1; i <= a.size(); ++i) {
        current[0] = int(i);
        for (qsizetype j = 1; j <= b.size(); ++j) {
            const int substitution = previous[size_t(j - 1)] + (a.at(i - 1) == b.at(j - 1) ? 0 : 1);
            current[size_t(j)] = std::min({previous[size_t(j)] + 1, current[size_t(j - 1)] + 1, substitution});
        }
        std::swap(previous, current);
    }
    return previous[size_t(b.size())];
}

// Score for a likely typo ("firefx" → "firefox"): capped below whole-word matches, so a near-miss
// ("terminal" vs "qterminal") never outranks an app that really is called that.
double typoScore(const QString &query, const QString &candidate) {
    if (query.size() < 4 || candidate.size() < 4 || qAbs(query.size() - candidate.size()) > 2) return 0;
    const double similarity =
        1.0 - double(levenshtein(query, candidate)) / double(qMax(query.size(), candidate.size()));
    return similarity < 0.75 ? 0.0 : std::min(0.84, similarity * 0.95);
}

QString normalizeQuery(const QString &query) {
    QString normalized = query.toLower().simplified();
    for (const QString &article : {u"the "_s, u"a "_s, u"an "_s, u"my "_s}) {
        if (normalized.startsWith(article)) normalized.remove(0, article.size());
    }
    return normalized;
}

bool executableExists(const QString &program) {
    if (program.startsWith(u'/')) return QFileInfo(program).isExecutable();
    return !QStandardPaths::findExecutable(program).isEmpty();
}

} // namespace

DesktopEntryIndex::DesktopEntryIndex(QStringList dataDirs, QStringList desktops, QObject *parent)
    : QObject(parent), dataDirs_(std::move(dataDirs)), desktops_(std::move(desktops)) {
    pool_.setMaxThreadCount(1);
}

DesktopEntryIndex::~DesktopEntryIndex() {
    pool_.waitForDone();
}

void DesktopEntryIndex::refresh() {
    const QStringList dirs = dataDirs_;
    const QStringList desktops = desktops_;
    const QPointer<DesktopEntryIndex> self(this);
    pool_.start([self, dirs, desktops, this] {
        QElapsedTimer timer;
        timer.start();
        QList<DesktopEntry> entries = scan(dirs, desktops, DesktopEntry::preferredLocales());
        qCDebug(lcApp) << "Indexed" << entries.size() << "applications in" << timer.elapsed() << "ms";
        // `this` stays valid: the destructor waits for the pool before anything is torn down.
        QMetaObject::invokeMethod(this, [self, entries = std::move(entries)]() mutable {
            if (!self) return;
            self->entries_ = std::move(entries);
            self->ready_ = true;
            emit self->ready(int(self->entries_.size()));
        }, Qt::QueuedConnection);
    });
}

void DesktopEntryIndex::refreshNow() {
    entries_ = scan(dataDirs_, desktops_, DesktopEntry::preferredLocales());
    ready_ = true;
    emit ready(int(entries_.size()));
}

QStringList DesktopEntryIndex::defaultDataDirs() {
    QStringList dirs{QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)};
    QString system = qEnvironmentVariable("XDG_DATA_DIRS");
    if (system.isEmpty()) system = u"/usr/local/share:/usr/share"_s;
    dirs += system.split(u':', Qt::SkipEmptyParts);
    QStringList cleaned;
    for (const QString &dir : std::as_const(dirs)) {
        const QString path = QDir::cleanPath(dir);
        if (!cleaned.contains(path)) cleaned.append(path);
    }
    return cleaned;
}

QStringList DesktopEntryIndex::currentDesktops() {
    return qEnvironmentVariable("XDG_CURRENT_DESKTOP").split(u':', Qt::SkipEmptyParts);
}

QList<DesktopEntry> DesktopEntryIndex::scan(const QStringList &dataDirs, const QStringList &desktops,
                                            const QStringList &locales) {
    QList<DesktopEntry> entries;
    QSet<QString> seenIds;
    for (const QString &dataDir : dataDirs) {
        const QString base = dataDir + u"/applications"_s;
        if (!QFileInfo(base).isDir()) continue;
        QDirIterator it(base, {u"*.desktop"_s}, QDir::Files, QDirIterator::Subdirectories | QDirIterator::FollowSymlinks);
        QStringList paths;
        while (it.hasNext()) paths.append(it.next());
        paths.sort();
        for (const QString &path : std::as_const(paths)) {
            const QString id = QDir(base).relativeFilePath(path).replace(u'/', u'-');
            // A higher-priority directory owns the ID even when it hides the entry (Hidden=true).
            if (seenIds.contains(id)) continue;
            seenIds.insert(id);

            QFile file(path);
            if (file.size() > kMaxDesktopFileSize || !file.open(QIODevice::ReadOnly)) continue;
            std::optional<DesktopEntry> entry = DesktopEntry::parse(file.readAll(), id, path, locales);
            if (!entry || entry->noDisplay || !entry->isShownIn(desktops)) continue;
            if (entry->exec.isEmpty() && !entry->dbusActivatable) continue;
            if (!entry->tryExec.isEmpty() && !executableExists(entry->tryExec)) continue;
            entries.append(*entry);
        }
    }
    std::sort(entries.begin(), entries.end(), [](const DesktopEntry &a, const DesktopEntry &b) {
        return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    });
    return entries;
}

double DesktopEntryIndex::matchScore(const DesktopEntry &entry, const QString &query) {
    const QString q = normalizeQuery(query);
    if (q.size() < 2) return 0;

    const QString name = entry.name.toLower().simplified();
    const QString generic = entry.genericName.toLower().simplified();
    const QString executable = entry.executableName().toLower();
    QString id = entry.id.toLower();
    if (id.endsWith(u".desktop")) id.chop(8);
    const QString idLast = id.section(u'.', -1);  // org.gnome.nautilus → nautilus

    double score = 0;
    const auto consider = [&score](double value) { score = std::max(score, value); };

    if (q == name || q == executable || q == idLast || q == id) consider(1.0);
    if (name.startsWith(q + u' ') || executable.startsWith(q + u'-') || idLast.startsWith(q + u'-')) consider(0.95);
    if (!generic.isEmpty() && q == generic) consider(0.9);
    for (const QString &keyword : entry.keywords) {
        if (keyword.toLower() == q) consider(0.85);
    }
    const QStringList queryWords = words(q);
    const QStringList nameWords = words(name);
    if (containsAllWords(nameWords, queryWords)) consider(0.85);
    if (containsAllWords(words(generic), queryWords)) consider(0.85);
    for (const QString &candidate : nameWords + words(executable) + QStringList{name, executable, idLast}) {
        consider(typoScore(q, candidate));
    }
    return score;
}

QList<DesktopEntryIndex::Match> DesktopEntryIndex::search(const QString &query, int limit) const {
    const bool gnome = desktops_.contains(u"GNOME"_s, Qt::CaseInsensitive);
    QList<Match> matches;
    for (const DesktopEntry &entry : entries_) {
        const double value = matchScore(entry, query);
        if (value > 0) matches.append(Match{entry, value});
    }
    // Ties: the desktop's own apps first (GNOME's Terminal over QTerminal), then the shorter name.
    std::stable_sort(matches.begin(), matches.end(), [gnome](const Match &a, const Match &b) {
        if (a.score != b.score) return a.score > b.score;
        const bool nativeA = gnome && a.entry.id.startsWith(u"org.gnome.");
        const bool nativeB = gnome && b.entry.id.startsWith(u"org.gnome.");
        if (nativeA != nativeB) return nativeA;
        return a.entry.name.size() < b.entry.name.size();
    });
    if (matches.size() > limit) matches.resize(limit);
    return matches;
}

std::optional<DesktopEntryIndex::Match> DesktopEntryIndex::best(const QString &query) const {
    const QList<Match> matches = search(query, 1);
    if (matches.isEmpty()) return std::nullopt;
    return matches.first();
}

double DesktopEntryIndex::score(const QString &query) const {
    const std::optional<Match> match = best(query);
    return match ? match->score : 0.0;
}

} // namespace nexa
