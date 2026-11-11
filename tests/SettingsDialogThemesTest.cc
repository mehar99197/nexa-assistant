// Settings → General's theme gallery: every pack under All, one category per chip, the choice kept while a chip hides
// its tile, and an old setting landing on its pack. The dialog is built with a plain API provider: nothing here starts
// a program or touches the network.

#include "app/SettingsDialog.h"
#include "app/SystemStatus.h"
#include "core/ai/ProviderRegistry.h"
#include "core/net/HttpClient.h"
#include "ui/NexaChrome.h"
#include "ui/NexaThemes.h"

#include "QtStringMakers.h"

#include <QAbstractButton>
#include <QListWidget>
#include <QPushButton>
#include <QTabWidget>
#include <QTest>

#include <catch2/catch_test_macros.hpp>

#include <memory>

using namespace nexa;
using namespace Qt::StringLiterals;

namespace {

struct ThemeRig {
    void open(const QString &theme) {
        Config config;
        config.ui.theme = theme;
        config.providers = {ProviderConfig{.id = u"groq"_s, .kind = QString(kKindOpenAiCompatible), .name = u"Groq"_s,
                                           .baseUrl = u"https://api.groq.com/openai/v1"_s}};
        config.activeProvider = u"groq"_s;
        SystemStatus status;
        dialog = std::make_unique<SettingsDialog>(config, &registry, &http, status);
        dialog->show();
    }
    QListWidget *gallery() const { return dialog->findChild<QListWidget *>(u"themeGallery"_s); }
    QAbstractButton *chip(const QString &category) const {
        for (QPushButton *button : dialog->findChildren<QPushButton *>(u"galleryChip"_s)) {
            if (button->property("category").toString() == category) return button;
        }
        return nullptr;
    }
    QStringList shown() const {
        QStringList ids;
        for (int i = 0; i < gallery()->count(); ++i) ids.append(gallery()->item(i)->data(Qt::UserRole).toString());
        return ids;
    }
    QString current() const {
        const QListWidgetItem *item = gallery()->currentItem();
        return item ? item->data(Qt::UserRole).toString() : QString();
    }

    HttpClient http;
    ProviderRegistry registry{&http};
    std::unique_ptr<SettingsDialog> dialog;
};

} // namespace

TEST_CASE("The theme gallery shows every pack under All, a category per chip, and keeps the choice while one hides it") {
    ThemeRig rig;
    rig.open(u"neon-cyan"_s);
    REQUIRE(rig.gallery());
    // All: every pack, in the catalogue's order, the one on current; a chip for every category.
    CHECK(rig.shown().size() == NexaThemes::all().size());
    CHECK(rig.shown().first() == u"nexa-dark"_s);
    CHECK(rig.current() == u"neon-cyan"_s);
    REQUIRE(rig.chip(QString()));
    CHECK(rig.chip(QString())->isChecked());
    for (const QString &category : NexaThemes::categories()) CHECK(rig.chip(category) != nullptr);

    // Nature: its packs only, nothing current — Neon Cyan is not among them — and OK keeps Neon Cyan.
    rig.chip(u"Nature"_s)->click();
    const QList<ThemePack> nature = NexaThemes::inCategory(u"Nature"_s);
    CHECK(rig.shown().size() == nature.size());
    for (const QString &id : rig.shown()) CHECK(NexaThemes::find(id)->category == u"Nature"_s);
    CHECK(rig.current().isEmpty());
    rig.dialog->accept();
    CHECK(rig.dialog->config().ui.theme == u"neon-cyan"_s);

    // Back to Neon: the tile is current again; a tile clicked is the choice.
    rig.chip(u"Neon"_s)->click();
    CHECK(rig.current() == u"neon-cyan"_s);
    rig.gallery()->setCurrentRow(1);
    const QString picked = rig.current();
    CHECK(NexaThemes::find(picked)->category == u"Neon"_s);
    CHECK(picked != u"neon-cyan"_s);
    rig.dialog->accept();
    CHECK(rig.dialog->config().ui.theme == picked);
}

TEST_CASE("An old or odd setting lands on its pack in the gallery, and every tile is a dark pack") {
    ThemeRig rig;
    rig.open(u"ocean-light"_s);
    CHECK(rig.current() == u"ocean"_s);
    rig.dialog->accept();
    CHECK(rig.dialog->config().ui.theme == u"ocean"_s);

    rig.open(u"system"_s);
    CHECK(rig.current() == u"nexa-dark"_s);
    for (const QString &id : rig.shown()) {
        INFO(id.toStdString());
        REQUIRE(NexaThemes::find(id).has_value());
        CHECK_FALSE(id.endsWith(u"-light"_s));
        CHECK_FALSE(id == u"system"_s);
    }
}

// NEXA_TEST_SCREENSHOTS=<dir>: the HUD category's tiles — each swatch wearing its chrome and its backdrop — as a
// picture to look at. Without the variable it only checks the chip shows them, named for their chrome.
TEST_CASE("The HUD chip shows the packs from the owner's pictures, each tile naming its chrome") {
    ThemeRig rig;
    rig.open(u"hud-console"_s);
    REQUIRE(rig.chip(u"HUD"_s));
    rig.chip(u"HUD"_s)->click();
    const QList<ThemePack> huds = NexaThemes::inCategory(u"HUD"_s);
    CHECK(rig.shown().size() == huds.size());
    CHECK(rig.current() == u"hud-console"_s);
    for (int i = 0; i < rig.gallery()->count(); ++i) {
        const QListWidgetItem *item = rig.gallery()->item(i);
        const ThemePack pack = *NexaThemes::find(item->data(Qt::UserRole).toString());
        INFO(pack.id.toStdString());
        CHECK(item->text().startsWith(pack.name));
        if (pack.chrome.panels != PanelChrome::Flat) CHECK(item->text().contains(panelChromeName(pack.chrome.panels)));
        CHECK_FALSE(item->icon().isNull());
    }
    const QString directory = qEnvironmentVariable("NEXA_TEST_SCREENSHOTS");
    if (!directory.isEmpty()) {
        if (auto *tabs = rig.dialog->findChild<QTabWidget *>()) {
            for (int i = 0; i < tabs->count(); ++i) {
                if (tabs->tabText(i) == u"General"_s) tabs->setCurrentIndex(i);
            }
        }
        rig.dialog->resize(760, 720);
        QTest::qWait(300);
        rig.dialog->grab().save(directory + u"/theme-gallery-hud.png"_s);
    }
}
