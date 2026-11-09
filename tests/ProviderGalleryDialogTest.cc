// The provider gallery: that it lists the presets it is given in PresetFilter's order, narrows them by the search
// box and the category chips, counts them, and returns the card chosen.

#include "app/ProviderGalleryDialog.h"

#include "QtStringMakers.h"

#include <QAbstractButton>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QTest>

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace Qt::StringLiterals;

namespace {

QList<ProviderPreset> sample() {
    ProviderPreset codex{.id = u"codex"_s, .kind = QString(kKindCodex), .name = u"Codex (ChatGPT)"_s, .requiresKey = false};
    codex.category = u"Programs"_s;
    ProviderPreset groq{.id = u"groq"_s, .kind = u"openai-compatible"_s, .name = u"Groq"_s,
                        .baseUrl = u"https://api.groq.com/openai/v1"_s};
    groq.category = u"Model makers"_s;
    groq.freeTier = true;
    ProviderPreset ollama{.id = u"ollama"_s, .kind = u"openai-compatible"_s, .name = u"Ollama (local)"_s,
                          .baseUrl = u"http://localhost:11434/v1"_s, .requiresKey = false};
    ollama.category = u"Local"_s;
    ProviderPreset together{.id = u"together"_s, .kind = u"openai-compatible"_s, .name = u"Together AI"_s,
                            .baseUrl = u"https://api.together.xyz/v1"_s};
    together.category = u"Inference hosts"_s;
    return {codex, groq, ollama, together};
}

const QStringList kCategories{u"Programs"_s, u"Gateways"_s, u"Model makers"_s, u"Inference hosts"_s, u"Local"_s};

QListWidget *cards(ProviderGalleryDialog &dialog) { return dialog.findChild<QListWidget *>(u"galleryList"_s); }

} // namespace

TEST_CASE("The gallery lists every preset with a count, and a chip only for a category that has one") {
    ProviderGalleryDialog dialog(sample(), kCategories);
    CHECK(dialog.shownIds() == QStringList{u"codex"_s, u"groq"_s, u"ollama"_s, u"together"_s});
    CHECK(dialog.findChild<QLabel *>(u"galleryCount"_s)->text() == u"4 of 4"_s);
    const QList<QPushButton *> chips = dialog.findChildren<QPushButton *>(u"galleryChip"_s);
    QStringList names;
    for (const QPushButton *chip : chips) names.append(chip->text());
    CHECK(names == QStringList{u"All"_s, u"Programs"_s, u"Model makers"_s, u"Inference hosts"_s, u"Local"_s});   // no Gateways
}

TEST_CASE("A card says the host, key or no key, and the free tier; a program says it runs on the PC") {
    ProviderGalleryDialog dialog(sample(), kCategories);
    QStringList texts;
    for (int row = 0; row < cards(dialog)->count(); ++row) texts.append(cards(dialog)->item(row)->text());
    CHECK(texts.at(0) == u"Codex (ChatGPT)\nruns on your PC · sign in with your account"_s);
    CHECK(texts.at(1) == u"Groq\napi.groq.com · key · free tier"_s);
    CHECK(texts.at(2) == u"Ollama (local)\nlocalhost:11434 · no key"_s);
}

TEST_CASE("Search and chips narrow the cards, and the count follows") {
    ProviderGalleryDialog dialog(sample(), kCategories);
    dialog.setQuery(u"free"_s);
    CHECK(dialog.shownIds() == QStringList{u"groq"_s});
    dialog.setQuery(u"nokey"_s);
    CHECK(dialog.shownIds() == QStringList{u"codex"_s, u"ollama"_s});
    CHECK(dialog.findChild<QLabel *>(u"galleryCount"_s)->text() == u"2 of 4"_s);
    dialog.setQuery(u"together.xyz"_s);
    CHECK(dialog.shownIds() == QStringList{u"together"_s});
    dialog.setQuery({});
    dialog.setCategory(u"Local"_s);
    CHECK(dialog.shownIds() == QStringList{u"ollama"_s});
    dialog.setQuery(u"zzz"_s);
    CHECK(dialog.shownCount() == 0);
    dialog.setCategory({});
    dialog.setQuery({});
    CHECK(dialog.shownCount() == 4);
}

TEST_CASE("Accepting returns the card in front, and nothing when there is none") {
    ProviderGalleryDialog dialog(sample(), kCategories);
    CHECK_FALSE(dialog.selectedPreset().has_value());
    dialog.setQuery(u"groq"_s);
    dialog.accept();
    REQUIRE(dialog.selectedPreset().has_value());
    CHECK(dialog.selectedPreset()->id == u"groq"_s);
    CHECK(dialog.selectedPreset()->baseUrl == u"https://api.groq.com/openai/v1"_s);

    ProviderGalleryDialog empty(sample(), kCategories);
    empty.setQuery(u"zzz"_s);
    empty.accept();                                  // nothing to take: stays open
    CHECK_FALSE(empty.selectedPreset().has_value());
}

TEST_CASE("Down from the search box moves into the cards and Enter there takes the one in front") {
    ProviderGalleryDialog dialog(sample(), kCategories);
    dialog.show();
    QLineEdit *search = dialog.findChild<QLineEdit *>(u"gallerySearch"_s);
    search->setFocus();
    QTest::keyClick(search, Qt::Key_Down);
    CHECK(dialog.focusWidget() == cards(dialog));
    CHECK(cards(dialog)->currentRow() == 0);          // the first card is in front, nothing skipped
    QTest::keyClick(cards(dialog), Qt::Key_Down);
    QTest::keyClick(cards(dialog), Qt::Key_Return);
    REQUIRE(dialog.selectedPreset().has_value());
    CHECK(dialog.selectedPreset()->id == u"groq"_s);
}

TEST_CASE("The real presets: every category chip leads to cards, and the count is the whole list") {
    const QList<ProviderPreset> &all = ProviderRegistry::presets();
    ProviderGalleryDialog dialog(all, ProviderRegistry::presetCategories());
    CHECK(dialog.shownCount() == all.size());
    CHECK(dialog.findChild<QLabel *>(u"galleryCount"_s)->text() == u"%1 of %1"_s.arg(all.size()));
    // Each lies under a category of the Add menu: none is left for "Other".
    for (const ProviderPreset &preset : all) {
        CAPTURE(preset.id, preset.category);
        CHECK(ProviderRegistry::presetCategories().contains(preset.category));
    }
    for (const QString &category : ProviderRegistry::presetCategories()) {
        dialog.setCategory(category);
        CAPTURE(category);
        CHECK(dialog.shownCount() > 0);
    }
}
