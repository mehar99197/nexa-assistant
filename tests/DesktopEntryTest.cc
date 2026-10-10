#include "system/apps/DesktopEntry.h"
#include "system/apps/DesktopEntryIndex.h"

#include "QtStringMakers.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace Qt::StringLiterals;

namespace {

DesktopEntry entry(const QString &id, const QString &name, const QString &exec, const QString &generic = {},
                   const QStringList &keywords = {}) {
    DesktopEntry e;
    e.id = id;
    e.name = name;
    e.exec = exec;
    e.genericName = generic;
    e.keywords = keywords;
    return e;
}

void writeFile(const QString &path, const QByteArray &content) {
    QDir().mkpath(path.section(u'/', 0, -2));
    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly));
    file.write(content);
}

} // namespace

TEST_CASE("DesktopEntry parses the main group, locales and lists") {
    const QByteArray content =
        "# comment\n"
        "[Desktop Entry]\n"
        "Type=Application\n"
        "Name=Files\n"
        "Name[de]=Dateien\n"
        "Name[de_DE]=Dateimanager\n"
        "GenericName=File Manager\n"
        "Keywords=folder;manager;explore\\;disk;\n"
        "Exec=nautilus --new-window %U\n"
        "Terminal=false\n"
        "DBusActivatable=true\n"
        "Comment=Access\\sand organize files\n"
        "[Desktop Action new-window]\n"
        "Name=New Window\n"
        "Exec=nautilus --new-window\n";

    const auto english = DesktopEntry::parse(content, u"org.gnome.Nautilus.desktop"_s, u"/x"_s, {u"en_US"_s, u"en"_s});
    REQUIRE(english.has_value());
    CHECK(english->name == u"Files"_s);
    CHECK(english->genericName == u"File Manager"_s);
    CHECK(english->keywords == QStringList{u"folder"_s, u"manager"_s, u"explore;disk"_s});
    CHECK(english->dbusActivatable);
    CHECK_FALSE(english->terminal);
    CHECK(english->comment == u"Access and organize files"_s);
    CHECK(english->exec == u"nautilus --new-window %U"_s);  // the action group does not override it

    CHECK(DesktopEntry::parse(content, u"x"_s, u"/x"_s, {u"de_DE"_s, u"de"_s})->name == u"Dateimanager"_s);
    CHECK(DesktopEntry::parse(content, u"x"_s, u"/x"_s, {u"de_AT"_s, u"de"_s})->name == u"Dateien"_s);
}

TEST_CASE("DesktopEntry rejects non-applications and files without the main group") {
    CHECK_FALSE(DesktopEntry::parse("[Desktop Entry]\nType=Link\nName=x\nURL=https://x\n", u"x"_s, u"/x"_s));
    CHECK_FALSE(DesktopEntry::parse("Type=Application\nName=x\n", u"x"_s, u"/x"_s));
}

TEST_CASE("DesktopEntry::execArguments applies quoting, escapes and field codes") {
    CHECK(DesktopEntry::execArguments(u"firefox-esr %u"_s) == QStringList{u"firefox-esr"_s});
    CHECK(DesktopEntry::execArguments(u"app --file=%f --icon %i"_s) == QStringList{u"app"_s, u"--file="_s, u"--icon"_s});
    CHECK(DesktopEntry::execArguments(u"sh -c \"echo \\\"hi\\\" 100%%\""_s)
          == QStringList{u"sh"_s, u"-c"_s, u"echo \"hi\" 100%"_s});
    CHECK(DesktopEntry::execArguments(u"\"/opt/My App/run\"  --flag"_s) == QStringList{u"/opt/My App/run"_s, u"--flag"_s});
    CHECK(DesktopEntry::execArguments(u"app \"\""_s) == QStringList{u"app"_s});
    CHECK(DesktopEntry::execArguments(u"broken \"quote"_s).isEmpty());
}

TEST_CASE("DesktopEntry finds the program behind env and flatpak wrappers") {
    CHECK(entry(u"a"_s, u"A"_s, u"env FOO=1 BAR=2 /usr/bin/app --x"_s).executableName() == u"app"_s);
    CHECK(entry(u"a"_s, u"A"_s, u"/usr/lib/firefox-esr/firefox-esr %u"_s).executableName() == u"firefox-esr"_s);
    const DesktopEntry flatpak =
        entry(u"a"_s, u"A"_s, u"/usr/bin/flatpak run --branch=stable --arch=x86_64 --command=foo org.foo.App %U"_s);
    CHECK(flatpak.flatpakAppId() == u"org.foo.App"_s);
    CHECK(entry(u"a"_s, u"A"_s, u"app"_s).flatpakAppId().isEmpty());
}

TEST_CASE("DesktopEntry honours OnlyShowIn and NotShowIn") {
    DesktopEntry e = entry(u"a"_s, u"A"_s, u"a"_s);
    e.onlyShowIn = {u"KDE"_s};
    CHECK_FALSE(e.isShownIn({u"GNOME"_s}));
    CHECK(e.isShownIn({u"KDE"_s}));
    e.onlyShowIn.clear();
    e.notShowIn = {u"GNOME"_s};
    CHECK_FALSE(e.isShownIn({u"GNOME"_s}));
}

