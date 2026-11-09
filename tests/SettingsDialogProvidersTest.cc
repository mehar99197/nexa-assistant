// Settings → Providers as a whole: the sign-in block each program row holds (one at a time, checked once when the
// tab shows, let go of when the row changes or the dialog closes), and the Add menu's groups. The programs are the
// fakes of tests/ (agy, codex): nothing here starts a real agy, opencode or Codex, signs anyone in or out, or touches
// a keyring — the fake agy's "keyring" is a file in the test's own folder.

#include "app/ProviderSignInBlock.h"
#include "app/SettingsDialog.h"
#include "app/SystemStatus.h"
#include "core/ai/ProviderRegistry.h"
#include "core/net/HttpClient.h"

#include "QtStringMakers.h"

#include <QAction>
#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>

#include <catch2/catch_test_macros.hpp>

#include <signal.h>

using namespace nexa;
using namespace Qt::StringLiterals;

namespace {

constexpr int kWaitMs = 15'000;

// The fakes' environment in a scratch folder, and a config naming them: agy, Codex and a plain API.
struct Rig {
    explicit Rig(const QString &agyPlan = u"Google AI Pro"_s, const QJsonObject &agyScript = {}) {
        qputenv("FAKE_AGY_ACP", QJsonDocument(agyScript).toJson(QJsonDocument::Compact));
        qputenv("FAKE_AGY_KEYRING", dir.filePath(u"keyring"_s).toUtf8());
        qputenv("FAKE_AGY_LOG", dir.filePath(u"agy-log.jsonl"_s).toUtf8());
        qputenv("FAKE_CODEX_STATE", dir.filePath(u"codex-state.json"_s).toUtf8());
        qputenv("FAKE_CODEX_LOG", dir.filePath(u"codex-log.jsonl"_s).toUtf8());
        QFile keyring(dir.filePath(u"keyring"_s));
        REQUIRE(keyring.open(QIODevice::WriteOnly));
        keyring.write(agyPlan.toUtf8());
        QFile state(dir.filePath(u"codex-state.json"_s));
        REQUIRE(state.open(QIODevice::WriteOnly));
        state.write(R"({"signedIn": true, "plan": "plus"})");
    }
    ~Rig() {
        dialog.reset();
        for (const char *name : {"FAKE_AGY_ACP", "FAKE_AGY_KEYRING", "FAKE_AGY_LOG", "FAKE_CODEX_STATE", "FAKE_CODEX_LOG"}) {
            qunsetenv(name);
        }
    }

    void open(std::function<bool()> replyRunning = {}) {
        Config config;
        config.providers = {
            ProviderConfig{.id = u"agy"_s, .kind = QString(kKindAgy), .name = u"agy"_s,
                           .baseUrl = QStringLiteral(NEXA_TESTS_DIR "/fake-agy/agy"), .requiresKey = false},
            ProviderConfig{.id = u"codex"_s, .kind = QString(kKindCodex), .name = u"Codex"_s,
                           .baseUrl = QStringLiteral(NEXA_TESTS_DIR "/fake-codex/codex"), .requiresKey = false},
            ProviderConfig{.id = u"groq"_s, .kind = QString(kKindOpenAiCompatible), .name = u"Groq"_s,
                           .baseUrl = u"https://api.groq.com/openai/v1"_s},
        };
        config.activeProvider = u"agy"_s;
        SystemStatus status;
        status.replyRunning = std::move(replyRunning);
        dialog = std::make_unique<SettingsDialog>(config, &registry, &http, status);
        dialog->show();
    }

    // The sign-in block now alive (deferred deletes done), or none.
    QList<ProviderSignInBlock *> blocks() const {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        return dialog->findChildren<ProviderSignInBlock *>();
    }
    ProviderSignInBlock *block() const {
        const QList<ProviderSignInBlock *> found = blocks();
        return found.size() == 1 ? found.first() : nullptr;
    }
    QString statusText() const {
        ProviderSignInBlock *shown = block();
        const QLabel *label = shown ? shown->findChild<QLabel *>(u"signInStatus"_s) : nullptr;
        return label ? label->text() : QString();
    }
    QListWidget *list() const { return dialog->findChild<QListWidget *>(u"providerList"_s); }
    QLineEdit *program() const { return dialog->findChild<QLineEdit *>(u"providerBaseUrl"_s); }

    int agyStarts() const {
        int count = 0;
        QFile file(dir.filePath(u"agy-log.jsonl"_s));
        if (!file.open(QIODevice::ReadOnly)) return 0;
        for (const QByteArray &line : file.readAll().split('\n')) {
            if (QJsonDocument::fromJson(line).object().contains(u"argv"_s)) ++count;
        }
        return count;
    }
    qint64 lastAgyPid() const {
        qint64 pid = 0;
        QFile file(dir.filePath(u"agy-log.jsonl"_s));
        if (!file.open(QIODevice::ReadOnly)) return 0;
        for (const QByteArray &line : file.readAll().split('\n')) {
            const QJsonObject entry = QJsonDocument::fromJson(line).object();
            if (entry.contains(u"argv"_s)) pid = qint64(entry.value(u"pid"_s).toInteger());
        }
        return pid;
    }

    QTemporaryDir dir;
    HttpClient http;
    ProviderRegistry registry{&http};
    std::unique_ptr<SettingsDialog> dialog;
};

bool alive(qint64 pid) { return pid > 0 && ::kill(pid_t(pid), 0) == 0; }

} // namespace

