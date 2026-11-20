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
#include <initializer_list>
#include <vector>

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>
#endif

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
    if (QDir::isAbsolutePath(program)) return QFileInfo(program).isExecutable();
    return !QStandardPaths::findExecutable(program).isEmpty();
}

#ifdef Q_OS_WIN
struct WinComScope {
    HRESULT hr{E_FAIL};
    WinComScope() : hr(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)) {}
    ~WinComScope() {
        if (hr == S_OK || hr == S_FALSE) ::CoUninitialize();
    }
    bool usable() const { return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE; }
};

template <typename T>
struct WinComPtr {
    T *ptr{nullptr};
    ~WinComPtr() { reset(); }
    WinComPtr() = default;
    WinComPtr(const WinComPtr &) = delete;
    WinComPtr &operator=(const WinComPtr &) = delete;
    void reset(T *next = nullptr) {
        if (ptr) ptr->Release();
        ptr = next;
    }
    T **put() {
        reset();
        return &ptr;
    }
    T *operator->() const { return ptr; }
    explicit operator bool() const { return ptr != nullptr; }
};

void addUnique(QStringList &list, std::initializer_list<QStringView> items) {
    for (const QStringView item : items) {
        const QString s = item.toString();
        if (!list.contains(s, Qt::CaseInsensitive)) list.append(s);
    }
}

