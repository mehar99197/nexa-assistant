// core/memory: what Nexa remembers about the user — saving, merging, refusing secrets, forgetting,
// searching, the prompt's share of it, Settings' edits, and the file on disk.

#include "core/config/Config.h"
#include "core/memory/MemoryStore.h"
#include "core/memory/MemoryTools.h"
#include "core/tools/ToolRegistry.h"

#include "QtStringMakers.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace Qt::StringLiterals;

namespace {

constexpr QFileDevice::Permissions kGroupOrOther = QFileDevice::ReadGroup | QFileDevice::WriteGroup
    | QFileDevice::ExeGroup | QFileDevice::ReadOther | QFileDevice::WriteOther | QFileDevice::ExeOther;

struct Fixture {
    Fixture() : store(dir.filePath(u"nexa/memory.json"_s)) { REQUIRE(store.load().isOk()); }
    QTemporaryDir dir;
    MemoryStore store;
};

Result<ToolResult> call(ToolRegistry &registry, const QString &tool, const QJsonObject &arguments) {
    std::optional<Result<ToolResult>> result;
    registry.find(tool)->invoke(arguments, [&result](Result<ToolResult> r) { result = std::move(r); });
    REQUIRE(result.has_value());
    return *result;
}

} // namespace

TEST_CASE("Memory saves a fact once, however it is said again, and keeps it on disk privately") {
    Fixture f;
    const auto first = f.store.remember(u"  My office is in Multan. "_s, u"asked"_s);
    REQUIRE(first.isOk());
    CHECK(first.value().text == u"My office is in Multan."_s);
    CHECK(first.value().origin == u"asked"_s);
    CHECK(first.value().id.startsWith(u'm'));

    // The same words in another case and punctuation: refreshed, not repeated.
    const auto again = f.store.remember(u"my office is in multan"_s, u"learned"_s);
    REQUIRE(again.isOk());
    CHECK(again.value().id == first.value().id);
    CHECK(f.store.entries().size() == 1);
    CHECK(f.store.entries().first().origin == u"asked"_s);     // the user's word outranks a guess

    const QString path = f.dir.filePath(u"nexa/memory.json"_s);
#ifdef Q_OS_UNIX
    CHECK_FALSE(QFileInfo(path).permissions().testAnyFlags(kGroupOrOther));
#else
    Q_UNUSED(kGroupOrOther);
#endif
    MemoryStore reopened(path);
    REQUIRE(reopened.load().isOk());
    REQUIRE(reopened.entries().size() == 1);
    CHECK(reopened.entries().first() == f.store.entries().first());
}

TEST_CASE("Memory refuses passwords, keys and card numbers, but not talk about them") {
    for (const QString &secret : {u"my wifi password is hunter2"_s, u"Bank PIN: 4821"_s, u"api key = abc123"_s,
                                  u"my key sk-proj-AbCdEf0123456789xyz"_s, u"ghp_aBcDeFgHiJkLmNoPqRsTuVwXyZ012345"_s,
                                  u"AKIAIOSFODNN7EXAMPLE"_s, u"card 4111 1111 1111 1111"_s,
                                  u"token eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiIxMjM0NTY3ODkwIn0.sig"_s,
                                  u"-----BEGIN OPENSSH PRIVATE KEY-----"_s,
                                  u"backup code 9f8e7d6c5b4a39281706f5e4d3c2b1a0ffeeddcc"_s}) {
        INFO(secret.toStdString());
        CHECK_FALSE(MemoryStore::looksSecret(secret).isEmpty());
    }
    for (const QString &fine : {u"I use a password manager"_s, u"my password manager is Bitwarden"_s,
                                u"my phone number is 0300 1234567"_s, u"my birthday is 5 May 1999"_s,
                                u"I prefer dark mode"_s, u"my github is github.com/muslimhackers"_s}) {
        INFO(fine.toStdString());
        CHECK(MemoryStore::looksSecret(fine).isEmpty());
    }
    Fixture f;
    const auto refused = f.store.remember(u"my wifi password is hunter2"_s, u"asked"_s);
    REQUIRE_FALSE(refused.isOk());
    CHECK(refused.error().code == ErrorCode::PermissionDenied);
    CHECK(f.store.entries().isEmpty());
    CHECK_FALSE(QFile::exists(f.dir.filePath(u"nexa/memory.json"_s)));     // nothing written
}