TEST_CASE("The Providers tab checks the program shown once, and not again for a tab switch") {
    Rig rig;
    rig.open();
    REQUIRE(QTest::qWaitFor([&] { return rig.statusText() == u"Signed in · Google AI Pro"_s; }, kWaitMs));
    CHECK(rig.agyStarts() == 1);

    rig.dialog->showTab(u"Voice"_s);
    rig.dialog->showTab(u"Providers"_s);
    QTest::qWait(300);
    CHECK(rig.agyStarts() == 1);                     // looked at a moment ago: not again
}

TEST_CASE("Every row has its own sign-in block, and the old one is gone when the row changes") {
    Rig rig;
    rig.open();
    ProviderSignInBlock *agyBlock = rig.block();
    REQUIRE(agyBlock != nullptr);

    rig.list()->setCurrentRow(2);                    // Groq: an API, no sign-in at all
    CHECK(rig.blocks().isEmpty());

    rig.list()->setCurrentRow(1);                    // Codex
    ProviderSignInBlock *codexBlock = rig.block();
    REQUIRE(codexBlock != nullptr);
    REQUIRE(QTest::qWaitFor([&] { return rig.statusText() == u"Signed in · ChatGPT · Plus"_s; }, kWaitMs));

    rig.list()->setCurrentRow(0);                    // agy again: a block made afresh, never two at once
    CHECK(rig.blocks().size() == 1);
}

TEST_CASE("Leaving the program field alone keeps the sign-in; naming another program makes a new one") {
    Rig rig;
    rig.open();
    ProviderSignInBlock *first = rig.block();
    REQUIRE(first != nullptr);
    REQUIRE(QTest::qWaitFor([&] { return rig.statusText() == u"Signed in · Google AI Pro"_s; }, kWaitMs));
    const int starts = rig.agyStarts();

    // Focus passing through the field says editingFinished without any change.
    emit rig.program()->editingFinished();
    CHECK(rig.block() == first);
    QTest::qWait(300);
    CHECK(rig.agyStarts() == starts);                // no second look for nothing

    rig.program()->setText(QStringLiteral(NEXA_TESTS_DIR "/fake-agy/./agy"));
    emit rig.program()->editingFinished();
    REQUIRE(rig.block() != nullptr);
    CHECK(QTest::qWaitFor([&] { return rig.agyStarts() == starts + 1; }, kWaitMs));    // looked up again where it was named
    CHECK(QTest::qWaitFor([&] { return rig.statusText() == u"Signed in · Google AI Pro"_s; }, kWaitMs));
}

TEST_CASE("Nothing is started while a reply is running") {
    Rig rig;
    rig.open([] { return true; });
    QTest::qWait(500);
    CHECK(rig.agyStarts() == 0);                     // agy's app-data folder and opencode's stream are left alone
    CHECK(rig.block() != nullptr);
}

TEST_CASE("Closing the dialog during a sign-in ends the program") {
    // The fake waits a minute before answering `authenticate`: the sign-in is under way when the dialog goes.
    QJsonObject script;
    script.insert(u"delayMs"_s, 60'000);
    Rig rig(QString(), script);                      // nobody signed in
    rig.open();
    REQUIRE(QTest::qWaitFor([&] { return rig.statusText().contains(u"Not signed in"_s); }, kWaitMs));
    ProviderSignInBlock *shown = rig.block();
    REQUIRE(shown != nullptr);

    shown->startSignIn();
    // agy offers two ways in: the menu asks which, and the first is taken.
    REQUIRE(QTest::qWaitFor([&] { return shown->findChild<QMenu *>() != nullptr; }, kWaitMs));
    shown->findChild<QMenu *>()->actions().first()->trigger();
    REQUIRE(QTest::qWaitFor([&] { return rig.statusText().contains(u"Waiting"_s) && rig.agyStarts() >= 3; }, kWaitMs));
    const qint64 pid = rig.lastAgyPid();
    REQUIRE(alive(pid));

    rig.dialog->reject();
    CHECK(QTest::qWaitFor([&] { return !alive(pid); }, 5'000));      // the program did not outlive the dialog
    CHECK(rig.blocks().isEmpty());
}

TEST_CASE("The Add menu groups the presets by category, programs first, and ends with Browse all") {
    Rig rig;
    rig.open();
    auto *add = rig.dialog->findChild<QToolButton *>(u"providerAdd"_s);
    REQUIRE(add != nullptr);
    QMenu *menu = add->menu();
    REQUIRE(menu != nullptr);

    const QList<QAction *> actions = menu->actions();
    REQUIRE(actions.size() > 3);
    CHECK(actions.first()->text() == u"Programs"_s);
    REQUIRE(actions.last()->text() == u"Browse all…"_s);
    CHECK(actions.at(actions.size() - 2)->isSeparator());

    int leaves = 0;
    QStringList titles;
    for (const QAction *action : actions) {
        if (!action->menu()) continue;
        titles.append(action->text());
        leaves += int(action->menu()->actions().size());
    }
    CHECK(leaves == rig.registry.availablePresets().size());     // every preset once
    CHECK(titles == ProviderRegistry::presetCategories());       // in the categories' order, none left out here

    // Choosing one adds it and shows its row.
    // (A hosted API, not a program: adding opencode here would start a real opencode to look at its sign-in.)
    QMenu *second = actions.at(1)->menu();
    REQUIRE(second != nullptr);
    const int before = rig.list()->count();
    second->actions().at(0)->trigger();
    CHECK(rig.list()->count() == before + 1);
    CHECK(rig.list()->currentRow() == before);
}