void enrichWindowsEntry(DesktopEntry &entry) {
    const QString nameLow = entry.name.toLower().simplified();
    const QString idLow = entry.id.toLower();
    const QString exeLow = entry.executableName().toLower();

    if (idLow.contains(u"windowscalculator"_s) || nameLow == u"calculator"_s || exeLow == u"calc.exe"_s) {
        if (entry.exec.isEmpty()) entry.exec = u"CalculatorApp.exe"_s;
        if (entry.genericName.isEmpty()) entry.genericName = u"Calculator"_s;
        addUnique(entry.keywords, {u"calc", u"calculator"});
    } else if (idLow.contains(u"immersivecontrolpanel"_s) || nameLow == u"settings"_s) {
        if (entry.exec.isEmpty()) entry.exec = u"SystemSettings.exe"_s;
        if (entry.genericName.isEmpty()) entry.genericName = u"Settings"_s;
        addUnique(entry.keywords, {u"settings", u"preferences", u"system settings"});
    } else if (idLow.contains(u"windowsnotepad"_s) || nameLow == u"notepad"_s || exeLow == u"notepad.exe"_s) {
        if (entry.exec.isEmpty()) entry.exec = u"Notepad.exe"_s;
        if (entry.genericName.isEmpty()) entry.genericName = u"Text Editor"_s;
        addUnique(entry.keywords, {u"notepad", u"text editor", u"editor"});
        addUnique(entry.categories, {u"TextEditor"});
    } else if (idLow.contains(u"microsoft.paint"_s) || nameLow == u"paint"_s || exeLow == u"mspaint.exe"_s) {
        if (entry.exec.isEmpty()) entry.exec = u"mspaint.exe"_s;
        addUnique(entry.keywords, {u"paint", u"mspaint", u"draw"});
    } else if (idLow.contains(u"windowsterminal"_s) || nameLow == u"terminal"_s || nameLow == u"windows terminal"_s
               || exeLow == u"wt.exe"_s || exeLow == u"windowsterminal.exe"_s) {
        if (entry.exec.isEmpty()) entry.exec = u"WindowsTerminal.exe"_s;
        if (entry.genericName.isEmpty()) entry.genericName = u"Terminal Emulator"_s;
        addUnique(entry.keywords, {u"wt", u"terminal", u"shell", u"console"});
        addUnique(entry.categories, {u"TerminalEmulator"});
    } else if (nameLow == u"command prompt"_s || exeLow == u"cmd.exe"_s) {
        if (entry.exec.isEmpty()) entry.exec = u"cmd.exe"_s;
        if (entry.genericName.isEmpty()) entry.genericName = u"Terminal Emulator"_s;
        addUnique(entry.keywords, {u"cmd", u"command prompt", u"terminal", u"shell"});
        addUnique(entry.categories, {u"TerminalEmulator"});
    } else if (nameLow.contains(u"powershell"_s) || exeLow == u"powershell.exe"_s || exeLow == u"pwsh.exe"_s) {
        if (entry.exec.isEmpty()) entry.exec = u"powershell.exe"_s;
        if (entry.genericName.isEmpty()) entry.genericName = u"Terminal Emulator"_s;
        addUnique(entry.keywords, {u"powershell", u"pwsh", u"terminal", u"shell"});
        addUnique(entry.categories, {u"TerminalEmulator"});
    } else if (idLow.contains(u"microsoft.windows.explorer"_s) || nameLow == u"file explorer"_s) {
        if (entry.exec.isEmpty()) entry.exec = u"explorer.exe"_s;
        if (entry.genericName.isEmpty()) entry.genericName = u"File Manager"_s;
        addUnique(entry.keywords, {u"explorer", u"files", u"file manager", u"file explorer", u"folder"});
    } else if (idLow.contains(u"microsoft.windows.controlpanel"_s) || nameLow == u"control panel"_s) {
        if (entry.exec.isEmpty()) entry.exec = u"control.exe"_s;
        addUnique(entry.keywords, {u"control", u"control panel"});
    } else if (nameLow == u"task manager"_s || exeLow == u"taskmgr.exe"_s) {
        if (entry.exec.isEmpty()) entry.exec = u"Taskmgr.exe"_s;
        addUnique(entry.keywords, {u"taskmgr", u"task manager", u"processes"});
    } else if (exeLow == u"chrome.exe"_s || nameLow == u"google chrome"_s) {
        if (entry.genericName.isEmpty()) entry.genericName = u"Web Browser"_s;
        addUnique(entry.keywords, {u"chrome", u"browser", u"web browser"});
        addUnique(entry.categories, {u"WebBrowser"});
    } else if (exeLow == u"msedge.exe"_s || nameLow == u"microsoft edge"_s) {
        if (entry.genericName.isEmpty()) entry.genericName = u"Web Browser"_s;
        addUnique(entry.keywords, {u"edge", u"browser", u"web browser"});
        addUnique(entry.categories, {u"WebBrowser"});
    } else if (exeLow == u"firefox.exe"_s || nameLow.contains(u"firefox"_s)) {
        if (entry.genericName.isEmpty()) entry.genericName = u"Web Browser"_s;
        addUnique(entry.keywords, {u"firefox", u"browser", u"web browser"});
        addUnique(entry.categories, {u"WebBrowser"});
    } else if (exeLow == u"brave.exe"_s || nameLow.contains(u"brave"_s)) {
        if (entry.genericName.isEmpty()) entry.genericName = u"Web Browser"_s;
        addUnique(entry.keywords, {u"brave", u"browser", u"web browser"});
        addUnique(entry.categories, {u"WebBrowser"});
    } else if (exeLow == u"code.exe"_s || nameLow.contains(u"visual studio code"_s)) {
        if (entry.genericName.isEmpty()) entry.genericName = u"Text Editor"_s;
        addUnique(entry.keywords, {u"vscode", u"code", u"editor"});
        addUnique(entry.categories, {u"TextEditor", u"IDE"});
    } else if (exeLow == u"notepad++.exe"_s || nameLow.contains(u"notepad++"_s)) {
        if (entry.genericName.isEmpty()) entry.genericName = u"Text Editor"_s;
        addUnique(entry.keywords, {u"notepad++", u"editor"});
        addUnique(entry.categories, {u"TextEditor"});
    } else if (exeLow == u"winword.exe"_s || exeLow == u"excel.exe"_s || exeLow == u"powerpnt.exe"_s
               || exeLow == u"soffice.exe"_s) {
        addUnique(entry.categories, {u"Office"});
    } else if (idLow.contains(u"screensketch"_s) || nameLow == u"snipping tool"_s) {
        if (entry.exec.isEmpty()) entry.exec = u"SnippingTool.exe"_s;
        addUnique(entry.keywords, {u"snip", u"snipping tool", u"screenshot"});
    } else if (idLow.contains(u"windowsstore"_s) || nameLow == u"microsoft store"_s) {
        if (entry.exec.isEmpty()) entry.exec = u"WinStore.App.exe"_s;
        addUnique(entry.keywords, {u"store", u"microsoft store"});
    } else if (idLow.contains(u"windows.photos"_s) || nameLow == u"photos"_s) {
        if (entry.exec.isEmpty()) entry.exec = u"Photos.exe"_s;
        addUnique(entry.keywords, {u"photos", u"pictures", u"gallery"});
    } else if (idLow.contains(u"windowscamera"_s) || nameLow == u"camera"_s) {
        if (entry.exec.isEmpty()) entry.exec = u"WindowsCamera.exe"_s;
        addUnique(entry.keywords, {u"camera", u"webcam"});
    } else if (idLow.contains(u"windowsalarms"_s) || nameLow == u"clock"_s) {
        if (entry.exec.isEmpty()) entry.exec = u"Time.exe"_s;
        addUnique(entry.keywords, {u"clock", u"alarm", u"timer"});
    } else if (idLow.contains(u"whatsapp"_s) || nameLow == u"whatsapp"_s) {
        if (entry.exec.isEmpty()) entry.exec = u"WhatsApp.exe"_s;
        addUnique(entry.keywords, {u"whatsapp"});
    }
}