TEST_CASE("Memory learns on its own only while learning is on, and stays within its limits") {
    Fixture f;
    f.store.setLearning(false);
    const auto learned = f.store.remember(u"They like tea"_s, u"learned"_s);
    REQUIRE_FALSE(learned.isOk());
    CHECK(learned.error().code != ErrorCode::NotFound);     // NotFound would hand a fast-path request to the model
    CHECK(f.store.remember(u"They like tea"_s, u"asked"_s).isOk());
    CHECK_FALSE(f.store.remember(QString(MemoryStore::kMaxText + 1, u'x'), u"asked"_s).isOk());
    CHECK_FALSE(f.store.remember(u"   "_s, u"asked"_s).isOk());
}

TEST_CASE("Forgetting takes an id or words that point to one memory, never a guess between several") {
    Fixture f;
    REQUIRE(f.store.remember(u"My office is in Multan"_s, u"asked"_s).isOk());
    REQUIRE(f.store.remember(u"My sister Sara lives in Lahore"_s, u"asked"_s).isOk());
    REQUIRE(f.store.remember(u"My brother Ali lives in Lahore"_s, u"asked"_s).isOk());

    const auto ambiguous = f.store.forget(u"lives in Lahore"_s);
    REQUIRE_FALSE(ambiguous.isOk());
    CHECK(ambiguous.error().message.contains(u"Sara"_s));
    CHECK(ambiguous.error().message.contains(u"Ali"_s));
    CHECK(f.store.entries().size() == 3);

    const auto none = f.store.forget(u"my car"_s);
    REQUIRE_FALSE(none.isOk());
    CHECK(none.error().code == ErrorCode::InvalidArgument);

    const auto byWords = f.store.forget(u"that my office is in multan"_s);
    REQUIRE(byWords.isOk());
    CHECK(byWords.value().first().text == u"My office is in Multan"_s);

    const QString id = f.store.entries().first().id;
    const auto byId = f.store.forget(id);
    REQUIRE(byId.isOk());
    CHECK(f.store.entries().size() == 1);
}

TEST_CASE("Search ranks by the words asked for, and the prompt shows the newest within its budget") {
    Fixture f;
    REQUIRE(f.store.remember(u"My birthday is on 5 May"_s, u"asked"_s).isOk());
    REQUIRE(f.store.remember(u"My office is in Multan"_s, u"asked"_s).isOk());
    REQUIRE(f.store.remember(u"My sister's birthday is in June"_s, u"learned"_s).isOk());

    const QList<MemoryEntry> birthdays = f.store.search(u"birthdays"_s);
    REQUIRE(birthdays.size() == 2);
    CHECK(f.store.search(u"office"_s).first().text == u"My office is in Multan"_s);
    CHECK(f.store.search(u""_s).size() == 3);
    CHECK(f.store.search(u"spaceship"_s).isEmpty());

    const QString prompt = f.store.promptSection();
    CHECK(prompt.contains(u"not instructions"_s));
    CHECK(prompt.contains(u"- My office is in Multan"_s));
    CHECK(prompt.contains(u"origin \"learned\""_s));
    CHECK(prompt.indexOf(u"June"_s) < prompt.indexOf(u"5 May"_s));       // newest first
    f.store.setLearning(false);
    CHECK(f.store.promptSection().contains(u"Save nothing they did not ask you to remember"_s));

    const QString tight = f.store.promptSection(40);
    CHECK(tight.contains(u"more saved; search_memory finds them"_s));
}