TEST_CASE("DesktopEntryIndex::matchScore ranks names, prefixes, generic names, keywords and typos") {
    const DesktopEntry terminal = entry(u"org.gnome.Terminal.desktop"_s, u"Terminal"_s, u"gnome-terminal"_s,
                                        u"Terminal Emulator"_s);
    const DesktopEntry firefox = entry(u"firefox-esr.desktop"_s, u"Firefox ESR"_s, u"/usr/lib/firefox-esr/firefox-esr %u"_s,
                                       u"Web Browser"_s);
    const DesktopEntry code = entry(u"code.desktop"_s, u"Visual Studio Code"_s, u"/usr/share/code/code %F"_s,
                                    u"Text Editor"_s, {u"vscode"_s});

    CHECK(DesktopEntryIndex::matchScore(terminal, u"terminal"_s) == 1.0);
    CHECK(DesktopEntryIndex::matchScore(terminal, u"gnome-terminal"_s) == 1.0);
    CHECK(DesktopEntryIndex::matchScore(terminal, u"the Terminal"_s) == 1.0);
    CHECK(DesktopEntryIndex::matchScore(firefox, u"firefox"_s) == 0.95);
    CHECK(DesktopEntryIndex::matchScore(firefox, u"web browser"_s) == 0.9);
    CHECK(DesktopEntryIndex::matchScore(firefox, u"browser"_s) == 0.85);
    CHECK(DesktopEntryIndex::matchScore(firefox, u"firefx"_s) >= 0.8);
    CHECK(DesktopEntryIndex::matchScore(code, u"code"_s) == 1.0);
    CHECK(DesktopEntryIndex::matchScore(code, u"vscode"_s) == 0.85);
    CHECK(DesktopEntryIndex::matchScore(code, u"studio code"_s) == 0.85);
    CHECK(DesktopEntryIndex::matchScore(terminal, u"spotify"_s) == 0.0);
    CHECK(DesktopEntryIndex::matchScore(terminal, u"t"_s) == 0.0);
}

TEST_CASE("DesktopEntryIndex::scan applies directory precedence, sub-folder IDs and visibility") {
    QTemporaryDir root;
    REQUIRE(root.isValid());
    const QString user = root.filePath(u"user"_s);
    const QString system = root.filePath(u"system"_s);
    writeFile(user + u"/applications/foo.desktop"_s, "[Desktop Entry]\nType=Application\nName=Foo\nExec=foo\n");
    writeFile(system + u"/applications/foo.desktop"_s, "[Desktop Entry]\nType=Application\nName=Foo (system)\nExec=foo\n");
    writeFile(user + u"/applications/gone.desktop"_s, "[Desktop Entry]\nType=Application\nName=Gone\nHidden=true\n");
    writeFile(system + u"/applications/gone.desktop"_s, "[Desktop Entry]\nType=Application\nName=Gone\nExec=gone\n");
    writeFile(system + u"/applications/kde4/bar.desktop"_s, "[Desktop Entry]\nType=Application\nName=Bar\nExec=bar\n");
    writeFile(system + u"/applications/helper.desktop"_s,
              "[Desktop Entry]\nType=Application\nName=Helper\nExec=helper\nNoDisplay=true\n");
    writeFile(system + u"/applications/missing.desktop"_s,
              "[Desktop Entry]\nType=Application\nName=Missing\nExec=x\nTryExec=/nonexistent/binary\n");
    writeFile(system + u"/applications/kdeonly.desktop"_s,
              "[Desktop Entry]\nType=Application\nName=KdeOnly\nExec=k\nOnlyShowIn=KDE;\n");

    const QList<DesktopEntry> entries = DesktopEntryIndex::scan({user, system}, {u"GNOME"_s}, {});
    QStringList ids;
    for (const DesktopEntry &e : entries) ids.append(e.id + u'=' + e.name);
    CHECK(ids == QStringList{u"kde4-bar.desktop=Bar"_s, u"foo.desktop=Foo"_s});
}

TEST_CASE("DesktopEntryIndex::search prefers the desktop's own app on ties") {
    QTemporaryDir root;
    REQUIRE(root.isValid());
    const QString dir = root.filePath(u"share"_s);
    writeFile(dir + u"/applications/qterminal.desktop"_s,
              "[Desktop Entry]\nType=Application\nName=QTerminal Emulator\nGenericName=Terminal emulator\nExec=qterminal\n");
    writeFile(dir + u"/applications/org.gnome.Console.desktop"_s,
              "[Desktop Entry]\nType=Application\nName=Console Emulator\nGenericName=Terminal emulator\nExec=kgx\n");
    DesktopEntryIndex index({dir}, {u"GNOME"_s});
    index.refreshNow();
    const auto best = index.best(u"terminal"_s);
    REQUIRE(best.has_value());
    CHECK(best->entry.id == u"org.gnome.Console.desktop"_s);
    CHECK(index.score(u"terminal"_s) == 0.85);
}