// Enumerates Windows Start Apps (UWP/Store/MSIX + shell apps such as Calculator, Settings,
// Notepad, Paint, Terminal, File Explorer) via shell:AppsFolder.
void scanWindowsAppsFolder(QList<DesktopEntry> &entries, QSet<QString> &seenIds, QSet<QString> &seenNames) {
    static constexpr GUID kFolderIdAppsFolder = {
        0x1e87508d, 0x89c2, 0x42f0, {0x8a, 0x7e, 0x64, 0x5a, 0x0f, 0x50, 0xca, 0x58}};
    static constexpr GUID kBhidEnumItems = {
        0x94f60519, 0x2850, 0x4924, {0xaa, 0x5a, 0xd1, 0x5e, 0x84, 0x86, 0x80, 0x39}};
    static constexpr IID kIidIShellItem = {
        0x43826d1e, 0xe718, 0x42ee, {0xbc, 0x55, 0xa1, 0xe2, 0x61, 0xc3, 0x7b, 0xfe}};
    static constexpr IID kIidIEnumShellItems = {
        0x70629033, 0xe363, 0x4a28, {0xa5, 0x67, 0x0d, 0xb7, 0x80, 0x06, 0xe6, 0xd7}};

    WinComScope com;
    if (!com.usable()) return;

    WinComPtr<IShellItem> folder;
    if (FAILED(::SHGetKnownFolderItem(kFolderIdAppsFolder, KF_FLAG_DEFAULT, nullptr, kIidIShellItem,
                                      reinterpret_cast<void **>(folder.put())))
        || !folder) {
        return;
    }
    WinComPtr<IEnumShellItems> enumerator;
    if (FAILED(folder->BindToHandler(nullptr, kBhidEnumItems, kIidIEnumShellItems,
                                     reinterpret_cast<void **>(enumerator.put())))
        || !enumerator) {
        return;
    }

    IShellItem *rawChild = nullptr;
    ULONG fetched = 0;
    while (enumerator->Next(1, &rawChild, &fetched) == S_OK && fetched == 1 && rawChild) {
        WinComPtr<IShellItem> child;
        child.reset(rawChild);
        rawChild = nullptr;

        LPWSTR rawName = nullptr;
        LPWSTR rawParse = nullptr;
        QString name;
        QString parsingName;
        if (SUCCEEDED(child->GetDisplayName(SIGDN_NORMALDISPLAY, &rawName)) && rawName) {
            name = QString::fromWCharArray(rawName).trimmed();
            ::CoTaskMemFree(rawName);
        }
        if (SUCCEEDED(child->GetDisplayName(SIGDN_PARENTRELATIVEPARSING, &rawParse)) && rawParse) {
            parsingName = QString::fromWCharArray(rawParse).trimmed();
            ::CoTaskMemFree(rawParse);
        }
        if (name.isEmpty() || parsingName.isEmpty()) continue;
        if (name.contains(u"uninstall"_s, Qt::CaseInsensitive)) continue;

        const QString nameKey = name.toLower().simplified();
        const QString id = u"winapp:"_s + parsingName;
        if (seenNames.contains(nameKey) || seenIds.contains(id)) continue;
        seenNames.insert(nameKey);
        seenIds.insert(id);

        DesktopEntry entry;
        entry.id = id;
        entry.path = u"shell:AppsFolder\\"_s + parsingName;
        entry.name = name;
        if (parsingName.endsWith(u".exe"_s, Qt::CaseInsensitive) && !parsingName.contains(u'!')) {
            entry.exec = u"\"%1\""_s.arg(parsingName.section(u'\\', -1).section(u'/', -1));
        }
        enrichWindowsEntry(entry);
        entries.append(std::move(entry));
    }
}
#endif

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
#ifdef Q_OS_WIN
    QStringList dirs = QStandardPaths::standardLocations(QStandardPaths::ApplicationsLocation);
    const QString programData = qEnvironmentVariable("ProgramData");
    if (!programData.isEmpty()) dirs.append(programData + u"/Microsoft/Windows/Start Menu/Programs"_s);