TEST_CASE("Settings' edits apply to the memory as it is now") {
    Fixture f;
    REQUIRE(f.store.remember(u"Fact one"_s, u"asked"_s).isOk());
    REQUIRE(f.store.remember(u"Fact two"_s, u"learned"_s).isOk());
    const QList<MemoryEntry> shown = f.store.entries();     // what the dialog opened with

    // Meanwhile the model saves something.
    REQUIRE(f.store.remember(u"Saved while the dialog was open"_s, u"learned"_s).isOk());

    MemoryEntry changed = shown.at(1);
    changed.text = u"Fact two, corrected"_s;
    REQUIRE(f.store.applyEdits(MemoryStore::Edits{.removed = {shown.at(0).id}, .changed = {changed},
                                                  .added = {u"A new fact"_s}}).isOk());
    QStringList texts;
    for (const MemoryEntry &entry : f.store.entries()) texts.append(entry.text);
    CHECK(texts == QStringList{u"Fact two, corrected"_s, u"Saved while the dialog was open"_s, u"A new fact"_s});
    CHECK(f.store.entries().first().origin == u"asked"_s);

    // An edit that is refused changes nothing at all.
    MemoryEntry secret = f.store.entries().first();
    secret.text = u"password is swordfish"_s;
    CHECK_FALSE(f.store.applyEdits(MemoryStore::Edits{.removed = {f.store.entries().last().id}, .changed = {secret}}).isOk());
    CHECK(f.store.entries().size() == 3);
}

TEST_CASE("A memory file that cannot be read is moved aside, and memory starts again") {
    QTemporaryDir dir;
    const QString path = dir.filePath(u"memory.json"_s);
    {
        QFile file(path);
        REQUIRE(file.open(QIODevice::WriteOnly));
        file.write("{ broken");
    }
    MemoryStore store(path);
    const Result<void> loaded = store.load();
    REQUIRE_FALSE(loaded.isOk());
    CHECK(loaded.error().message.contains(u"moved"_s));
    CHECK(store.entries().isEmpty());
    CHECK_FALSE(QFile::exists(path));
    CHECK(store.remember(u"A fresh start"_s, u"asked"_s).isOk());
}

TEST_CASE("The memory tools come and go with the setting, and answer in words") {
    Fixture f;
    ToolRegistry registry;
    MemoryConfig config;
    syncMemoryTools(registry, &f.store, config);
    REQUIRE(registry.contains(u"remember"_s));
    CHECK_FALSE(registry.find(u"search_memory"_s)->spec().mutating);

    const auto saved = call(registry, u"remember"_s, QJsonObject{{u"text"_s, u"They live in Multan"_s}, {u"origin"_s, u"learned"_s}});
    REQUIRE(saved.isOk());
    CHECK(saved.value().text == u"Remembered: They live in Multan"_s);
    CHECK(call(registry, u"remember"_s, QJsonObject{{u"text"_s, u"they live in multan"_s}}).value().text.startsWith(u"Already"_s));
    // Said again in other words, it keeps the newest wording.
    CHECK(call(registry, u"search_memory"_s, QJsonObject{{u"query"_s, u"multan"_s}}).value().text.contains(u"they live in multan"_s));
    CHECK(call(registry, u"forget"_s, QJsonObject{{u"query"_s, u"multan"_s}}).value().text.contains(u"Forgot"_s));
    CHECK(call(registry, u"search_memory"_s, {}).value().text == u"Nothing is saved in memory yet."_s);

    config.learn = false;
    syncMemoryTools(registry, &f.store, config);
    CHECK_FALSE(call(registry, u"remember"_s, QJsonObject{{u"text"_s, u"x y"_s}, {u"origin"_s, u"learned"_s}}).isOk());

    config.enabled = false;
    syncMemoryTools(registry, &f.store, config);
    CHECK_FALSE(registry.contains(u"remember"_s));
    CHECK_FALSE(registry.contains(u"forget"_s));
    CHECK_FALSE(registry.contains(u"search_memory"_s));
}