#else
    QStringList dirs{QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)};
    QString system = qEnvironmentVariable("XDG_DATA_DIRS");
    if (system.isEmpty()) system = u"/usr/local/share:/usr/share"_s;
    dirs += system.split(u':', Qt::SkipEmptyParts);
#endif
    QStringList cleaned;
    for (const QString &dir : std::as_const(dirs)) {
        const QString path = QDir::cleanPath(dir);
        if (!path.isEmpty() && !cleaned.contains(path)) cleaned.append(path);
    }
    return cleaned;
}

QStringList DesktopEntryIndex::currentDesktops() {
#ifdef Q_OS_WIN
    return {u"Windows"_s};
#else
    return qEnvironmentVariable("XDG_CURRENT_DESKTOP").split(u':', Qt::SkipEmptyParts);
#endif
}

QList<DesktopEntry> DesktopEntryIndex::scan(const QStringList &dataDirs, const QStringList &desktops,
                                            const QStringList &locales) {
    QList<DesktopEntry> entries;
    QSet<QString> seenIds;
#ifdef Q_OS_WIN
    QSet<QString> seenWinNames;
    QList<DesktopEntry> deferredShellLinks;
#endif
    for (const QString &dataDir : dataDirs) {
        const QString base = dataDir + u"/applications"_s;
        if (QFileInfo(base).isDir()) {
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
#ifdef Q_OS_WIN
        if (QFileInfo(dataDir).isDir()) {
            QDirIterator it(dataDir, {u"*.lnk"_s}, QDir::Files, QDirIterator::Subdirectories);
            QStringList links;
            while (it.hasNext()) links.append(it.next());
            links.sort();
            for (const QString &path : std::as_const(links)) {
                const QFileInfo info(path);
                const QString name = info.completeBaseName().trimmed();
                if (name.isEmpty() || name.contains(u"uninstall"_s, Qt::CaseInsensitive)) continue;
                const QString target = info.symLinkTarget();
                if (!target.isEmpty() && !QFileInfo::exists(target)) continue;
                const QString id = u"win:"_s + QDir(dataDir).relativeFilePath(path).replace(u'\\', u'/');
                if (seenIds.contains(id)) continue;
                seenIds.insert(id);

                DesktopEntry entry;
                entry.id = id;
                entry.path = path;
                entry.name = name;
                if (!target.isEmpty()) entry.exec = u"\"%1\""_s.arg(target);
                enrichWindowsEntry(entry);
                if (!target.isEmpty()) {
                    seenWinNames.insert(name.toLower().simplified());
                    entries.append(std::move(entry));
                } else {
                    // Shell PIDL shortcuts (File Explorer, Control Panel, Run) have no symLinkTarget:
                    // keep them as fallback if shell:AppsFolder does not already provide them.
                    deferredShellLinks.append(std::move(entry));
                }
            }
        }
#endif
    }
#ifdef Q_OS_WIN
    if (desktops.contains(u"Windows"_s, Qt::CaseInsensitive)) {
        scanWindowsAppsFolder(entries, seenIds, seenWinNames);
    }
    for (DesktopEntry &shellLink : deferredShellLinks) {
        const QString nameKey = shellLink.name.toLower().simplified();
        if (seenWinNames.contains(nameKey)) continue;
        seenWinNames.insert(nameKey);
        entries.append(std::move(shellLink));
    }
#endif
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
#ifdef Q_OS_WIN
    const QString executableFull = entry.executableName().toLower();
    QString executable = executableFull;
    if (executable.endsWith(u".exe"_s)) executable.chop(4);
#else
    const QString executable = entry.executableName().toLower();
#endif
    QString id = entry.id.toLower();
    if (id.endsWith(u".desktop")) id.chop(8);
    const QString idLast = id.section(u'.', -1);  // org.gnome.nautilus → nautilus

    double score = 0;
    const auto consider = [&score](double value) { score = std::max(score, value); };

    if (q == name || q == executable
#ifdef Q_OS_WIN
        || q == executableFull
#endif
        || q == idLast || q == id) consider(1.0);
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
